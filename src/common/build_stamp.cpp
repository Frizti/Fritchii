// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdio>

#include "common/build_stamp.h"

namespace Common {

// 16-byte marker followed by a 16-byte slot. Packaging tools find the marker in the binary and
// write the slot; the volatile reads keep the compiler from folding either. A plain build's slot
// holds a value derived from the marker; anything else (a stamp, zeros, an edit) is a stamped copy.
extern "C" {
extern const volatile unsigned char fritchii_build_stamp[32];
const volatile unsigned char fritchii_build_stamp[32] = {
    0xc5, 0xe5, 0x91, 0xf0, 0xf3, 0x0a, 0xfe, 0x2c, 0xe3, 0x2d, 0xf2, 0xb0, 0x60, 0x6e, 0x9d, 0xc1,
    0x70, 0x51, 0x06, 0xa0, 0xb6, 0x58, 0x05, 0x48, 0x4a, 0x51, 0xb5, 0xe8, 0xb9, 0x1c, 0x66, 0x63,
};
}

std::string BuildStamp() {
    unsigned char id[16];
    bool plain = true;
    for (std::size_t i = 0; i < 16; ++i) {
        id[i] = fritchii_build_stamp[16 + i];
        const unsigned char base = fritchii_build_stamp[i];
        plain &= id[i] == static_cast<unsigned char>(base * 7 + 13 + i);
    }
    if (plain) {
        return {};
    }
    char hex[33];
    for (std::size_t i = 0; i < 16; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", id[i]);
    }
    return std::string(hex, 32);
}

} // namespace Common
