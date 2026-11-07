#pragma once
#include "Arm64SignatureScanner.h"
#include <vector>
#include <string>
namespace script_host {
struct Image {
    uintptr_t base=0;
    std::vector<arm64_signature::CodeRange> code,data,writable;
    std::vector<uint8_t> buildId;
};
bool Resolve(const Image&,std::string*);
bool Ready() noexcept;
bool Operational() noexcept;
uintptr_t Base() noexcept;
bool CodeOffset(uintptr_t,uintptr_t,size_t,uintptr_t*);
bool Read(uintptr_t,void*,size_t) noexcept;
bool EnsureTickObserver();
std::string InitializationStatus();
}
