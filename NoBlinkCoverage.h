// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NoBlinkCoverage
{
    constexpr size_t kActorCoverage = 0x674 + 0xA9;
    constexpr size_t kCount = 8;

    // FUN_01a6db40 resource+0x33 switch, 01a6defc..01a6dfa3.
    // Material selectors index owner+0xA8; these are selectors 1..8 only.
    // Preserve selector zero and every adjacent field.
    constexpr void Accumulate(uint8_t type, uint8_t (&flags)[kCount])
    {
        switch (type)
        {
        case 2: flags[0] = 1; break;
        case 3: flags[0] = flags[1] = 1; break;
        case 4: flags[0] = flags[1] = flags[2] = 1; break;
        case 5: flags[0] = flags[1] = flags[2] = flags[3] = flags[7] = 1; break;
        case 6: flags[0] = flags[1] = flags[2] = flags[7] = 1; break;
        case 0x12: flags[4] = 1; break;
        case 0x22: flags[6] = 1; break;
        case 0x32: flags[5] = 1; break;
        default: break;
        }
    }

    inline void Publish(uint8_t* actor, const uint8_t (&flags)[kCount])
    {
        std::memcpy(actor + kActorCoverage, flags, kCount);
    }
}
