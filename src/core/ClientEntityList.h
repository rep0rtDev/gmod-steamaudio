// src/core/ClientEntityList.h
//
// Native walk of the client's entity list (IClientEntityList from client.dll)
// producing EntitySnapshot records for DynamicOccluders without going through
// Lua.
//
// Everything here depends on vtable layouts of *public* Source SDK 2013
// interfaces (IClientEntityList, IClientUnknown, ICollideable,
// IClientNetworkable, IVModelInfoClient). Garry's Mod ships a modified
// engine, so none of the default slot indices below are known to match the
// live client: they are the SDK 2013 declaration order and are treated as a
// hypothesis. Slots are configurable (config/steamaudio_signatures.json,
// "entity_list"), every slot pointer must point at code in the owning module,
// and the layout is validated in-map against facts that must hold before a
// single record is produced:
//
//   * GetHighestEntityIndex() is within [0, 16384];
//   * entity 0 (the world) resolves, its collideable's model name is
//     "maps/<current map>.bsp";
//   * the local player entity resolves and its ClientClass network name ends
//     with "Player".
//
// When validation fails the walker stays inactive and the Lua fallback
// (steamaudio.PushEntity from ents.GetAll()) provides the snapshot instead.
// Ordinary playback is never affected.
//
// Thread-safety: game thread only.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/DynamicOccluders.h"
#include "util/Json.h"
#include "util/SignatureScanner.h"

namespace sa {

struct EntityListSlots {
    // IClientEntityList (client.dll, "VClientEntityList003")
    int32_t getClientEntity = 3;       // IClientEntity* GetClientEntity(int entnum)
    int32_t getHighestEntityIndex = 6; // int GetHighestEntityIndex()
    // IClientUnknown (primary vtable of the entity; IHandleEntity has dtor +
    // SetRefEHandle + GetRefEHandle first)
    int32_t getCollideable = 3;        // ICollideable* GetCollideable()
    int32_t getClientNetworkable = 4;  // IClientNetworkable* GetClientNetworkable()
    // ICollideable
    int32_t obbMins = 3;               // const Vector& OBBMins()
    int32_t obbMaxs = 4;               // const Vector& OBBMaxs()
    int32_t getCollisionModel = 9;     // const model_t* GetCollisionModel()
    int32_t getCollisionOrigin = 10;   // const Vector& GetCollisionOrigin()
    int32_t getCollisionAngles = 11;   // const QAngle& GetCollisionAngles()
    int32_t getSolid = 13;             // SolidType_t GetSolid()
    // IClientNetworkable
    int32_t getClientClass = 2;        // ClientClass* GetClientClass()
    int32_t isDormant = 8;             // bool IsDormant()
    // ClientClass: pointers m_pCreateFn, m_pCreateEventFn, then char* m_pNetworkName
    int32_t networkNamePointerIndex = 2;
    // IVModelInfoClient (engine.dll): dtor, GetModel, GetModelIndex, GetModelName
    int32_t getModelName = 3;          // const char* GetModelName(const model_t*)

    bool Valid() const;
    void Parse(const JsonValue& node);
};

struct EntityListConfig {
    std::string module = "client.dll";
    std::string interfaceVersion = "VClientEntityList003";
    std::string modelInfoModule = "engine.dll";
    // Tried in order; GMod's engine exports a newer version than SDK 2013.
    std::vector<std::string> modelInfoInterfaces = {"VModelInfoClient006", "VModelInfoClient004"};
    std::vector<std::string> playerClassSuffixes = {"Player"};
    int32_t maxEntities = 16384;
    bool disabled = false;
    EntityListSlots slots;

    void Parse(const JsonValue& node);
    void ParseSlots(const JsonValue& slotsNode);
};

class EntitySnapshotSweep {
public:
    enum class ReadResult { Present, Missing, Fault };

    void Begin(int32_t highest)
    {
        Reset();
        m_highest = std::clamp(highest, 0, 16384);
        m_next = 1;
        m_active = true;
        m_pending.reserve(static_cast<size_t>(m_highest));
    }
    void Reset()
    {
        m_active = false;
        m_next = m_highest = 0;
        m_faults = 0;
        m_pending.clear();
        m_recent.clear();
    }
    template <class Reader, class Continue>
    bool Advance(Reader&& read, Continue&& canContinue, size_t maxEntities, std::vector<EntitySnapshot>& out)
    {
        m_recent.clear();
        if (!m_active)
            return false;
        size_t count = 0;
        while (m_next <= m_highest && count < std::max(size_t(1), maxEntities) &&
               (count == 0 || canContinue())) {
            EntitySnapshot entity;
            const int32_t index = m_next++;
            const ReadResult result = read(index, entity);
            ++count;
            if (result == ReadResult::Fault)
                ++m_faults;
            if (result != ReadResult::Present)
                continue;
            entity.index = index;
            m_pending.push_back(entity);
            m_recent.push_back(std::move(entity));
        }
        if (m_next <= m_highest)
            return false;
        out.swap(m_pending);
        m_pending.clear();
        m_active = false;
        return true;
    }
    bool InProgress() const { return m_active; }
    int32_t NextIndex() const { return m_active ? m_next : 0; }
    int32_t HighestIndex() const { return m_highest; }
    uint64_t Faults() const { return m_faults; }
    const std::vector<EntitySnapshot>& Recent() const { return m_recent; }

private:
    bool m_active = false;
    int32_t m_next = 0;
    int32_t m_highest = 0;
    uint64_t m_faults = 0;
    std::vector<EntitySnapshot> m_pending;
    std::vector<EntitySnapshot> m_recent;
};

class ClientEntityList {
public:
    enum class State { Uninitialized, Unvalidated, Validated, Failed };
    enum class SnapshotResult { Unavailable, Pending, Complete };

    // Resolves the interfaces. Does not touch any entity yet: validation
    // needs a loaded map and happens on the first Snapshot() call.
    bool Initialize(const EntityListConfig& config, std::string& error);
    void Shutdown();

    State GetState() const { return m_state; }
    bool Active() const { return m_state == State::Validated; }
    const std::string& Description() const { return m_description; }
    const std::string& LastError() const { return m_error; }

    // Forgets validation (map change: the world model name changes).
    void Invalidate();

    // Fills `out` with every live entity. Returns false when the walker is not
    // validated (validation is attempted when `mapName` is non-empty and
    // `localPlayer` > 0). `mapName` is the bare map name ("gm_construct").
    bool Snapshot(const std::string& mapName, int32_t localPlayer, std::vector<EntitySnapshot>& out);
    SnapshotResult PollSnapshot(const std::string& mapName, int32_t localPlayer,
                                std::vector<EntitySnapshot>& out, float budgetMs);
    bool SnapshotInProgress() const { return m_sweep.InProgress(); }
    const std::vector<EntitySnapshot>& SnapshotUpdates() const { return m_sweep.Recent(); }
    void CancelSnapshot();

    struct Stats {
        uint64_t snapshots = 0;
        uint64_t entitiesVisited = 0;
        uint64_t entitiesEmitted = 0;
        uint64_t readFailures = 0; // guarded reads that faulted/returned garbage
        int32_t lastHighestIndex = 0;
        uint64_t scanSlices = 0;
        uint32_t lastSnapshotSlices = 0;
        uint32_t lastSnapshotMicros = 0;
        int32_t nextIndex = 0;
        bool scanPending = false;
    };
    const Stats& GetStats() const { return m_stats; }

private:
    bool Validate(const std::string& mapName, int32_t localPlayer, std::string& why);
    bool SlotsPlausible(std::string& why) const;

    EntityListConfig m_config;
    State m_state = State::Uninitialized;
    std::string m_description;
    std::string m_error;
    void* m_entityList = nullptr;
    void* m_modelInfo = nullptr;
    std::optional<ModuleImage> m_client;
    std::optional<ModuleImage> m_engine;
    Stats m_stats;
    bool m_loggedFailure = false;
    EntitySnapshotSweep m_sweep;
    std::string m_snapshotMap;
    std::chrono::steady_clock::time_point m_snapshotStart{};
    uint32_t m_snapshotSlices = 0;
};

} // namespace sa
