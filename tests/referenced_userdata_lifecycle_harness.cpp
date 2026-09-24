// The runner inserts the actual ForceGlobalRelease and RemoveTable functions.
// Adapters model cache ownership and reference release, not the engine ABI/GC.
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <vector>

struct FakeLua {};
struct ReferencedLuaUserData
{
	void* data;
	int releases = 0;
	FakeLua* releasedBy = nullptr;
	static inline std::function<void()> onRelease;
	void* GetData() { return data; }
	void SetData(void* value) { data = value; }
	void Release(FakeLua* lua)
	{
		if (onRelease) onRelease();
		data = nullptr;
		releasedBy = lua;
		++releases;
	}
	static void ForceGlobalRelease(void* data);
};
namespace Lua
{
	struct StateData
	{
		FakeLua* pLua;
		std::unordered_map<void*, ReferencedLuaUserData*> cache;
		auto& GetPushedUserData() { return cache; }
	};
	static std::vector<StateData*> states;
	static const auto& GetAllLuaData() { return states; }
}
struct INetworkStringTable
{
	bool* destroyed;
	bool destructorHook;
	int GetTableId() { return 0; }
	~INetworkStringTable()
	{
		if (destructorHook) ReferencedLuaUserData::ForceGlobalRelease(this);
		*destroyed = true;
	}
};
struct TableContainer
{
	struct Tables { int removed = -1; void FastRemove(int id) { removed = id; } } m_Tables;
};
static TableContainer* networkStringTableContainerServer;
struct CGameServer
{
	INetworkStringTable* m_pModelPrecacheTable = nullptr;
	INetworkStringTable* m_pSoundPrecacheTable = nullptr;
	INetworkStringTable* m_pDecalPrecacheTable = nullptr;
	INetworkStringTable* m_pGenericPrecacheTable = nullptr;
	INetworkStringTable* m_pDynamicModelsTable = nullptr;
	INetworkStringTable* m_pInstanceBaselineTable = nullptr;
	INetworkStringTable* m_pLightStyleTable = nullptr;
	INetworkStringTable* m_pUserInfoTable = nullptr;
	INetworkStringTable* m_pServerStartupTable = nullptr;
	INetworkStringTable* m_pDownloadableFileTable = nullptr;
};
namespace Util { static CGameServer* server; static void DoUnsafeCodeCheck(FakeLua*) {} }
static INetworkStringTable* inputTable;
static INetworkStringTable* Get_INetworkStringTable(FakeLua*, int, bool) { return inputTable; }
static void DeleteGlobal_INetworkStringTable(INetworkStringTable* value)
{ ReferencedLuaUserData::ForceGlobalRelease(value); }
[[maybe_unused]] static void Delete_INetworkStringTable(FakeLua* lua, INetworkStringTable* value)
{
	for (auto* state : Lua::states)
	{
		if (state->pLua != lua) continue;
		auto it = state->cache.find(value);
		if (it != state->cache.end()) { it->second->Release(lua); state->cache.erase(it); }
	}
}
#define LUA_FUNCTION_STATIC(name) static int name(FakeLua* LUA)

// INSERT_PRODUCTION_METHODS

static int checks = 0, failures = 0;
static void Check(bool condition, const char* name)
{
	++checks;
	if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}
struct Fixture
{
	int target = 0, other = 0, missing = 0;
	FakeLua luaA, luaB, luaC;
	ReferencedLuaUserData targetA{&target}, targetB{&target};
	ReferencedLuaUserData otherA{&other}, otherB{&other}, otherC{&other};
	Lua::StateData a{&luaA, {}}, b{&luaB, {}}, c{&luaC, {}};
	Fixture()
	{
		a.cache = {{&target, &targetA}, {&other, &otherA}};
		b.cache = {{&target, &targetB}, {&other, &otherB}};
		c.cache = {{&other, &otherC}};
		Lua::states = {&a, &b, &c};
	}
	~Fixture() { ReferencedLuaUserData::onRelease = {}; Lua::states.clear(); }
	void CheckOthers()
	{
		Check(otherA.data == &other && otherB.data == &other && otherC.data == &other,
			"unrelated userdata stays valid in every Lua state");
		Check(!otherA.releases && !otherB.releases && !otherC.releases,
			"unrelated references are retained");
	}
};
static void TestGlobalRelease()
{
	Fixture f;
	ReferencedLuaUserData::onRelease = [&] {
		Check(!f.targetA.data && !f.targetB.data, "all target wrappers invalid before releasing references");
	};
	ReferencedLuaUserData::ForceGlobalRelease(&f.target);
	Check(!f.a.cache.count(&f.target) && !f.b.cache.count(&f.target), "released target removed from every cache");
	Check(f.targetA.releases == 1 && f.targetB.releases == 1, "each target reference released once");
	Check(f.targetA.releasedBy == &f.luaA && f.targetB.releasedBy == &f.luaB, "references released by their owning Lua state");
	f.CheckOthers();
	ReferencedLuaUserData::ForceGlobalRelease(&f.target);
	Check(f.targetA.releases == 1 && f.targetB.releases == 1, "duplicate destructor notification is harmless");
	ReferencedLuaUserData::onRelease = {};
	ReferencedLuaUserData fresh{&f.target};
	bool inserted = f.a.cache.emplace(&f.target, &fresh).second;
	Check(inserted, "reused native address can receive a fresh wrapper");
	ReferencedLuaUserData::ForceGlobalRelease(&f.target);
	Check(fresh.releases == 1 && f.targetA.releases == 1, "address reuse never releases the old wrapper again");
}
static void TestMissingAndNullData()
{
	Fixture f;
	ReferencedLuaUserData::ForceGlobalRelease(&f.missing);
	Check(f.targetA.data == &f.target && f.targetB.data == &f.target, "absent pointer is a no-op");
	f.CheckOthers();
	f.targetA.SetData(nullptr);
	ReferencedLuaUserData::ForceGlobalRelease(&f.target);
	Check(f.targetA.releases == 1 && !f.a.cache.count(&f.target), "already invalid wrapper releases its cached reference");
}
static void TestRemoveTable(bool destructorHook)
{
	Fixture f;
	bool destroyed = false;
	auto* table = new INetworkStringTable{&destroyed, destructorHook};
	void* key = table;
	f.a.cache.erase(&f.target); f.b.cache.erase(&f.target);
	f.targetA.SetData(table); f.targetB.SetData(table);
	f.a.cache.emplace(table, &f.targetA); f.b.cache.emplace(table, &f.targetB);
	TableContainer container;
	CGameServer server;
	server.m_pModelPrecacheTable = table;
	networkStringTableContainerServer = &container;
	Util::server = &server;
	inputTable = table;
	Check(stringtable_RemoveTable(&f.luaA) == 0, "RemoveTable succeeds");
	Check(destroyed && container.m_Tables.removed == 0, "native table destroyed and removed from container");
	Check(!server.m_pModelPrecacheTable, "engine table pointer cleared");
	Check(!f.targetA.data && !f.targetB.data, "RemoveTable invalidates wrappers in other Lua states");
	Check(!f.a.cache.count(key) && !f.b.cache.count(key), "RemoveTable clears all target cache entries");
	Check(f.targetA.releases == 1 && f.targetB.releases == 1, "RemoveTable releases each reference exactly once");
	f.CheckOthers();
}
int main()
{
	TestGlobalRelease();
	TestMissingAndNullData();
	TestRemoveTable(false);
	TestRemoveTable(true);
	std::printf("Referenced userdata lifecycle: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
