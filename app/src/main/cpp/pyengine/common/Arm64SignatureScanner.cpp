#include "Arm64SignatureScanner.h"

#include <cstring>
#include <limits>
#include <unordered_map>
#include <algorithm>
#include <array>

namespace arm64_signature {
namespace {

uint32_t readInstruction(const uint8_t* data) noexcept {
    uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool addSigned(uintptr_t value, int64_t delta, uintptr_t* result) noexcept {
    if (delta >= 0) {
        const uint64_t amount = static_cast<uint64_t>(delta);
        if (amount > std::numeric_limits<uintptr_t>::max() - value) return false;
        *result = value + static_cast<uintptr_t>(amount);
    } else {
        const uint64_t amount = static_cast<uint64_t>(-(delta + 1)) + 1U;
        if (amount > value) return false;
        *result = value - static_cast<uintptr_t>(amount);
    }
    return true;
}

unsigned fixedBits(uint32_t value) noexcept {
    unsigned count = 0;
    while (value) {
        value &= value - 1U;
        ++count;
    }
    return count;
}

}  // namespace

struct ScanIndex::Impl {
    struct Word { size_t count = 0; bool selected = false; std::vector<Location> positions; };
    // Only used while constructing this immutable transaction's postings. A
    // sparse contiguous table avoids one unordered-map node lookup per image
    // instruction. The bitmap is only a negative filter: every possible hit
    // still compares the complete instruction value in the probe chain.
    struct LookupSlot { uint32_t value = 0; Word* word = nullptr; };
    static constexpr size_t kMaximumLookupSlots = 1U << 20U;
    static constexpr size_t kFilterBits = 1U << 16U;
    std::vector<CodeRange> ranges;
    std::unordered_map<uint32_t, Word> words;
    std::vector<LookupSlot> lookup;
    std::array<uint64_t, kFilterBits / 64U> filter{};
    size_t positions = 0;
    static uint32_t hash(uint32_t value) noexcept {
        value ^= value >> 16U;
        value *= 0x7feb352dU;
        value ^= value >> 15U;
        value *= 0x846ca68bU;
        return value ^ (value >> 16U);
    }
    bool buildLookup(bool selected_only) {
        size_t count = 0;
        for (const auto& item : words)
            count += !selected_only || item.second.selected;
        if (count > kMaximumLookupSlots / 2U) return false;
        size_t capacity = 2U;
        while (capacity < count * 2U) capacity *= 2U;
        lookup.assign(capacity, {});
        filter.fill(0);
        const size_t mask = capacity - 1U;
        for (auto& item : words) {
            if (selected_only && !item.second.selected) continue;
            const uint32_t hashed = hash(item.first);
            const size_t bit = hashed >> 16U;
            filter[bit / 64U] |= uint64_t{1} << (bit % 64U);
            size_t slot = hashed & mask;
            while (lookup[slot].word) slot = (slot + 1U) & mask;
            lookup[slot] = {item.first, &item.second};
        }
        return true;
    }
    Word* find(uint32_t value) const noexcept {
        const uint32_t hashed = hash(value);
        const size_t bit = hashed >> 16U;
        if (!(filter[bit / 64U] & (uint64_t{1} << (bit % 64U)))) return nullptr;
        const size_t mask = lookup.size() - 1U;
        size_t slot = hashed & mask;
        for (;;) {
            const auto& candidate = lookup[slot];
            if (!candidate.word || candidate.value == value) return candidate.word;
            slot = (slot + 1U) & mask;
        }
    }
    size_t anchor(const Signature& signature) const noexcept {
        size_t best = signature.count, frequency = SIZE_MAX;
        for (size_t i = 0; i < signature.count; ++i) {
            if (signature.instructions[i].mask != UINT32_MAX) continue;
            const auto found = words.find(signature.instructions[i].value);
            if (found != words.end() && found->second.count < frequency) {
                best = i; frequency = found->second.count;
            }
        }
        return best;
    }
};
ScanIndex::ScanIndex() noexcept = default;
ScanIndex::~ScanIndex() = default;

bool ScanIndex::Build(const CodeRange* ranges, size_t range_count,
                      const Signature* signatures, size_t signature_count) noexcept {
    impl_.reset();
    constexpr size_t kMaximumPositions = 2U * 1024U * 1024U;
    constexpr size_t kMaximumWordPositions = 32768U;
    if (!ranges || !range_count || !signatures || !signature_count) return false;
    try {
        auto next = std::make_unique<Impl>();
        next->ranges.assign(ranges, ranges + range_count);
        for (const auto& range : next->ranges)
            if ((!range.data && range.size) || (range.address & 3U) ||
                range.size > UINTPTR_MAX - range.address) return false;
        for (size_t i = 0; i < signature_count; ++i) {
            const auto& signature = signatures[i];
            if (!signature.instructions || !signature.count || signature.count > 256U) continue;
            for (size_t w = 0; w < signature.count; ++w) {
                if (signature.instructions[w].mask == UINT32_MAX) {
                    next->words.try_emplace(signature.instructions[w].value);
                    if (next->words.size() > Impl::kMaximumLookupSlots / 2U) return false;
                }
            }
        }
        if (next->words.empty() || !next->buildLookup(false)) return false;
        for (const auto& range : next->ranges)
            for (size_t offset = 0; range.size >= 4U && offset <= range.size - 4U; offset += 4U) {
                if (auto* word = next->find(readInstruction(range.data + offset))) ++word->count;
            }
        for (size_t i = 0; i < signature_count; ++i) {
            const auto& signature = signatures[i];
            if (!signature.instructions || !signature.count || signature.count > 256U) continue;
            const size_t at = next->anchor(signature);
            if (at == signature.count) continue;
            auto& word = next->words.at(signature.instructions[at].value);
            if (word.selected || word.count > kMaximumWordPositions) continue;
            if (word.count > kMaximumPositions - next->positions) return false;
            word.selected = true; next->positions += word.count;
            word.positions.reserve(word.count);
        }
        if (!next->buildLookup(true)) return false;
        for (size_t r = 0; r < range_count; ++r) {
            const auto& range = ranges[r];
            for (size_t offset = 0; range.size >= 4U && offset <= range.size - 4U; offset += 4U) {
                if (auto* word = next->find(readInstruction(range.data + offset)))
                    word->positions.push_back({r, offset});
            }
        }
        // Scan-time Candidates uses the complete frequency table; construction
        // acceleration storage is not retained for the transaction lifetime.
        std::vector<Impl::LookupSlot>{}.swap(next->lookup);
        impl_ = std::move(next);
        return true;
    } catch (...) { return false; } // Preserve the complete linear scan on allocation failure.
}

bool ScanIndex::Candidates(const CodeRange* ranges, size_t range_count,
                           const Signature& signature, size_t* anchor,
                           const std::vector<Location>** locations) const noexcept {
    if (!impl_ || !ranges || !anchor || !locations || !signature.instructions ||
        !signature.count || signature.count > 256U || range_count != impl_->ranges.size()) return false;
    for (size_t r = 0; r < range_count; ++r) {
        const auto& original = impl_->ranges[r];
        if (ranges[r].data != original.data || ranges[r].size != original.size ||
            ranges[r].address != original.address) return false;
    }
    const size_t at = impl_->anchor(signature);
    if (at == signature.count) return false;
    const auto found = impl_->words.find(signature.instructions[at].value);
    if (found == impl_->words.end() || !found->second.selected) return false;
    *anchor = at; *locations = &found->second.positions;
    return true;
}
size_t ScanIndex::PositionCount() const noexcept { return impl_ ? impl_->positions : 0U; }

namespace {
template<bool Indexed>
ScanResult scanImpl(const CodeRange* ranges, size_t range_count,
                    const Signature& signature, size_t branch_word,
                    const Signature* callee_body,uintptr_t callee_address=0,
                    uintptr_t reference_address=0,size_t adrp_word=0,size_t consumer_word=0,
                    const ScanIndex* index=nullptr) noexcept {
    constexpr size_t kMaximumInstructions = 256U;
    if (!ranges || !range_count || !signature.instructions || !signature.count ||
        signature.count > kMaximumInstructions || signature.entry_delta % 4 != 0) {
        return {};
    }
    size_t anchor = 0;
    unsigned most_fixed_bits = 0;
    for (size_t i = 0; i < signature.count; ++i) {
        const unsigned bits = fixedBits(signature.instructions[i].mask);
        if (bits > most_fixed_bits) {
            most_fixed_bits = bits;
            anchor = i;
        }
    }
    // An all-wildcard pattern cannot identify code.
    if (!most_fixed_bits) return {};
    // Validate every segment before publishing even a unique earlier match.
    for (size_t i = 0; i < range_count; ++i) {
        const CodeRange& range = ranges[i];
        if ((!range.data && range.size) || (range.address & 3U) != 0U ||
            range.size > std::numeric_limits<uintptr_t>::max() - range.address) {
            return {};
        }
    }

    const std::vector<ScanIndex::Location>* positions = nullptr;
    if constexpr (Indexed) {
        if (!index || !index->Candidates(ranges, range_count, signature, &anchor, &positions))
            return scanImpl<false>(ranges, range_count, signature, branch_word,
                callee_body, callee_address, reference_address, adrp_word, consumer_word, nullptr);
    }
    const size_t bytes = signature.count * sizeof(uint32_t);
    const size_t anchor_offset = anchor * sizeof(uint32_t);
    const Instruction& anchor_instruction = signature.instructions[anchor];
    ScanResult result{ScanStatus::NotFound, 0, 0};
    size_t position = 0;
    for (size_t i = 0; i < range_count; ++i) {
        const CodeRange& range = ranges[i];
        if (range.size < bytes) continue;
        const size_t last = range.size - bytes;
        size_t linear_offset = 0;
        for (;;) {
            size_t offset = 0;
            if constexpr (Indexed) {
                while (position < positions->size() && (*positions)[position].range < i) ++position;
                if (position == positions->size() || (*positions)[position].range != i) break;
                const size_t at = (*positions)[position++].offset;
                if (at < anchor_offset) continue;
                offset = at - anchor_offset;
                if (offset > last) continue;
            } else {
                if (linear_offset > last) break;
                offset = linear_offset; linear_offset += sizeof(uint32_t);
            }
            if (((readInstruction(range.data + offset + anchor_offset) ^
                  anchor_instruction.value) & anchor_instruction.mask) != 0U) {
                continue;
            }
            bool matches = true;
            for (size_t word = 0; word < signature.count; ++word) {
                if (word == anchor) continue;
                const Instruction& expected = signature.instructions[word];
                if (((readInstruction(range.data + offset + word * sizeof(uint32_t)) ^
                      expected.value) & expected.mask) != 0U) {
                    matches = false;
                    break;
                }
            }
            if (matches && (callee_body || callee_address)) {
                if (branch_word >= signature.count || (callee_body && (!callee_body->instructions ||
                    !callee_body->count || callee_body->count > 256U))) return {};
                const uint32_t call=readInstruction(range.data+offset+branch_word*4U);
                if ((call&0xfc000000U)!=0x94000000U) continue;
                int64_t imm=call&0x03ffffffU;
                if (imm&0x02000000U) imm-=1LL<<26U;
                uintptr_t target=0;
                if (!addSigned(range.address+offset+branch_word*4U,imm*4,&target)) continue;
                if (callee_address && target!=callee_address) continue;
                if (callee_body) {
                const uint8_t* body=nullptr;
                for (size_t r=0;r<range_count;++r) {
                    const auto& segment=ranges[r];
                    if (target>=segment.address && target-segment.address<=segment.size &&
                        callee_body->count*4U<=segment.size-(target-segment.address))
                        body=segment.data+(target-segment.address);
                }
                if (!body) continue;
                for (size_t w=0;w<callee_body->count;++w)
                    if (((readInstruction(body+w*4U)^callee_body->instructions[w].value)&callee_body->instructions[w].mask)!=0U) {matches=false;break;}
                }
            }
            if(matches && reference_address) {
                if(adrp_word>=signature.count || consumer_word>=signature.count)return {};
                uintptr_t decoded=0;
                const uintptr_t pc=range.address+offset+adrp_word*4U;
                const uint32_t adr=readInstruction(range.data+offset+adrp_word*4U);
                const uint32_t consumer=readInstruction(range.data+offset+consumer_word*4U);
                const bool valid=adrp_word==consumer_word
                    ? DecodeAdrReference(pc,adr,&decoded)
                    : DecodeAdrpReference(pc,adr,consumer,&decoded);
                matches=valid && decoded==reference_address;
            }
            uintptr_t entry = 0;
            if (!matches || !addSigned(range.address + offset, signature.entry_delta,
                                       &entry) || entry < range.address ||
                range.size < sizeof(uint32_t) ||
                entry - range.address > range.size - sizeof(uint32_t)) {
                continue;
            }
            // Duplicate ranges can refer to the same physical code location.
            if (result.matches && result.address == entry) continue;
            if (++result.matches == 2U) {
                return {ScanStatus::Ambiguous, 0, 2};
            }
            result.address = entry;
            result.status = ScanStatus::Unique;
        }
    }
    return result;
}
} // namespace
ScanResult Scan(const CodeRange* ranges,size_t count,const Signature& signature,const ScanIndex* index) noexcept {
    return index ? scanImpl<true>(ranges,count,signature,0,nullptr,0,0,0,0,index)
                 : scanImpl<false>(ranges,count,signature,0,nullptr);
}
ScanResult ScanWithCallIdentity(const CodeRange* ranges,size_t count,const Signature& signature,
                               size_t branch_word,const Signature& body,const ScanIndex* index) noexcept {
    return index ? scanImpl<true>(ranges,count,signature,branch_word,&body,0,0,0,0,index)
                 : scanImpl<false>(ranges,count,signature,branch_word,&body);
}
ScanResult ScanWithCallAddress(const CodeRange* ranges,size_t count,const Signature& signature,
                              size_t branch_word,uintptr_t address,const ScanIndex* index) noexcept {
    if (!address || address&3U) return {};
    return index ? scanImpl<true>(ranges,count,signature,branch_word,nullptr,address,0,0,0,index)
                 : scanImpl<false>(ranges,count,signature,branch_word,nullptr,address);
}
ScanResult ScanWithReferenceAddress(const CodeRange* ranges,size_t count,const Signature& signature,
                                   size_t adrp_word,size_t consumer_word,uintptr_t address,const ScanIndex* index) noexcept {
    if(!address)return {};
    return index ? scanImpl<true>(ranges,count,signature,0,nullptr,0,address,adrp_word,consumer_word,index)
                 : scanImpl<false>(ranges,count,signature,0,nullptr,0,address,adrp_word,consumer_word);
}

uint32_t InstructionMask(uint32_t word) noexcept {
    if ((word & 0x7c000000U) == 0x14000000U) return 0xfc000000U;  // B, BL
    if ((word & 0xff000010U) == 0x54000000U) return 0xff00001fU;  // B.cond
    if ((word & 0x7e000000U) == 0x34000000U) return 0xff00001fU;  // CBZ, CBNZ
    if ((word & 0x7e000000U) == 0x36000000U) return 0xfff8001fU;  // TBZ, TBNZ
    if ((word & 0x1f000000U) == 0x10000000U) return 0x9f00001fU;  // ADR, ADRP
    if ((word & 0x3b000000U) == 0x18000000U) return 0xff00001fU;  // LDR literal
    return 0xffffffffU;
}

bool DecodeAdrpReference(uintptr_t pc, uint32_t adrp, uint32_t consumer,
                         uintptr_t* target) noexcept {
    if (!target) return false;
    *target = 0;
    if ((pc & 3U) != 0U || (adrp & 0x9f000000U) != 0x90000000U ||
        (adrp & 31U) == 31U || ((consumer >> 5U) & 31U) != (adrp & 31U)) {
        return false;
    }
    uint64_t offset = 0;
    if ((consumer & 0xff800000U) == 0x91000000U) {  // ADD Xd, Xn, #imm
        offset = (consumer >> 10U) & 0xfffU;
        if (consumer & (1U << 22U)) offset *= 4096U;
    } else if ((consumer & 0x3f000000U) == 0x39000000U &&
               !(((consumer >> 30U) & 3U) == 3U &&
                 ((consumer >> 22U) & 3U) == 2U)) {  // integer LDR/STR, not PRFM
        offset = static_cast<uint64_t>((consumer >> 10U) & 0xfffU)
                 << ((consumer >> 30U) & 3U);
    } else {
        return false;
    }
    const uint32_t immediate = (((adrp >> 5U) & 0x7ffffU) << 2U) |
                               ((adrp >> 29U) & 3U);
    const int64_t signed_immediate = (immediate & (1U << 20U))
        ? static_cast<int64_t>(immediate) - (1LL << 21U)
        : static_cast<int64_t>(immediate);
    uintptr_t page = 0;
    if (!addSigned(pc & ~uintptr_t{0xfffU}, signed_immediate * 4096LL, &page) ||
        offset > std::numeric_limits<uintptr_t>::max() - page) {
        return false;
    }
    *target = page + static_cast<uintptr_t>(offset);
    return true;
}
bool DecodeAdrReference(uintptr_t pc,uint32_t adr,uintptr_t* target) noexcept {
    if(!target)return false;
    *target=0;
    if(pc&3U || (adr&0x9f000000U)!=0x10000000U || (adr&31U)==31U)return false;
    const uint32_t bits=((adr>>5U)&0x7ffffU)<<2U | ((adr>>29U)&3U);
    const int64_t delta=bits&(1U<<20U)?static_cast<int64_t>(bits)-(1LL<<21U):bits;
    return addSigned(pc,delta,target);
}

}  // namespace arm64_signature


