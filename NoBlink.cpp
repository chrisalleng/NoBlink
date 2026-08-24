// SPDX-License-Identifier: GPL-3.0-only

/**
 * NoBlink - keeps a character visible across a visible-equipment change.
 *
 * Stock behaviour: when a character changes visible gear, the client destroys their whole
 * presentation and rebuilds it. They wink out of existence for the rebuild, and anyone
 * targeting them loses their target. There is no in-place gear-change path in the client at
 * all -- its only load path is the actor constructor, where the resource chain starts empty.
 *
 * NoBlink skips that teardown and rebinds the presentation on the LIVE actor instead, so the
 * actor is never destroyed and the target is never dropped. Two things make that work:
 *
 *   1. The presentation loader only ever APPENDS resources, so the old look has to be
 *      released explicitly or the character wears both garments at once. Release mirrors
 *      FUN_01a6b480 @ 0x1a6b480: model-list owner at actor+0x674, chain root at +0x44, freed
 *      through the root's virtual deleting destructor at +0x18(1).
 *
 *   2. Ordering. Most resources are shared across a body swap, and releasing one drops its
 *      last reference, so clearing first and rebuilding evicts resources that were about to
 *      be re-requested. The new look is attached first and each old node unlinked afterwards.
 *
 * The subtle part is knowing WHEN the new look is complete. For any actor that is not the
 * current/self actor, FUN_01b141f0 @ 0x1b141f0 does not do the work at all: it allocates a
 * 0x48 task, stores it at actor+0xa08, and returns. The model rebuild, every attach and every
 * deferred-resource registration happen later, inside that task. And when the task runs, any
 * look slot whose resource is not already resident attaches NOTHING -- it parks a completion
 * callback and moves on, so that body part has no node at all until the load lands.
 *
 * So completion needs both halves: actor+0xa08 going null says the task has run, which fixes
 * the target (what it bound synchronously plus what it parked), and arrival against that
 * target -- counted by node identity -- is what finally releases the old look. Releasing on
 * either half alone puts the character on screen without a body part.
 *
 * Suppression is OFF by default. Loading the plugin does not change stock behaviour.
 *
 * Every address resolves by unique byte signature at runtime; none are hardcoded. See the
 * README for the command surface and the measurements behind the above.
 */

#include "Ashita.h"
#include "Commands.h"

#include <intrin.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kLogError = static_cast<uint32_t>(Ashita::LogLevel::Error);
    constexpr uint32_t kLogInfo  = static_cast<uint32_t>(Ashita::LogLevel::Info);

    constexpr size_t kEntityActorPointer = 0xA0;   // ActorPointer
    constexpr size_t kEntityStatusActor  = 0xA4;
    constexpr size_t kEntityTimedActor   = 0xA8;
    constexpr size_t kEntityType         = 0xEE;
    constexpr size_t kEntityBusy         = 0xF2;   // signed 16-bit presentation busy count
    constexpr size_t kEntityModelFlags   = 0xF4;   // ModelUpdateFlags
    constexpr size_t kEntityFlags0       = 0x120;  // Render.Flags0
    constexpr size_t kEntityWord130      = 0x130;

    // The window of entity words sampled every frame in watch mode. Chosen to span the
    // actor pointers, the busy count, the model-update flags and the whole Render block,
    // and then reported by which words actually move. The point is that it is NOT selected
    // from a hypothesis: whatever the client toggles across a gear change shows up here,
    // including nothing at all, which is itself the answer if the hide lives in the actor.
    constexpr size_t kEntityWindowBase  = 0xA0;
    constexpr size_t kEntityWindowWords = 40;      // 0xA0 .. 0x140

    // Two windows into the actor. The head carries the model binding at +0x58; the tail
    // covers the deferred presentation task at +0xa08 and the derived private appearance
    // at +0xa0c/+0xa0e/+0xa10/+0xa12, which is where the reload keeps its state. Without
    // the tail, "no actor word moved" would only mean "not in the first 96 bytes".
    constexpr size_t kActorHeadWords = 24;         // 0x00 .. 0x60
    constexpr size_t kActorTailBase  = 0x9E0;
    constexpr size_t kActorTailWords = 20;         // 0x9E0 .. 0xA30

    // The actor's model-list owner, constructed in place by FUN_01a6b3e0 (0x1a6b3e0), which
    // installs vtable 0x1d6975c and clears the model-chain root at +0x44. Attaching a look
    // slot appends to that chain through FUN_01a69ea0 (0x1a69ea0), so re-running the loader
    // on a live actor ADDS the new gear without removing the old -- the character ends up
    // wearing both. The client never hits this because its only load path is the actor
    // constructor, where the chain is already empty.
    constexpr size_t kActorModelManager = 0x674;
    constexpr size_t kModelChainRoot    = 0x44;
    // Virtual deleting destructor slot. FUN_01a6b480 (0x1a6b480) empties the whole chain
    // with a single +0x18(1) call on the root, then clears it; the model's own destructor
    // walks its +0x4 successors. This mirrors that exactly.
    constexpr size_t kDeletingDtorSlot = 0x18;
    // Intrusive successor link. FUN_01a6b570 splices models out of the manager chain through
    // "+0x44/+4", and FUN_01a69e30 appends by writing the terminal +0x4 link.
    constexpr size_t kChainNext = 0x04;
    // A model's attached-resource chain head, linked by the same +0x4 successor field.
    constexpr size_t kModelResources = 0x20;
    // The serialized 0x48 deferred presentation task FUN_01b141f0 builds through
    // FUN_01b16ee0. Non-null exactly while presentation work is still outstanding.
    constexpr size_t kActorDeferredTask = 0xA08;

    // Longest resource chain recorded before a rebind, and the longest walk allowed when
    // searching for one node. Measured chains are 9 to 11 nodes, so these are far above any
    // real value and exist only to bound a corrupt or unterminated list.
    constexpr uint32_t kMaxSnapshot = 64;
    constexpr uint32_t kMaxWalk     = 192;

    using DeletingDtor_t = void(__thiscall*)(void* self, int flags);

    std::atomic<uint64_t> g_ChainsCleared{0};
    // Counts swaps that could not use the preserving path and took the clear-then-load
    // fallback. A nonzero value means the chain was not the shape that path assumes.
    std::atomic<uint64_t> g_Fallbacks{0};

    // ---- deferred removal ----------------------------------------------------------------
    // The loader will not attach a resource that is not already resident, so removing the old
    // look in the same call reproduces the gap it was meant to close (measured: res 11 -> 0
    // -> 7). Instead the old nodes are recorded and released only once every replacement has
    // landed -- counted exactly, see the deferred-resource accounting below.
    //
    // Unlike the refuted hold design this calls the loader EXACTLY ONCE and never detaches
    // the chain root: the chain stays well-formed and only recorded nodes are ever removed.
    // Repeated loader calls were what made that attempt pile up eight resources per retry.
    constexpr uint32_t kRemoveMaxFrames = 150;

    struct PendingRemoval
    {
        void* Entity;
        uintptr_t Actor;
        void* Model;
        void* Nodes[kMaxSnapshot];
        uint32_t Count;
        // Replacement nodes this swap must see attached before the old look may go. Not known
        // when the hold starts: for a remote actor the loader has not done anything yet, so
        // this is computed at the frame the presentation task finishes, as the nodes it bound
        // synchronously plus one for every resource it parked.
        uint32_t Expected;
        bool TaskDone;
        uint32_t Frames;
    };
    constexpr size_t kMaxPendingRemovals = 4;
    PendingRemoval g_Removals[kMaxPendingRemovals]{};

    std::atomic<uint64_t> g_Deferred{0};
    std::atomic<uint64_t> g_DeferredDone{0};
    std::atomic<uint64_t> g_DeferredTimeout{0};

    // ---- deferred-resource accounting ----------------------------------------------------
    // Why a slot is simply absent rather than late-but-visible, and how we know when it is
    // no longer absent.
    //
    // FUN_01b12e20 (0x1b12e20) asks FUN_01ab3010 (0x1ab3010) for each look slot's resource.
    // That returns an already-loaded graph only when FUN_01ab4e60 finds the resource id in
    // the loaded registry at manager+0xd50; on a miss it creates a document request and
    // returns the request's handle instead. FUN_01ab0f90 (0x1ab0f90) then reports the handle
    // unusable -- its resource object is still null -- and the dispatcher attaches NOTHING.
    // It registers a one-shot completion callback through FUN_01ab1020 (0x1ab1020) and moves
    // on. The callback (FUN_01b13fc0 / FUN_01b10d40) re-tests FUN_01ab0f90 when the request
    // completes and only then calls FUN_01b10c00 -> FUN_01b0f610 to attach the node.
    //
    // So the node for a non-resident slot does not exist during the wait, which is why no
    // per-node state can report the wait and why the character is simply missing that body
    // part. The number of FUN_01ab1020 registrations the loader makes for our actor IS the
    // client's own count of what is still outstanding, and it is exact.
    //
    // actor+0xa08 is not that count. It is the serialized presentation task, and it clears
    // while a request is still in flight (measured ~758ms early), which is precisely what
    // dropped the chest on the Hume Tunic direction.
    constexpr char kDeferRegisterPattern[] =
        "53566A006A0468840000008BD9E8????????8BF083C40C";
    // push ebx; push esi; push 0; push 4 -- six bytes, no position-dependent operand, so the
    // trampoline is a straight copy plus a jump back.
    constexpr uint8_t kDeferRegisterPrologue[6] = {0x53, 0x56, 0x6A, 0x00, 0x6A, 0x04};
    constexpr size_t kDeferRegisterStolen       = 6;

    // FUN_01ab0f90 - the usable/idle predicate. Resolved only to read the resource-link arena
    // manager out of its `mov ecx, [imm32]`, which the loader relocates. FUN_01ab4110 bounds
    // a valid link to [manager+8, manager+0x40004], so every link a PC's resource nodes carry
    // at +0x08 is a cell of that one 256KB arena -- including the addresses this plugin's
    // earlier notes described as two unrelated descriptor blocks.
    constexpr char kUsablePattern[] =
        "568BF18B0D????????8B0650E8????????84C075025EC3";
    constexpr size_t kUsableArenaImm = 0x05;
    constexpr size_t kArenaFirst     = 0x8;
    constexpr size_t kArenaLast      = 0x40004;

    uintptr_t g_ArenaManagerPtr = 0;

    // ecx = handle cell, edx unused, then the four stack arguments FUN_01ab1020 takes:
    // callback, context, actor, cancel flag. Callee-cleaned `ret 0x10` in both conventions.
    using DeferRegister_t = void(__fastcall*)(void* cell, void* unused, void* callback,
        void* context, void* actor, void* flag);

    DeferRegister_t g_DeferTramp = nullptr;
    uint8_t* g_DeferTarget       = nullptr;
    uint8_t g_DeferOriginal[5]{};
    bool g_DeferHooked = false;

    // Set only for the duration of our own loader call, on the calling thread, so the count
    // covers exactly the resources that call parked.
    // The loader call is synchronous and single-threaded, so ANY registration that happens
    // between its entry and its return was caused by it. That window, not the actor match, is
    // what the hold count uses. The actor match is kept purely as a cross-check: if the loader
    // passed a different pointer down to FUN_01b12e20 than the one we called it with, the two
    // counts disagree and the disagreement is reported instead of silently reading zero.
    std::atomic<bool> g_DeferWindow{false};
    std::atomic<uint32_t> g_DeferInWindow{0};
    std::atomic<void*> g_DeferLastActor{nullptr};

    std::atomic<void*> g_DeferActor{nullptr};
    std::atomic<uint32_t> g_DeferPending{0};
    std::atomic<uint64_t> g_DeferRegistered{0};
    std::atomic<uint64_t> g_DeferInWindowTotal{0};
    // Hook liveness. Counts EVERY registration in the client, for any actor. Without it a
    // reading of "0 parked" cannot be told apart from "the hook never fires", which is the
    // failure mode that has produced false results in this investigation before. A parked
    // count of zero means nothing unless this one is moving.
    std::atomic<uint64_t> g_DeferSeen{0};

    // Signatures taken against a retail FFXiMain.dll rebuilt at base 0x1a40000, sha256
    // eae30ddd8830067c684d5f08cf33a5406184e6ba488947ecb62f839c88647b1d. Each occurs exactly
    // once in that image. Relative call displacements are wildcarded, and initialization
    // refuses to hook unless every resolved address lies inside the loaded module.
    constexpr char kTeardownPattern[] =
        "568B7424088BCEE8????????84C074426683BEF4000000007509F686200100"
        "0001742F8B86300100008BCE25FFFFFDFF";

    // FUN_01b141f0 - the repeatable actor presentation loader. Given selector -1 it re-reads
    // the entity's current packed look slots and rebinds the presentation resources against
    // the EXISTING actor (FUN_01b12e20 selects graphs "by entity Type, actor class, race, and
    // packed look slots"; the normal constructor FUN_01b055a0 builds presentation state with
    // selector -1). This is the rebind path that lets us skip the teardown entirely.
    // Occurs exactly once in the pinned PE.
    constexpr char kLoaderPattern[] =
        "53568BF132DB8B467085C074198B80200100008BC8C1E907F6C1017507C1E8"
        "08A8017402B3018B168BCEFF920403000084C0";

    // FUN_01b172a0 - clears the actor's derived private appearance (+0xa0c, +0xa0e,
    // +0xa10 bit 0, and 8 dwords at +0xa12). The XiSkeletonActor2 constructors run this
    // right after the base constructor, which is why a freshly built actor re-derives its
    // look. Without it the loader sees a valid cache and the appearance never changes.
    constexpr char kClearAppearancePattern[] =
        "8BD157B90800000033C08DBA120A0000F3AB80A2100A0000FE6689820E0A0000"
        "88820C0A00005FC3";

    // FUN_01b17090 - the entity-bound XiSkeletonActor2 constructor. Only used to read the
    // class vtable out of its `mov [esi], imm32`, which the loader relocates, so the guard
    // below is ASLR-correct without hardcoding anything.
    constexpr char kCtorPattern[] =
        "8B44240C8B542404568BF18B4C240C5051528BCEE8????????8BCEC706????????E8????????8BC65EC2";
    constexpr size_t kCtorVtableImm = 0x1D;

    constexpr uint8_t kExpectedPrologue[5] = {0x56, 0x8B, 0x74, 0x24, 0x08};
    constexpr size_t kPatchSize            = 5;

    // FUN_01ad2830 - the complete presentation teardown itself. Hooked purely to attribute
    // WHICH call site actually destroys the actor during a gear change. Suppressing
    // FUN_01ad5d00 did not stop the blink in retail, so the real teardown reaches
    // 0x1ad2830 by some other route; guessing which one from the disassembly has already
    // been wrong once, so this measures it instead.
    constexpr char kTeardownAllPattern[] =
        "568BF1E8????????8BCEE8????????8BCEE8????????8BCEE8????????8BCEE8????????";

    // push esi; mov esi, ecx; call rel32  -- the 5-byte patch splits that call, so the
    // trampoline reproduces all 8 bytes with the displacement recomputed.
    constexpr uint8_t kTeardownAllPrologue[4] = {0x56, 0x8B, 0xF1, 0xE8};
    constexpr size_t kTeardownAllStolen       = 8;

    using TeardownAll_t = void(__fastcall*)(void* entity, void* unused);

    TeardownAll_t g_TeardownAllTramp = nullptr;
    uint8_t* g_TeardownAllTarget     = nullptr;
    uint8_t g_TeardownAllOriginal[kPatchSize]{};
    bool g_TeardownAllHooked = false;

    struct CallerEntry
    {
        uintptr_t Address;
        uint32_t Count;
    };
    constexpr size_t kMaxCallers = 16;
    CallerEntry g_Callers[kMaxCallers]{};
    std::atomic<uint32_t> g_CallerCount{0};
    std::atomic<uint32_t> g_DestroyTotal{0};

    // Offset of the rel32 for `call FUN_01ad5780` (the eligibility predicate) inside
    // FUN_01ad5d00, and the address of the following instruction.
    constexpr size_t kPredicateRel32 = 0x08;
    constexpr size_t kPredicateNext  = 0x0C;

    constexpr int32_t kSelectorDefault = -1;

    using Teardown_t  = void(__cdecl*)(void* entity);
    using Predicate_t = uint8_t(__fastcall*)(void* entity, void* unused);
    // __thiscall(actor, selector) expressed for MSVC: ecx = actor, edx unused, selector on
    // the stack. Callee cleans in both conventions, so this is ABI-compatible.
    using Loader_t = uint32_t(__fastcall*)(void* actor, void* unused, int32_t selector);

    using ClearAppearance_t = void(__fastcall*)(void* actor, void* unused);

    Predicate_t g_Predicate           = nullptr;
    Loader_t g_Loader                 = nullptr;
    ClearAppearance_t g_ClearAppearance = nullptr;
    uintptr_t g_SkeletonActorVtable   = 0;
    // Diagnostic: the vtable actually seen on a candidate actor, so a guard mismatch is
    // visible instead of silently falling through.
    std::atomic<uintptr_t> g_ObservedVtable{0};
    // OFF by default. Loading the plugin must not change stock behaviour.
    std::atomic<bool> g_Suppress{false};
    std::atomic<uint64_t> g_Suppressed{0};

    Teardown_t g_Trampoline = nullptr;
    uint8_t* g_HookTarget   = nullptr;
    uint8_t g_OriginalBytes[kPatchSize]{};
    bool g_Hooked = false;

    // ---- trace state ------------------------------------------------------------------
    struct Sample
    {
        uint32_t Frame;
        double Milliseconds;
        uintptr_t Actor;
        // Raw entity window and the head of the actor object. Both are dumped by which
        // words moved, so the trace does not presuppose where the hide lives.
        uint32_t EntityWords[kEntityWindowWords];
        uint32_t ActorWords[kActorHeadWords];
        uint32_t ActorTail[kActorTailWords];
        // Number of models attached to the actor. This is the symptom itself: a slot whose
        // resource deferred is a model that is not in the chain yet, which is what "wearing
        // no chestpiece for a moment" looks like from here. actor+0x58 stays bound through
        // it, so nothing else sampled can see it.
        uint32_t ChainLen;
        // Resources attached to the first model. A PC is ONE model with a resource per look
        // slot (FUN_01a69ea0 attaches them; FUN_01a69e40 removes one), so this -- not the
        // model count -- is where a deferred chestpiece actually shows up as missing.
        uint32_t ResCount;
    };

    constexpr size_t kMaxSamples = 400;

    // Watch mode: sample one entity by index every frame, regardless of teardowns. The
    // teardown-armed trace cannot see a suppressed gear change, because under suppression
    // there is no teardown to arm it.
    constexpr uint32_t kNoWatchIndex = 0xFFFFFFFFu;
    std::atomic<uint32_t> g_WatchIndex{kNoWatchIndex};

    std::atomic<void*> g_Watched{nullptr};
    Sample g_Samples[kMaxSamples]{};
    std::atomic<uint32_t> g_SampleCount{0};
    std::atomic<bool> g_Tracing{false};
    std::atomic<uint64_t> g_Teardowns{0};
    std::atomic<uint32_t> g_Frame{0};

    LARGE_INTEGER g_Freq{};
    LARGE_INTEGER g_TraceStart{};

    // ---- blink measurement ------------------------------------------------------------
    // The real signal. actor+0x58 is the model binding: the teardown releases it and it
    // reads null until the model is resident again. The entity-level fields never show a
    // gap, so this is the only thing that tracks what the player actually sees.
    constexpr size_t kActorModelBinding = 0x58;

    std::atomic<bool> g_Pending{false};
    LARGE_INTEGER g_PendingStart{};
    std::atomic<uint32_t> g_PendingFrames{0};

    // ---- target continuity ------------------------------------------------------------
    // The symptom that actually matters: while the gear change is hidden, anyone targeting
    // the character loses their target. Unlike mesh residency this is binary and sticky, so
    // it is the metric a fix has to move.
    std::atomic<uint32_t> g_TargetDrops{0};
    std::atomic<uint32_t> g_TargetLastServerId{0};
    std::atomic<bool> g_TargetLastActive{false};
    std::atomic<uint32_t> g_TargetSamples{0};

    std::atomic<uint32_t> g_BlinkCount{0};
    std::atomic<uint32_t> g_BlinkFrames{0};
    std::atomic<uint64_t> g_BlinkMicros{0};
    std::atomic<uint32_t> g_BlinkMaxMicros{0};

    // ---- busy-count window --------------------------------------------------------------
    // FUN_01ad6100 (0x1ad6100) treats the entity as settled only while signed entity+0xf2
    // is nonpositive, and the count moves solely through actor vslots +0x170/+0x174
    // (FUN_01ac32e0 / FUN_01ac32f0). If the client hides the model behind that count, a
    // gear change raises it for a measurable run of frames. If it never leaves zero, the
    // hide is not the busy count and this metric says so.
    std::atomic<uint32_t> g_BusyWindows{0};
    std::atomic<uint32_t> g_BusyFrames{0};
    std::atomic<uint32_t> g_BusyMaxFrames{0};
    std::atomic<int32_t> g_BusyPeak{0};
    std::atomic<uint32_t> g_BusyRun{0};

    uintptr_t ReadPtr(const void* base, const size_t offset)
    {
        uintptr_t v{};
        std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(v));
        return v;
    }

    uint32_t Read32(const void* base, const size_t offset)
    {
        uint32_t v{};
        std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(v));
        return v;
    }

    uint16_t Read16(const void* base, const size_t offset)
    {
        uint16_t v{};
        std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(v));
        return v;
    }

    int16_t ReadS16(const void* base, const size_t offset)
    {
        int16_t v{};
        std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(v));
        return v;
    }

    /**
     * Empties the actor's model chain, leaving the model-list owner itself intact and in the
     * state FUN_01a6b3e0 leaves it in on a fresh actor. This is the release half that an
     * in-place rebind needs: without it the loader appends the new look to the old one and
     * the character wears both garments at once.
     *
     * Returns false and touches nothing if any link in the walk is unreadable, so a layout
     * surprise falls through to the stock path rather than corrupting an actor.
     */
    void** ChainRoot(void* actor)
    {
        auto* root = reinterpret_cast<void**>(
            static_cast<uint8_t*>(actor) + kActorModelManager + kModelChainRoot);
        return ::IsBadReadPtr(root, sizeof(void*)) ? nullptr : root;
    }

    /**
     * Resolves a chain head's virtual deleting destructor, or null if any link of the walk
     * is unreadable. Callers treat null as "do not touch this actor".
     */
    DeletingDtor_t ChainDtor(void* head)
    {
        if (head == nullptr || ::IsBadReadPtr(head, sizeof(void*)))
            return nullptr;
        auto* vtable = *reinterpret_cast<void***>(head);
        if (::IsBadReadPtr(vtable, kDeletingDtorSlot + sizeof(void*)))
            return nullptr;
        return reinterpret_cast<DeletingDtor_t>(vtable[kDeletingDtorSlot / sizeof(void*)]);
    }

    /**
     * Frees a whole chain. Flag 1 = delete; the head's own destructor walks and frees its
     * +0x4 successors, exactly as FUN_01a6b480 does.
     */
    bool FreeChain(void* head)
    {
        if (head == nullptr)
            return true;
        auto dtor = ChainDtor(head);
        if (dtor == nullptr)
            return false;
        dtor(head, 1);
        return true;
    }

    /**
     * Counts the nodes in a chain that are not in a recorded set -- the replacements this
     * swap has bound so far. Old and new nodes are interleaved (the loader inserts in
     * descriptor order, measured at chain positions 2..4), so counting is by identity, not
     * by position or by chain length.
     */
    uint32_t CountUnrecorded(void* head, void* const* recorded, const uint32_t recordedCount)
    {
        uint32_t fresh = 0;
        void* node     = head;
        for (uint32_t guard = 0; node != nullptr && guard < kMaxWalk; ++guard)
        {
            if (::IsBadReadPtr(node, kChainNext + sizeof(void*)))
                break;

            bool known = false;
            for (uint32_t i = 0; i < recordedCount; ++i)
            {
                if (recorded[i] == node)
                {
                    known = true;
                    break;
                }
            }
            if (!known)
                ++fresh;

            node = *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + kChainNext);
        }
        return fresh;
    }

    uint32_t ChainLength(void* head)
    {
        uint32_t n = 0;
        for (void* node = head; node != nullptr && n < 64; ++n)
        {
            if (::IsBadReadPtr(node, kChainNext + sizeof(void*)))
                break;
            node = *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + kChainNext);
        }
        return n;
    }

    /**
     * Removes one exact node from a resource chain and frees it, mirroring FUN_01a69e40:
     * relink the predecessor past it, detach its own successor, then delete it through
     * virtual slot +0x18. A node that is no longer present is left alone.
     */
    void UnlinkAndFree(void** head, void* target)
    {
        void** link = head;
        for (uint32_t guard = 0; guard < kMaxWalk; ++guard)
        {
            if (::IsBadReadPtr(link, sizeof(void*)))
                return;
            void* cur = *link;
            if (cur == nullptr)
                return;   // not in the chain any more

            if (cur == target)
            {
                if (::IsBadReadPtr(cur, kChainNext + sizeof(void*)))
                    return;
                auto* next = reinterpret_cast<void**>(
                    static_cast<uint8_t*>(cur) + kChainNext);
                *link = *next;
                *next = nullptr;   // detach before deleting so only this node is freed
                FreeChain(cur);
                return;
            }

            if (::IsBadReadPtr(cur, kChainNext + sizeof(void*)))
                return;
            link = reinterpret_cast<void**>(static_cast<uint8_t*>(cur) + kChainNext);
        }
    }

    /**
     * Rebind by attaching the new look BEFORE releasing the old one.
     *
     * Measured across a body swap: of nine attached resources, SEVEN carry the same link
     * either side of it -- head, hands, legs, feet and so on. Clearing the whole chain threw
     * all seven away and rebuilt them, and because releasing a resource drops its last
     * reference the re-request missed the loaded registry and had to raise a document
     * request. That self-inflicted eviction is the ~720ms fill-in.
     *
     * The new resources are INSERTED IN DESCRIPTOR ORDER, not appended -- measured by
     * diffing the chain across a swap, where the three new nodes landed at positions 2..4.
     * So the old nodes are not a contiguous run afterwards and each has to be unlinked
     * individually, which is what FUN_01a69e40 does for one node.
     *
     * Every check that can refuse runs BEFORE the loader call. Once the loader has run this
     * always reports success, because the caller's fallback would otherwise run the loader a
     * second time and land both sets of deferred callbacks on the same model.
     *
     * Cost: between the insert and the release both looks are attached. When everything
     * binds immediately that window is zero frames.
     */
    void QueueRemoval(void* entity, void* actor, void* model, void* const* nodes,
        const uint32_t count, const uint32_t expected)
    {
        for (auto& slot : g_Removals)
        {
            if (slot.Entity != nullptr)
                continue;

            slot.Entity   = entity;
            slot.Actor    = reinterpret_cast<uintptr_t>(actor);
            slot.Model    = model;
            slot.Count    = count;
            slot.Expected = expected;
            slot.TaskDone = expected != 0;
            slot.Frames   = 0;
            for (uint32_t i = 0; i < count; ++i)
                slot.Nodes[i] = nodes[i];
            g_Deferred.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // No free slot: release now rather than leak the old look permanently.
        void** root = ChainRoot(actor);
        if (root != nullptr && *root == model)
        {
            auto* resHead = reinterpret_cast<void**>(
                static_cast<uint8_t*>(model) + kModelResources);
            for (uint32_t i = 0; i < count; ++i)
                UnlinkAndFree(resHead, nodes[i]);
        }
    }

    bool RebindPreservingOld(void* entityOfActor, void* actor)
    {
        void** root = ChainRoot(actor);
        if (root == nullptr || g_Loader == nullptr)
            return false;

        void* model = *root;
        if (model == nullptr || ::IsBadReadPtr(model, kModelResources + sizeof(void*)))
            return false;

        auto* resHead = reinterpret_cast<void**>(
            static_cast<uint8_t*>(model) + kModelResources);

        void* oldHead = *resHead;
        if (oldHead == nullptr)
        {
            g_Loader(actor, nullptr, kSelectorDefault);   // nothing to preserve
            return true;
        }

        // Snapshot every existing node, validating each before anything is mutated.
        void* snapshot[kMaxSnapshot];
        uint32_t count = 0;
        for (void* node = oldHead; node != nullptr; )
        {
            if (count >= kMaxSnapshot || ::IsBadReadPtr(node, kChainNext + sizeof(void*)))
                return false;
            if (ChainDtor(node) == nullptr)
                return false;
            snapshot[count++] = node;
            node = *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + kChainNext);
        }

        // Arm the registration count for this actor. The window deliberately stays open past
        // the loader call: FUN_01b141f0 does the work inline only for the self/current actor
        // or Flags0 bits 7/8. For anyone else -- which is every case this plugin observes --
        // it allocates a 0x48 task, stores it at actor[0x282] (= +0xa08), and returns having
        // done NOTHING. The model rebuild, the attaches and every FUN_01ab1020 registration
        // happen later, when that task runs LAB_01b12e00 -> FUN_01b12e20.
        //
        // Counting only across the call was therefore a false negative that read as "nothing
        // parked" for every remote swap, and acting on it released the old look while the
        // replacement did not yet exist -- one frame with the character wearing nothing.
        g_DeferActor.store(actor, std::memory_order_relaxed);
        g_DeferInWindow.store(0, std::memory_order_relaxed);
        g_DeferWindow.store(true, std::memory_order_relaxed);

        g_Loader(actor, nullptr, kSelectorDefault);

        const uint32_t deferred = g_DeferInWindow.load(std::memory_order_relaxed);
        const bool queued       = ReadPtr(actor, kActorDeferredTask) != 0;

        // Past this point, always report success -- see the note above about double loads.
        // If the loader rebuilt the model, the old resources went with it.
        if (*root != model)
        {
            g_ChainsCleared.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // The work ran inline and parked nothing: every replacement is already attached, so
        // release the old look now and the swap completes within this call with no doubled
        // window at all. Requiring the task pointer to be clear is what makes this safe --
        // without it this branch fires for every remote swap before anything has happened.
        if (!queued && deferred == 0)
        {
            g_DeferWindow.store(false, std::memory_order_relaxed);
            g_DeferActor.store(nullptr, std::memory_order_relaxed);
            for (uint32_t i = 0; i < count; ++i)
                UnlinkAndFree(resHead, snapshot[i]);
            g_ChainsCleared.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Otherwise hold the old look. Holding it is the whole point: a slot whose resource
        // is not resident has no node at all, so releasing early is releasing into nothing.
        // When the work ran inline we already know how many replacements to expect; when it
        // was queued we do not, and ServiceRemovals computes it when the task finishes.
        const uint32_t attached = queued ? 0 : CountUnrecorded(*resHead, snapshot, count);
        QueueRemoval(entityOfActor, actor, model, snapshot, count,
            queued ? 0 : attached + deferred);
        g_ChainsCleared.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool ClearModelChain(void* actor)
    {
        void** root = ChainRoot(actor);
        if (root == nullptr)
            return false;

        void* model = *root;
        if (model == nullptr)
            return true;   // already empty; nothing to release
        if (ChainDtor(model) == nullptr)
            return false;

        FreeChain(model);
        *root = nullptr;
        g_ChainsCleared.fetch_add(1, std::memory_order_relaxed);
        return true;
    }


    /**
     * Trigger only. Latches the entity whose live actor this call destroyed, and starts a
     * fresh trace. Deliberately does not attempt any repair.
     */
    void __cdecl TeardownDetour(void* entity)
    {
        if (entity == nullptr)
        {
            return;
        }

        // --- suppression path -----------------------------------------------------------
        // Mirror the stock condition exactly (same predicate, same triggers). When it holds
        // for an ordinary PC that already has a live actor, rebind the presentation in place
        // instead of destroying it, so actor+0x58 is never released and there is no gap.
        if (g_Suppress.load(std::memory_order_relaxed) && g_Predicate != nullptr && g_Loader != nullptr)
        {
            const uintptr_t actor = ReadPtr(entity, kEntityActorPointer);
            const uint16_t f4     = Read16(entity, kEntityModelFlags);
            const uint32_t flags0 = Read32(entity, kEntityFlags0);
            const uint8_t type    = *(static_cast<const uint8_t*>(entity) + kEntityType);

            const bool triggered = (f4 != 0) || ((flags0 & 1u) != 0);

            // Only touch actors we can prove are the class whose layout we are about to
            // write into. Anything else (mounts, costumes, other actor families) falls
            // through to the stock path untouched.
            uintptr_t actorVtable = 0;
            if (actor != 0 && !::IsBadReadPtr(reinterpret_cast<void*>(actor), sizeof(uintptr_t)))
            {
                actorVtable = ReadPtr(reinterpret_cast<void*>(actor), 0);
                if (triggered && type == 0)
                    g_ObservedVtable.store(actorVtable, std::memory_order_relaxed);
            }

            // The appearance-cache clear below is XiSkeletonActor2-only layout, so it is
            // applied only when the actor really is that class. PC actors are a different
            // class (vtable 0x1d6ff40) and are suppressed without it.
            const bool isSkeletonActor2 = actorVtable != 0 && g_SkeletonActorVtable != 0 &&
                actorVtable == g_SkeletonActorVtable;

            if (actorVtable != 0 && type == 0 && triggered && g_Predicate(entity, nullptr) != 0)
            {
                // Release the currently attached models before anything else. The loader only
                // ever appends, so skipping this leaves the old gear on the character
                // alongside the new. This runs before the triggers are consumed so that a
                // layout surprise falls through to a fully stock teardown, with the entity
                // exactly as the stock function expects to find it.
                // Verify the chain is walkable before mutating anything, so a layout
                // surprise falls through to a fully stock teardown with the entity exactly
                // as the stock function expects to find it.
                if (ChainRoot(reinterpret_cast<void*>(actor)) == nullptr)
                {
                    g_Trampoline(entity);
                    return;
                }

                // Consume the triggers exactly as the stock tail does. Deliberately leave
                // +0x130 bit 17 alone: the stock code only clears it on the way to a
                // teardown, and we are not tearing down.
                uint16_t cleared = 0;
                std::memcpy(static_cast<uint8_t*>(entity) + kEntityModelFlags, &cleared, sizeof(cleared));
                const uint32_t newFlags = flags0 & ~1u;
                std::memcpy(static_cast<uint8_t*>(entity) + kEntityFlags0, &newFlags, sizeof(newFlags));

                // Invalidate the cached appearance, exactly as the constructor does, then let
                // the loader re-derive from the entity's current look slots. Note this never
                // fires for a PC: it is XiSkeletonActor2-only layout, and a player's actor is
                // the base class (vtable 0x1d6ff40).
                if (isSkeletonActor2)
                    g_ClearAppearance(reinterpret_cast<void*>(actor), nullptr);

                // Attach the new look before releasing the old, so the unchanged resources
                // never lose their last reference. Falls back to the verified
                // clear-then-load path if the chain is not the expected shape.
                if (!RebindPreservingOld(entity, reinterpret_cast<void*>(actor)))
                {
                    if (ClearModelChain(reinterpret_cast<void*>(actor)))
                        g_Loader(reinterpret_cast<void*>(actor), nullptr, kSelectorDefault);
                    g_Fallbacks.fetch_add(1, std::memory_order_relaxed);
                }

                g_Suppressed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        const uintptr_t before = ReadPtr(entity, kEntityActorPointer);
        g_Trampoline(entity);
        const uintptr_t after = ReadPtr(entity, kEntityActorPointer);

        if (before == 0 || after != 0)
            return;

        g_Teardowns.fetch_add(1, std::memory_order_relaxed);

        // Arm a blink measurement for every teardown.
        g_Watched.store(entity, std::memory_order_relaxed);
        ::QueryPerformanceCounter(&g_PendingStart);
        g_PendingFrames.store(0, std::memory_order_relaxed);
        g_Pending.store(true, std::memory_order_relaxed);

        // Start a new trace on the first teardown after a dump/reset.
        if (!g_Tracing.load(std::memory_order_relaxed))
        {
            g_SampleCount.store(0, std::memory_order_relaxed);
            ::QueryPerformanceCounter(&g_TraceStart);
            g_Watched.store(entity, std::memory_order_relaxed);
            g_Tracing.store(true, std::memory_order_relaxed);
        }
    }

    void RecordCaller(const uintptr_t caller)
    {
        const uint32_t count = g_CallerCount.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (g_Callers[i].Address == caller)
            {
                ++g_Callers[i].Count;
                return;
            }
        }
        if (count < kMaxCallers)
        {
            g_Callers[count].Address = caller;
            g_Callers[count].Count   = 1;
            g_CallerCount.store(count + 1, std::memory_order_relaxed);
        }
    }

    /**
     * Attribution only. Records which call site destroyed a live actor.
     */
    void __fastcall TeardownAllDetour(void* entity, void*)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());

        const uintptr_t before = entity ? ReadPtr(entity, kEntityActorPointer) : 0;
        g_TeardownAllTramp(entity, nullptr);
        const uintptr_t after = entity ? ReadPtr(entity, kEntityActorPointer) : 0;

        if (before != 0 && after == 0)
        {
            g_DestroyTotal.fetch_add(1, std::memory_order_relaxed);
            RecordCaller(caller);
        }
    }

    /**
     * Accounting only. Every look slot whose resource is not resident parks itself here, so
     * counting the registrations our own loader call makes for our own actor gives the exact
     * number of replacements still to arrive. Outside that window the detour does nothing but
     * pass the call through.
     */
    void __fastcall DeferRegisterDetour(void* cell, void* unused, void* callback,
        void* context, void* actor, void* flag)
    {
        g_DeferSeen.fetch_add(1, std::memory_order_relaxed);

        // The window now spans the queued presentation task, not just the loader call, so it
        // covers many frames and other actors' registrations can fall inside it. Only ours
        // are counted, and the unfiltered tally is kept beside it so an attribution failure
        // shows up as a disagreement rather than as a silent zero.
        if (g_DeferWindow.load(std::memory_order_relaxed))
        {
            g_DeferInWindowTotal.fetch_add(1, std::memory_order_relaxed);
            g_DeferLastActor.store(actor, std::memory_order_relaxed);

            if (actor != nullptr && actor == g_DeferActor.load(std::memory_order_relaxed))
            {
                g_DeferInWindow.fetch_add(1, std::memory_order_relaxed);
                g_DeferRegistered.fetch_add(1, std::memory_order_relaxed);
            }
        }

        g_DeferTramp(cell, unused, callback, context, actor, flag);
    }

    bool WriteCode(void* address, const void* data, const size_t size)
    {
        DWORD previous{};
        if (!::VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous))
            return false;
        std::memcpy(address, data, size);
        DWORD restored{};
        ::VirtualProtect(address, size, previous, &restored);
        ::FlushInstructionCache(::GetCurrentProcess(), address, size);
        return true;
    }
}

class NoBlink final : public IPlugin
{
    IAshitaCore* m_Core   = nullptr;
    ILogManager* m_Logger = nullptr;
    uint32_t m_Id         = 0;

public:
    const char* GetName(void) const override
    {
        return "NoBlink";
    }
    const char* GetAuthor(void) const override
    {
        return "Brunas";
    }
    const char* GetDescription(void) const override
    {
        return "Keeps characters visible and targetable across gear changes.";
    }
    const char* GetLink(void) const override
    {
        return "https://github.com/chrisalleng/NoBlink";
    }
    double GetVersion(void) const override
    {
        return 2.0;
    }
    double GetInterfaceVersion(void) const override
    {
        return ASHITA_INTERFACE_VERSION;
    }
    int32_t GetPriority(void) const override
    {
        return 0;
    }
    uint32_t GetFlags(void) const override
    {
        return static_cast<uint32_t>(Ashita::PluginFlags::UseCommands) |
               static_cast<uint32_t>(Ashita::PluginFlags::UseDirect3D);
    }

    bool Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id) override
    {
        m_Core   = core;
        m_Logger = logger;
        m_Id     = id;

        ::QueryPerformanceFrequency(&g_Freq);

        auto* pointers           = core->GetPointerManager();
        const uintptr_t teardown = pointers->Add("noblink_teardown", "FFXiMain.dll", kTeardownPattern, 0, 0);
        if (teardown == 0)
        {
            logger->Logf(kLogError, "NoBlink", "Failed to resolve the teardown signature.");
            return false;
        }

        if (!this->VerifyInModule(teardown))
        {
            this->Cleanup();
            return false;
        }

        const uintptr_t loader = pointers->Add("noblink_loader", "FFXiMain.dll", kLoaderPattern, 0, 0);
        if (loader == 0)
        {
            logger->Logf(kLogError, "NoBlink", "Failed to resolve the presentation loader.");
            this->Cleanup();
            return false;
        }
        if (!this->VerifyInModule(loader))
        {
            this->Cleanup();
            return false;
        }

        // Recover the eligibility predicate from the teardown's own call displacement, so it
        // needs no signature and cannot be matched independently.
        int32_t displacement{};
        std::memcpy(&displacement, reinterpret_cast<const void*>(teardown + kPredicateRel32), sizeof(displacement));
        const uintptr_t predicate = teardown + kPredicateNext + displacement;
        if (!this->VerifyInModule(predicate))
        {
            this->Cleanup();
            return false;
        }

        const uintptr_t clearFn = pointers->Add("noblink_clearlook", "FFXiMain.dll", kClearAppearancePattern, 0, 0);
        const uintptr_t ctor    = pointers->Add("noblink_ctor", "FFXiMain.dll", kCtorPattern, 0, 0);
        if (clearFn == 0 || ctor == 0 || !this->VerifyInModule(clearFn) || !this->VerifyInModule(ctor))
        {
            logger->Logf(kLogError, "NoBlink", "Failed to resolve the appearance-clear helper or constructor.");
            this->Cleanup();
            return false;
        }

        // The vtable immediate is relocated by the loader, so reading it live yields the
        // runtime address for this module base.
        std::memcpy(&g_SkeletonActorVtable, reinterpret_cast<const void*>(ctor + kCtorVtableImm),
            sizeof(g_SkeletonActorVtable));

        g_ClearAppearance = reinterpret_cast<ClearAppearance_t>(clearFn);
        g_Loader    = reinterpret_cast<Loader_t>(loader);
        g_Predicate = reinterpret_cast<Predicate_t>(predicate);
        logger->Logf(kLogInfo, "NoBlink", "clearlook %p, skeleton-actor vtable %p",
            reinterpret_cast<void*>(clearFn), reinterpret_cast<void*>(g_SkeletonActorVtable));
        logger->Logf(kLogInfo, "NoBlink", "loader %p, predicate %p",
            reinterpret_cast<void*>(loader), reinterpret_cast<void*>(predicate));

        auto* target = reinterpret_cast<uint8_t*>(teardown);
        if (std::memcmp(target, kExpectedPrologue, kPatchSize) != 0)
        {
            logger->Logf(kLogError, "NoBlink", "Unexpected prologue at %p.", target);
            this->Cleanup();
            return false;
        }

        if (!this->InstallHook(target))
        {
            this->Cleanup();
            return false;
        }

        const uintptr_t teardownAll = pointers->Add("noblink_teardownall", "FFXiMain.dll", kTeardownAllPattern, 0, 0);
        if (teardownAll == 0 || !this->VerifyInModule(teardownAll) ||
            !this->InstallTeardownAllHook(reinterpret_cast<uint8_t*>(teardownAll)))
        {
            logger->Logf(kLogError, "NoBlink", "Failed to hook the teardown for caller attribution.");
            this->RemoveHook();
            this->Cleanup();
            return false;
        }

        const uintptr_t deferReg = pointers->Add("noblink_deferreg", "FFXiMain.dll", kDeferRegisterPattern, 0, 0);
        if (deferReg == 0 || !this->VerifyInModule(deferReg) ||
            !this->InstallDeferHook(reinterpret_cast<uint8_t*>(deferReg)))
        {
            logger->Logf(kLogError, "NoBlink", "Failed to hook the deferred-resource registration.");
            this->RemoveTeardownAllHook();
            this->RemoveHook();
            this->Cleanup();
            return false;
        }

        // Diagnostic only: the resource-link arena, read out of the usable/idle predicate's
        // relocated `mov ecx, [imm32]`. A missing one costs the /noblink res arena line and
        // nothing else, so it does not fail initialization.
        const uintptr_t usable = pointers->Add("noblink_usable", "FFXiMain.dll", kUsablePattern, 0, 0);
        if (usable != 0 && this->VerifyInModule(usable))
        {
            std::memcpy(&g_ArenaManagerPtr, reinterpret_cast<const void*>(usable + kUsableArenaImm),
                sizeof(g_ArenaManagerPtr));
        }
        else
        {
            logger->Logf(kLogInfo, "NoBlink", "No arena manager pointer; /noblink res omits arena offsets.");
        }

        logger->Logf(kLogInfo, "NoBlink", "Tracing teardowns at %p. Measurement build, no repair.", target);
        return true;
    }

    void Release(void) override
    {
        this->RemoveDeferHook();
        this->RemoveTeardownAllHook();
        this->RemoveHook();
        this->Cleanup();
    }

    bool Direct3DInitialize(IDirect3DDevice8*) override
    {
        // Required when UseDirect3D is set; the base implementation returns false, which
        // makes Ashita reject the plugin.
        return true;
    }

    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override
    {
        const uint32_t frame = g_Frame.fetch_add(1, std::memory_order_relaxed);

        // Target continuity, sampled every frame.
        if (auto* target = m_Core->GetMemoryManager()->GetTarget())
        {
            const bool active       = target->GetIsActive(0) != 0;
            const uint32_t serverId = target->GetServerId(0);

            const bool wasActive       = g_TargetLastActive.load(std::memory_order_relaxed);
            const uint32_t wasServerId = g_TargetLastServerId.load(std::memory_order_relaxed);

            if (wasActive && wasServerId != 0 && (!active || serverId != wasServerId))
            {
                g_TargetDrops.fetch_add(1, std::memory_order_relaxed);
                m_Logger->Logf(kLogInfo, "NoBlink",
                    "TARGET DROP at frame %u: was active on %08X, now %s %08X",
                    frame, wasServerId, active ? "active on" : "inactive", serverId);
            }

            g_TargetLastActive.store(active, std::memory_order_relaxed);
            g_TargetLastServerId.store(serverId, std::memory_order_relaxed);
            if (active)
                g_TargetSamples.fetch_add(1, std::memory_order_relaxed);
        }

        // Close out a pending blink as soon as the model binding is back.
        if (g_Pending.load(std::memory_order_relaxed))
        {
            void* watched = g_Watched.load(std::memory_order_relaxed);
            const uint32_t frames = g_PendingFrames.fetch_add(1, std::memory_order_relaxed) + 1;

            bool bound = false;
            if (watched != nullptr)
            {
                const uintptr_t actor = ReadPtr(watched, kEntityActorPointer);
                if (actor != 0 && !::IsBadReadPtr(reinterpret_cast<void*>(actor), kActorModelBinding + 4))
                    bound = ReadPtr(reinterpret_cast<void*>(actor), kActorModelBinding) != 0;
            }

            if (bound || frames > 300)
            {
                LARGE_INTEGER now{};
                ::QueryPerformanceCounter(&now);
                const uint32_t micros = g_Freq.QuadPart
                    ? (uint32_t)((now.QuadPart - g_PendingStart.QuadPart) * 1000000ll / g_Freq.QuadPart)
                    : 0;

                g_BlinkCount.fetch_add(1, std::memory_order_relaxed);
                g_BlinkFrames.fetch_add(frames, std::memory_order_relaxed);
                g_BlinkMicros.fetch_add(micros, std::memory_order_relaxed);
                uint32_t prevMax = g_BlinkMaxMicros.load(std::memory_order_relaxed);
                while (micros > prevMax &&
                       !g_BlinkMaxMicros.compare_exchange_weak(prevMax, micros, std::memory_order_relaxed))
                {
                }
                g_Pending.store(false, std::memory_order_relaxed);
            }
        }

        this->ServiceRemovals();

        // Watch mode resolves the entity by index every frame and samples continuously, so
        // it works identically with suppression on (no teardown) and off.
        void* entity = nullptr;
        const uint32_t watch = g_WatchIndex.load(std::memory_order_relaxed);
        if (watch != kNoWatchIndex)
        {
            entity = m_Core->GetMemoryManager()->GetEntity()->GetRawEntity(watch);
            if (entity != nullptr)
                this->AccumulateBusy(ReadS16(entity, kEntityBusy));
        }
        else if (g_Tracing.load(std::memory_order_relaxed))
        {
            entity = g_Watched.load(std::memory_order_relaxed);
        }

        if (entity == nullptr)
            return;

        const uint32_t count = g_SampleCount.load(std::memory_order_relaxed);
        if (count >= kMaxSamples)
        {
            g_Tracing.store(false, std::memory_order_relaxed);
            return;
        }

        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);

        Sample& s      = g_Samples[count];
        s.Frame        = frame;
        s.Milliseconds = g_Freq.QuadPart
            ? (double)(now.QuadPart - g_TraceStart.QuadPart) * 1000.0 / (double)g_Freq.QuadPart
            : 0.0;
        s.Actor = ReadPtr(entity, kEntityActorPointer);

        std::memcpy(s.EntityWords, static_cast<const uint8_t*>(entity) + kEntityWindowBase,
            sizeof(s.EntityWords));

        std::memset(s.ActorWords, 0, sizeof(s.ActorWords));
        std::memset(s.ActorTail, 0, sizeof(s.ActorTail));
        s.ChainLen = 0;
        if (s.Actor != 0)
        {
            auto* actor = reinterpret_cast<uint8_t*>(s.Actor);
            if (!::IsBadReadPtr(actor, sizeof(s.ActorWords)))
                std::memcpy(s.ActorWords, actor, sizeof(s.ActorWords));
            if (!::IsBadReadPtr(actor + kActorTailBase, sizeof(s.ActorTail)))
                std::memcpy(s.ActorTail, actor + kActorTailBase, sizeof(s.ActorTail));
            s.ResCount = 0;
            if (void** root = ChainRoot(actor))
            {
                s.ChainLen = ChainLength(*root);
                void* model = *root;
                if (model != nullptr && !::IsBadReadPtr(model, kModelResources + sizeof(void*)))
                {
                    void* head = *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(model) + kModelResources);
                    s.ResCount = ChainLength(head);
                }
            }
        }

        g_SampleCount.store(count + 1, std::memory_order_relaxed);
    }

    bool HandleCommand(int32_t, const char* command, bool) override
    {
        std::vector<std::string> args;
        if (Ashita::Commands::GetCommandArgs(command, &args) == 0 || args.empty())
            return false;
        if (_stricmp(args[0].c_str(), "/noblink") != 0)
            return false;

        const char* verb = args.size() > 1 ? args[1].c_str() : "status";

        if (_stricmp(verb, "dump") == 0)
            this->Dump();
        else if (_stricmp(verb, "res") == 0)
            this->DumpResources();
        else if (_stricmp(verb, "callers") == 0)
        {
            const HMODULE module = ::GetModuleHandleA("FFXiMain.dll");
            const uintptr_t base = reinterpret_cast<uintptr_t>(module);
            const uint32_t n     = g_CallerCount.load(std::memory_order_relaxed);
            m_Logger->Logf(kLogInfo, "NoBlink", "--- actor destroys: %u total, %u distinct callers ---",
                g_DestroyTotal.load(std::memory_order_relaxed), n);
            for (uint32_t i = 0; i < n; ++i)
            {
                // Report the static address so it can be looked up directly in the catalog.
                const uintptr_t staticAddr = 0x1a40000 + (g_Callers[i].Address - base);
                m_Logger->Logf(kLogInfo, "NoBlink", "  %5u  ret=%08X  static=%08X",
                    g_Callers[i].Count, static_cast<uint32_t>(g_Callers[i].Address),
                    static_cast<uint32_t>(staticAddr));
            }
            m_Logger->Logf(kLogInfo, "NoBlink", "--- end callers ---");
            this->Report("caller attribution dumped to the Ashita log");
        }
        else if (_stricmp(verb, "watch") == 0)
        {
            // "target" latches the index of whatever is currently targeted and then forgets
            // the target entirely. The target entry is only used to discover the index --
            // it is never read as a metric, which is the trap that made the earlier target
            // continuity numbers meaningless.
            //
            // A character NAME is the argument to prefer. Entity indices are assigned by
            // login order and change between relaunches, and watching the wrong one still
            // produces plausible-looking numbers -- a trap that has cost whole sessions here.
            // A name cannot silently resolve to the wrong character.
            uint32_t index    = kNoWatchIndex;
            const char* named = nullptr;
            if (args.size() > 2 && _stricmp(args[2].c_str(), "off") == 0)
            {
                index = kNoWatchIndex;
            }
            else if (args.size() > 2 && _stricmp(args[2].c_str(), "target") == 0)
            {
                index = m_Core->GetMemoryManager()->GetTarget()->GetTargetIndex(0);
            }
            else if (args.size() > 2 && ::isdigit(static_cast<unsigned char>(args[2][0])) == 0)
            {
                named       = args[2].c_str();
                auto* ents  = m_Core->GetMemoryManager()->GetEntity();
                for (uint32_t i = 1; i < 0x900; ++i)
                {
                    const char* name = ents->GetName(i);
                    if (name != nullptr && _stricmp(name, named) == 0)
                    {
                        index = i;
                        break;
                    }
                }
            }
            else if (args.size() > 2)
            {
                index = static_cast<uint32_t>(::strtoul(args[2].c_str(), nullptr, 0));
            }
            else
            {
                index = m_Core->GetMemoryManager()->GetTarget()->GetTargetIndex(0);
            }

            if (named != nullptr && index == kNoWatchIndex)
            {
                char miss[192];
                ::sprintf_s(miss, "no visible entity named '%s'; watch unchanged", named);
                this->Report(miss);
                return true;
            }

            char buffer[192];
            if (index == kNoWatchIndex || index == 0)
            {
                g_WatchIndex.store(kNoWatchIndex, std::memory_order_relaxed);
                g_Tracing.store(false, std::memory_order_relaxed);
                ::sprintf_s(buffer, "watch off");
            }
            else
            {
                auto* entity = m_Core->GetMemoryManager()->GetEntity()->GetRawEntity(index);
                if (entity == nullptr)
                {
                    ::sprintf_s(buffer, "no entity at index %u; watch unchanged", index);
                }
                else
                {
                    g_SampleCount.store(0, std::memory_order_relaxed);
                    g_BusyRun.store(0, std::memory_order_relaxed);
                    ::QueryPerformanceCounter(&g_TraceStart);
                    g_WatchIndex.store(index, std::memory_order_relaxed);
                    const char* name = m_Core->GetMemoryManager()->GetEntity()->GetName(index);
                    ::sprintf_s(buffer, "watching '%s' at entity index %u (%p), sampling every frame",
                        name != nullptr ? name : "?", index, entity);
                }
            }
            this->Report(buffer);
        }
        else if (_stricmp(verb, "target") == 0 && args.size() > 2)
        {
            const uint32_t index = static_cast<uint32_t>(::strtoul(args[2].c_str(), nullptr, 0));
            m_Core->GetMemoryManager()->GetTarget()->SetTarget(index, true);
            g_TargetLastActive.store(false, std::memory_order_relaxed);
            g_TargetLastServerId.store(0, std::memory_order_relaxed);
            char buffer[128];
            ::sprintf_s(buffer, "targeting entity index %u", index);
            this->Report(buffer);
        }
        else if (_stricmp(verb, "on") == 0)
        {
            g_Suppress.store(true, std::memory_order_relaxed);
            this->Report("suppression ON (rebind in place, no teardown)");
        }
        else if (_stricmp(verb, "off") == 0)
        {
            g_Suppress.store(false, std::memory_order_relaxed);
            this->Report("suppression OFF (stock teardown, still measuring)");
        }
        else if (_stricmp(verb, "reset") == 0)
        {
            g_Tracing.store(false, std::memory_order_relaxed);
            g_SampleCount.store(0, std::memory_order_relaxed);
            g_Watched.store(nullptr, std::memory_order_relaxed);
            g_Teardowns.store(0, std::memory_order_relaxed);
            g_Pending.store(false, std::memory_order_relaxed);
            g_BlinkCount.store(0, std::memory_order_relaxed);
            g_BlinkFrames.store(0, std::memory_order_relaxed);
            g_BlinkMicros.store(0, std::memory_order_relaxed);
            g_BlinkMaxMicros.store(0, std::memory_order_relaxed);
            g_Suppressed.store(0, std::memory_order_relaxed);
            g_TargetDrops.store(0, std::memory_order_relaxed);
            g_TargetSamples.store(0, std::memory_order_relaxed);
            g_CallerCount.store(0, std::memory_order_relaxed);
            g_DestroyTotal.store(0, std::memory_order_relaxed);
            g_BusyWindows.store(0, std::memory_order_relaxed);
            g_BusyFrames.store(0, std::memory_order_relaxed);
            g_BusyMaxFrames.store(0, std::memory_order_relaxed);
            g_BusyPeak.store(0, std::memory_order_relaxed);
            g_BusyRun.store(0, std::memory_order_relaxed);
            g_ChainsCleared.store(0, std::memory_order_relaxed);
            g_Fallbacks.store(0, std::memory_order_relaxed);
            g_Deferred.store(0, std::memory_order_relaxed);
            g_DeferredDone.store(0, std::memory_order_relaxed);
            g_DeferredTimeout.store(0, std::memory_order_relaxed);
            g_DeferRegistered.store(0, std::memory_order_relaxed);
            g_DeferInWindowTotal.store(0, std::memory_order_relaxed);
            g_DeferSeen.store(0, std::memory_order_relaxed);
            g_DeferLastActor.store(nullptr, std::memory_order_relaxed);
            ::QueryPerformanceCounter(&g_TraceStart);
            this->Report("counters reset; watch (if set) keeps sampling");
        }
        else
        {
            const uint32_t blinks = g_BlinkCount.load(std::memory_order_relaxed);
            const double meanMs   = blinks ? (double)g_BlinkMicros.load(std::memory_order_relaxed) / blinks / 1000.0 : 0.0;
            const double meanFr   = blinks ? (double)g_BlinkFrames.load(std::memory_order_relaxed) / blinks : 0.0;

            const uint32_t windows = g_BusyWindows.load(std::memory_order_relaxed);
            const uint32_t watch   = g_WatchIndex.load(std::memory_order_relaxed);

            char buffer[512];
            ::sprintf_s(buffer, "suppress %s | TARGET DROPS %u (%u tracked frames) | vt %08X/%08X, rebinds %llu | teardowns %llu | blinks %u, mean %.1fms (%.2f frames), max %.1fms",
                g_Suppress.load(std::memory_order_relaxed) ? "on" : "off",
                g_TargetDrops.load(std::memory_order_relaxed),
                g_TargetSamples.load(std::memory_order_relaxed),
                static_cast<uint32_t>(g_ObservedVtable.load(std::memory_order_relaxed)),
                static_cast<uint32_t>(g_SkeletonActorVtable),
                static_cast<unsigned long long>(g_Suppressed.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_Teardowns.load(std::memory_order_relaxed)),
                blinks, meanMs, meanFr,
                g_BlinkMaxMicros.load(std::memory_order_relaxed) / 1000.0);
            this->Report(buffer);

            if (watch == kNoWatchIndex)
            {
                this->Report("watch off -- /noblink watch target before reading busy numbers");
            }
            else
            {
                // "parked N (M matched our actor) of K clientwide". N is what the hold uses.
                // N vs M exposes an attribution bug; K vs 0 proves the hook fires at all.
                // Reading N without K is how a broken hook would look exactly like success.
                ::sprintf_s(buffer,
                    "watch idx %u, %u samples | rebinds %llu, fallbacks %llu | held looks %llu (released %llu, timeout %llu) | parked %llu (%llu matched, last actor %08X) of %llu clientwide | busy windows %u, %u frames total, longest %u, peak %d",
                    watch, g_SampleCount.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(g_ChainsCleared.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_Fallbacks.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_Deferred.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_DeferredDone.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_DeferredTimeout.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_DeferInWindowTotal.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_DeferRegistered.load(std::memory_order_relaxed)),
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                        g_DeferLastActor.load(std::memory_order_relaxed))),
                    static_cast<unsigned long long>(g_DeferSeen.load(std::memory_order_relaxed)),
                    windows,
                    g_BusyFrames.load(std::memory_order_relaxed),
                    g_BusyMaxFrames.load(std::memory_order_relaxed),
                    g_BusyPeak.load(std::memory_order_relaxed));
                this->Report(buffer);
            }
        }
        return true;
    }

private:
    /**
     * Releases a queued old look once every replacement has landed.
     *
     * Completion has two parts, and both are needed. actor+0xa08 going null says the queued
     * presentation task has run -- necessary, because until then nothing has been attached at
     * all -- but not sufficient, because the task parks whatever was not resident and those
     * arrive later still. Releasing on the task pointer alone drops the old look about 758ms
     * early, which is the missing chestpiece. So the task pointer only fixes the target, and
     * arrival against that target, counted by node identity, is what releases.
     *
     * This never calls the loader -- doing so repeatedly is what made the earlier hold attempt
     * pile up resources -- so the worst case is holding the old look until the timeout.
     */
    /**
     * Drops a hold without releasing anything, and disarms the registration window if it was
     * armed for that actor. Leaving it armed would let the next swap inherit a stale count.
     */
    void AbandonHold(PendingRemoval& slot)
    {
        if (g_DeferActor.load(std::memory_order_relaxed) ==
            reinterpret_cast<void*>(slot.Actor))
        {
            g_DeferWindow.store(false, std::memory_order_relaxed);
            g_DeferActor.store(nullptr, std::memory_order_relaxed);
        }
        slot.Entity = nullptr;
    }

    void ServiceRemovals(void)
    {
        for (auto& slot : g_Removals)
        {
            if (slot.Entity == nullptr)
                continue;

            slot.Frames += 1;

            // Abandon if the actor was replaced underneath us: the stock path rebuilt the
            // presentation and the recorded nodes died with it.
            const uintptr_t actor = ReadPtr(slot.Entity, kEntityActorPointer);
            if (actor == 0 || actor != slot.Actor)
            {
                this->AbandonHold(slot);
                continue;
            }

            void** root = ChainRoot(reinterpret_cast<void*>(actor));
            if (root == nullptr || *root != slot.Model)
            {
                this->AbandonHold(slot);   // model rebuilt; old nodes went with it
                continue;
            }

            auto* resHead = reinterpret_cast<void**>(
                static_cast<uint8_t*>(slot.Model) + kModelResources);

            const uint32_t arrived = CountUnrecorded(*resHead, slot.Nodes, slot.Count);

            // The presentation task holds every attach and every FUN_01ab1020 registration
            // this swap makes, so nothing can be concluded until it has finished. The frame
            // actor+0xa08 goes null, `arrived` is exactly what the task bound synchronously
            // and the registration count is exactly what it parked -- so their sum is how
            // many replacement nodes must exist before the old look may go.
            if (!slot.TaskDone)
            {
                if (ReadPtr(reinterpret_cast<void*>(actor), kActorDeferredTask) != 0)
                {
                    if (slot.Frames < kRemoveMaxFrames)
                        continue;
                }
                else
                {
                    slot.TaskDone = true;
                    slot.Expected = arrived + g_DeferInWindow.load(std::memory_order_relaxed);
                }
            }

            const bool settled  = slot.TaskDone && arrived >= slot.Expected;
            const bool timedOut = slot.Frames >= kRemoveMaxFrames;
            if (!settled && !timedOut)
                continue;

            // Disarm the registration window now that this swap is finished with.
            if (g_DeferActor.load(std::memory_order_relaxed) == reinterpret_cast<void*>(actor))
            {
                g_DeferWindow.store(false, std::memory_order_relaxed);
                g_DeferActor.store(nullptr, std::memory_order_relaxed);
            }

            for (uint32_t i = 0; i < slot.Count; ++i)
                UnlinkAndFree(resHead, slot.Nodes[i]);

            slot.Entity = nullptr;
            if (timedOut && !settled)
                g_DeferredTimeout.fetch_add(1, std::memory_order_relaxed);
            else
                g_DeferredDone.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * Tracks runs of frames where the entity's presentation busy count is raised. A run is
     * closed the frame the count returns to nonpositive, which is the same threshold
     * FUN_01ad6100 uses to decide the entity has settled.
     */
    void AccumulateBusy(const int16_t busy)
    {
        const uint32_t run = g_BusyRun.load(std::memory_order_relaxed);

        if (busy > 0)
        {
            g_BusyRun.store(run + 1, std::memory_order_relaxed);
            g_BusyFrames.fetch_add(1, std::memory_order_relaxed);

            int32_t peak = g_BusyPeak.load(std::memory_order_relaxed);
            while (busy > peak && !g_BusyPeak.compare_exchange_weak(peak, busy, std::memory_order_relaxed))
            {
            }
            return;
        }

        if (run == 0)
            return;

        g_BusyRun.store(0, std::memory_order_relaxed);
        g_BusyWindows.fetch_add(1, std::memory_order_relaxed);

        uint32_t longest = g_BusyMaxFrames.load(std::memory_order_relaxed);
        while (run > longest &&
               !g_BusyMaxFrames.compare_exchange_weak(longest, run, std::memory_order_relaxed))
        {
        }
    }

    /**
     * Prints the watched actor's attached-resource chain, one line per node.
     *
     * The steady counts (9 and 11 for the two test garments) do not match the nine look
     * slots one-to-one, so which node belongs to which slot is an open question. Diffing
     * this across a swap answers it directly, and that mapping is what a per-slot rebind
     * needs: FUN_01a69e40 removes one exact node, but only if we know which one.
     */
    void DumpResources(void)
    {
        const uint32_t watch = g_WatchIndex.load(std::memory_order_relaxed);
        if (watch == kNoWatchIndex)
        {
            this->Report("no watch set; /noblink watch <index> first");
            return;
        }

        auto* entity = m_Core->GetMemoryManager()->GetEntity()->GetRawEntity(watch);
        if (entity == nullptr)
        {
            this->Report("watched entity is gone");
            return;
        }

        const uintptr_t actor = ReadPtr(entity, kEntityActorPointer);
        if (actor == 0)
        {
            this->Report("watched entity has no actor");
            return;
        }

        void** root = ChainRoot(reinterpret_cast<void*>(actor));
        void* model = (root != nullptr) ? *root : nullptr;
        if (model == nullptr || ::IsBadReadPtr(model, kModelResources + sizeof(void*)))
        {
            this->Report("watched actor has no model");
            return;
        }

        void* node = *reinterpret_cast<void**>(static_cast<uint8_t*>(model) + kModelResources);

        m_Logger->Logf(kLogInfo, "NoBlink", "--- resources of model %p (actor %08X) ---",
            model, static_cast<uint32_t>(actor));

        // The link each node carries at +0x08 is a cell of ONE 256KB arena -- FUN_01ab4110
        // accepts a link only within [manager+8, manager+0x40004]. Reporting each link as an
        // offset into that arena is what shows the "two descriptor blocks" for what they are:
        // two allocation regions of the same allocator, not two resource families.
        uintptr_t arena = 0;
        if (g_ArenaManagerPtr != 0 &&
            !::IsBadReadPtr(reinterpret_cast<void*>(g_ArenaManagerPtr), sizeof(uintptr_t)))
        {
            arena = *reinterpret_cast<uintptr_t*>(g_ArenaManagerPtr);
        }
        if (arena != 0)
        {
            m_Logger->Logf(kLogInfo, "NoBlink", "  link arena %08X..%08X (0x40000 bytes)",
                static_cast<uint32_t>(arena + kArenaFirst),
                static_cast<uint32_t>(arena + kArenaLast));
        }

        m_Logger->Logf(kLogInfo, "NoBlink",
            "  idx     node      +00      +04      +08      +0C      +10      +14      +18      +1C   arena+");

        uint32_t index = 0;
        for (; node != nullptr && index < 64; ++index)
        {
            if (::IsBadReadPtr(node, 0x20))
                break;

            uint32_t w[8];
            std::memcpy(w, node, sizeof(w));

            char where[16] = "        -";
            if (arena != 0 && w[2] >= arena + kArenaFirst && w[2] <= arena + kArenaLast)
                ::sprintf_s(where, "%8X", static_cast<uint32_t>(w[2] - arena));

            m_Logger->Logf(kLogInfo, "NoBlink",
                "  %3u %08X %08X %08X %08X %08X %08X %08X %08X %08X %s",
                index, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(node)),
                w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], where);

            node = *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + kChainNext);
        }
        m_Logger->Logf(kLogInfo, "NoBlink", "--- end resources (%u nodes) ---", index);

        char buffer[128];
        ::sprintf_s(buffer, "dumped %u resource nodes to the Ashita log", index);
        this->Report(buffer);
    }

    void Dump(void)
    {
        const uint32_t count = g_SampleCount.load(std::memory_order_relaxed);
        m_Logger->Logf(kLogInfo, "NoBlink", "--- trace: %u samples, %llu teardowns ---",
            count, static_cast<unsigned long long>(g_Teardowns.load(std::memory_order_relaxed)));
        if (count == 0)
        {
            m_Logger->Logf(kLogInfo, "NoBlink", "--- end trace (empty) ---");
            return;
        }

        // Only report words that actually move during the trace; a steady column is noise.
        // Both windows are reduced the same way, so the dump shows what the client touched
        // across the gear change rather than the fields this plugin happens to suspect.
        uint32_t entityVarying[kEntityWindowWords];
        uint32_t entityCount = 0;
        for (uint32_t w = 0; w < kEntityWindowWords; ++w)
        {
            for (uint32_t i = 1; i < count; ++i)
            {
                if (g_Samples[i].EntityWords[w] != g_Samples[0].EntityWords[w])
                {
                    entityVarying[entityCount++] = w;
                    break;
                }
            }
        }

        uint32_t actorVarying[kActorHeadWords];
        uint32_t actorCount = 0;
        for (uint32_t w = 0; w < kActorHeadWords; ++w)
        {
            for (uint32_t i = 1; i < count; ++i)
            {
                if (g_Samples[i].ActorWords[w] != g_Samples[0].ActorWords[w])
                {
                    actorVarying[actorCount++] = w;
                    break;
                }
            }
        }

        uint32_t tailVarying[kActorTailWords];
        uint32_t tailCount = 0;
        for (uint32_t w = 0; w < kActorTailWords; ++w)
        {
            for (uint32_t i = 1; i < count; ++i)
            {
                if (g_Samples[i].ActorTail[w] != g_Samples[0].ActorTail[w])
                {
                    tailVarying[tailCount++] = w;
                    break;
                }
            }
        }

        char header[512];
        int n = ::sprintf_s(header, "  idx frame     ms  busy models  res   actor");
        for (uint32_t v = 0; v < entityCount && n < 400; ++v)
            n += ::sprintf_s(header + n, sizeof(header) - n, "     e+%02X",
                static_cast<uint32_t>(kEntityWindowBase) + entityVarying[v] * 4);
        for (uint32_t v = 0; v < actorCount && n < 440; ++v)
            n += ::sprintf_s(header + n, sizeof(header) - n, "     a+%02X", actorVarying[v] * 4);
        for (uint32_t v = 0; v < tailCount && n < 470; ++v)
            n += ::sprintf_s(header + n, sizeof(header) - n, "    a+%03X",
                static_cast<uint32_t>(kActorTailBase) + tailVarying[v] * 4);
        m_Logger->Logf(kLogInfo, "NoBlink", "%s", header);
        m_Logger->Logf(kLogInfo, "NoBlink",
            "  (entity words varying: %u of %u; actor head: %u of %u; actor tail: %u of %u)",
            entityCount, static_cast<uint32_t>(kEntityWindowWords),
            actorCount, static_cast<uint32_t>(kActorHeadWords),
            tailCount, static_cast<uint32_t>(kActorTailWords));

        constexpr uint32_t kBusyWord = (kEntityBusy - kEntityWindowBase) / 4;
        for (uint32_t i = 0; i < count; ++i)
        {
            const Sample& s = g_Samples[i];
            // +0xf2 is the high half of the word at +0xf0, read back as signed.
            const int16_t busy = static_cast<int16_t>(s.EntityWords[kBusyWord] >> 16);

            char line[1024];
            int p = ::sprintf_s(line, "  %3u %5u %6.1f %5d %6u %4u  %08X",
                i, s.Frame, s.Milliseconds, busy, s.ChainLen, s.ResCount,
                static_cast<uint32_t>(s.Actor));
            for (uint32_t v = 0; v < entityCount && p < 900; ++v)
                p += ::sprintf_s(line + p, sizeof(line) - p, " %08X", s.EntityWords[entityVarying[v]]);
            for (uint32_t v = 0; v < actorCount && p < 940; ++v)
                p += ::sprintf_s(line + p, sizeof(line) - p, " %08X", s.ActorWords[actorVarying[v]]);
            for (uint32_t v = 0; v < tailCount && p < 980; ++v)
                p += ::sprintf_s(line + p, sizeof(line) - p, " %08X", s.ActorTail[tailVarying[v]]);
            m_Logger->Logf(kLogInfo, "NoBlink", "%s", line);
        }
        m_Logger->Logf(kLogInfo, "NoBlink", "--- end trace ---");

        char buffer[128];
        ::sprintf_s(buffer, "dumped %u samples to the Ashita log", count);
        this->Report(buffer);
    }

    void Report(const char* message)
    {
        char buffer[320];
        ::sprintf_s(buffer, "[\x1e\x51NoBlink\x1e\x01] %s", message);
        m_Core->GetChatManager()->Write(0, false, buffer);
        m_Logger->Logf(kLogInfo, "NoBlink", "%s", message);
    }

    bool VerifyInModule(const uintptr_t teardown)
    {
        const HMODULE module = ::GetModuleHandleA("FFXiMain.dll");
        if (module == nullptr)
            return false;

        MODULEINFO info{};
        if (!::GetModuleInformation(::GetCurrentProcess(), module, &info, sizeof(info)))
            return false;

        const uintptr_t base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        const bool inside    = teardown >= base && teardown < base + info.SizeOfImage;

        m_Logger->Logf(kLogInfo, "NoBlink", "FFXiMain.dll base %p size 0x%X; teardown %p (module+0x%X) in-module=%s",
            info.lpBaseOfDll, info.SizeOfImage, reinterpret_cast<void*>(teardown),
            static_cast<uint32_t>(teardown - base), inside ? "yes" : "NO");

        if (!inside)
            m_Logger->Logf(kLogError, "NoBlink", "Resolved outside FFXiMain.dll. Refusing to hook.");

        return inside;
    }

    bool InstallHook(uint8_t* target)
    {
        auto* trampoline = static_cast<uint8_t*>(
            ::VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (trampoline == nullptr)
            return false;

        std::memcpy(trampoline, target, kPatchSize);
        trampoline[kPatchSize] = 0xE9;
        const int32_t backRel  = static_cast<int32_t>((target + kPatchSize) - (trampoline + kPatchSize + 5));
        std::memcpy(trampoline + kPatchSize + 1, &backRel, sizeof(backRel));

        std::memcpy(g_OriginalBytes, target, kPatchSize);

        uint8_t patch[kPatchSize];
        patch[0]              = 0xE9;
        const int32_t hookRel = static_cast<int32_t>(
            reinterpret_cast<uint8_t*>(&TeardownDetour) - (target + kPatchSize));
        std::memcpy(patch + 1, &hookRel, sizeof(hookRel));

        if (!WriteCode(target, patch, kPatchSize))
        {
            ::VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        g_Trampoline = reinterpret_cast<Teardown_t>(trampoline);
        g_HookTarget = target;
        g_Hooked     = true;
        return true;
    }

    /**
     * Hooks FUN_01ad2830. The 5-byte patch splits `call rel32`, so the trampoline
     * reproduces `push esi; mov esi, ecx; call <target>` with the displacement recomputed
     * for its own address, then jumps back past the stolen 8 bytes.
     */
    bool InstallTeardownAllHook(uint8_t* target)
    {
        if (std::memcmp(target, kTeardownAllPrologue, sizeof(kTeardownAllPrologue)) != 0)
        {
            m_Logger->Logf(kLogError, "NoBlink", "Unexpected teardown prologue at %p.", target);
            return false;
        }

        int32_t originalRel{};
        std::memcpy(&originalRel, target + 4, sizeof(originalRel));
        const uint8_t* callTarget = target + kTeardownAllStolen + originalRel;

        auto* tramp = static_cast<uint8_t*>(
            ::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (tramp == nullptr)
            return false;

        tramp[0] = 0x56; // push esi
        tramp[1] = 0x8B; // mov esi, ecx
        tramp[2] = 0xF1;
        tramp[3] = 0xE8; // call <callTarget>
        const int32_t newRel = static_cast<int32_t>(callTarget - (tramp + 8));
        std::memcpy(tramp + 4, &newRel, sizeof(newRel));
        tramp[8] = 0xE9; // jmp target+8
        const int32_t backRel = static_cast<int32_t>((target + kTeardownAllStolen) - (tramp + 13));
        std::memcpy(tramp + 9, &backRel, sizeof(backRel));

        std::memcpy(g_TeardownAllOriginal, target, kPatchSize);

        uint8_t patch[kPatchSize];
        patch[0]              = 0xE9;
        const int32_t hookRel = static_cast<int32_t>(
            reinterpret_cast<uint8_t*>(&TeardownAllDetour) - (target + kPatchSize));
        std::memcpy(patch + 1, &hookRel, sizeof(hookRel));

        if (!WriteCode(target, patch, kPatchSize))
        {
            ::VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }

        g_TeardownAllTramp  = reinterpret_cast<TeardownAll_t>(tramp);
        g_TeardownAllTarget = target;
        g_TeardownAllHooked = true;
        return true;
    }

    /**
     * Hooks FUN_01ab1020. The six stolen bytes are four whole instructions with no
     * position-dependent operand, so the trampoline is a straight copy followed by a jump
     * back past them; ECX still carries the handle cell the rest of the function reads.
     */
    bool InstallDeferHook(uint8_t* target)
    {
        if (std::memcmp(target, kDeferRegisterPrologue, sizeof(kDeferRegisterPrologue)) != 0)
        {
            m_Logger->Logf(kLogError, "NoBlink", "Unexpected registration prologue at %p.", target);
            return false;
        }

        auto* tramp = static_cast<uint8_t*>(
            ::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (tramp == nullptr)
            return false;

        std::memcpy(tramp, target, kDeferRegisterStolen);
        tramp[kDeferRegisterStolen] = 0xE9;
        const int32_t backRel       = static_cast<int32_t>(
            (target + kDeferRegisterStolen) - (tramp + kDeferRegisterStolen + 5));
        std::memcpy(tramp + kDeferRegisterStolen + 1, &backRel, sizeof(backRel));

        std::memcpy(g_DeferOriginal, target, kPatchSize);

        uint8_t patch[kPatchSize];
        patch[0]              = 0xE9;
        const int32_t hookRel = static_cast<int32_t>(
            reinterpret_cast<uint8_t*>(&DeferRegisterDetour) - (target + kPatchSize));
        std::memcpy(patch + 1, &hookRel, sizeof(hookRel));

        if (!WriteCode(target, patch, kPatchSize))
        {
            ::VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }

        g_DeferTramp  = reinterpret_cast<DeferRegister_t>(tramp);
        g_DeferTarget = target;
        g_DeferHooked = true;
        return true;
    }

    void RemoveDeferHook(void)
    {
        if (!g_DeferHooked)
            return;

        WriteCode(g_DeferTarget, g_DeferOriginal, kPatchSize);
        g_DeferHooked = false;
        ::Sleep(50);

        if (g_DeferTramp != nullptr)
        {
            ::VirtualFree(reinterpret_cast<void*>(g_DeferTramp), 0, MEM_RELEASE);
            g_DeferTramp = nullptr;
        }
        g_DeferTarget = nullptr;
    }

    void RemoveTeardownAllHook(void)
    {
        if (!g_TeardownAllHooked)
            return;

        WriteCode(g_TeardownAllTarget, g_TeardownAllOriginal, kPatchSize);
        g_TeardownAllHooked = false;
        ::Sleep(50);

        if (g_TeardownAllTramp != nullptr)
        {
            ::VirtualFree(reinterpret_cast<void*>(g_TeardownAllTramp), 0, MEM_RELEASE);
            g_TeardownAllTramp = nullptr;
        }
        g_TeardownAllTarget = nullptr;
    }

    void RemoveHook(void)
    {
        if (!g_Hooked)
            return;

        WriteCode(g_HookTarget, g_OriginalBytes, kPatchSize);
        g_Hooked = false;
        g_Tracing.store(false, std::memory_order_relaxed);

        ::Sleep(50);

        if (g_Trampoline != nullptr)
        {
            ::VirtualFree(reinterpret_cast<void*>(g_Trampoline), 0, MEM_RELEASE);
            g_Trampoline = nullptr;
        }
        g_HookTarget = nullptr;
    }

    void Cleanup(void)
    {
        if (m_Core != nullptr)
            m_Core->GetPointerManager()->Delete("noblink_teardown");
    }
};

__declspec(dllexport) IPlugin* __stdcall expCreatePlugin(const char*)
{
    return new NoBlink();
}

__declspec(dllexport) void __stdcall expDestroyPlugin(void* instance)
{
    delete static_cast<NoBlink*>(instance);
}

__declspec(dllexport) double __stdcall expGetInterfaceVersion(void)
{
    return ASHITA_INTERFACE_VERSION;
}
