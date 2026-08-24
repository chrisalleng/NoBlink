// SPDX-License-Identifier: GPL-3.0-only

/**
 * NoBlink - keeps a character visible across a visible-equipment change.
 *
 * The client has no in-place gear-change path: its only load path is the actor constructor,
 * so a change means destroying the whole presentation and rebuilding it. That rebuild is the
 * blink, and it is why anyone targeting the character loses their target.
 *
 * NoBlink hooks the teardown and rebinds on the live actor instead. Two things make that work,
 * and both are easy to get subtly wrong:
 *
 *   Ordering. The loader only ever APPENDS, so the old look must be released or the character
 *   wears both garments. But most resources are shared across a swap and releasing one drops
 *   its last reference, so releasing first evicts resources that are about to be re-requested.
 *   New look first, then unlink each old node.
 *
 *   Completion. For anyone but the current/self actor, FUN_01b141f0 does not do the work at
 *   all -- it queues a task at actor+0xa08 and returns. When that task runs, any slot whose
 *   resource is not resident attaches NOTHING; it parks a callback and moves on. So the old
 *   look must be held until the task has run AND everything it parked has arrived. Releasing
 *   on either condition alone puts the character on screen without a body part.
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

    // Entity
    constexpr size_t kEntityActor      = 0xA0;
    constexpr size_t kEntityType       = 0xEE;   // 0 = PC
    constexpr size_t kEntityModelFlags = 0xF4;
    constexpr size_t kEntityFlags0     = 0x120;

    // Actor. The model-list owner is built in place by FUN_01a6b3e0; FUN_01a6b480 empties a
    // chain with a single virtual +0x18(1) on its root, and each object's destructor walks its
    // own +0x4 successors. The resource chain of a model uses the same successor link.
    constexpr size_t kActorModelOwner  = 0x674;
    constexpr size_t kModelChainRoot   = 0x44;
    constexpr size_t kDeletingDtorSlot = 0x18;
    constexpr size_t kChainNext        = 0x04;
    constexpr size_t kModelResources   = 0x20;
    constexpr size_t kActorTask        = 0xA08;   // queued presentation task, null when done

    // Walk bounds. Real chains are 9-11 nodes; these only stop a corrupt list.
    constexpr uint32_t kMaxNodes = 64;
    constexpr uint32_t kMaxWalk  = 192;

    // Ceiling on a hold, so a resource that never arrives cannot strand the old look.
    constexpr uint32_t kHoldMaxFrames = 150;

    constexpr int32_t kSelector = -1;   // re-read the entity's current packed look slots

    // Signatures against a retail FFXiMain.dll rebuilt at base 0x1a40000, sha256
    // eae30ddd8830067c684d5f08cf33a5406184e6ba488947ecb62f839c88647b1d. Each occurs exactly
    // once there; relative call displacements are wildcarded.
    constexpr char kTeardownSig[] =            // FUN_01ad5d00, the gear-change teardown
        "568B7424088BCEE8????????84C074426683BEF4000000007509F686200100"
        "0001742F8B86300100008BCE25FFFFFDFF";
    constexpr char kLoaderSig[] =              // FUN_01b141f0, the repeatable presentation load
        "53568BF132DB8B467085C074198B80200100008BC8C1E907F6C1017507C1E8"
        "08A8017402B3018B168BCEFF920403000084C0";
    constexpr char kParkSig[] =                // FUN_01ab1020, parks a non-resident resource
        "53566A006A0468840000008BD9E8????????8BF083C40C";

    // Bytes each hook steals: whole instructions, none position-dependent, so each trampoline
    // is a straight copy plus a jump back.
    constexpr uint8_t kTeardownEntry[] = {0x56, 0x8B, 0x74, 0x24, 0x08};
    constexpr uint8_t kParkEntry[]     = {0x53, 0x56, 0x6A, 0x00, 0x6A, 0x04};
    constexpr size_t kJmpSize          = 5;

    // The eligibility predicate FUN_01ad5780 is recovered from the teardown's own call
    // displacement rather than matched separately.
    constexpr size_t kPredicateRel  = 0x08;
    constexpr size_t kPredicateNext = 0x0C;

    using Teardown_t     = void(__cdecl*)(void* entity);
    using Predicate_t    = uint8_t(__fastcall*)(void* entity, void* unused);
    using DeletingDtor_t = void*(__thiscall*)(void* self, int flags);
    // __thiscall expressed for MSVC: ecx = this, edx unused, remaining args on the stack.
    // Callee cleans in both conventions, so these are ABI-compatible.
    using Loader_t = uint32_t(__fastcall*)(void* actor, void* unused, int32_t selector);
    using Park_t   = void(__fastcall*)(void* cell, void* unused, void* callback, void* context,
        void* actor, void* flag);

    Predicate_t g_Predicate = nullptr;
    Loader_t g_Loader       = nullptr;
    Teardown_t g_Teardown   = nullptr;   // trampoline to the stock teardown
    Park_t g_Park           = nullptr;   // trampoline to the stock park

    std::atomic<bool> g_Enabled{true};
    std::atomic<bool> g_Self{true};
    std::atomic<bool> g_Others{true};

    // Refreshed each frame so the teardown hook can tell self from everyone else without
    // calling into Ashita from inside the hook.
    std::atomic<void*> g_SelfEntity{nullptr};

    /**
     * An old look kept attached until its replacement is complete.
     *
     * Parked is per-hold, not global: several characters can be mid-swap at once, and a shared
     * counter would let one swap's parked resources decide another's release.
     */
    struct Hold
    {
        void* Entity;
        void* Actor;
        void* Model;
        void* Nodes[kMaxNodes];
        uint32_t Count;
        uint32_t Parked;     // resources this swap deferred, counted by the park hook
        uint32_t Expected;    // replacement nodes required; resolved when the task finishes
        bool TaskDone;
        uint32_t Frames;
    };
    Hold g_Holds[4]{};

    // Set only for the duration of a synchronous loader call, which is where a self swap does
    // all of its parking. Remote swaps park later, from the task, and are attributed by actor.
    void* g_ArmedActor    = nullptr;
    uint32_t g_ArmedParks = 0;

    template <typename T>
    T Read(const void* base, const size_t offset)
    {
        T v{};
        std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(v));
        return v;
    }

    void** ChainRoot(void* actor)
    {
        auto* root = reinterpret_cast<void**>(
            static_cast<uint8_t*>(actor) + kActorModelOwner + kModelChainRoot);
        return ::IsBadReadPtr(root, sizeof(void*)) ? nullptr : root;
    }

    void** ResourceHead(void* model)
    {
        return reinterpret_cast<void**>(static_cast<uint8_t*>(model) + kModelResources);
    }

    void** NextLink(void* node)
    {
        return reinterpret_cast<void**>(static_cast<uint8_t*>(node) + kChainNext);
    }

    /** A chain head's virtual deleting destructor, or null if any link of the walk is bad. */
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
     * Records every node currently attached, refusing on anything unreadable. Runs before any
     * mutation, so a layout surprise can still fall through to a fully stock teardown.
     */
    bool Snapshot(void* model, void* out[kMaxNodes], uint32_t& count)
    {
        count = 0;
        for (void* node = *ResourceHead(model); node != nullptr; node = *NextLink(node))
        {
            if (count >= kMaxNodes || ::IsBadReadPtr(node, kChainNext + sizeof(void*)))
                return false;
            if (ChainDtor(node) == nullptr)
                return false;
            out[count++] = node;
        }
        return true;
    }

    /**
     * Nodes present that are not in the recorded set: the replacements bound so far. New nodes
     * are inserted in descriptor order rather than appended, so this counts by identity and
     * never by length or position.
     */
    uint32_t CountFresh(void* head, void* const* recorded, const uint32_t recordedCount)
    {
        uint32_t fresh = 0;
        void* node     = head;
        for (uint32_t guard = 0; node != nullptr && guard < kMaxWalk; ++guard)
        {
            if (::IsBadReadPtr(node, kChainNext + sizeof(void*)))
                break;

            bool known = false;
            for (uint32_t i = 0; i < recordedCount && !known; ++i)
                known = recorded[i] == node;
            if (!known)
                ++fresh;

            node = *NextLink(node);
        }
        return fresh;
    }

    /**
     * Unlinks one exact node and frees it, as FUN_01a69e40 does: relink the predecessor past
     * it, detach its successor so only this node dies, then delete through +0x18. A node no
     * longer present is left alone.
     */
    void UnlinkAndFree(void** head, void* target)
    {
        void** link = head;
        for (uint32_t guard = 0; guard < kMaxWalk; ++guard)
        {
            if (::IsBadReadPtr(link, sizeof(void*)))
                return;
            void* cur = *link;
            if (cur == nullptr || ::IsBadReadPtr(cur, kChainNext + sizeof(void*)))
                return;

            if (cur == target)
            {
                auto dtor = ChainDtor(cur);
                if (dtor == nullptr)
                    return;
                *link            = *NextLink(cur);
                *NextLink(cur)   = nullptr;
                dtor(cur, 1);
                return;
            }
            link = NextLink(cur);
        }
    }

    void ReleaseNodes(void* model, void* const* nodes, const uint32_t count)
    {
        void** head = ResourceHead(model);
        for (uint32_t i = 0; i < count; ++i)
            UnlinkAndFree(head, nodes[i]);
    }

    Hold* FindHold(void* actor)
    {
        for (auto& hold : g_Holds)
        {
            if (hold.Entity != nullptr && hold.Actor == actor)
                return &hold;
        }
        return nullptr;
    }

    void BeginHold(void* entity, void* actor, void* model, void* const* nodes,
        const uint32_t count, const uint32_t parked, const uint32_t expected)
    {
        for (auto& hold : g_Holds)
        {
            if (hold.Entity != nullptr)
                continue;

            hold.Entity   = entity;
            hold.Actor    = actor;
            hold.Model    = model;
            hold.Count    = count;
            hold.Parked   = parked;
            hold.Expected = expected;
            hold.TaskDone = expected != 0;
            hold.Frames   = 0;
            std::memcpy(hold.Nodes, nodes, count * sizeof(void*));
            return;
        }

        // Every slot busy: release now rather than leak the old look permanently.
        ReleaseNodes(model, nodes, count);
    }

    /**
     * Runs the presentation load on the live actor, then either releases the old look or holds
     * it until the replacement is complete. The caller has already validated the chain and
     * consumed the triggers, so this cannot refuse.
     */
    void Rebind(void* entity, void* actor, void* model, void* const* nodes, const uint32_t count)
    {
        void** root = ChainRoot(actor);

        g_ArmedActor = actor;
        g_ArmedParks = 0;
        g_Loader(actor, nullptr, kSelector);
        const uint32_t parked = g_ArmedParks;
        g_ArmedActor          = nullptr;

        if (root == nullptr || *root != model)
            return;   // loader rebuilt the model; the old nodes went with it

        // A queued task means nothing has happened yet, so there is nothing to compare against
        // and no basis on which to release. Without this check the branch below fires on every
        // remote swap and strips the character bare for a frame.
        if (Read<void*>(actor, kActorTask) != nullptr)
        {
            BeginHold(entity, actor, model, nodes, count, parked, 0);
            return;
        }

        // Ran inline, so the target is known now: what it attached plus what it parked.
        const uint32_t attached = CountFresh(*ResourceHead(model), nodes, count);
        if (parked == 0)
            ReleaseNodes(model, nodes, count);   // complete already, no doubled window at all
        else
            BeginHold(entity, actor, model, nodes, count, parked, attached + parked);
    }

    /** True if the teardown was replaced with an in-place rebind. */
    bool Suppress(void* entity)
    {
        const bool isSelf = entity == g_SelfEntity.load(std::memory_order_relaxed);
        if (!g_Enabled.load(std::memory_order_relaxed))
            return false;
        if (!(isSelf ? g_Self : g_Others).load(std::memory_order_relaxed))
            return false;
        if (g_Predicate == nullptr || g_Loader == nullptr)
            return false;

        void* actor = Read<void*>(entity, kEntityActor);
        if (actor == nullptr || ::IsBadReadPtr(actor, sizeof(void*)))
            return false;
        if (Read<uint8_t>(entity, kEntityType) != 0)
            return false;   // PCs only; mounts and other actor families keep the stock path

        // Mirror the stock trigger condition exactly.
        const uint16_t modelFlags = Read<uint16_t>(entity, kEntityModelFlags);
        const uint32_t flags0     = Read<uint32_t>(entity, kEntityFlags0);
        if (modelFlags == 0 && (flags0 & 1u) == 0)
            return false;
        if (g_Predicate(entity, nullptr) == 0)
            return false;

        // Record the old look before touching anything, so an unrecognised layout can still
        // fall through to a fully stock teardown with the entity exactly as it expects.
        void** root = ChainRoot(actor);
        if (root == nullptr || *root == nullptr ||
            ::IsBadReadPtr(*root, kModelResources + sizeof(void*)))
            return false;

        void* model = *root;
        void* nodes[kMaxNodes];
        uint32_t count = 0;
        if (!Snapshot(model, nodes, count))
            return false;

        // Consume the triggers as the stock tail does. Deliberately leave +0x130 bit 17 alone:
        // the stock code only clears it on the way to a teardown, and we are not tearing down.
        const uint16_t cleared  = 0;
        const uint32_t newFlags = flags0 & ~1u;
        std::memcpy(static_cast<uint8_t*>(entity) + kEntityModelFlags, &cleared, sizeof(cleared));
        std::memcpy(static_cast<uint8_t*>(entity) + kEntityFlags0, &newFlags, sizeof(newFlags));

        if (count == 0)
            g_Loader(actor, nullptr, kSelector);   // nothing to preserve
        else
            Rebind(entity, actor, model, nodes, count);
        return true;
    }

    void __cdecl TeardownDetour(void* entity)
    {
        if (entity == nullptr)
            return;
        if (!Suppress(entity))
            g_Teardown(entity);
    }

    /**
     * Accounting only. Every slot whose resource is not resident parks itself here, so counting
     * the calls made for an actor gives exactly how many replacements are still to arrive.
     */
    void __fastcall ParkDetour(void* cell, void* unused, void* callback, void* context,
        void* actor, void* flag)
    {
        if (actor != nullptr)
        {
            if (actor == g_ArmedActor)
                ++g_ArmedParks;
            else if (Hold* hold = FindHold(actor))
                ++hold->Parked;
        }

        g_Park(cell, unused, callback, context, actor, flag);
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

    /** A 5-byte jump patch with a trampoline that replays the stolen bytes. */
    class CodeHook
    {
        uint8_t* m_Target = nullptr;
        uint8_t* m_Tramp  = nullptr;
        uint8_t m_Original[kJmpSize]{};
        size_t m_Stolen = 0;

    public:
        bool Install(uint8_t* target, const void* detour, const uint8_t* entry,
            const size_t stolen)
        {
            if (target == nullptr || std::memcmp(target, entry, stolen) != 0)
                return false;

            m_Tramp = static_cast<uint8_t*>(
                ::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (m_Tramp == nullptr)
                return false;

            std::memcpy(m_Tramp, target, stolen);
            m_Tramp[stolen]       = 0xE9;
            const int32_t backRel = static_cast<int32_t>(
                (target + stolen) - (m_Tramp + stolen + kJmpSize));
            std::memcpy(m_Tramp + stolen + 1, &backRel, sizeof(backRel));

            std::memcpy(m_Original, target, kJmpSize);

            uint8_t patch[kJmpSize];
            patch[0]              = 0xE9;
            const int32_t hookRel = static_cast<int32_t>(
                static_cast<const uint8_t*>(detour) - (target + kJmpSize));
            std::memcpy(patch + 1, &hookRel, sizeof(hookRel));

            if (!WriteCode(target, patch, kJmpSize))
            {
                ::VirtualFree(m_Tramp, 0, MEM_RELEASE);
                m_Tramp = nullptr;
                return false;
            }

            m_Target = target;
            m_Stolen = stolen;
            return true;
        }

        void* Trampoline(void) const
        {
            return m_Tramp;
        }

        void Remove(void)
        {
            if (m_Target == nullptr)
                return;

            WriteCode(m_Target, m_Original, kJmpSize);
            m_Target = nullptr;
            ::Sleep(50);   // let any thread inside the trampoline leave before freeing it

            ::VirtualFree(m_Tramp, 0, MEM_RELEASE);
            m_Tramp = nullptr;
        }
    };

    CodeHook g_TeardownHook;
    CodeHook g_ParkHook;
}

class NoBlink final : public IPlugin
{
    IAshitaCore* m_Core   = nullptr;
    ILogManager* m_Logger = nullptr;

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

    bool Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t) override
    {
        m_Core   = core;
        m_Logger = logger;

        const uintptr_t teardown = this->Resolve("noblink_teardown", kTeardownSig);
        const uintptr_t loader   = this->Resolve("noblink_loader", kLoaderSig);
        const uintptr_t park     = this->Resolve("noblink_park", kParkSig);
        if (teardown == 0 || loader == 0 || park == 0)
        {
            logger->Logf(kLogError, "NoBlink",
                "Could not locate the retail gear-change functions; this client build is not supported.");
            this->Cleanup();
            return false;
        }

        const int32_t rel          = Read<int32_t>(reinterpret_cast<void*>(teardown), kPredicateRel);
        const uintptr_t predicate = teardown + kPredicateNext + rel;
        if (!this->InModule(predicate))
        {
            this->Cleanup();
            return false;
        }

        g_Loader    = reinterpret_cast<Loader_t>(loader);
        g_Predicate = reinterpret_cast<Predicate_t>(predicate);

        if (!g_TeardownHook.Install(reinterpret_cast<uint8_t*>(teardown), &TeardownDetour,
                kTeardownEntry, sizeof(kTeardownEntry)) ||
            !g_ParkHook.Install(reinterpret_cast<uint8_t*>(park), &ParkDetour, kParkEntry,
                sizeof(kParkEntry)))
        {
            logger->Logf(kLogError, "NoBlink", "Unexpected code at a hook site; refusing to patch.");
            this->Release();
            return false;
        }

        g_Teardown = reinterpret_cast<Teardown_t>(g_TeardownHook.Trampoline());
        g_Park     = reinterpret_cast<Park_t>(g_ParkHook.Trampoline());

        logger->Logf(kLogInfo, "NoBlink", "Loaded. /noblink for commands.");
        return true;
    }

    void Release(void) override
    {
        g_ParkHook.Remove();
        g_TeardownHook.Remove();
        this->Cleanup();
    }

    bool Direct3DInitialize(IDirect3DDevice8*) override
    {
        // Required when UseDirect3D is set; the base returns false, which fails the load.
        return true;
    }

    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override
    {
        auto* memory = m_Core->GetMemoryManager();
        auto* party  = memory ? memory->GetParty() : nullptr;
        auto* ents   = memory ? memory->GetEntity() : nullptr;
        if (party != nullptr && ents != nullptr)
            g_SelfEntity.store(ents->GetRawEntity(party->GetMemberTargetIndex(0)),
                std::memory_order_relaxed);

        this->ServiceHolds();
    }

    bool HandleCommand(int32_t, const char* command, bool) override
    {
        std::vector<std::string> args;
        if (Ashita::Commands::GetCommandArgs(command, &args) == 0 || args.empty())
            return false;
        if (_stricmp(args[0].c_str(), "/noblink") != 0)
            return false;

        const char* verb  = args.size() > 1 ? args[1].c_str() : nullptr;
        const char* value = args.size() > 2 ? args[2].c_str() : nullptr;

        if (verb == nullptr)
        {
            this->Report("/noblink on/off             -- keep characters visible across gear changes");
            this->Report("/noblink self [on/off]      -- apply it to your own character");
            this->Report("/noblink others [on/off]    -- apply it to everyone else");
            this->ReportState();
        }
        else if (_stricmp(verb, "on") == 0 || _stricmp(verb, "off") == 0)
        {
            g_Enabled.store(_stricmp(verb, "on") == 0, std::memory_order_relaxed);
            this->ReportState();
        }
        else if (_stricmp(verb, "self") == 0)
        {
            this->Set(g_Self, value);
        }
        else if (_stricmp(verb, "others") == 0)
        {
            this->Set(g_Others, value);
        }
        else
        {
            this->Report("unknown option; /noblink for commands");
        }
        return true;
    }

private:
    /** Sets a flag from an explicit on/off, or toggles it when no value was given. */
    void Set(std::atomic<bool>& flag, const char* value)
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
        this->ReportState();
    }

    void ReportState(void)
    {
        char buffer[128];
        ::sprintf_s(buffer, "%s -- self %s, others %s",
            g_Enabled.load(std::memory_order_relaxed) ? "on" : "off",
            g_Self.load(std::memory_order_relaxed) ? "on" : "off",
            g_Others.load(std::memory_order_relaxed) ? "on" : "off");
        this->Report(buffer);
    }

    void Report(const char* text)
    {
        char buffer[256];
        ::sprintf_s(buffer, "[NoBlink] %s", text);
        m_Core->GetChatManager()->Write(0, false, buffer);
    }

    /**
     * Releases each held look once its replacement is complete.
     *
     * The task pointer going null says the queued work has run -- necessary, because until then
     * nothing is attached -- and at that moment what it bound plus what it parked is the target.
     * Arrival against that target, by node identity, is what releases. Either half alone
     * releases too early and shows the character without a body part.
     */
    void ServiceHolds(void)
    {
        for (auto& hold : g_Holds)
        {
            if (hold.Entity == nullptr)
                continue;

            hold.Frames += 1;
            const bool expired = hold.Frames >= kHoldMaxFrames;

            // Abandon if the stock path took over: the recorded nodes died with the old actor
            // or the rebuilt model, and releasing them would be a double free.
            void** root = nullptr;
            if (Read<void*>(hold.Entity, kEntityActor) == hold.Actor)
                root = ChainRoot(hold.Actor);
            if (root == nullptr || *root != hold.Model)
            {
                hold.Entity = nullptr;
                continue;
            }

            const uint32_t arrived = CountFresh(*ResourceHead(hold.Model), hold.Nodes, hold.Count);

            if (!hold.TaskDone)
            {
                if (Read<void*>(hold.Actor, kActorTask) != nullptr)
                {
                    if (!expired)
                        continue;
                }
                else
                {
                    hold.TaskDone = true;
                    hold.Expected = arrived + hold.Parked;
                }
            }

            if (!expired && arrived < hold.Expected)
                continue;

            ReleaseNodes(hold.Model, hold.Nodes, hold.Count);
            hold.Entity = nullptr;
        }
    }

    uintptr_t Resolve(const char* name, const char* signature)
    {
        const uintptr_t address = m_Core->GetPointerManager()->Add(name, "FFXiMain.dll", signature, 0, 0);
        return (address != 0 && this->InModule(address)) ? address : 0;
    }

    bool InModule(const uintptr_t address)
    {
        const HMODULE module = ::GetModuleHandleA("FFXiMain.dll");
        MODULEINFO info{};
        if (module == nullptr ||
            !::GetModuleInformation(::GetCurrentProcess(), module, &info, sizeof(info)))
            return false;

        const uintptr_t base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        if (address >= base && address < base + info.SizeOfImage)
            return true;

        m_Logger->Logf(kLogError, "NoBlink", "Resolved %p outside FFXiMain.dll; refusing to hook.",
            reinterpret_cast<void*>(address));
        return false;
    }

    void Cleanup(void)
    {
        if (m_Core == nullptr)
            return;
        auto* pointers = m_Core->GetPointerManager();
        pointers->Delete("noblink_teardown");
        pointers->Delete("noblink_loader");
        pointers->Delete("noblink_park");
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
