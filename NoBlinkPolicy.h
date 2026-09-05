#pragma once

#include <cstdint>

namespace NoBlinkPolicy
{
    constexpr bool ShouldRebind(
        const uint8_t entityType, const uint16_t actorLock, const bool serialized = false)
    {
        return entityType == 0 && (actorLock == 0 || serialized);
    }

    constexpr bool ShouldSerializeAppearanceChange(const uint32_t changedSlots)
    {
        return changedSlots > 1;
    }

    constexpr bool ReplacementReady(const uint32_t arrived, const uint32_t expected,
        const uint32_t completionMask, const bool taskPending)
    {
        return !taskPending && arrived >= expected && (completionMask & 0x1FFu) == 0x1FFu;
    }
}
