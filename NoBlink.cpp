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
 * Every address resolves by unique byte signature at runtime; none are hardcoded.
 */

#include "Ashita.h"
#include "Commands.h"

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

    // ---- entity layout ------------------------------------------------------------------
    constexpr size_t kEntityActorPointer = 0xA0;    // ActorPointer
    constexpr size_t kEntityType         = 0xEE;    // 0 = PC
    constexpr size_t kEntityModelFlags   = 0xF4;    // ModelUpdateFlags
    constexpr size_t kEntityFlags0       = 0x120;   // Render.Flags0

    // ---- actor layout -------------------------------------------------------------------
    // The actor's model-list owner, constructed in place by FUN_01a6b3e0 @ 0x1a6b3e0, which
    // installs its vtable and clears the model-chain root at +0x44.
    constexpr size_t kActorModelManager = 0x674;
    constexpr size_t kModelChainRoot    = 0x44;
    // Virtual deleting destructor slot. FUN_01a6b480 @ 0x1a6b480 empties a whole chain with a
    // single +0x18(1) call on the root; the object's own destructor walks its +0x4 successors.
    constexpr size_t kDeletingDtorSlot = 0x18;
    // Intrusive successor link, shared by the model chain and each model's resource chain.
    constexpr size_t kChainNext     = 0x04;
    constexpr size_t kModelResources = 0x20;
    // The serialized 0x48 presentation task FUN_01b141f0 builds through FUN_01b16ee0.
    // Non-null exactly while the queued presentation work has not run yet.
    constexpr size_t kActorDeferredTask = 0xA08;

    // Bounds for chain walks. Measured chains are 9 to 11 nodes, so these only exist to stop
    // a corrupt or unterminated list, never to truncate a real one.
    constexpr uint32_t kMaxSnapshot = 64;
    constexpr uint32_t kMaxWalk     = 192;

    // ---- signatures ---------------------------------------------------------------------
    // Taken against a retail FFXiMain.dll rebuilt at base 0x1a40000, sha256
    // eae30ddd8830067c684d5f08cf33a5406184e6ba488947ecb62f839c88647b1d. Each occurs exactly
    // once in that image. Relative call displacements are wildcarded, and initialization
    // refuses to hook unless every resolved address lies inside the loaded module.

    // FUN_01ad5d00 - the gear-change teardown. Hooked and replaced with an in-place rebind.
    constexpr char kTeardownPattern[] =
        "568B7424088BCEE8????????84C074426683BEF4000000007509F686200100"
        "0001742F8B86300100008BCE25FFFFFDFF";
    constexpr uint8_t kExpectedPrologue[5] = {0x56, 0x8B, 0x74, 0x24, 0x08};
    constexpr size_t kPatchSize            = 5;

    // Offset of the rel32 for `call FUN_01ad5780` (the eligibility predicate) inside the
    // teardown, and the address of the following instruction. Recovering the predicate from
    // the teardown's own displacement means it needs no signature of its own.
    constexpr size_t kPredicateRel32 = 0x08;
    constexpr size_t kPredicateNext  = 0x0C;

    // FUN_01b141f0 - the repeatable presentation loader. Selector -1 makes it re-read the
    // entity's current packed look slots and rebind against the EXISTING actor, which is the
    // path that lets the teardown be skipped entirely.
    constexpr char kLoaderPattern[] =
        "53568BF132DB8B467085C074198B80200100008BC8C1E907F6C1017507C1E8"
        "08A8017402B3018B168BCEFF920403000084C0";

    // FUN_01ab1020 - registers the one-shot completion callback for a resource that was not
    // resident. Hooked only to count them: that count is the client's own tally of what this
    // swap still has outstanding.
    constexpr char kDeferRegisterPattern[] =
        "53566A006A0468840000008BD9E8????????8BF083C40C";
    // push ebx; push esi; push 0; push 4 -- six bytes, four whole instructions, no
    // position-dependent operand, so the trampoline is a straight copy plus a jump back.
    constexpr uint8_t kDeferRegisterPrologue[6] = {0x53, 0x56, 0x6A, 0x00, 0x6A, 0x04};
    constexpr size_t kDeferRegisterStolen       = 6;

    constexpr int32_t kSelectorDefault = -1;

    using Teardown_t     = void(__cdecl*)(void* entity);
    using Predicate_t    = uint8_t(__fastcall*)(void* entity, void* unused);
    using DeletingDtor_t = void*(__thiscall*)(void* self, int flags);
    // __thiscall(actor, selector) expressed for MSVC: ecx = actor, edx unused, selector on
    // the stack. Callee cleans in both conventions, so this is ABI-compatible.
    using Loader_t = uint32_t(__fastcall*)(void* actor, void* unused, int32_t selector);
    // ecx = handle cell, edx unused, then FUN_01ab1020's four stack arguments: callback,
    // context, actor, cancel flag. Callee-cleaned `ret 0x10` in both conventions.
    using DeferRegister_t = void(__fastcall*)(void* cell, void* unused, void* callback,
        void* context, void* actor, void* flag);

    Predicate_t g_Predicate = nullptr;
    Loader_t g_Loader       = nullptr;

    Teardown_t g_Trampoline = nullptr;
    uint8_t* g_HookTarget   = nullptr;
    uint8_t g_OriginalBytes[kPatchSize]{};
    bool g_Hooked = false;

    DeferRegister_t g_DeferTramp = nullptr;
    uint8_t* g_DeferTarget       = nullptr;
    uint8_t g_DeferOriginal[kPatchSize]{};
    bool g_DeferHooked = false;

    // ---- settings -----------------------------------------------------------------------
    std::atomic<bool> g_Enabled{true};
    std::atomic<bool> g_Self{true};
    std::atomic<bool> g_Others{true};

    // The self entity, refreshed each frame so the detour can tell self from everyone else
    // without calling into Ashita from the hook.
    std::atomic<void*> g_SelfEntity{nullptr};

    // ---- deferred-resource accounting ----------------------------------------------------
    // The window stays armed from the rebind until the hold completes, because for a remote
    // actor the registrations happen inside the queued presentation task rather than during
    // the loader call. Only our actor's registrations are counted.
    std::atomic<bool> g_DeferWindow{false};
    std::atomic<void*> g_DeferActor{nullptr};
    std::atomic<uint32_t> g_DeferInWindow{0};

    // ---- held looks ---------------------------------------------------------------------
    struct PendingRemoval
    {
        void* Entity;
        uintptr_t Actor;
        void* Model;
        void* Nodes[kMaxSnapshot];
        uint32_t Count;
        // How many replacement nodes must exist before the old look may go. Not known when
        // the hold starts if the work was queued; resolved when the task finishes.
        uint32_t Expected;
        bool TaskDone;
        uint32_t Frames;
    };
    constexpr size_t kMaxPendingRemovals = 4;
    PendingRemoval g_Removals[kMaxPendingRemovals]{};
    // Ceiling on a hold, so a resource that never arrives cannot strand the old look forever.
    constexpr uint32_t kRemoveMaxFrames = 150;

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

    void** ChainRoot(void* actor)
    {
        auto* root = reinterpret_cast<void**>(
            static_cast<uint8_t*>(actor) + kActorModelManager + kModelChainRoot);
        return ::IsBadReadPtr(root, sizeof(void*)) ? nullptr : root;
    }

    /**
     * Resolves a chain head's virtual deleting destructor, or null if any link of the walk is
     * unreadable. Callers treat null as "do not touch this actor".
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
     * Counts the nodes in a chain that are not in a recorded set -- the replacements this swap
     * has bound so far. Old and new nodes are interleaved, because the loader inserts in
     * descriptor order rather than appending, so counting is by identity and never by chain
     * length or position.
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

    /**
     * Removes one exact node from a resource chain and frees it, mirroring FUN_01a69e40:
     * relink the predecessor past it, detach its own successor so only this node dies, then
     * delete it through virtual slot +0x18. A node no longer present is left alone.
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
                auto* next = reinterpret_cast<void**>(static_cast<uint8_t*>(cur) + kChainNext);
                *link      = *next;
                *next      = nullptr;
                FreeChain(cur);
                return;
            }

            if (::IsBadReadPtr(cur, kChainNext + sizeof(void*)))
                return;
            link = reinterpret_cast<void**>(static_cast<uint8_t*>(cur) + kChainNext);
        }
    }

    void DisarmWindow(void* actor)
    {
        if (g_DeferActor.load(std::memory_order_relaxed) == actor)
        {
            g_DeferWindow.store(false, std::memory_order_relaxed);
            g_DeferActor.store(nullptr, std::memory_order_relaxed);
        }
    }

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
            return;
        }

        // No free slot: release now rather than leak the old look permanently.
        DisarmWindow(actor);
        void** root = ChainRoot(actor);
        if (root != nullptr && *root == model)
        {
            auto* resHead = reinterpret_cast<void**>(
                static_cast<uint8_t*>(model) + kModelResources);
            for (uint32_t i = 0; i < count; ++i)
                UnlinkAndFree(resHead, nodes[i]);
        }
    }

    /**
     * Rebind by attaching the new look BEFORE releasing the old one.
     *
     * Measured across a body swap: of nine attached resources, seven carry the same link
     * either side of it. Clearing the whole chain threw all seven away, and because releasing
     * a resource drops its last reference the re-request then missed the loaded registry and
     * had to raise a document request -- a self-inflicted stall of some 720ms.
     *
     * Every check that can refuse runs BEFORE the loader call. Once the loader has run this
     * always reports success, because the caller's fallback would otherwise run the loader a
     * second time and land both sets of deferred callbacks on the same model.
     */
    bool RebindPreservingOld(void* entity, void* actor)
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
        for (void* node = oldHead; node != nullptr;)
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
        // or Flags0 bits 7/8. For anyone else it allocates a task, stores it at +0xa08, and
        // returns having done NOTHING -- the attaches and every registration happen later.
        g_DeferActor.store(actor, std::memory_order_relaxed);
        g_DeferInWindow.store(0, std::memory_order_relaxed);
        g_DeferWindow.store(true, std::memory_order_relaxed);

        g_Loader(actor, nullptr, kSelectorDefault);

        const uint32_t deferred = g_DeferInWindow.load(std::memory_order_relaxed);
        const bool queued       = ReadPtr(actor, kActorDeferredTask) != 0;

        // Past this point, always report success. If the loader rebuilt the model, the old
        // resources went with it and there is nothing left to release.
        if (*root != model)
        {
            DisarmWindow(actor);
            return true;
        }

        // The work ran inline and parked nothing: every replacement is already attached, so
        // release the old look now and the swap completes within this call. Requiring the task
        // pointer to be clear is what makes this safe -- without it this fires for every
        // remote swap before anything has happened, and strips the character bare.
        if (!queued && deferred == 0)
        {
            DisarmWindow(actor);
            for (uint32_t i = 0; i < count; ++i)
                UnlinkAndFree(resHead, snapshot[i]);
            return true;
        }

        // Otherwise hold the old look. Holding it is the whole point: a slot whose resource is
        // not resident has no node at all, so releasing early is releasing into nothing.
        const uint32_t attached = queued ? 0 : CountUnrecorded(*resHead, snapshot, count);
        QueueRemoval(entity, actor, model, snapshot, count, queued ? 0 : attached + deferred);
        return true;
    }

    /**
     * Empties the actor's model chain, leaving the model-list owner in the state a fresh actor
     * has. Only used as the fallback when the chain is not the shape the preserving path
     * expects.
     */
    bool ClearModelChain(void* actor)
    {
        void** root = ChainRoot(actor);
        if (root == nullptr)
            return false;

        void* model = *root;
        if (model == nullptr)
            return true;
        if (ChainDtor(model) == nullptr)
            return false;

        FreeChain(model);
        *root = nullptr;
        return true;
    }

    void __cdecl TeardownDetour(void* entity)
    {
        if (entity == nullptr)
            return;

        const bool isSelf = entity == g_SelfEntity.load(std::memory_order_relaxed);
        const bool act    = g_Enabled.load(std::memory_order_relaxed) &&
            (isSelf ? g_Self.load(std::memory_order_relaxed)
                    : g_Others.load(std::memory_order_relaxed));

        if (act && g_Predicate != nullptr && g_Loader != nullptr)
        {
            const uintptr_t actor = ReadPtr(entity, kEntityActorPointer);
            const uint16_t f4     = Read16(entity, kEntityModelFlags);
            const uint32_t flags0 = Read32(entity, kEntityFlags0);
            const uint8_t type    = *(static_cast<const uint8_t*>(entity) + kEntityType);

            const bool triggered = (f4 != 0) || ((flags0 & 1u) != 0);

            // Only touch an actor that exists and is readable. Anything else -- mounts,
            // costumes, other actor families -- falls through to the stock path untouched.
            uintptr_t actorVtable = 0;
            if (actor != 0 && !::IsBadReadPtr(reinterpret_cast<void*>(actor), sizeof(uintptr_t)))
                actorVtable = ReadPtr(reinterpret_cast<void*>(actor), 0);

            // Mirror the stock condition exactly: same eligibility predicate, same triggers.
            if (actorVtable != 0 && type == 0 && triggered && g_Predicate(entity, nullptr) != 0)
            {
                // Verify the chain is walkable before mutating anything, so a layout surprise
                // falls through to a fully stock teardown with the entity exactly as the stock
                // function expects to find it.
                if (ChainRoot(reinterpret_cast<void*>(actor)) == nullptr)
                {
                    g_Trampoline(entity);
                    return;
                }

                // Consume the triggers exactly as the stock tail does. Deliberately leave
                // +0x130 bit 17 alone: the stock code only clears it on the way to a teardown,
                // and we are not tearing down.
                uint16_t cleared = 0;
                std::memcpy(static_cast<uint8_t*>(entity) + kEntityModelFlags, &cleared,
                    sizeof(cleared));
                const uint32_t newFlags = flags0 & ~1u;
                std::memcpy(static_cast<uint8_t*>(entity) + kEntityFlags0, &newFlags,
                    sizeof(newFlags));

                if (!RebindPreservingOld(entity, reinterpret_cast<void*>(actor)))
                {
                    if (ClearModelChain(reinterpret_cast<void*>(actor)))
                        g_Loader(reinterpret_cast<void*>(actor), nullptr, kSelectorDefault);
                }
                return;
            }
        }

        g_Trampoline(entity);
    }

    /**
     * Accounting only. Every look slot whose resource is not resident parks itself here, so
     * counting the registrations made for the actor being rebound gives the exact number of
     * replacements still to arrive. Outside that window the detour only passes the call on.
     */
    void __fastcall DeferRegisterDetour(void* cell, void* unused, void* callback,
        void* context, void* actor, void* flag)
    {
        if (g_DeferWindow.load(std::memory_order_relaxed) && actor != nullptr &&
            actor == g_DeferActor.load(std::memory_order_relaxed))
        {
            g_DeferInWindow.fetch_add(1, std::memory_order_relaxed);
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
        return 1.0;
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

        auto* pointers           = core->GetPointerManager();
        const uintptr_t teardown = pointers->Add("noblink_teardown", "FFXiMain.dll", kTeardownPattern, 0, 0);
        const uintptr_t loader   = pointers->Add("noblink_loader", "FFXiMain.dll", kLoaderPattern, 0, 0);
        if (teardown == 0 || loader == 0)
        {
            logger->Logf(kLogError, "NoBlink", "Could not find the retail gear-change functions.");
            this->Cleanup();
            return false;
        }
        if (!this->VerifyInModule(teardown) || !this->VerifyInModule(loader))
        {
            this->Cleanup();
            return false;
        }

        // Recover the eligibility predicate from the teardown's own call displacement, so it
        // needs no signature and cannot be matched independently.
        int32_t displacement{};
        std::memcpy(&displacement, reinterpret_cast<const void*>(teardown + kPredicateRel32),
            sizeof(displacement));
        const uintptr_t predicate = teardown + kPredicateNext + displacement;
        if (!this->VerifyInModule(predicate))
        {
            this->Cleanup();
            return false;
        }

        g_Loader    = reinterpret_cast<Loader_t>(loader);
        g_Predicate = reinterpret_cast<Predicate_t>(predicate);

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

        const uintptr_t deferReg = pointers->Add("noblink_deferreg", "FFXiMain.dll", kDeferRegisterPattern, 0, 0);
        if (deferReg == 0 || !this->VerifyInModule(deferReg) ||
            !this->InstallDeferHook(reinterpret_cast<uint8_t*>(deferReg)))
        {
            logger->Logf(kLogError, "NoBlink", "Could not hook the deferred-resource registration.");
            this->RemoveHook();
            this->Cleanup();
            return false;
        }

        logger->Logf(kLogInfo, "NoBlink", "Loaded. /noblink for commands.");
        return true;
    }

    void Release(void) override
    {
        this->RemoveDeferHook();
        this->RemoveHook();
        this->Cleanup();
    }

    bool Direct3DInitialize(IDirect3DDevice8*) override
    {
        // Required when UseDirect3D is set; the base implementation returns false, which makes
        // Ashita reject the plugin.
        return true;
    }

    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override
    {
        // Cache the self entity so the teardown detour can tell self from everyone else
        // without calling into Ashita from inside the hook.
        auto* memory = m_Core->GetMemoryManager();
        if (memory != nullptr)
        {
            auto* party = memory->GetParty();
            auto* ents  = memory->GetEntity();
            if (party != nullptr && ents != nullptr)
                g_SelfEntity.store(ents->GetRawEntity(party->GetMemberTargetIndex(0)),
                    std::memory_order_relaxed);
        }

        this->ServiceRemovals();
    }

    bool HandleCommand(int32_t, const char* command, bool) override
    {
        std::vector<std::string> args;
        if (Ashita::Commands::GetCommandArgs(command, &args) == 0 || args.empty())
            return false;
        if (_stricmp(args[0].c_str(), "/noblink") != 0)
            return false;

        const char* verb = args.size() > 1 ? args[1].c_str() : nullptr;
        const char* val  = args.size() > 2 ? args[2].c_str() : nullptr;

        if (verb == nullptr)
        {
            this->Report("/noblink on|off              -- keep characters visible across gear changes");
            this->Report("/noblink self [on|off]       -- apply it to your own character");
            this->Report("/noblink others [on|off]     -- apply it to everyone else");
            char state[160];
            ::sprintf_s(state, "currently: %s, self %s, others %s",
                g_Enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_Self.load(std::memory_order_relaxed) ? "on" : "off",
                g_Others.load(std::memory_order_relaxed) ? "on" : "off");
            this->Report(state);
            return true;
        }

        if (_stricmp(verb, "on") == 0 || _stricmp(verb, "off") == 0)
        {
            g_Enabled.store(_stricmp(verb, "on") == 0, std::memory_order_relaxed);
            this->ReportSetting("noblink", g_Enabled.load(std::memory_order_relaxed));
            return true;
        }

        if (_stricmp(verb, "self") == 0)
        {
            this->Apply(g_Self, val, "self");
            return true;
        }

        if (_stricmp(verb, "others") == 0)
        {
            this->Apply(g_Others, val, "others");
            return true;
        }

        this->Report("unknown option; /noblink for commands");
        return true;
    }

private:
    /** Sets a flag from an explicit on/off, or toggles it when no value was given. */
    void Apply(std::atomic<bool>& flag, const char* value, const char* label)
    {
        if (value == nullptr)
            flag.store(!flag.load(std::memory_order_relaxed), std::memory_order_relaxed);
        else if (_stricmp(value, "on") == 0)
            flag.store(true, std::memory_order_relaxed);
        else if (_stricmp(value, "off") == 0)
            flag.store(false, std::memory_order_relaxed);
        else
        {
            this->Report("expected on, off, or nothing to toggle");
            return;
        }
        this->ReportSetting(label, flag.load(std::memory_order_relaxed));
    }

    void ReportSetting(const char* label, const bool on)
    {
        char buffer[96];
        ::sprintf_s(buffer, "%s %s", label, on ? "on" : "off");
        this->Report(buffer);
    }

    void Report(const char* text)
    {
        char buffer[256];
        ::sprintf_s(buffer, "[NoBlink] %s", text);
        m_Core->GetChatManager()->Write(0, false, buffer);
    }

    /**
     * Releases a held look once every replacement has landed.
     *
     * Completion has two parts and needs both. actor+0xa08 going null says the queued
     * presentation task has run -- necessary, because until then nothing has been attached at
     * all -- but not sufficient, because the task parks whatever was not resident and those
     * arrive later still. So the task pointer only fixes the target, and arrival against that
     * target, counted by node identity, is what releases.
     *
     * This never calls the loader again, so the worst case is holding the old look until the
     * frame ceiling.
     */
    void ServiceRemovals(void)
    {
        for (auto& slot : g_Removals)
        {
            if (slot.Entity == nullptr)
                continue;

            slot.Frames += 1;

            // Abandon if the actor was replaced or the model rebuilt underneath us: the stock
            // path took over and the recorded nodes died with it.
            const uintptr_t actor = ReadPtr(slot.Entity, kEntityActorPointer);
            if (actor == 0 || actor != slot.Actor)
            {
                this->AbandonHold(slot);
                continue;
            }

            void** root = ChainRoot(reinterpret_cast<void*>(actor));
            if (root == nullptr || *root != slot.Model)
            {
                this->AbandonHold(slot);
                continue;
            }

            auto* resHead = reinterpret_cast<void**>(
                static_cast<uint8_t*>(slot.Model) + kModelResources);

            const uint32_t arrived = CountUnrecorded(*resHead, slot.Nodes, slot.Count);

            if (!slot.TaskDone)
            {
                if (ReadPtr(reinterpret_cast<void*>(actor), kActorDeferredTask) != 0)
                {
                    if (slot.Frames < kRemoveMaxFrames)
                        continue;
                }
                else
                {
                    // The task has run: `arrived` is what it bound synchronously and the
                    // registration count is what it parked, so their sum is the target.
                    slot.TaskDone = true;
                    slot.Expected = arrived + g_DeferInWindow.load(std::memory_order_relaxed);
                }
            }

            const bool settled = slot.TaskDone && arrived >= slot.Expected;
            if (!settled && slot.Frames < kRemoveMaxFrames)
                continue;

            DisarmWindow(reinterpret_cast<void*>(actor));
            for (uint32_t i = 0; i < slot.Count; ++i)
                UnlinkAndFree(resHead, slot.Nodes[i]);
            slot.Entity = nullptr;
        }
    }

    /** Drops a hold without releasing anything, and disarms its registration window. */
    void AbandonHold(PendingRemoval& slot)
    {
        DisarmWindow(reinterpret_cast<void*>(slot.Actor));
        slot.Entity = nullptr;
    }

    bool VerifyInModule(const uintptr_t address)
    {
        const HMODULE module = ::GetModuleHandleA("FFXiMain.dll");
        if (module == nullptr)
            return false;

        MODULEINFO info{};
        if (!::GetModuleInformation(::GetCurrentProcess(), module, &info, sizeof(info)))
            return false;

        const uintptr_t base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        const bool inside    = address >= base && address < base + info.SizeOfImage;
        if (!inside)
            m_Logger->Logf(kLogError, "NoBlink",
                "Resolved %p outside FFXiMain.dll. Refusing to hook.",
                reinterpret_cast<void*>(address));
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
        const int32_t backRel  = static_cast<int32_t>(
            (target + kPatchSize) - (trampoline + kPatchSize + 5));
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
     * Hooks FUN_01ab1020. The six stolen bytes are four whole instructions with no
     * position-dependent operand, so the trampoline is a straight copy followed by a jump back
     * past them; ECX still carries the handle cell the rest of the function reads.
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

    void RemoveHook(void)
    {
        if (!g_Hooked)
            return;

        WriteCode(g_HookTarget, g_OriginalBytes, kPatchSize);
        g_Hooked = false;
        ::Sleep(50);

        if (g_Trampoline != nullptr)
        {
            ::VirtualFree(reinterpret_cast<void*>(g_Trampoline), 0, MEM_RELEASE);
            g_Trampoline = nullptr;
        }
        g_HookTarget = nullptr;
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

    void Cleanup(void)
    {
        if (m_Core == nullptr)
            return;
        auto* pointers = m_Core->GetPointerManager();
        pointers->Delete("noblink_teardown");
        pointers->Delete("noblink_loader");
        pointers->Delete("noblink_deferreg");
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
