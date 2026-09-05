#include "NoBlinkChain.h"
#include "NoBlinkPolicy.h"

namespace
{
    struct Node
    {
        int Id;
        Node* Next;
    };

    constexpr bool KeepsOldChainAliveButHiddenFromLoader()
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
static_assert(KeepsOldChainAliveButHiddenFromLoader());

int main()
{
    return 0;
}
