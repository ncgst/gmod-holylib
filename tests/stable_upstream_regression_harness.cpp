#include <array>
#include <bitset>
#include <cassert>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

struct LuaArgumentError { int argument; };

namespace player_test {
struct CBaseEntity {
    explicit CBaseEntity(bool player = false) : player(player) {}
    bool IsPlayer() const { return player; }
    bool player;
};
struct CBasePlayer : CBaseEntity { CBasePlayer() : CBaseEntity(true) {} };
struct EHANDLE {};
namespace GarrysMod::Lua {
namespace Type { constexpr int Entity = 1; }
struct ILuaObject {
    CBaseEntity* entity = nullptr;
    void SetFromStack(int) {}
    CBaseEntity* GetEntity() { return entity; }
};
struct ILuaInterface {
    ILuaObject object;
    EHANDLE handle;
    bool hasHandle = true;
    ILuaObject* NewTemporaryObject() { return &object; }
    template<class T> T* GetUserType(int, int) { return hasHandle ? &handle : nullptr; }
    [[noreturn]] void ArgError(int argument, const char*) { throw LuaArgumentError{argument}; }
};
}
struct EntityList {
    CBaseEntity* entity = nullptr;
    CBaseEntity* GetBaseEntity(const EHANDLE&) { return entity; }
};
namespace Util {
EntityList* entitylist = nullptr;
CBasePlayer* Get_Player(GarrysMod::Lua::ILuaInterface*, int, bool);
}
// PLAYER_FUNCTION
void run() {
    GarrysMod::Lua::ILuaInterface lua;
    EntityList entities;
    CBasePlayer player;
    CBaseEntity prop;
    for (bool fallback : {false, true}) {
        Util::entitylist = fallback ? nullptr : &entities;
        for (CBaseEntity* entity : {static_cast<CBaseEntity*>(&player), &prop, static_cast<CBaseEntity*>(nullptr)}) {
            lua.object.entity = entities.entity = entity;
            for (bool error : {false, true}) {
                bool caught = false;
                try {
                    CBasePlayer* result = Util::Get_Player(&lua, 7, error);
                    assert(result == (entity == &player ? &player : nullptr));
                } catch (const LuaArgumentError& e) {
                    caught = true;
                    assert(e.argument == 7);
                }
                assert(caught == (error && entity != &player));
            }
        }
    }
    Util::entitylist = &entities;
    lua.hasHandle = false;
    assert(Util::Get_Player(&lua, 7, false) == nullptr);
    try { Util::Get_Player(&lua, 7, true); assert(false); }
    catch (const LuaArgumentError& e) { assert(e.argument == 7); }
}
}

namespace transmit_test {
constexpr int MAX_PLAYERS = 128;
constexpr int MAX_EDICTS = 256;
struct edict_t { int m_EdictIndex; };
struct CBaseEntity {
    edict_t value;
    bool player;
    bool hasEdict = true;
    const edict_t* edict() const { return hasEdict ? &value : nullptr; }
    bool IsPlayer() const { return player; }
};
template<int N> struct CBitVec {
    std::bitset<N> bits;
    void Clear(int index) { assert(index >= 0 && index < N); bits.reset(index); }
    void ClearAll() { bits.reset(); }
};
static CBitVec<MAX_EDICTS> g_pShouldPrevent[MAX_PLAYERS];
// TRANSMIT_FUNCTION
void run() {
    for (int slot : {1, 64, MAX_PLAYERS}) {
        for (auto& recipient : g_pShouldPrevent) {
            recipient.bits.set(slot);
            recipient.bits.set(200); // An unrelated entity must retain its policy.
        }
        const CBaseEntity departing{{slot}, true};
        CleanupSetPreventTransmit(&departing);
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            assert(!g_pShouldPrevent[i].bits.test(slot));
            assert(g_pShouldPrevent[i].bits.test(200) == (i != slot - 1));
        }
        // Removing an ordinary entity clears its column, not recipient rows.
        const CBaseEntity prop{{200}, false};
        CleanupSetPreventTransmit(&prop);
        for (const auto& recipient : g_pShouldPrevent) assert(recipient.bits.none());
    }
    g_pShouldPrevent[0].bits.set(2);
    const CBaseEntity invalid{{2}, true, false};
    CleanupSetPreventTransmit(&invalid);
    assert(g_pShouldPrevent[0].bits.test(2));
}
}

namespace stringtable_test {
struct CNetworkStringTable {
    int writes = 0;
    std::string data;
    int GetNumStrings() const { return 4; }
    void SetStringUserData(int index, int length, const void* value) {
        assert(index == 1);
        assert(length >= 0);
        ++writes;
        data.assign(value ? static_cast<const char*>(value) : "", length);
    }
};
struct FakeLua {
    CNetworkStringTable table;
    int index = 1;
    int length = 0;
    const char* bytes;
    size_t size;
    double CheckNumber(int) const { return index; }
    double CheckNumberOpt(int, double) const { return length; }
    [[noreturn]] void ArgError(int argument, const char*) { throw LuaArgumentError{argument}; }
};
void* Get_INetworkStringTable(FakeLua* lua, int, bool) { return &lua->table; }
namespace Util {
const char* CheckLString(FakeLua* lua, int, size_t* length) {
    *length = lua->size;
    return lua->bytes;
}
void DoUnsafeCodeCheck(FakeLua*) {}
}
#define LUA_FUNCTION_STATIC(name) static int name(FakeLua* LUA)
// STRINGTABLE_FUNCTION
#undef LUA_FUNCTION_STATIC
void run() {
    // Embedded NULs are data; no terminator is included in the allocation.
    auto* bytes = new char[4]{'a', '\0', 'b', 'c'};
    FakeLua lua{{}, 1, 0, bytes, 4};
    for (int length : {0, 2, 4}) {
        lua.length = length;
        const int before = lua.table.writes;
        INetworkStringTable_SetStringUserData(&lua);
        assert(lua.table.writes == before + 1);
        assert(lua.table.data == std::string(bytes, length == 0 ? 4 : length));
    }
    for (int length : {5, -1, 2147483647}) {
        lua.length = length;
        const int before = lua.table.writes;
        try { INetworkStringTable_SetStringUserData(&lua); assert(false); }
        catch (const LuaArgumentError& e) { assert(e.argument == 4); }
        assert(lua.table.writes == before);
    }
    lua.index = 4;
    lua.length = 0;
    const int before = lua.table.writes;
    INetworkStringTable_SetStringUserData(&lua);
    assert(lua.table.writes == before);
    delete[] bytes;
}
}

namespace callback_test {
namespace GarrysMod::Lua { struct ILuaInterface {}; }
struct IJobEntry {
    virtual ~IJobEntry() = default;
    virtual bool OnThink(GarrysMod::Lua::ILuaInterface*) = 0;
};
struct ModuleData { std::vector<IJobEntry*> pEntries; };
ModuleData* activeData = nullptr;
ModuleData* GetUtilLuaData(GarrysMod::Lua::ILuaInterface*) { return activeData; }
class CUtilModule { public: void LuaThink(GarrysMod::Lua::ILuaInterface*); };
#define VPROF_BUDGET(...)
// CALLBACK_FUNCTION
#undef VPROF_BUDGET
int destroyed = 0;
struct Entry : IJobEntry {
    std::function<bool()> think;
    explicit Entry(std::function<bool()> callback) : think(std::move(callback)) {}
    ~Entry() override { ++destroyed; }
    bool OnThink(GarrysMod::Lua::ILuaInterface*) override { return think(); }
};
void run() {
    CUtilModule module;
    GarrysMod::Lua::ILuaInterface lua;
    module.LuaThink(&lua); // Missing state is a valid lifecycle condition.
    ModuleData data;
    activeData = &data;
    module.LuaThink(&lua);
    bool ready = false;
    int pendingCalls = 0, completedCalls = 0;
    data.pEntries.push_back(new Entry([&] {
        ++pendingCalls;
        return ready;
    }));
    data.pEntries.push_back(new Entry([&] {
        // Force several reallocations while the production loop is in OnThink.
        for (int i = 0; i < 256; ++i)
            data.pEntries.push_back(new Entry([&] { ++completedCalls; return true; }));
        return true;
    }));
    module.LuaThink(&lua);
    assert(pendingCalls == 1 && completedCalls == 256);
    assert(destroyed == 257 && data.pEntries.size() == 1);
    ready = true;
    module.LuaThink(&lua);
    assert(pendingCalls == 2 && destroyed == 258 && data.pEntries.empty());
}
}

namespace shutdown_test {
std::vector<std::string> events;
namespace GarrysMod::Lua {
constexpr int INDEX_GLOBAL = 0;
struct ILuaInterface {
    void PushNil() {}
    void SetField(int, const char*) {}
};
}
GarrysMod::Lua::ILuaInterface* g_Lua = nullptr;
struct ModuleManager {
    bool binary = false;
    bool IsMarkedAsBinaryModule() const { return binary; }
    void LevelShutdown() { events.emplace_back(g_Lua ? "level-live" : "level-final"); }
    void LuaShutdown(GarrysMod::Lua::ILuaInterface* lua) {
        assert(lua && lua == g_Lua);
        events.emplace_back("lua-shutdown");
    }
} g_pModuleManager;
namespace Util {
std::vector<int> g_pReference;
struct Debug { bool GetBool() const { return false; } } holylib_debug_mainutil;
}
void Msg(const char*, int) {}
void RemoveCoreTestFunctions(GarrysMod::Lua::ILuaInterface*) { events.emplace_back("remove-tests"); }
namespace Lua {
void Shutdown();
void FinalShutdown();
void ManualShutdown();
void RemoveLuaData(GarrysMod::Lua::ILuaInterface* lua) {
    assert(lua == g_Lua);
    events.emplace_back("remove-data");
}
}
#define PROJECT_NAME "regression"
// SHUTDOWN_FUNCTIONS
#undef PROJECT_NAME
void run() {
    GarrysMod::Lua::ILuaInterface lua;
    for (bool binary : {false, true}) {
        events.clear();
        g_pModuleManager.binary = binary;
        g_Lua = &lua;
        Util::g_pReference = {7, 8};
        Lua::ManualShutdown();
        assert(g_Lua == nullptr && Util::g_pReference.empty());
        const std::vector<std::string> expected = binary
            ? std::vector<std::string>{"level-live", "lua-shutdown", "remove-tests", "remove-data", "level-final"}
            : std::vector<std::string>{"lua-shutdown", "remove-tests", "remove-data"};
        assert(events == expected);
        Lua::ManualShutdown(); // Repeated shutdown of an absent state is inert.
        assert(events == expected);
    }
}
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string name = argv[1];
    if (name == "player") player_test::run();
    else if (name == "transmit") transmit_test::run();
    else if (name == "stringtable") stringtable_test::run();
    else if (name == "callbacks") callback_test::run();
    else if (name == "shutdown") shutdown_test::run();
    else assert(false);
}
