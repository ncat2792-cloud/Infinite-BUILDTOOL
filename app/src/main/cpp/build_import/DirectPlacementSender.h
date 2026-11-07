#ifndef INFINITE_BUILD_DIRECT_PLACEMENT_SENDER_H
#define INFINITE_BUILD_DIRECT_PLACEMENT_SENDER_H

#include <cstdint>
#include <string>

namespace build_import {

// Immediate block placement for the projection printer.
//
// The confirmation-driven printer path issues one ItemUse, waits up to 1.2 s for
// the client world to reflect it, and retries up to three times. That is
// correct but slow, and on servers that validate every block it is far slower
// than a human placing the same blocks.
//
// This path calls the client's own GameMode::buildBlock instead. The game then
// assembles the InventoryTransaction packet itself, applies its local
// prediction and sends the request, which is exactly what happens when the
// player taps a block. Placement is therefore as immediate as a normal click,
// and the caller can keep firing at the configured rate without waiting for a
// per-block round trip.
//
// The trade-off is deliberate: without a per-block confirmation the printer
// cannot detect a server rejection, so the UI exposes it as a switch and the
// verified path remains available.
//
// Every address below belongs to the NetEase 3.10.5 client. They are resolved
// and fingerprinted at startup; a mismatch disables this path instead of
// jumping into unknown code.
class DirectPlacementSender {
public:
    // Verifies the 3.10.5 buildBlock profile. Safe to call repeatedly; the
    // result is cached after the first successful verification.
    static bool available();
    // Human readable reason when available() is false.
    static const char* diagnostic();

    // Places `block_name` at the given position on `face`. Must run on the
    // local-player game tick thread. `hotbar_slot` selects which hotbar
    // entry is used (-1 keeps the current selection).
    static bool place(const std::string& block_name, int32_t x, int32_t y, int32_t z,
                      int face, int32_t hotbar_slot, std::string* error);
};

}  // namespace build_import

#endif  // INFINITE_BUILD_DIRECT_PLACEMENT_SENDER_H
