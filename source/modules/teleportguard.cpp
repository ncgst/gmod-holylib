#include "LuaInterface.h"
#include "detours.h"
#include "module.h"
#include "lua.h"
#include "util.h"
#include "teleport_guard.h"
#include "player.h"
#include <atomic>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static bool IsActive();

class CTeleportGuardModule : public IModule
{
public:
	void InitDetour(bool bPreServer) override;
	void LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit) override;
	void LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua) override;
	const char* Name() override { return "teleportguard"; }
	int Compatibility() override { return LINUX64; }
	bool SupportsMultipleLuaStates() override { return false; }
	// Map scripts may lift their fallback restriction once IsActive() succeeds.
	// Removing the guard underneath those scripts would make the map unsafe.
	bool CanEnableAtRuntime() override { return false; }
	bool CanDisableAtRuntime() override { return !IsActive(); }
};

static CTeleportGuardModule g_pTeleportGuardModule;
IModule* pTeleportGuardModule = &g_pTeleportGuardModule;

using TriggerTouch = void (*)(CBaseEntity*, CBaseEntity*);
static Detouring::Hook detour_CTriggerTeleport_Touch;
static Detouring::Hook detour_CTriggerTeleportRelative_Touch;
static std::atomic<std::uint64_t> blockedTouches{0};

static void GuardedTouch(Detouring::Hook& detour, CBaseEntity* trigger, CBaseEntity* other)
{
	if (!other)
		return;
	// Include the serial number so an entity deleted/replaced by a filter cannot
	// suppress a different entity that reuses its slot during the same call.
	HolyLib::TeleportGuard::Scope scope(other->GetRefEHandle().ToInt());
	if (!scope.Entered())
	{
		blockedTouches.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	detour.GetTrampoline<TriggerTouch>()(trigger, other);
}

static void hook_CTriggerTeleport_Touch(CBaseEntity* trigger, CBaseEntity* other)
{
	GuardedTouch(detour_CTriggerTeleport_Touch, trigger, other);
}

static void hook_CTriggerTeleportRelative_Touch(CBaseEntity* trigger, CBaseEntity* other)
{
	GuardedTouch(detour_CTriggerTeleportRelative_Touch, trigger, other);
}

static bool IsActive()
{
	return detour_CTriggerTeleport_Touch.IsEnabled() && detour_CTriggerTeleportRelative_Touch.IsEnabled();
}

LUA_FUNCTION_STATIC(teleportguard_IsActive)
{
	LUA->PushBool(IsActive());
	return 1;
}

LUA_FUNCTION_STATIC(teleportguard_GetBlockedCount)
{
	LUA->PushNumber(static_cast<double>(blockedTouches.load(std::memory_order_relaxed)));
	return 1;
}

void CTeleportGuardModule::InitDetour(bool bPreServer)
{
	if (bPreServer)
		return;
	SourceSDK::FactoryLoader server_loader("server");
	// Linux64 GMod: recovered from the CTriggerTeleport/CTriggerTeleportRelative
	// RTTI vtables, Touch slot 102. These are unique entry signatures, not offsets.
	// The functions call PassesTriggerFilters, then the toucher's Teleport.
	// Verified server.so SHA256 is recorded in docs/teleportguard.md.
	const std::vector<Symbol> normal = {
		Symbol::FromName("_ZN16CTriggerTeleport5TouchEP11CBaseEntity"),
		Symbol::FromSignature("\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x55\x49\x89\xF5\x41\x54\x53\x48\x89\xFB\x48\x83\xEC\x28\x48\x8B\x07\xFF\x90\xF8\x07\x00\x00\x84\xC0")
	};
	const std::vector<Symbol> relative = {
		Symbol::FromName("_ZN24CTriggerTeleportRelative5TouchEP11CBaseEntity"),
		Symbol::FromSignature("\x55\x48\x89\xE5\x41\x54\x49\x89\xF4\x53\x48\x89\xFB\x48\x83\xEC\x10\x48\x8B\x07\xFF\x90\xF8\x07\x00\x00\x84\xC0\x75\x12")
	};
	Detour::Create(&detour_CTriggerTeleport_Touch, "CTriggerTeleport::Touch", server_loader.GetModule(), normal, (void*)hook_CTriggerTeleport_Touch, m_pID);
	Detour::Create(&detour_CTriggerTeleportRelative_Touch, "CTriggerTeleportRelative::Touch", server_loader.GetModule(), relative, (void*)hook_CTriggerTeleportRelative_Touch, m_pID);
	if (!IsActive())
	{
		Detour::Remove(m_pID);
		Warning(PROJECT_NAME ": teleportguard unavailable; keep map teleport restrictions enabled\n");
	}
}

void CTeleportGuardModule::LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit)
{
	if (bServerInit)
		return;
	Util::StartTable(pLua);
		Util::AddFunc(pLua, teleportguard_IsActive, "IsActive");
		Util::AddFunc(pLua, teleportguard_GetBlockedCount, "GetBlockedCount");
	Util::FinishTable(pLua, "teleportguard");
}

void CTeleportGuardModule::LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua)
{
	Util::NukeTable(pLua, "teleportguard");
}
