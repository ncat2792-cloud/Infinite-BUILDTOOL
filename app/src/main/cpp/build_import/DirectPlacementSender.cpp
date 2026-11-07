#include "DirectPlacementSender.h"

#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace build_import {
namespace {

// NetEase 3.10.5 (build-id a494de85059e92abc01d634303923946561a8a8b).
//
// GameMode::buildBlock(this, const BlockPos&, unsigned char face, bool) is the
// same entry point a real tap on a block reaches. Its prologue is checked
// below, and its body was confirmed to read this->actor at +8 (matching
// GameMode_ActorOffset) and to call ItemStack::isNull at 0xD795008 before it
// proceeds, which is exactly the guard a stock placement performs.
constexpr uintptr_t kBuildBlockRva = 0x0D06EA80ULL;
constexpr uintptr_t kGetCarriedItemRva = 0x0D1D701CULL;

constexpr size_t kGameModeActorOffset = 0x8U;
constexpr size_t kPlayerGameModeOffset = 0x13E8U;

constexpr std::array<uint8_t, 32U> kBuildBlockFingerprint{{
    0xFFU, 0x83U, 0x04U, 0xD1U, 0xFDU, 0x7BU, 0x0DU, 0xA9U,
    0xFCU, 0x67U, 0x0EU, 0xA9U, 0xF8U, 0x5FU, 0x0FU, 0xA9U,
    0xF6U, 0x57U, 0x10U, 0xA9U, 0xF4U, 0x4FU, 0x11U, 0xA9U,
    0xFDU, 0x43U, 0x03U, 0x91U, 0x59U, 0xD0U, 0x3BU, 0xD5U,
}};
constexpr std::array<uint8_t, 32U> kGetCarriedItemFingerprint{{
    0x08U, 0x14U, 0x46U, 0xF9U, 0x09U, 0x61U, 0x44U, 0x39U,
    0x89U, 0x00U, 0x00U, 0x34U, 0xA0U, 0xC5U, 0x02U, 0xB0U,
    0x00U, 0xA0U, 0x02U, 0x91U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x00U, 0x91U, 0x40U, 0xF9U, 0x01U, 0x11U, 0x40U, 0xB9U,
}};

struct BlockPos {
    int32_t x;
    int32_t y;
    int32_t z;
};
static_assert(sizeof(BlockPos) == 12U, "unexpected BlockPos ABI");

using GetCarriedItem = const void* (*)(void* player);
using BuildBlock = void (*)(void* game_mode, const void* pos, uint8_t face, bool);

struct Resolved {
    uintptr_t build_block = 0;
    uintptr_t get_carried_item = 0;
    bool verified = false;
};

Resolved& resolved() {
    static Resolved value;
    return value;
}

std::string& diagnostic() {
    static std::string value = "尚未初始化";
    return value;
}

bool exactExecutable(uintptr_t module_base, uintptr_t address) {
    if (!module_base || address < module_base) return false;
    const uintptr_t offset = address - module_base;
    uintptr_t resolved_address = 0;
    return ResolveMinecraftExecutableOffset(module_base, offset, 4U, &resolved_address) &&
           resolved_address == address;
}

template <size_t N>
bool matchFingerprint(uintptr_t module_base, uintptr_t rva,
                      const std::array<uint8_t, N>& fingerprint,
                      uintptr_t* output) {
    uintptr_t address = 0;
    if (!ResolveMinecraftExecutableOffset(module_base, rva, fingerprint.size(), &address)) {
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(address), fingerprint.data(),
                    fingerprint.size()) != 0) {
        return false;
    }
    if (output) *output = address;
    return true;
}

bool verify() {
    if (resolved().verified) return true;
    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) {
        diagnostic() = "libminecraftpe.so 尚未加载";
        return false;
    }
    Resolved candidate;
    if (!matchFingerprint(module_base, kBuildBlockRva, kBuildBlockFingerprint,
                          &candidate.build_block)) {
        diagnostic() = "3.10.5 GameMode::buildBlock 指纹不匹配";
        return false;
    }
    if (!matchFingerprint(module_base, kGetCarriedItemRva, kGetCarriedItemFingerprint,
                          &candidate.get_carried_item)) {
        diagnostic() = "3.10.5 Player::getCarriedItem 指纹不匹配";
        return false;
    }
    if (!exactExecutable(module_base, candidate.build_block) ||
        !exactExecutable(module_base, candidate.get_carried_item)) {
        diagnostic() = "3.10.5 放置函数不在可执行段内";
        return false;
    }
    candidate.verified = true;
    resolved() = candidate;
    diagnostic() = "直放已就绪";
    return true;
}

bool fail(std::string* error, const char* detail) {
    if (error) *error = detail;
    return false;
}

}  // namespace

bool DirectPlacementSender::available() {
    if (resolved().verified) return true;
    // Verification is retried until it succeeds: the module library can be
    // loaded before the game image has finished mapping.
    return verify();
}

const char* DirectPlacementSender::diagnostic() { return diagnostic().c_str(); }

bool DirectPlacementSender::place(const std::string& block_name, int32_t x, int32_t y, int32_t z,
                                  int face, int32_t hotbar_slot, std::string* error) {
    if (error) error->clear();
    if (!available()) return fail(error, diagnostic().c_str());
    if (face < 0 || face > 5) return fail(error, "放置面必须在 0..5 之间");
    if (block_name.empty()) return fail(error, "目标方块名为空");

    // The verified game tick owns the local player pointer; a JNI or UI thread
    // must never read it.
    if (!IsMinecraftUpdateGameThread()) {
        return fail(error, "直放只能在本地玩家 tick 线程执行");
    }
    void* const player = GetLocalPlayerPointer();
    if (!player || !IsMemoryReadable(player, kPlayerGameModeOffset + sizeof(uintptr_t))) {
        return fail(error, "本地玩家指针不可用");
    }

    // Hotbar selection is left to the caller: the printer already synchronises
    // it, and buildBlock uses whatever the player currently holds.
    (void)hotbar_slot;
    (void)block_name;

    void* game_mode = nullptr;
    std::memcpy(&game_mode, static_cast<const uint8_t*>(player) + kPlayerGameModeOffset,
                sizeof(game_mode));
    if (!game_mode || !IsMemoryReadable(game_mode, kGameModeActorOffset + sizeof(void*))) {
        return fail(error, "GameMode 指针不可用");
    }

    // The carried item must exist: buildBlock silently no-ops on an empty hand.
    const void* carried = reinterpret_cast<GetCarriedItem>(resolved().get_carried_item)(player);
    if (!carried) return fail(error, "当前手持物品为空");

    const BlockPos position{x, y, z};
    reinterpret_cast<BuildBlock>(resolved().build_block)(
        game_mode, &position, static_cast<uint8_t>(face), false);
    return true;
}

}  // namespace build_import
