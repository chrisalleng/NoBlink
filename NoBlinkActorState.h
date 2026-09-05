#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NoBlinkActorState
{
    constexpr size_t kModelOwnerFlags = 0x674 + 0x30;
    constexpr uint32_t kGraphicsSetupComplete = 4u;

    // Existing du1-eff84653ed2670fb: FUN_01a6c290 skips per-model graphics setup
    // while owner+0x30 bit 2 is set. Preserve all other owner state and actor identity.
    inline void InvalidateGraphics(uint8_t* actor)
    {
        uint32_t flags = 0;
        std::memcpy(&flags, actor + kModelOwnerFlags, sizeof(flags));
        flags &= ~kGraphicsSetupComplete;
        std::memcpy(actor + kModelOwnerFlags, &flags, sizeof(flags));
    }

    constexpr size_t kRenderCache = 0x674 + 0xB8;
    constexpr size_t kRenderRefreshRequest = 0xA04;

    inline void RequestFullRender(uint8_t* actor)
    {
        InvalidateGraphics(actor);
        // du1-d2074dc02531afb4 / du1-93cd344a392bcc59: cached rendering can skip
        // both model pose publication and skinned-stream writes when owner+B8 is 1.
        // du1-899cd6f8908da92c / FUN_01ae4a60: A04 requests the active render-update
        // path, which sets the per-actor graphics-pass refresh gate for its resources.
        // Use the actor-local request; do not overwrite the process-global pass gate.
        actor[kRenderCache] = 0;
        actor[kRenderRefreshRequest] = 1;
    }

    constexpr size_t kAppearanceMetadata = 0x878;
    constexpr size_t kAppearanceMetadataSize = 0x26;
    constexpr size_t kAppearanceCompletionMask = 0x8B0;

    /** Restores the appearance fields initialized by a stock actor construction. */
    inline void ResetAppearance(uint8_t* actor)
    {
        std::memset(actor + kAppearanceMetadata, 0xFF, kAppearanceMetadataSize);
        const uint32_t empty = 0;
        std::memcpy(actor + kAppearanceCompletionMask, &empty, sizeof(empty));
    }
}
