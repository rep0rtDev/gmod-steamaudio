// src/core/ClientEntityList.cpp
#include "core/ClientEntityList.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "core/EngineInterfaces.h"
#include "core/SourceInterfaces.h"
#include "util/Logging.h"
#include "util/SignatureScanner.h"

namespace sa {

namespace {

constexpr size_t kMaxModelName = 260;
constexpr size_t kMaxClassName = 128;
constexpr int32_t kMaxSlot = 256;

// Plain-data view of one entity, filled by the guarded reader.
struct RawEntity {
    float origin[3] = {};
    float angles[3] = {};
    float mins[3] = {};
    float maxs[3] = {};
    int32_t solid = 0;
    uint8_t dormant = 0;
    uint8_t haveBounds = 0;
    char model[kMaxModelName] = {};
    char networkName[kMaxClassName] = {};
};

struct ReaderContext {
    void* entityList = nullptr;
    void* modelInfo = nullptr;
    const EntityListSlots* slots = nullptr;
    const ModuleImage* client = nullptr;
    const ModuleImage* engine = nullptr;
    bool skipDormantGeometry = false;
};

enum ReadResult : int32_t { kReadOk = 0, kReadNull = 1, kReadFault = 2 };

// True when `object` is a readable pointer whose vtable slot `slot` points at
// code inside `image`.
bool SlotIsCode(const void* object, int32_t slot, const ModuleImage& image)
{
    return HasCodeVTableSlots(object, image, {slot});
}

bool ReadVec3(const void* ptr, float out[3])
{
    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (!addr || !IsMemoryReadable(addr, sizeof(float) * 3))
        return false;
    std::memcpy(out, ptr, sizeof(float) * 3);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) && std::fabs(out[0]) < 1e6f &&
           std::fabs(out[1]) < 1e6f && std::fabs(out[2]) < 1e6f;
}

// Copies a NUL-terminated string page-safely. Returns false when `src` is not
// readable or has no terminator within `cap` bytes.
bool CopyString(const char* src, char* dst, size_t cap)
{
    dst[0] = 0;
    const auto addr = reinterpret_cast<uintptr_t>(src);
    if (!addr)
        return false;
    for (size_t i = 0; i + 1 < cap; ++i) {
        if ((i == 0 || ((addr + i) & 0xFFF) == 0) && !IsMemoryReadable(addr + i, 1))
            return false;
        dst[i] = src[i];
        if (dst[i] == 0)
            return true;
    }
    dst[cap - 1] = 0;
    return false;
}

// Reads entity `index`. No C++ objects with destructors in here: on MSVC the
// caller wraps this in __try.
int32_t ReadEntityUnguarded(const ReaderContext& ctx, int32_t index, RawEntity& out)
{
    const EntityListSlots& s = *ctx.slots;
    void* entity = CallVirtual<void*>(ctx.entityList, s.getClientEntity, index);
    if (!entity)
        return kReadNull;
    if (!HasCodeVTableSlots(entity, *ctx.client, {s.getCollideable, s.getClientNetworkable}))
        return kReadFault;

    void* networkable = CallVirtual<void*>(entity, s.getClientNetworkable);
    if (networkable) {
        if (!HasCodeVTableSlots(networkable, *ctx.client, {s.isDormant, s.getClientClass}))
            return kReadFault;
        out.dormant = CallVirtual<bool>(networkable, s.isDormant) ? 1 : 0;
        if (out.dormant && ctx.skipDormantGeometry)
            return kReadOk;
        const void* clientClass = CallVirtual<const void*>(networkable, s.getClientClass);
        if (clientClass) {
            const auto ccAddr = reinterpret_cast<uintptr_t>(clientClass);
            const size_t nameOffset = static_cast<size_t>(s.networkNamePointerIndex) * sizeof(void*);
            if (IsMemoryReadable(ccAddr, nameOffset + sizeof(void*))) {
                const char* name = *reinterpret_cast<const char* const*>(ccAddr + nameOffset);
                CopyString(name, out.networkName, sizeof(out.networkName));
            }
        }
    }

    void* collideable = CallVirtual<void*>(entity, s.getCollideable);
    if (!collideable)
        return kReadNull;
    if (!HasCodeVTableSlots(collideable, *ctx.client, {s.getCollisionOrigin, s.getCollisionAngles,
                                                     s.getCollisionModel, s.getSolid, s.obbMins, s.obbMaxs}))
        return kReadFault;

    const float* origin = CallVirtual<const float*>(collideable, s.getCollisionOrigin);
    const float* angles = CallVirtual<const float*>(collideable, s.getCollisionAngles);
    if (!ReadVec3(origin, out.origin) || !ReadVec3(angles, out.angles))
        return kReadFault;
    const float* mins = CallVirtual<const float*>(collideable, s.obbMins);
    const float* maxs = CallVirtual<const float*>(collideable, s.obbMaxs);
    out.haveBounds = (ReadVec3(mins, out.mins) && ReadVec3(maxs, out.maxs)) ? 1 : 0;
    out.solid = CallVirtual<int32_t>(collideable, s.getSolid);
    if (out.solid < 0 || out.solid > 16)
        return kReadFault;

    const void* model = CallVirtual<const void*>(collideable, s.getCollisionModel);
    out.model[0] = 0;
    if (model && ctx.modelInfo) {
        const char* name = CallVirtual<const char*>(ctx.modelInfo, s.getModelName, model);
        if (name && !CopyString(name, out.model, sizeof(out.model)))
            return kReadFault;
    }
    return kReadOk;
}

int32_t ReadEntity(const ReaderContext& ctx, int32_t index, RawEntity& out)
{
#if defined(_MSC_VER)
    __try {
        return ReadEntityUnguarded(ctx, index, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kReadFault;
    }
#else
    return ReadEntityUnguarded(ctx, index, out);
#endif
}

int32_t ReadHighestIndexUnguarded(const ReaderContext& ctx)
{
    return CallVirtual<int32_t>(ctx.entityList, ctx.slots->getHighestEntityIndex);
}

int32_t ReadHighestIndex(const ReaderContext& ctx)
{
#if defined(_MSC_VER)
    __try {
        return ReadHighestIndexUnguarded(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#else
    return ReadHighestIndexUnguarded(ctx);
#endif
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
bool EntityListSlots::Valid() const
{
    const int32_t all[] = {getClientEntity, getHighestEntityIndex, getCollideable, getClientNetworkable, obbMins,
                           obbMaxs, getCollisionModel, getCollisionOrigin, getCollisionAngles, getSolid,
                           getClientClass, isDormant, getModelName};
    for (int32_t v : all)
        if (v < 0 || v >= kMaxSlot)
            return false;
    return networkNamePointerIndex >= 0 && networkNamePointerIndex < 16;
}

void EntityListSlots::Parse(const JsonValue& s)
{
    if (!s.IsObject())
        return;
    getClientEntity = s["get_client_entity"].AsInt(getClientEntity);
    getHighestEntityIndex = s["get_highest_entity_index"].AsInt(getHighestEntityIndex);
    getCollideable = s["get_collideable"].AsInt(getCollideable);
    getClientNetworkable = s["get_client_networkable"].AsInt(getClientNetworkable);
    obbMins = s["obb_mins"].AsInt(obbMins);
    obbMaxs = s["obb_maxs"].AsInt(obbMaxs);
    getCollisionModel = s["get_collision_model"].AsInt(getCollisionModel);
    getCollisionOrigin = s["get_collision_origin"].AsInt(getCollisionOrigin);
    getCollisionAngles = s["get_collision_angles"].AsInt(getCollisionAngles);
    getSolid = s["get_solid"].AsInt(getSolid);
    getClientClass = s["get_client_class"].AsInt(getClientClass);
    isDormant = s["is_dormant"].AsInt(isDormant);
    networkNamePointerIndex = s["network_name_pointer_index"].AsInt(networkNamePointerIndex);
    getModelName = s["get_model_name"].AsInt(getModelName);
}

void EntityListConfig::Parse(const JsonValue& node)
{
    if (!node.IsObject())
        return;
    module = node["module"].AsString(module);
    interfaceVersion = node["interface"].AsString(interfaceVersion);
    modelInfoModule = node["modelinfo_module"].AsString(modelInfoModule);
    if (node["modelinfo_interfaces"].IsArray()) {
        modelInfoInterfaces.clear();
        for (const JsonValue& v : node["modelinfo_interfaces"].AsArray())
            if (v.IsString())
                modelInfoInterfaces.push_back(v.AsString());
    }
    if (node["player_class_suffixes"].IsArray()) {
        playerClassSuffixes.clear();
        for (const JsonValue& v : node["player_class_suffixes"].AsArray())
            if (v.IsString())
                playerClassSuffixes.push_back(v.AsString());
    }
    maxEntities = std::clamp(node["max_entities"].AsInt(maxEntities), 64, 65536);
    disabled = node["disabled"].AsBool(disabled);
}

void EntityListConfig::ParseSlots(const JsonValue& slotsNode)
{
    slots.Parse(slotsNode);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
bool ClientEntityList::Initialize(const EntityListConfig& config, std::string& error)
{
    Shutdown();
    m_config = config;
    if (config.disabled) {
        error = "disabled by configuration";
        m_state = State::Failed;
        return false;
    }
    if (!config.slots.Valid()) {
        error = "invalid slot configuration";
        m_state = State::Failed;
        return false;
    }
#ifdef _WIN32
    HMODULE client = GetModuleHandleA(config.module.c_str());
    if (!client) {
        error = "GetModuleHandle(" + config.module + ") failed";
        m_state = State::Failed;
        return false;
    }
    m_client = GetLoadedModuleImage(config.module);
    m_engine = GetLoadedModuleImage(config.modelInfoModule);
    if (!m_client || !m_engine) {
        error = "module image unavailable for " + (m_client ? config.modelInfoModule : config.module);
        m_state = State::Failed;
        return false;
    }

    auto factory = reinterpret_cast<src::CreateInterfaceFn>(
        reinterpret_cast<void*>(GetProcAddress(client, "CreateInterface")));
    if (!factory) {
        error = "CreateInterface export missing in " + config.module;
        m_state = State::Failed;
        return false;
    }
    int rc = 0;
    m_entityList = factory(config.interfaceVersion.c_str(), &rc);
    if (!m_entityList) {
        error = "CreateInterface(" + config.interfaceVersion + ") returned null";
        m_state = State::Failed;
        return false;
    }

    HMODULE engine = GetModuleHandleA(config.modelInfoModule.c_str());
    auto engineFactory =
        engine ? reinterpret_cast<src::CreateInterfaceFn>(reinterpret_cast<void*>(GetProcAddress(engine, "CreateInterface")))
               : nullptr;
    std::string modelInfoVersion;
    if (engineFactory) {
        for (const std::string& version : config.modelInfoInterfaces) {
            m_modelInfo = engineFactory(version.c_str(), &rc);
            if (m_modelInfo) {
                modelInfoVersion = version;
                break;
            }
        }
    }
    if (!m_modelInfo) {
        error = "no IVModelInfoClient interface resolved in " + config.modelInfoModule;
        m_entityList = nullptr;
        m_state = State::Failed;
        return false;
    }

    std::string why;
    if (!SlotsPlausible(why)) {
        error = "entity list slots implausible: " + why;
        m_entityList = nullptr;
        m_modelInfo = nullptr;
        m_state = State::Failed;
        return false;
    }
    m_description = config.interfaceVersion + " + " + modelInfoVersion;
    m_state = State::Unvalidated;
    SA_LOGI("[ents] %s resolved (%s); layout validation deferred until in-map", config.module.c_str(),
            m_description.c_str());
    return true;
#else
    error = "native entity list access is only implemented for Windows builds";
    m_state = State::Failed;
    return false;
#endif
}

void ClientEntityList::Shutdown()
{
    m_entityList = nullptr;
    m_modelInfo = nullptr;
    m_client.reset();
    m_engine.reset();
    m_state = State::Uninitialized;
    m_description.clear();
    m_error.clear();
    m_loggedFailure = false;
}

void ClientEntityList::Invalidate()
{
    if (m_state == State::Validated)
        m_state = State::Unvalidated;
}

bool ClientEntityList::SlotsPlausible(std::string& why) const
{
    if (!m_client || !m_engine) {
        why = "module image unavailable";
        return false;
    }
    if (!SlotIsCode(m_entityList, m_config.slots.getClientEntity, *m_client) ||
        !SlotIsCode(m_entityList, m_config.slots.getHighestEntityIndex, *m_client)) {
        why = "IClientEntityList slots do not point into " + m_config.module;
        return false;
    }
    if (!SlotIsCode(m_modelInfo, m_config.slots.getModelName, *m_engine)) {
        why = "IVModelInfoClient::GetModelName slot does not point into " + m_config.modelInfoModule;
        return false;
    }
    return true;
}

bool ClientEntityList::Validate(const std::string& mapName, int32_t localPlayer, std::string& why)
{
    if (!m_client || !m_engine) {
        why = "module image unavailable";
        return false;
    }
    ReaderContext ctx;
    ctx.entityList = m_entityList;
    ctx.modelInfo = m_modelInfo;
    ctx.slots = &m_config.slots;
    ctx.client = &*m_client;
    ctx.engine = &*m_engine;

    const int32_t highest = ReadHighestIndex(ctx);
    if (highest < 0 || highest > m_config.maxEntities) {
        why = "GetHighestEntityIndex() implausible: " + std::to_string(highest);
        return false;
    }
    if (highest == 0) {
        why = "no entities yet";
        return false;
    }
    RawEntity world;
    const int32_t rc = ReadEntity(ctx, 0, world);
    if (rc != kReadOk) {
        why = rc == kReadNull ? "world entity is null" : "reading the world entity faulted";
        return false;
    }
    const std::string worldModel = Lower(world.model);
    const std::string expected = "maps/" + Lower(mapName) + ".bsp";
    if (worldModel != expected) {
        why = "world model '" + worldModel + "' != '" + expected + "'";
        return false;
    }
    if (world.solid != kSolidBsp) {
        why = "world solid type " + std::to_string(world.solid) + " != SOLID_BSP";
        return false;
    }
    if (localPlayer > 0 && localPlayer <= highest) {
        RawEntity player;
        const int32_t prc = ReadEntity(ctx, localPlayer, player);
        if (prc == kReadFault) {
            why = "reading the local player faulted";
            return false;
        }
        if (prc == kReadOk) {
            const std::string cls = player.networkName;
            bool isPlayer = false;
            for (const std::string& suffix : m_config.playerClassSuffixes)
                isPlayer = isPlayer || EndsWith(cls, suffix);
            if (!isPlayer) {
                why = "local player class '" + cls + "' has no player suffix";
                return false;
            }
        }
    }
    return true;
}

bool ClientEntityList::Snapshot(const std::string& mapName, int32_t localPlayer, std::vector<EntitySnapshot>& out)
{
    out.clear();
    if (m_state == State::Uninitialized || m_state == State::Failed || !m_entityList)
        return false;
    if (m_state == State::Unvalidated) {
        if (mapName.empty())
            return false;
        std::string why;
        if (!Validate(mapName, localPlayer, why)) {
            m_error = why;
            if (why == "no entities yet")
                return false; // not in-game yet; retry later
            if (!m_loggedFailure) {
                SA_LOGW("[ents] native entity list layout rejected: %s (Lua fallback stays active)", why.c_str());
                m_loggedFailure = true;
            }
            m_state = State::Failed;
            return false;
        }
        m_state = State::Validated;
        m_error.clear();
        SA_LOGI("[ents] native entity list validated on %s (%s)", mapName.c_str(), m_description.c_str());
    }

    if (!m_client || !m_engine) {
        m_error = "module image unavailable";
        m_state = State::Failed;
        return false;
    }
    ReaderContext ctx;
    ctx.entityList = m_entityList;
    ctx.modelInfo = m_modelInfo;
    ctx.slots = &m_config.slots;
    ctx.client = &*m_client;
    ctx.engine = &*m_engine;
    ctx.skipDormantGeometry = true;

    const int32_t highest = ReadHighestIndex(ctx);
    if (highest < 0 || highest > m_config.maxEntities) {
        m_error = "GetHighestEntityIndex() implausible: " + std::to_string(highest);
        SA_LOGW("[ents] %s; disabling native walk", m_error.c_str());
        m_state = State::Failed;
        return false;
    }
    ++m_stats.snapshots;
    m_stats.lastHighestIndex = highest;
    out.reserve(static_cast<size_t>(highest));
    uint64_t faults = 0;
    RawEntity raw;
    for (int32_t i = 1; i <= highest; ++i) {
        ++m_stats.entitiesVisited;
        raw = RawEntity{};
        const int32_t rc = ReadEntity(ctx, i, raw);
        if (rc == kReadNull)
            continue;
        if (rc == kReadFault) {
            ++faults;
            ++m_stats.readFailures;
            continue;
        }
        EntitySnapshot e;
        e.index = i;
        e.model = raw.model;
        e.origin = Vec3{raw.origin[0], raw.origin[1], raw.origin[2]};
        e.angles = Vec3{raw.angles[0], raw.angles[1], raw.angles[2]};
        if (raw.haveBounds) {
            e.mins = Vec3{raw.mins[0], raw.mins[1], raw.mins[2]};
            e.maxs = Vec3{raw.maxs[0], raw.maxs[1], raw.maxs[2]};
        }
        e.solid = raw.solid;
        e.dormant = raw.dormant != 0;
        const std::string cls = raw.networkName;
        for (const std::string& suffix : m_config.playerClassSuffixes)
            e.player = e.player || EndsWith(cls, suffix);
        out.push_back(std::move(e));
        ++m_stats.entitiesEmitted;
    }
    // A layout that faults on a sizeable share of live entities is wrong even
    // if the world/player probes passed.
    if (faults > 8 && faults * 4 > static_cast<uint64_t>(highest)) {
        m_error = std::to_string(faults) + " of " + std::to_string(highest) + " entity reads faulted";
        SA_LOGW("[ents] %s; disabling native walk", m_error.c_str());
        m_state = State::Failed;
        out.clear();
        return false;
    }
    return true;
}

} // namespace sa
