/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "pointers.hpp"

#include "util/signature.hpp"

#include "../global.hpp"

#include <cstdio>

namespace er::params {

Pointers gPointers;

static uintptr_t scanIndirectPointer(const char *pattern, size_t offset) {
    util::Signature sig(pattern);
    if (auto res = sig.scan()) {
        auto addr = res.add(res.add(offset).as<std::uint32_t &>() + offset + 4);
        return addr.as<uintptr_t>();
    }
    return 0;
}

#define initIndirectPointer(name, pattern, offset) do { \
    if (name == 0) { \
        name = scanIndirectPointer(pattern, offset); \
        if (name == 0) break; \
    } \
} while (0)

void Pointers::init() {
    if (initialized_) return;
    initIndirectPointer(csRegulationManager, "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0B 4C 8B C0 48 8B D7", 3);
    if (csRegulationManager == 0 && gGameVersion != 0) {
        // Fallback: known RVAs of the CSRegulationManager global, generated offline
        // from per-version tables (veeenu/eldenring-practice-tool lib/libeldenring
        // codegen, verified against game versions 2.06.0-2.07.2). Keyed by the exe's
        // file version (major.minor.patch), with whole-lines sharing one RVA.
        uint64_t version = gGameVersion;
        uint32_t keyHi = static_cast<uint32_t>(version >> 32);            // dwFileVersionMS: major.minor
        uint32_t patch = static_cast<uint32_t>((version >> 16) & 0xFFFF); // dwFileVersionLS high: patch
        uintptr_t rva = 0;
        if (keyHi == 0x00020006) {
            rva = 0x3d86c58; // 2.06.0 - 2.06.2
        } else if (keyHi == 0x00020007 && patch < 2) {
            rva = 0x3d8acd8; // 2.07.0 - 2.07.1
        } else if (keyHi == 0x00020007) {
            rva = 0x3d8dcf8; // 2.07.2
        }
        if (rva != 0) {
            csRegulationManager = util::Module(nullptr).base().as<uintptr_t>() + rva;
            fwprintf(stderr, L"params: using known regulation-manager RVA for game version %u.%u.%u\n",
                     keyHi >> 16, keyHi & 0xFFFF, patch);
        }
    }
    if (csRegulationManager == 0) {
        uint64_t version = gGameVersion;
        fwprintf(stderr, L"params: regulation-manager signature not found in eldenring.exe "
                         L"(game version %u.%u.%u.%u); param tables unavailable\n",
                 unsigned(version >> 48), unsigned(version >> 32) & 0xFFFF,
                 unsigned(version >> 16) & 0xFFFF, unsigned(version) & 0xFFFF);
    }
    initialized_ = true;
}

}
