#ifndef PYLOADER_ARM64_SIGNATURE_SCANNER_H
#define PYLOADER_ARM64_SIGNATURE_SCANNER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace arm64_signature {

struct Instruction {
    uint32_t value = 0;
    uint32_t mask = 0xffffffffU;
};

// The caller supplies readable executable image ranges. Address is the image
// virtual address, which may differ from data when scanning an ELF file.
struct CodeRange {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uintptr_t address = 0;
};

struct Signature {
    const Instruction* instructions = nullptr;
    size_t count = 0;
    // Allows an anchor inside a function to identify its entry point.
    int32_t entry_delta = 0;
};

// One resolution transaction only: ranges must stay immutable until all scans
// finish, before any Dobby patch. Positions cover every aligned word in every
// executable range; this is an acceleration index, never an RVA hint/cache.
class ScanIndex {
public:
    struct Location { size_t range; size_t offset; };
    ScanIndex() noexcept;
    ~ScanIndex();
    ScanIndex(const ScanIndex&) = delete;
    ScanIndex& operator=(const ScanIndex&) = delete;
    bool Build(const CodeRange* ranges, size_t range_count,
               const Signature* signatures, size_t signature_count) noexcept;
    bool Candidates(const CodeRange* ranges, size_t range_count,
                    const Signature& signature, size_t* anchor,
                    const std::vector<Location>** locations) const noexcept;
    size_t PositionCount() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class ScanStatus : uint8_t {
    Unique,
    NotFound,
    Ambiguous,
    InvalidInput,
};

struct ScanResult {
    ScanStatus status = ScanStatus::InvalidInput;
    uintptr_t address = 0;
    // Capped at two; an ambiguous result never publishes an address.
    size_t matches = 0;
};

ScanResult Scan(const CodeRange* ranges, size_t range_count,
                const Signature& signature, const ScanIndex* index = nullptr) noexcept;
// A complete short/templated owner can share machine code except for one BL.
// This searches all matches but publishes only the uniquely identified owner
// whose decoded callee has the independently reviewed complete leaf body.
ScanResult ScanWithCallIdentity(const CodeRange* ranges, size_t range_count,
                               const Signature& signature, size_t branch_word,
                               const Signature& callee_body, const ScanIndex* index = nullptr) noexcept;
ScanResult ScanWithCallAddress(const CodeRange* ranges,size_t range_count,
                              const Signature& signature,size_t branch_word,
                              uintptr_t callee_address, const ScanIndex* index = nullptr) noexcept;
// Select an otherwise templated constructor by its decoded, independently
// discovered class table. The whole constructor body is still checked.
ScanResult ScanWithReferenceAddress(const CodeRange* ranges,size_t range_count,
                                   const Signature& signature,size_t adrp_word,
                                   size_t consumer_word,uintptr_t address, const ScanIndex* index = nullptr) noexcept;

// Masks only PC-relative immediates, preserving opcodes and register operands.
// Object field offsets and calling conventions remain part of the signature.
uint32_t InstructionMask(uint32_t instruction) noexcept;

// Decodes ADRP followed by ADD (64-bit) or an integer unsigned-offset
// load/store using the ADRP destination register. The pair may locate a
// writable global such as the interpreter's execution-lock flag.
bool DecodeAdrpReference(uintptr_t adrp_address, uint32_t adrp,
                         uint32_t consumer, uintptr_t* target) noexcept;
bool DecodeAdrReference(uintptr_t instruction_address,uint32_t adr,uintptr_t* target) noexcept;

}  // namespace arm64_signature

#endif


