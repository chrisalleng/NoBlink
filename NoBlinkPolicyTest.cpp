#include "NoBlinkActorState.h"
#include "NoBlinkChain.h"
#include "NoBlinkCoverage.h"
#include "NoBlinkPolicy.h"

#include <algorithm>
#include <cstdint>

namespace
{
    struct Node
    {
        int Id;
        Node* Next;
    };

    constexpr bool KeepsInitializedModelButHidesOldLookFromLoader()
    {
        Node oldHelm{2, nullptr};
        Node oldBaseHead{1, &oldHelm};
        Node* live = &oldBaseHead;

        Node* detached = NoBlinkChain::Detach(live);
        if (live != nullptr || detached != &oldBaseHead)
            return false;

        Node newBaseHead{3, nullptr};
        live = &newBaseHead;
        const bool appended = NoBlinkChain::Append(live, detached,
            [](Node* node) constexpr -> Node*& { return node->Next; },
            [](Node*) constexpr { return true; }, 8);
        return appended && live == &newBaseHead && live->Next == &oldBaseHead &&
            live->Next->Next == &oldHelm;
    }



}

static_assert(NoBlinkPolicy::ShouldRebind(0, 0));
static_assert(!NoBlinkPolicy::ShouldRebind(0, 1));
static_assert(!NoBlinkPolicy::ShouldRebind(0, 0xFFFF));
static_assert(!NoBlinkPolicy::ShouldRebind(1, 0));
static_assert(NoBlinkPolicy::ShouldRebind(0, 1, true));
static_assert(!NoBlinkPolicy::ShouldRebind(1, 0, true));
static_assert(!NoBlinkPolicy::ShouldSerializeAppearanceChange(0));
static_assert(!NoBlinkPolicy::ShouldSerializeAppearanceChange(1));
static_assert(NoBlinkPolicy::ShouldSerializeAppearanceChange(2));
static_assert(NoBlinkPolicy::ShouldSerializeAppearanceChange(8));
static_assert(!NoBlinkPolicy::ReplacementReady(10, 11, 0x1FF, false));
static_assert(!NoBlinkPolicy::ReplacementReady(11, 11, 0x1DF, false));
static_assert(!NoBlinkPolicy::ReplacementReady(11, 11, 0x1FF, true));
static_assert(NoBlinkPolicy::ReplacementReady(11, 11, 0x1FF, false));
static_assert(NoBlinkPolicy::ReplacementReady(12, 11, 0x1FF, false));
static_assert(!NoBlinkPolicy::ReplacementReady(0, 0, 0, false));
static_assert(KeepsInitializedModelButHidesOldLookFromLoader());

int main()
{
    // A rebind must reopen graphics setup without resetting unrelated owner/actor state.
    for (const uint32_t initial : {0u, 4u, 0x17u, 0xFFFFFFFFu})
    {
        uint8_t state[0x8B8];
        std::fill(state, state + sizeof(state), uint8_t{0xA5});
        std::memcpy(state + 0x6A4, &initial, sizeof(initial));
        NoBlinkActorState::InvalidateGraphics(state);
        for (size_t offset = 0; offset < sizeof(state); ++offset)
        {
            const uint32_t expectedFlags = initial & ~4u;
            uint8_t expected = 0xA5;
            if (offset >= 0x6A4 && offset < 0x6A8)
                expected = reinterpret_cast<const uint8_t*>(&expectedFlags)[offset - 0x6A4];
            if (state[offset] != expected)
                return 2;
        }
    }
    // The refresh must touch exactly these existing render controls and preserve actor identity,
    // resource ownership, task links, and all unrelated bytes. This is not a visual regression.
    uint8_t refresh[0xA0C];
    std::fill(refresh, refresh + sizeof(refresh), uint8_t{0xA5});
    const uint32_t beforeFlags = 0xFFFFFFFF;
    std::memcpy(refresh + 0x6A4, &beforeFlags, sizeof(beforeFlags));
    NoBlinkActorState::RequestFullRender(refresh);
    for (size_t offset = 0; offset < sizeof(refresh); ++offset)
    {
        uint8_t expected = 0xA5;
        const uint32_t afterFlags = beforeFlags & ~4u;
        if (offset >= 0x6A4 && offset < 0x6A8)
            expected = reinterpret_cast<const uint8_t*>(&afterFlags)[offset - 0x6A4];
        else if (offset == 0x72C)
            expected = 0;
        else if (offset == 0xA04)
            expected = 1;
        if (refresh[offset] != expected)
            return 3;
    }
    uint8_t actor[0x8B8];
    std::fill(actor, actor + sizeof(actor), uint8_t{0xA5});

    NoBlinkActorState::ResetAppearance(actor);

    for (size_t offset = 0; offset < sizeof(actor); ++offset)
    {
        const bool metadata = offset >= NoBlinkActorState::kAppearanceMetadata &&
            offset < NoBlinkActorState::kAppearanceMetadata +
                NoBlinkActorState::kAppearanceMetadataSize;
        const bool completion = offset >= NoBlinkActorState::kAppearanceCompletionMask &&
            offset < NoBlinkActorState::kAppearanceCompletionMask + sizeof(uint32_t);
        const uint8_t expected = metadata ? 0xFF : completion ? 0x00 : 0xA5;
        if (actor[offset] != expected)
            return 1;
    }
    // Captured 3.0 head materials select 1,3,8. Old hat flags hide 1 and 3;
    // the unequipped appearance retains body coverage 5 but must restore all three head groups.
    uint8_t stale[NoBlinkCoverage::kCount]{};
    NoBlinkCoverage::Accumulate(4, stale);
    NoBlinkCoverage::Accumulate(0x12, stale);
    if (stale[0] != 1 || stale[2] != 1 || stale[7] != 0) return 4;
    uint8_t current[NoBlinkCoverage::kCount]{};
    NoBlinkCoverage::Accumulate(0, current);
    NoBlinkCoverage::Accumulate(0x12, current);
    if (current[0] != 0 || current[2] != 0 || current[7] != 0 || current[4] != 1) return 5;
    std::fill(actor, actor + sizeof(actor), uint8_t{0xA5});
    NoBlinkCoverage::Publish(actor, current);
    for (size_t offset = 0; offset < sizeof(actor); ++offset)
    {
        const bool coverage = offset >= NoBlinkCoverage::kActorCoverage &&
            offset < NoBlinkCoverage::kActorCoverage + NoBlinkCoverage::kCount;
        const auto expected = coverage ? current[offset - NoBlinkCoverage::kActorCoverage] : 0xA5;
        if (actor[offset] != expected) return 6;
    }
    return 0;
}
