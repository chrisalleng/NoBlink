#include "NoBlinkPolicy.h"

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

int main()
{
    return 0;
}
