// SPDX-License-Identifier: GPL-3.0-only
// Win32 regression fixtures exercise production code with stubbed resource loading.
// Visual camera/render correctness still requires the documented in-game checks.
#include "NoBlink.cpp"
#include <cstdio>
#include <cstdlib>

namespace CoverageTests
{
namespace
{
void Check(bool b, const char *why)
{
    if (!b)
    {
        std::printf("FAIL: %s\n", why);
        std::exit(1);
    }
}
template <class T> void Put(void *p, size_t o, T v)
{
    std::memcpy(static_cast<uint8_t *>(p) + o, &v, sizeof(v));
}
} // namespace
void Run()
{
    uint8_t actor[0xA0C]{}, model[0xC8]{}, model2[0xC8]{}, nodes[4][0x3C]{}, sources[4][0x64]{};
    void *handles[4];
    for (int i = 0; i < 4; ++i)
    {
        handles[i] = sources[i];
        Put<void *>(nodes[i], 8, &handles[i]);
        if (i < 3)
            Put<void *>(nodes[i], 4, nodes[i + 1]);
    }
    Put<void *>(actor, kActorModelOwner + kModelChainRoot, model);
    Put<void *>(model, kModelResources, nodes[0]);
    // Large hat, body and boots. The base mesh is first, so reconstruction must not depend on draw
    // order.
    Put<uint8_t>(sources[1], 0x33, 4);
    Put<uint8_t>(sources[2], 0x33, 0x12);
    Put<uint8_t>(sources[3], 0x33, 0x32);
    Check(RebuildCoverage(actor), "initial coverage");
    auto *flags = actor + NoBlinkCoverage::kActorCoverage;
    Check(flags[0] && flags[1] && flags[2] && flags[4] && flags[5], "current contributors lost");
    // Remove hat from the chain, without changing actor or underlying base mesh.
    Put<void *>(nodes[0], 4, nodes[2]);
    Check(RebuildCoverage(actor), "removed hat rebuild");
    Check(!flags[0] && !flags[1] && !flags[2] && flags[4] && flags[5],
          "old hat mask persisted or current clothing lost");
    // Smaller hat replaces the old one: strict reduction rather than OR accumulation.
    Put<void *>(nodes[0], 4, nodes[1]);
    Put<uint8_t>(sources[1], 0x33, 2);
    Check(RebuildCoverage(actor), "smaller hat rebuild");
    Check(flags[0] && !flags[1] && !flags[2] && flags[4] && flags[5], "smaller hat not reduced");
    Put<uint8_t>(sources[3], 0x33, 0);
    Check(RebuildCoverage(actor), "short boots rebuild");
    Check(!flags[5] && flags[4], "boot-only coverage persisted");
    // Full chain across outer models. Invalid/cyclic input must not publish partial state.
    Put<void *>(nodes[1], 4, nullptr);
    Put<void *>(model, 4, model2);
    Put<void *>(model2, kModelResources, nodes[2]);
    Check(RebuildCoverage(actor), "outer model omitted");
    Check(flags[0] && flags[4], "outer model coverage lost");
    uint8_t saved[sizeof(actor)];
    std::memcpy(saved, actor, sizeof(actor));
    Put<void *>(nodes[3], 4, nodes[2]);
    Check(!RebuildCoverage(actor), "resource cycle accepted");
    Check(std::memcmp(saved, actor, sizeof(actor)) == 0, "partial write on resource cycle");
    Put<void *>(nodes[3], 4, nullptr);
    Put<void *>(model2, 4, model);
    Check(!RebuildCoverage(actor), "model cycle accepted");
    Check(std::memcmp(saved, actor, sizeof(actor)) == 0, "partial write on model cycle");
    Put<void *>(model2, 4, nullptr);
    Put<void *>(nodes[1], 8, reinterpret_cast<void *>(1));
    Check(!RebuildCoverage(actor), "unreadable resource accepted");
    Check(std::memcmp(saved, actor, sizeof(actor)) == 0, "partial write on bad resource");
    Put<void *>(nodes[1], 8, &handles[1]);
    Put<uint32_t>(nodes[1], 0xC, 1);
    Check(RebuildCoverage(actor), "presentation suppression rejected");
    Check(!flags[0] && flags[4], "suppressed resource contributed");
    Put<uint32_t>(nodes[1], 0xC, 0);
    Put<uint8_t>(sources[1], 0x32, 1);
    Check(RebuildCoverage(actor), "derived node rejected");
    Check(!flags[0], "derived resource interpreted as simple renderer");
    std::puts("PASS: production coverage rebuild restores removed head/boot sections, retains "
              "current coverage, ignores resource order, validates full walk before writes");
}

} // namespace CoverageTests

namespace PoseTests
{
namespace
{
void Check(bool b, const char *why)
{
    if (!b)
    {
        std::printf("FAIL: %s\n", why);
        std::exit(1);
    }
}
template <class T> void Put(void *p, size_t o, T v)
{
    std::memcpy(static_cast<uint8_t *>(p) + o, &v, sizeof(v));
}
uint8_t entity[0x200]{}, actor[0xA0C]{}, model[0xC8]{}, descriptor[0x70]{}, oldNode[0x3C]{},
    newNode[0x3C]{};
void *handle = descriptor;
float oldPose[2][16]{}, newPose[2][16]{};
uint8_t newMask[2] = {0x80, 0x80};
uint32_t loads = 0;
uint32_t __fastcall Load(void *a, void *, int32_t selector)
{
    Check(a == actor && selector == -1, "load forwarding");
    ++loads;
    // Reproduce the catalogued initializer: same skeleton receives new identity matrices.
    for (auto &m : newPose)
    {
        std::memset(m, 0, sizeof(m));
        m[0] = m[5] = m[10] = m[15] = 1;
    }
    Put<void *>(model, 0x14, newPose);
    Put<void *>(model, 0x1C, newMask);
    Put<void *>(model, 0x20, newNode);
    Put<void *>(newNode, 4, nullptr);
    Put<uint32_t>(actor, NoBlinkActorState::kAppearanceCompletionMask, 0x1FF);
    return 1;
}
} // namespace
void Run()
{
    Put<void *>(entity, kEntityActor, actor);
    Put<void *>(actor, kActorModelOwner + kModelChainRoot, model);
    Put<void *>(model, 0x0C, &handle);
    Put<uint16_t>(descriptor, 0x32, 2);
    Put<void *>(model, 0x14, oldPose);
    Put<void *>(model, 0x20, oldNode);
    for (auto &m : oldPose)
    {
        m[0] = m[5] = m[10] = m[15] = 1;
        m[12] = 2;
        m[13] = -1.7f;
        m[14] = 3;
    }
    oldPose[1][12] = 2.5f;
    g_Loader = &Load;
    Hold hold{};
    void *nodes[] = {oldNode};
    Rebind(hold, entity, actor, model, nodes, 1);
    Check(loads == 1, "loader skipped");
    Check(std::memcmp(oldPose, newPose, sizeof(oldPose)) == 0,
          "rebind reset attachment pose to identity");
    Check(Read<void *>(model, 0x14) == newPose && Read<void *>(model, 0x1C) == newMask,
          "native ownership/mask replaced");
    Check(Read<void *>(entity, kEntityActor) == actor, "actor identity changed");
    PoseSnapshot snap;
    CapturePose(model, snap);
    Check(snap.Valid, "capture failed");
    uint32_t changed = 0;
    newPose[1][12] = 99;
    Check(RestorePose(model, snap, changed) && changed == 1, "changed matrix count");
    uint8_t other[0x70]{};
    void *otherHandle = other;
    Put<uint16_t>(other, 0x32, 2);
    Put<void *>(model, 0x0C, &otherHandle);
    newPose[1][12] = 99;
    Check(!RestorePose(model, snap, changed) && newPose[1][12] == 99,
          "different skeleton overwritten");
    Put<void *>(model, 0x0C, &handle);
    Put<uint16_t>(descriptor, 0x32, 1);
    Check(!RestorePose(model, snap, changed), "different count overwritten");
    Put<uint16_t>(descriptor, 0x32, 2);
    Check(!RestorePose(actor, snap, changed), "different model accepted");
    Put<void *>(model, 0x14, reinterpret_cast<void *>(1));
    CapturePose(model, snap);
    Check(!snap.Valid, "bad matrix read accepted");
    Put<void *>(model, 0x14, newPose);
    Put<uint16_t>(descriptor, 0x32, 257);
    CapturePose(model, snap);
    Check(!snap.Valid, "bone limit ignored");
    Put<uint16_t>(descriptor, 0x32, 2);
    newPose[0][0] = NAN;
    CapturePose(model, snap);
    Check(!snap.Valid, "nonfinite pose accepted");
    std::puts(
        "PASS: production rebind preserves same-skeleton attachment pose and native ownership");
}

} // namespace PoseTests

namespace LifecycleTests
{
namespace
{
alignas(void *) uint8_t entity[0x200], actor[0xA0C], model[0xC8];
struct Resource
{
    void **Vtable;
    void *Next;
    void *Handle;
    bool Alive;
} nodes[32];
uint8_t roots[2][0x2C]{};
void *rootHandles[2];
void *vtable[7];
int used, deleted, loads, teardowns;
bool deferFirst;
void Check(bool ok, const char *why)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", why);
        std::exit(1);
    }
}
template <class T> void Put(void *p, size_t off, T value)
{
    std::memcpy(static_cast<uint8_t *>(p) + off, &value, sizeof(value));
}
void *__fastcall Delete(void *p, void *, int)
{
    auto *r = static_cast<Resource *>(p);
    Check(r->Alive, "double release");
    Check(r->Next == nullptr, "released successor");
    r->Alive = false;
    ++deleted;
    return p;
}
Resource *New()
{
    Check(used < 32, "fixture exhausted");
    auto *r = &nodes[used++];
    *r = {vtable, nullptr, &rootHandles[(used - 1) % 2], true};
    return r;
}
void Attach()
{
    auto *r = New();
    r->Next = *ResourceHead(model);
    *ResourceHead(model) = r;
}
uint32_t __fastcall Load(void *p, void *, int32_t)
{
    Check(p == actor, "wrong actor");
    ++loads;
    Attach();
    if (loads == 1 && deferFirst)
    {
        ++g_ArmedParks;
        Put<uint32_t>(actor, NoBlinkActorState::kAppearanceCompletionMask, 0x1DF);
    }
    else
    {
        Attach();
        Put<uint32_t>(actor, NoBlinkActorState::kAppearanceCompletionMask, 0x1FF);
    }
    return 1;
}
uint8_t __fastcall Eligible(void *, void *)
{
    return 1;
}
void __cdecl Teardown(void *)
{
    ++teardowns;
}
void Setup(bool deferred)
{
    std::memset(entity, 0, sizeof(entity));
    std::memset(actor, 0, sizeof(actor));
    std::memset(model, 0, sizeof(model));
    for (auto &hold : g_Holds)
        hold = {};
    ClearAppearances();
    used = deleted = loads = teardowns = 0;
    deferFirst = deferred;
    for (int i = 0; i < 2; ++i)
    {
        rootHandles[i] = roots[i];
        Put<void *>(roots[i], 0x28, &rootHandles[i]);
    }
    vtable[6] = reinterpret_cast<void *>(&Delete);
    *ResourceHead(model) = New();
    *ChainRoot(actor) = model;
    Put<void *>(entity, kEntityActor, actor);
    Put<uint16_t>(entity, kEntityModelFlags, 1);
    Put<uint32_t>(actor, NoBlinkActorState::kModelOwnerFlags, 0x17);
    Put<uint8_t>(actor, NoBlinkActorState::kRenderCache, 1);
    Put<uint8_t>(actor, NoBlinkActorState::kRenderRefreshRequest, 0);
    g_Loader = &Load;
    g_Predicate = &Eligible;
    g_Teardown = &Teardown;
    g_Enabled = true;
    g_Self = true;
    g_Others = true;
    TeardownDetour(entity);
    Check(loads == 1 && teardowns == 0, "initial swap hit stock teardown");
    Check(Read<uint32_t>(actor, NoBlinkActorState::kModelOwnerFlags) == 0x13,
          "initial setup gate not cleared");
}
void AssertSettled()
{
    Check(FindHold(actor) == nullptr, "hold did not settle");
    Check(Read<void *>(entity, kEntityActor) == actor && *ChainRoot(actor) == model,
          "identity changed");
    Check(teardowns == 0, "unexpected stock destruction");
    Check(Read<uint8_t>(actor, NoBlinkActorState::kRenderCache) == 0,
          "cached render not invalidated");
    Check(Read<uint8_t>(actor, NoBlinkActorState::kRenderRefreshRequest) == 1,
          "stock render refresh not requested");
    Check(Read<uint32_t>(actor, NoBlinkActorState::kModelOwnerFlags) == 0x13,
          "settled setup gate not cleared");
    Check(Read<uint32_t>(actor, 0x7D8) == 0x20202020, "weapon motion not invalidated");
    uint32_t count = 0;
    void *recorded[kMaxNodes];
    Check(Snapshot(model, recorded, count) && count == 2, "old resources left in final chain");
    Check(recorded[0] == &nodes[used - 1] && recorded[1] == &nodes[used - 2],
          "resource order changed unexpectedly");
}
} // namespace
void Run()
{
    Setup(true);
    ServiceHolds();
    Check(loads == 1 && deleted == 0, "released before callback");
    Check(Read<uint8_t>(actor, NoBlinkActorState::kRenderCache) == 1 &&
              Read<uint8_t>(actor, NoBlinkActorState::kRenderRefreshRequest) == 0,
          "full refresh happened before final settle");
    Put<uint32_t>(actor, NoBlinkActorState::kModelOwnerFlags, 0x17);
    Attach(); // callback attaches, but full mask is not published until callback completes
    ServiceHolds();
    Check(loads == 1 && deleted == 0, "node count bypassed incomplete readiness mask");
    Check(Read<uint32_t>(actor, NoBlinkActorState::kModelOwnerFlags) == 0x13,
          "late arrival not invalidated");
    Put<uint32_t>(actor, NoBlinkActorState::kAppearanceCompletionMask, 0x1FF);
    ServiceHolds();
    Check(loads == 2 && deleted == 0, "final pass ordering");
    Put<uint32_t>(actor, NoBlinkActorState::kModelOwnerFlags, 0x17);
    ServiceHolds();
    AssertSettled();
    Check(deleted == 3, "wrong retained release count");

    Setup(false);
    ServiceHolds();
    ServiceHolds();
    AssertSettled();

    Setup(false);
    Put<uint16_t>(entity, kEntityLook + 2, 0x1234); // a newer look while the first load settles
    ServiceHolds();
    Check(loads == 2 && !FindHold(actor)->FinalPass, "new look not coalesced");
    ServiceHolds();
    ServiceHolds();
    AssertSettled();

    Setup(true);
    FindHold(actor)->Frames = kHoldMaxFrames - 1;
    ServiceHolds();
    Check(FindHold(actor) == nullptr && deleted == 1 && teardowns == 0,
          "timeout lifetime fallback");

    Setup(true);
    Put<void *>(entity, kEntityActor, nullptr); // stock zone/despawn owns those resources now
    ServiceHolds();
    Check(FindHold(actor) == nullptr && deleted == 0, "stale actor resources freed twice");
    std::puts("PASS: production rebind/hold paths: direct, delayed, incomplete mask, coalescing, "
              "timeout, stale actor; actor identity preserved");
}

} // namespace LifecycleTests

int main()
{
    CoverageTests::Run();
    PoseTests::Run();
    LifecycleTests::Run();
    return 0;
}
