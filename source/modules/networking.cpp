#include "filesystem_base.h" // Has to be before symbols.h
#include "LuaInterface.h"
#include "detours.h"
#include "module.h"
#include "lua.h"
#include "vprof.h"
#include "framesnapshot.h"
#include "packed_entity.h"
#include "server_class.h"
#include "dt.h"
#include "edict.h"
#include "eiface.h"
#include "baseclient.h"
#include <bitset>
#include <chrono>
#include <cstdint>
#include <vector>
#include <datacache/imdlcache.h>
#include <cmodel_private.h>
#include "server.h"
#include "hltvserver.h"
#define protected public
#include "player.h"
#undef protected
#include "SkyCamera.h"
#include "sourcesdk/GameEventManager.h"
#include "sourcesdk/ccservernetworkproperty.h"
#include "sourcesdk/datatablestack.h"
#include "networking_pvs_cache.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

class CNetworkingModule : public IModule
{
public:
	void Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn) override;
	void InitDetour(bool bPreServer) override;
	void Shutdown() override;
	void OnEntityCreated(CBaseEntity* pEntity) override;
	void OnEntityDeleted(CBaseEntity* pEntity) override;
	void ClientDisconnect(edict_t* pClient) override;
	void ServerActivate(edict_t* pEdictList, int edictCount, int clientMax) override;
	const char* Name() override { return "networking"; };
	int Compatibility() override { return LINUX32 | LINUX64;  }; // ToDo: Fix CBaseClient offset being broken on 64x causing the access to CGameClient::m_pCurrentFrame to return a invalid pointer
};

/*
 * This module can't be disabled at runtime!
 * 
 * This is because of it replacing the entire CChangeFrameList class which is used & stored in the engine.
 * I need to restore and fix the code in Shutdown to allow proper unloading.
 */

static CNetworkingModule g_pNetworkingModule;
IModule* pNetworkingModule = &g_pNetworkingModule;

/*
=============================================================================
Simplified BSD License, see http://www.opensource.org/licenses/
-----------------------------------------------------------------------------
Copyright (c) 2017, sigsegv <sigsegv@sigpipe.info>
see https://github.com/rafradek/sigsegv-mvm/tree/master?tab=License-1-ov-file for full BSD License
*/

// This is originally from here: https://github.com/rafradek/sigsegv-mvm/blob/910b92456c7578a3eb5dff2a7e7bf4bc906677f7/src/mod/perf/sendprop_optimize.cpp#L35-L144
class CChangeFrameList : public IChangeFrameList
{
public:
	void Init(int nProperties, int iCurTick)
	{
		VPROF_BUDGET("CChangeFrameList::Init", VPROF_BUDGETGROUP_OTHER_NETWORKING);
		m_ChangeTicks.SetSize(nProperties);
		for (int i=0; i < nProperties; ++i)
			m_ChangeTicks[i] = iCurTick;
	}
public:
	virtual void Release()
	{
		--m_CopyCounter;
		if (m_CopyCounter < 0)
			delete this;
	}

	virtual IChangeFrameList* Copy()
	{
		//VPROF_BUDGET("CChangeFrameList::Copy", VPROF_BUDGETGROUP_OTHER_NETWORKING);
		++m_CopyCounter;
		return this;
	}

	virtual int GetNumProps()
	{
		return m_ChangeTicks.Count();
	}

	virtual void SetChangeTick(const int *pPropIndices, int nPropIndices, const int iTick)
	{
		VPROF_BUDGET("CChangeFrameList::SetChangeTick", VPROF_BUDGETGROUP_OTHER_NETWORKING);
		bool same = (int)m_LastChangeTicks.size() == nPropIndices;
		m_LastChangeTicks.resize(nPropIndices);
		for (int i=0; i < nPropIndices; ++i)
		{
			int prop = pPropIndices[i];
			m_ChangeTicks[prop] = iTick;
			
			same = same && m_LastChangeTicks[i] == prop;
			m_LastChangeTicks[i] = prop;
		}

		if (!same)
			m_LastSameTickNum = iTick;

		m_LastChangeTickNum = iTick;
		if (m_LastChangeTicks.capacity() > m_LastChangeTicks.size() * 8)
			m_LastChangeTicks.shrink_to_fit();
	}

	virtual int GetPropsChangedAfterTick(int iTick, int *iOutProps, int nMaxOutProps)
	{
		// Should we remove vprof here? It could slow this entire thing down since it's called so often
		// VPROF_BUDGET("CChangeFrameList::GetPropsChangedAfterTick", VPROF_BUDGETGROUP_OTHER_NETWORKING);
		int nOutProps = 0;
		if (iTick + 1 >= m_LastSameTickNum)
		{
			if (iTick >= m_LastChangeTickNum)
				return 0;

			nOutProps = m_LastChangeTicks.size();
			for (int i=0; i < nOutProps; ++i)
				iOutProps[i] = m_LastChangeTicks[i];

			return nOutProps;
		} else {
			int c = m_ChangeTicks.Count();
			for (int i=0; i < c; ++i)
			{
				if (m_ChangeTicks[i] > iTick)
				{
					iOutProps[nOutProps] = i;
					++nOutProps;
				}
			}

			return nOutProps;
		}
	}

protected:
	virtual ~CChangeFrameList()
	{
	}

private:
	CUtlVector<int>		m_ChangeTicks;

	int m_CopyCounter = 0;
	int m_LastChangeTickNum = 0;
	int m_LastSameTickNum = 0;
	std::vector<int> m_LastChangeTicks;
};

// -------------------------------------------------------------------------------------------------
// sigsegv part

static Detouring::Hook detour_AllocChangeFrameList;
IChangeFrameList* hook_AllocChangeFrameList(int nProperties, int iCurTick)
{
	VPROF_BUDGET("AllocChangeFrameList", VPROF_BUDGETGROUP_OTHER_NETWORKING);
	CChangeFrameList* pRet = new CChangeFrameList;
	pRet->Init(nProperties, iCurTick);

	return pRet;
}

// -------------------------------------------------------------------------------------------------
// HolyLib part

static CBitVec<MAX_EDICTS> g_pShouldPrevent[MAX_PLAYERS];
static Detouring::Hook detour_CBaseEntity_GMOD_ShouldPreventTransmitToPlayer;
static bool hook_CBaseEntity_GMOD_ShouldPreventTransmitToPlayer(CBaseEntity* ent, CBasePlayer* ply)
{
	edict_t* pEdict = ent->edict();
	if (!pEdict)
		return false;

	edict_t* pPlayerEdict = ply->edict();
	if (!pPlayerEdict)
		return false;

	return g_pShouldPrevent[pPlayerEdict->m_EdictIndex-1].IsBitSet(pEdict->m_EdictIndex);
}

static void CleanupSetPreventTransmit(const CBaseEntity* ent)
{
	const edict_t* pEdict = ent->edict();
	if (!pEdict)
		return;

	const int entIndex = pEdict->m_EdictIndex;
	if (!ent->IsPlayer())
	{
		for (int i=0; i<MAX_PLAYERS; ++i)
		{
			g_pShouldPrevent[i].Clear(entIndex);
		}
		return;
	}

	g_pShouldPrevent[entIndex-1].ClearAll();
}

static Detouring::Hook detour_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer;
static Symbols::CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer func_GMODSetShouldPrevent = nullptr;

Symbols::CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer Networking_GetSetShouldPreventTransmit()
{
	return func_GMODSetShouldPrevent;
}

static void hook_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer(CBaseEntity* pEnt, CBasePlayer* pPly, bool bPreventTransmit)
{
	const edict_t* pEdict = pEnt->edict();
	if (!pEdict)
		return;

	const edict_t* pPlayerEdict = pPly->edict();
	if (!pPlayerEdict)
		return;

	const int plyIndex = pPlayerEdict->m_EdictIndex - 1;
	const int entIndex = pEdict->m_EdictIndex;
	if (bPreventTransmit) {
		g_pShouldPrevent[plyIndex].Set(entIndex);
	} else {
		g_pShouldPrevent[plyIndex].Clear(entIndex);
	}
}

// -------------------------------------------------------------------------------------------------
// HolyLib part

static Detouring::Hook detour_CGMOD_Player_CreateViewModel;
static ConVar networking_maxviewmodels("holylib_networking_maxviewmodels", "3", 0, "Determines how many view models each player gets.", true, 0, true, 3);
static void hook_CGMOD_Player_CreateViewModel(CBasePlayer* pPlayer, int viewmodelindex)
{
	if (viewmodelindex >= networking_maxviewmodels.GetInt())
		return;

	detour_CGMOD_Player_CreateViewModel.GetTrampoline<Symbols::CGMOD_Player_CreateViewModel>()(pPlayer, viewmodelindex);
}

// -------------------------------------------------------------------------------------------------

// Everything below is also from many places inside https://github.com/rafradek/sigsegv-mvm/tree/cd2ee719719cda9c24da6e395557fdb66487adfe
#define PROP_INDEX_INVALID 0xffff
#define INVALID_PROP_INDEX 65535
struct PropIndexData
{
	unsigned short offset = 0;
	unsigned short element = 0;
	unsigned short index1 = PROP_INDEX_INVALID;
	unsigned short index2 = PROP_INDEX_INVALID;
};

struct SpecialSendPropCalc
{
	const int index;
};

struct SpecialDataTableCalc
{
	std::vector<int> propIndexes;
	int baseOffset;
};

class ServerClassCache
{
public:
	std::vector<PropIndexData> prop_offset_sendtable;
	std::vector<SpecialSendPropCalc> prop_special;
	unordered_map<const SendProp *, SpecialDataTableCalc> datatable_special;

	unsigned short *prop_offsets;

	//CSendNode **send_nodes;

	// prop indexes that are stopped from being send to players
	unsigned char *prop_cull;

	// prop indexes that are stopped from being send to players
	unsigned short *prop_propproxy_first;
};

bool* player_local_exclusive_send_proxy;
inline ServerClassCache* GetServerClassCache(const SendTable *pTable)
{
	return (ServerClassCache*)pTable->m_pPrecalc->m_pDTITable;
}

static Detouring::Hook detour_SendTable_CullPropsFromProxies;
static int hook_SendTable_CullPropsFromProxies( 
	const SendTable *pTable,
	
	const int *pStartProps,
	int nStartProps,

	const int iClient,
	
	const CSendProxyRecipients *pOldStateProxies,
	const int nOldStateProxies, 
	
	const CSendProxyRecipients *pNewStateProxies,
	const int nNewStateProxies,
	
	int *pOutProps,
	int nMaxOutProps
	)
{
	int count = 0;
	auto &prop_cull = GetServerClassCache(pTable)->prop_cull;
	for (int i = 0; i <nStartProps; ++i) {
		int prop = pStartProps[i];
		int proxyindex = prop_cull[prop];
		if (proxyindex < 254 ) {
			if (pNewStateProxies[proxyindex].m_Bits.IsBitSet(iClient)) {
				pOutProps[count++] = prop;
			}
		} else {
			pOutProps[count++] = prop;
		}
	}

	return count;
}

void AddOffsetToList(ServerClassCache &cache, int offset, int index, int element) {
	int size = cache.prop_offset_sendtable.size();
	for (int i = 0; i < size; ++i) {
		if (cache.prop_offset_sendtable[i].offset == (unsigned short) offset) {
			cache.prop_offset_sendtable[i].index2 = (unsigned short) index;
			return;
		}
	}

	cache.prop_offset_sendtable.emplace_back();
	PropIndexData &data = cache.prop_offset_sendtable.back();
	data.offset = (unsigned short) offset;
	data.index1 = (unsigned short) index;
	data.element = (unsigned short) element;
};

void PropScan(int off, SendTable *s_table, int &index)
{
	for (int i = 0; i < s_table->GetNumProps(); ++i) {
		SendProp *s_prop = s_table->GetProp(i);

		if (s_prop->GetDataTable() != nullptr) {
			PropScan(off + s_prop->GetOffset(), s_prop->GetDataTable(), index);
		} else {
			//Msg("Scan Data table for %d %s %d is %d %d %d\n", index, s_prop->GetName(),  off + s_prop->GetOffset(), off, s_prop->GetProxyFn(), s_prop->GetDataTableProxyFn());
			index++;
			//onfound(s_prop, off + s_prop->GetOffset());
		}
	}
}

SendTableProxyFn local_sendtable_proxy;
void RecurseStack(ServerClassCache &cache, unsigned char* stack, CSendNode *node, CSendTablePrecalc *precalc)
{
	//stack[node->m_RecursiveProxyIndex] = strcmp(node->m_pTable->GetName(), "DT_TFNonLocalPlayerExclusive") == 0;
	stack[node->m_RecursiveProxyIndex] = node->m_DataTableProxyIndex;
	if (node->m_DataTableProxyIndex < 254) {
		//cache.send_nodes[node->m_DataTableProxyIndex] = node;
		player_local_exclusive_send_proxy[node->m_DataTableProxyIndex] = precalc->m_DatatableProps[node->m_iDatatableProp]->GetDataTableProxyFn() == local_sendtable_proxy;
	}
			
	//("data %d %d %s %d\n", node->m_RecursiveProxyIndex, stack[node->m_RecursiveProxyIndex], node->m_pTable->GetName(), node->m_nRecursiveProps);
	for (int i = 0; i < node->m_Children.Count(); ++i) {
		CSendNode *child = node->m_Children[i];
		RecurseStack(cache, stack, child, precalc);
	}
}

// -------------------------------------------------------------------------------------------------
// HolyLib part

// ToDo: Move this into util.h
static inline void CBitVec_AndNot(CBitVec<MAX_EDICTS>* a, const CBitVec<MAX_EDICTS>* b)
{
	uint32* aBase = a->Base();
	const uint32* bBase = b->Base();
	int nWords = a->GetNumDWords();

	for (int i = 0; i < nWords; ++i)
	{
		aBase[i] = aBase[i] & ~bBase[i];
	}
}

/*
 * For now this is called from the pvs module meaning we RELY on it.
 * What did we change? basically nothing yet. I'm just testing around.
 * 
 * NOTE: It's shit & somehow were loosing performance to something. Probably us detouring it is causing our performance loss.
 */
static ConVar* sv_force_transmit_ents = nullptr;
static CBaseEntity* g_pEntityCache[MAX_EDICTS] = {nullptr};
#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)
static bool g_bEntityCacheSeeded = false;
#endif
bool g_pReplaceCServerGameEnts_CheckTransmit = false;
static edict_t* world_edict = nullptr;

// Offset & helper functions

static DTVarByOffset m_Local_Offset("DT_LocalPlayerExclusive", "m_Local");
static DTVarByOffset m_SkyBox3DArea_Offset("DT_Local", "m_skybox3d.area");
static inline int GetSkybox3DArea(const void* pPlayer) // Fully safe access :3
{
	const void* pLocal = m_Local_Offset.GetPointer(pPlayer);
	if (!pLocal)
		return 255; // 255 is the default max value used so we just fallback to that.

	const void* pSkybox3DArea = m_SkyBox3DArea_Offset.GetPointer(pLocal);
	if (!pSkybox3DArea)
		return 255;

	return *(int*)pSkybox3DArea;
}

static inline CBaseEntity* EHandleToEntity(const CBaseHandle* pHandle)
{
	if (!g_pEntityList)
	{
		const int nEntIndex = pHandle->GetEntryIndex();
		if (nEntIndex < 0 || nEntIndex >= MAX_EDICTS)
			return nullptr;

		// Resolve through the engine instead of reading g_pEntityCache.
		//
		// Without gEntList nothing scrubs a cache slot when its entity is freed, and this function can be
		// reached from hook_CBaseCombatCharacter_SetTransmit on a tick where our CheckTransmit never ran,
		// so the slot need not have been rebuilt. The serial-number test below is a *virtual* call, so a
		// freed entity faults while dereferencing its own vtable pointer - before the test can reject it.
		// Validating a pointer by calling a virtual on that same pointer is circular; that is exactly the
		// 2026-07-26 21:08 segfault (jumped to non-executable memory out of a stale vtable).
		//
		// PEntityOfEntIndex bounds-checks against num_edicts and rejects free edicts, and EdictToBaseEntity
		// reads the edict - engine-owned memory that is never freed - not the entity. What comes back is
		// therefore always either a live entity or nullptr, which makes GetRefEHandle() safe to call.
		// 32x keeps the fast array lookup: there CBaseHandle::Get() goes through CBaseEntityList and
		// validates the serial against the CEntInfo array without ever touching the object.
		CBaseEntity* pEnt = Util::GetCBaseEntityFromIndex(nEntIndex);
		if (!pEnt)
			return nullptr;

		if (pEnt->GetRefEHandle() != *pHandle) // To compare the serial number mimicking CBaseEntityList::LookupEntity
			return nullptr;

		return pEnt;
	}

	return (CBaseEntity*)pHandle->Get();
}

// Resolves an edict index straight from the engine, bypassing g_pEntityCache.
// Used to rebuild the cache when the entity listener could not be registered (see UpdateEntities).
// Deliberately goes through PEntityOfEntIndex rather than indexing world_edict directly: slots past
// sv.num_edicts are zeroed, so IsFree() reads false for them and we would hand a zeroed edict to
// EdictToBaseEntity. PEntityOfEntIndex bounds-checks against num_edicts *and* rejects free edicts.
static inline CBaseEntity* EdictIndexToEntity(const int nEntIndex)
{
	return Util::GetCBaseEntityFromIndex(nEntIndex);
}

// Without g_pEntityList we never registered the entity listener (util.cpp), so OnEntityCreated /
// OnEntityDeleted never fire and the cache must be rebuilt every tick. On Linux 64x, do one initial
// rebuild even with a listener: plugin_load can happen after ServerActivate and AddListenerEntity
// does not replay entities that already exist. A partial rebuild is not sufficient because handle
// resolution also needs weapons, viewmodels and hands that need not occur in CheckTransmit's edict list.
static inline void RebuildEntityCacheForTick()
{
#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)
	if (g_pEntityList && g_bEntityCacheSeeded)
		return;
#else
	if (g_pEntityList)
		return;
#endif

	for (int i = 0; i < MAX_EDICTS; ++i)
		g_pEntityCache[i] = EdictIndexToEntity(i);

#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)
	g_bEntityCacheSeeded = g_pEntityList != nullptr;
#endif
}

// CCServerNetworkProperty::GetNetworkParent() calls m_hParent.Get(), which is the SDK's own inline
// and dereferences g_pEntityList *unconditionally*. If gEntList resolution fails, calling it faults,
// so route through the safe handle fallback instead.
static inline CCServerNetworkProperty* GetNetworkParentSafe(CCServerNetworkProperty* pProp)
{
	if (!pProp || !pProp->m_hParent.IsValid())
		return nullptr;

	if (g_pEntityList)
		return pProp->GetNetworkParent(); // 32x: the SDK path is safe, keep it.

	CBaseEntity* pParent = EHandleToEntity(&pProp->m_hParent);
	return pParent ? (CCServerNetworkProperty*)pParent->NetworkProp() : nullptr;
}

static DTVarByOffset m_Hands_Offset("DT_GMOD_Player", "m_Hands");
static inline CBaseEntity* GetGMODPlayerHands(const void* pPlayer)
{
	return EHandleToEntity((CBaseHandle*)m_Hands_Offset.GetPointer(pPlayer));
}

static DTVarByOffset m_hActiveWeapon_Offset("DT_BaseCombatCharacter", "m_hActiveWeapon");
static inline CBaseEntity* GetActiveWeapon(const void* pPlayer)
{
	return EHandleToEntity((CBaseHandle*)m_hActiveWeapon_Offset.GetPointer(pPlayer));
}

static DTVarByOffset m_hMyWeapons_Offset("DT_BaseCombatCharacter", "m_hMyWeapons", sizeof(CBaseCombatWeaponHandle));
static inline CBaseEntity* GetMyWeapon(const void* pPlayer, const int nWeaponSlot)
{
	return EHandleToEntity((CBaseCombatWeaponHandle*)m_hMyWeapons_Offset.GetPointerArray(pPlayer, nWeaponSlot));
}

// All weapon handles of a player as raw bytes, to notice when one of them changed.
static constexpr size_t nMyWeaponHandlesSize = MAX_WEAPONS * sizeof(CBaseCombatWeaponHandle);
static inline const void* GetMyWeaponHandles(const void* pPlayer)
{
	return m_hMyWeapons_Offset.GetPointerArray(pPlayer, 0);
}

static DTVarByOffset m_hViewModel_Offset("DT_BasePlayer", "m_hViewModel", sizeof(CBasePlayer::CBaseViewModelHandle));
static inline CBaseViewModel* GetViewModel(const void* pPlayer, const int nViewModelSlot)
{
	return (CBaseViewModel*)EHandleToEntity((CBasePlayer::CBaseViewModelHandle*)m_hViewModel_Offset.GetPointerArray(pPlayer, nViewModelSlot));
}

// Don't use CBasePlayer::GetObserverMode()/GetObserverTarget(), they are virtual and GMod's vtable differs from the SDK.
// On current x86-64 branch builds the SDK's slots hold SetObserverMode(int) and ObserverUse(bool) instead.
static DTVarByOffset m_iObserverMode_Offset("DT_BasePlayer", "m_iObserverMode");
static inline int GetObserverMode(const void* pPlayer)
{
	return *(const int*)m_iObserverMode_Offset.GetPointer(pPlayer);
}

static DTVarByOffset m_hObserverTarget_Offset("DT_BasePlayer", "m_hObserverTarget");
static inline CBaseEntity* GetObserverTarget(const void* pPlayer)
{
	return EHandleToEntity((CBaseHandle*)m_hObserverTarget_Offset.GetPointer(pPlayer));
}

// DT_LocalPlayerExclusive is based off CBasePlayer! So we don't need to get it first as offsets are all for CBasePlayer
static DTVarByOffset m_hViewEntity_Offset("DT_LocalPlayerExclusive", "m_hViewEntity");
static inline CBaseEntity* GetViewEntity(void* pPlayer)
{
	return EHandleToEntity((CBaseHandle*)m_hViewEntity_Offset.GetPointer(pPlayer));
}

static DTVarByOffset m_Collision_Offset("DT_BaseEntity", "m_Collision");
static inline CCollisionProperty* GetEntityCollisionProperty(const void* pEnt)
{
	return (CCollisionProperty*)m_Collision_Offset.GetPointer(pEnt);
}

static DTVarByOffset m_CollisionMins_Offset("DT_CollisionProperty", "m_vecMins");
static inline const Vector& GetCollisionPropertyMins(const void* pCollisionProp)
{
	return *(Vector*)m_CollisionMins_Offset.GetPointer(pCollisionProp);
}

static DTVarByOffset m_CollisionMaxs_Offset("DT_CollisionProperty", "m_vecMaxs");
static inline const Vector& GetCollisionPropertyMaxs(const void* pCollisionProp)
{
	return *(Vector*)m_CollisionMaxs_Offset.GetPointer(pCollisionProp);
}

static ConVar networking_bind_gmodhands_to_player("holylib_networking_bind_gmodhands_to_player", "1", 0, "If enabled, the GMOD Hands / Player:SetHands entity will be bound to the player and only networked if the player is networked");
static ConVar networking_bind_viewmodels_to_player("holylib_networking_bind_viewmodels_to_player", "1", 0, "If enabled, the viewmodels will be bound to the player and only networked if the player is networked");
static ConVar networking_cachedump("holylib_networking_cachedump", "0", 0, "Debug. You wouldn't need this...");
static ConVar networking_areasplit("holylib_networking_areasplit", "0", 0, "PVS entities are split into areas");
static ConVar networking_fastcharactertransmit("holylib_networking_fastcharactertransmit", "1", 0, "Experimental");
static ConVar networking_bind_manipulators("holylib_networking_bind_manipulators", "1", 0, "If enabled, bone and flex manipulators of always transmitted entities are sent with them instead of asking the parent's ShouldTransmit for every recipient, which runs the parent's UpdateTransmitState (a Lua call for scripted entities)");

// GMod's bone and flex manipulators (Entity:ManipulateBone*, Entity:SetFlex*) are full check entities parented to the
// manipulated entity. Their ShouldTransmit returns the parent's ShouldTransmit for every recipient, and with the base
// implementation that runs the parent's UpdateTransmitState again. For a parent in the always transmit state the answer
// is FL_EDICT_ALWAYS, so such a manipulator is sent like an always transmitted entity, still honouring its own prevent bit.
// Players are excluded since their answer depends on the recipient. They are recognized by their network class.
// The parent is checked again for every recipient: during the tick a manipulator can be detached, and its parent can
// change its transmit state.
static inline bool HasAlwaysTransmittedParent(CCServerNetworkProperty* pNetProp)
{
	CCServerNetworkProperty* pParent = GetNetworkParentSafe(pNetProp);
	edict_t* pParentEdict = pParent ? pParent->edict() : nullptr;
	if (!pParentEdict || pParentEdict->m_EdictIndex <= gpGlobals->maxClients)
		return false;

	return (pParentEdict->m_fStateFlags & (FL_EDICT_ALWAYS|FL_EDICT_DONTSEND)) == FL_EDICT_ALWAYS;
}

static inline bool IsManipulatorOfAlwaysTransmitted(edict_t* pEdict)
{
	CCServerNetworkProperty* pNetProp = static_cast<CCServerNetworkProperty*>(pEdict->GetNetworkable());
	ServerClass* pServerClass = pNetProp ? pNetProp->m_pServerClass : nullptr;
	const char* pNetworkName = pServerClass ? pServerClass->GetName() : nullptr;
	if (!pNetworkName || (strcmp(pNetworkName, "CBoneManipulate") != 0 && strcmp(pNetworkName, "CFlexManipulate") != 0))
		return false;

	return HasAlwaysTransmittedParent(pNetProp);
}

struct EntityTransmitCache // Well.... Still kinda acts as a tick-based cache, though its a good foundation now.
{
	// Updates the cache for the current tick
	void UpdateEntities(const unsigned short *pEdictIndices, const int nEdicts)
	{
		if (!world_edict || !gpGlobals) // ServerActivate may never have run (require("holylib") / runtime enable)
			return;

		m_bIsActivelyNetworking = true;

#if NETWORKING_STATE_DEBUGGING
		memset(pEntityTransmitStates, 0, sizeof(pEntityTransmitStates));
#endif
		Plat_FastMemset(&pAlwaysTransmitBits, 0, sizeof(pAlwaysTransmitBits) * 4); // Again, very "safe"
		nAlwaysTransmitPlayerCount = 0;
		//pAlwaysTransmitBits.ClearAll();
		//pNeverTransmitBits.ClearAll();
		//pPVSTransmitBits.ClearAll();
		//pFullTransmitBits.ClearAll();

		nPVSEdictCount = -1;
		nFullEdictCount = -1; // -1 so that we can use preincrement :hehe:
		nAlwaysManipulatorCount = -1;
		Plat_FastMemset(pPVSEntityList, 0, sizeof(pPVSEntityList) * 2); // * 2 to also clear nFullEdictList which lies directly after it in memory. I know. very "safe" but I want this in 1 call

		// nAreaEntities is ~1 MB (255 areas x 512 entities) and only AddPVSEntity ever fills it, which only
		// runs with areasplit enabled. Clearing it unconditionally every tick evicts a megabyte of cache
		// immediately before the transmit loop wants that cache. The flag keeps a runtime 1 -> 0 flip
		// correct: we still do one final clear, so EntityRemoved never walks a stale nCount.
		const bool bBindManipulators = networking_bind_manipulators.GetBool();
		const bool bAreaSplit = networking_areasplit.GetBool();
		if (bAreaSplit || m_bAreaCacheFilled)
		{
			Plat_FastMemset(nAreaEntities, 0, sizeof(nAreaEntities));
			m_bAreaCacheFilled = bAreaSplit;
		}

		// NOTE: g_pEntityCache was already rebuilt for this tick by the caller, before it touched
		//       g_pPlayerTransmitCache - see RebuildEntityCacheForTick.

		// First we build data based off all players
		bool bBindGmodHandsToPlayer = networking_bind_gmodhands_to_player.GetBool();
		bool bBindViewModelsToPlayer = networking_bind_viewmodels_to_player.GetBool();
		if (networking_fastcharactertransmit.GetBool())
		{
			for (int iPlayerIndex = 1; iPlayerIndex <= gpGlobals->maxClients; ++iPlayerIndex)
			{
				CBaseEntity* pPlayer = g_pEntityCache[iPlayerIndex];
				if (!pPlayer)
					continue;

				if (bBindGmodHandsToPlayer)
				{
					CBaseEntity* pHands = GetGMODPlayerHands(pPlayer);
					edict_t* pHandsEdict = pHands ? pHands->edict() : nullptr;
					if (pHandsEdict)
						pNeverTransmitBits.Set(pHandsEdict->m_EdictIndex); // We make hands never transmit by default simply to save performance by reducing PVS checks.
				}

				if (bBindViewModelsToPlayer)
				{
					for (int i=0; i<MAX_VIEWMODELS; ++i)
					{
						CBaseEntity* pViewModel = GetViewModel(pPlayer, i);
						edict_t* pViewModelEdict = pViewModel ? pViewModel->edict() : nullptr;
						if (pViewModelEdict)
							pNeverTransmitBits.Set(pViewModelEdict->m_EdictIndex);
					}
				}

				// Now let's also mark all weapons a player has so that they won't later enter into useless PVS checks.
				for (int i=0; i<MAX_WEAPONS; ++i)
				{
					CBaseEntity *pWeapon = GetMyWeapon(pPlayer, i);
					edict_t* pWeaponEdict = pWeapon ? pWeapon->edict() : nullptr;
					if (pWeaponEdict)
						pNeverTransmitBits.Set(pWeaponEdict->m_EdictIndex);
				}
			}
		}

		for (int i=0; i < nEdicts; ++i)
		{
			int iEdict = pEdictIndices[i];
			edict_t *pEdict = &world_edict[iEdict];

			if (pNeverTransmitBits.IsBitSet(iEdict))
				continue;

			const int nFlags = pEdict->m_fStateFlags & (FL_EDICT_DONTSEND|FL_EDICT_ALWAYS|FL_EDICT_PVSCHECK|FL_EDICT_FULLCHECK);
			if (nFlags & FL_EDICT_DONTSEND)
			{
				// RaphaelIT7: Just because FL_EDICT_DONTSEND is set, doesn't mean it cannot be transmitted....
				//pNeverTransmitBits.Set(iEdict);
				continue;
			}

			if (nFlags & FL_EDICT_ALWAYS)
			{
				MarkAlwaysTransmit(pEdict, iEdict);
				continue;
			}

			CBaseEntity* pEnt = g_pEntityCache[iEdict];
			if (pEnt)
			{
				if (pEnt->edict() != pEdict)
					Warning(PROJECT_NAME " - networking: Entity cache is unreliable! We are cooked!\n");

				if (nFlags == FL_EDICT_FULLCHECK)
				{
					if (bBindManipulators && IsManipulatorOfAlwaysTransmitted(pEdict))
					{
						pAlwaysManipulatorList[++nAlwaysManipulatorCount] = pEnt;
						continue;
					}

					pFullEntityList[++nFullEdictCount] = pEnt;
					pFullTransmitBits.Set(iEdict);
					continue;
				}

				if (nFlags & FL_EDICT_PVSCHECK)
				{
					if (networking_areasplit.GetBool())
						AddPVSEntity(pEnt);
					else
						pPVSEntityList[++nPVSEdictCount] = pEnt;

					pPVSTransmitBits.Set(iEdict);
					continue;
				}
			}

			// It remained? So never send it!
			// pNeverTransmitBits.Set(iEdict);
		}

		if (networking_cachedump.GetBool())
		{
			Msg("Fullcheck:\n");
			for (int i=0; i<=nFullEdictCount; ++i)
				Msg("    %i: %s[%i]\n", i, pFullEntityList[i]->GetClassname(), pFullEntityList[i]->edict()->m_EdictIndex);

			Msg("PVS:\n");
			for (int i=0; i<=nPVSEdictCount; ++i)
				Msg("    %i: %s[%i]\n", i, pPVSEntityList[i]->GetClassname(), pPVSEntityList[i]->edict()->m_EdictIndex);

			Msg("Never:\n");
			for (int i=0; i<=pNeverTransmitBits.GetNumBits(); ++i)
			{
				if (pNeverTransmitBits.IsBitSet(i))
					Msg("    %i\n", i);
			}

			Msg("Always:\n");
			for (int i=0; i<=pAlwaysTransmitBits.GetNumBits(); ++i)
			{
				if (pAlwaysTransmitBits.IsBitSet(i))
					Msg("    %i\n", i);
			}

			Msg("Always (players, transmitted through SetTransmit):\n");
			for (int i=0; i<nAlwaysTransmitPlayerCount; ++i)
				Msg("    %i\n", pAlwaysTransmitPlayers[i]);

			if (networking_areasplit.GetBool())
			{
				for (int nArea = 0; nArea<MAX_MAP_AREAS-1; ++nArea)
				{
					AreaCache& pArea = nAreaEntities[nArea];
					if (pArea.nCount == 0)
						continue;

					Msg("Area %i [%i]:\n", nArea+1, pArea.nCount);
					for (int i=0; i<pArea.nCount; ++i)
						Msg("    %i: %s[%i]\n", i, pArea.pEntities[i]->GetClassname(), pArea.pEntities[i]->edict()->m_EdictIndex);
				}
			}

			Msg("EntityCache:\n");
			for (int i=0; i<MAX_EDICTS; ++i)
				Msg("    %i: %s[%i]\n", i, g_pEntityCache[i] ? g_pEntityCache[i]->GetClassname() : "NULL", g_pEntityCache[i] ? g_pEntityCache[i]->edict()->m_EdictIndex : -1);

			if (networking_fastcharactertransmit.GetBool())
			{
				for (int iPlayerIndex = 1; iPlayerIndex <= gpGlobals->maxClients; ++iPlayerIndex)
				{
					CBaseEntity* pPlayer = g_pEntityCache[iPlayerIndex];
					if (!pPlayer)
						continue;

					Msg("Player - %i - %p:\n", iPlayerIndex, pPlayer);
					if (bBindGmodHandsToPlayer)
					{
						CBaseHandle* pHandsHandle = (CBaseHandle*)m_Hands_Offset.GetPointer(pPlayer);
						CBaseEntity* pHands = GetGMODPlayerHands(pPlayer);
						if (pHands)
							Msg("	GModHands - %p %i %i %i\n", pHandsHandle, pHandsHandle->ToInt(), pHandsHandle->GetEntryIndex(), pHandsHandle->GetSerialNumber());
					}

					if (bBindViewModelsToPlayer)
					{
						for (int i=0; i<MAX_VIEWMODELS; ++i)
						{
							CBasePlayer::CBaseViewModelHandle* pViewModelHandle = (CBasePlayer::CBaseViewModelHandle*)m_hViewModel_Offset.GetPointerArray(pPlayer, i);
							CBaseEntity* pViewModel = GetViewModel(pPlayer, i);
							if (pViewModel)
								Msg("	ViewModel: %i - %p %i %i %i\n", i, pViewModelHandle, pViewModelHandle->ToInt(), pViewModelHandle->GetEntryIndex(), pViewModelHandle->GetSerialNumber());
						}
					}

					// Now let's also mark all weapons a player has so that they won't later enter into useless PVS checks.
					for (int i=0; i<MAX_WEAPONS; ++i)
					{
						CBaseCombatWeaponHandle* pWeaponHandle = (CBaseCombatWeaponHandle*)m_hMyWeapons_Offset.GetPointerArray(pPlayer, i);
						CBaseEntity *pWeapon = GetMyWeapon(pPlayer, i);
						if (pWeapon)
							Msg("	Weapon: %i - %p %i %i %i\n", i, pWeaponHandle, pWeaponHandle->ToInt(), pWeaponHandle->GetEntryIndex(), pWeaponHandle->GetSerialNumber());
					}
				}
			}

			networking_cachedump.SetValue(0);
		}
	}

	// Marks an FL_EDICT_ALWAYS entity and the move parents that SetTransmit would force along with it.
	// Players are not marked: a set bit makes CheckTransmit skip their SetTransmit hook, which is the only place
	// that sends their bound attachments and weapons, so the owner would lose its viewmodels.
	// CheckTransmit calls SetTransmit on them for every recipient instead, which also forces the rest of the chain.
	inline void MarkAlwaysTransmit(edict_t* pEdict, int iEdict)
	{
		while (pEdict) // Stop if we got no edict / for example have no further parent
		{
			if (iEdict >= 1 && iEdict <= gpGlobals->maxClients)
			{
				AddAlwaysTransmitPlayer(iEdict);
				return;
			}

			pAlwaysTransmitBits.Set(iEdict);

			CCServerNetworkProperty *pEnt = static_cast<CCServerNetworkProperty*>(pEdict->GetNetworkable());
			if (!pEnt)
				break;

			CCServerNetworkProperty *pParent = GetNetworkParentSafe(pEnt);
			if (!pParent)
				break;

			pEdict = pParent->edict();
			if (pEdict)
				iEdict = pEdict->m_EdictIndex; // Source engine normally uses pParent->entindex() which needs no null check due to it using ENTINDEX internally
		}
	}

	inline void AddAlwaysTransmitPlayer(int iEdict)
	{
		for (int i=0; i<nAlwaysTransmitPlayerCount; ++i)
		{
			if (pAlwaysTransmitPlayers[i] == iEdict)
				return;
		}

		if (nAlwaysTransmitPlayerCount < MAX_PLAYERS)
			pAlwaysTransmitPlayers[nAlwaysTransmitPlayerCount++] = iEdict;
	}

	void EntityRemoved(CBaseEntity* pEntity, edict_t* pEdict)
	{
		// Not networking? Then we can easily skip all this since everything is cleared on the next transmit anyways
		if (!m_bIsActivelyNetworking)
			return;

		// Deleted during networking? Now you fked up.

		int nIndex = pEdict->m_EdictIndex;
		pAlwaysTransmitBits.Clear(nIndex);
		pNeverTransmitBits.Clear(nIndex);
		pPVSTransmitBits.Clear(nIndex);
		pFullTransmitBits.Clear(nIndex);

		// nEntityCluster[nIndex] = 0;
		// bDirtyEntities.Clear(nIndex);

		for (int i = 0; i<=nFullEdictCount; ++i)
		{
			CBaseEntity* pFullEnt = pFullEntityList[i];
			if (pFullEnt != pEntity)
				continue;

			if (i < nFullEdictCount)
				memmove(&pFullEntityList[i], &pFullEntityList[i + 1], (nFullEdictCount - i) * sizeof(CBaseEntity*));

			pFullEntityList[nFullEdictCount--] = nullptr;
			break;
		}

		for (int i = 0; i<=nPVSEdictCount; ++i)
		{
			CBaseEntity* pPVSEnt = pPVSEntityList[i];
			if (pPVSEnt != pEntity)
				continue;

			if (i < nPVSEdictCount)
				memmove(&pPVSEntityList[i], &pPVSEntityList[i + 1], (nPVSEdictCount - i) * sizeof(CBaseEntity*));

			pPVSEntityList[nPVSEdictCount--] = nullptr;
			break;
		}

		for (int i = 0; i<=nAlwaysManipulatorCount; ++i)
		{
			if (pAlwaysManipulatorList[i] != pEntity)
				continue;

			if (i < nAlwaysManipulatorCount)
				memmove(&pAlwaysManipulatorList[i], &pAlwaysManipulatorList[i + 1], (nAlwaysManipulatorCount - i) * sizeof(CBaseEntity*));

			pAlwaysManipulatorList[nAlwaysManipulatorCount--] = nullptr;
			break;
		}

		for (int nArea = 0; nArea<MAX_MAP_AREAS-1; ++nArea)
		{
			AreaCache& pArea = nAreaEntities[nArea];
			if (pArea.nCount == 0)
				continue;

			for (int i=0; i<pArea.nCount; ++i)
			{
				if (pArea.pEntities[i] != pEntity)
					continue;

				if (i < (pArea.nCount - 1))
					memmove(&pArea.pEntities[i], &pArea.pEntities[i + 1], (pArea.nCount - i - 1) * sizeof(CBaseEntity*));

				pArea.pEntities[--pArea.nCount] = nullptr; // nCount is a count here, unlike the preincremented lists above
				break;
			}
		}

		DevMsg(PROJECT_NAME " - networking: An entity (class: %s) was deleted during networking! This is utterly expensive, stop this >:(\n", pEntity->GetClassname());
	}

	// Only called with areasplit enabled
	void AddPVSEntity(CBaseEntity* pEntity)
	{
#if 0
		int nArea = Util::engineserver->GetArea(pEntity->GetAbsOrigin());
		if (nArea == 0)
		{
			// Pray that this fallback works
			nArea = Util::engineserver->GetArea(pEntity->WorldSpaceCenter());
		}
#endif
		const CCollisionProperty* pCollision = GetEntityCollisionProperty(pEntity);
		if (!pCollision)
		{
			int nArea = Util::engineserver->GetArea(pEntity->GetAbsOrigin());
			
			if (nArea >= MAX_MAP_AREAS || nArea <= 0)
			{
				pPVSEntityList[++nPVSEdictCount] = pEntity;
				return;
			}

			// We do -1 since nArea 0 is not actually an valid area
			AreaCache& pArea = nAreaEntities[nArea-1];
			if (pArea.nCount >= nMaxEntitiesPerArea)
			{
				pPVSEntityList[++nPVSEdictCount] = pEntity;
				return;
			}

			pArea.pEntities[pArea.nCount++] = pEntity;
			return;
		}

		const Vector& mins = GetCollisionPropertyMins( pCollision );
		const Vector& maxs = GetCollisionPropertyMaxs( pCollision );

		Vector vecResult;
		pCollision->CollisionToWorldSpace( mins, &vecResult );
		int minsArea = Util::engineserver->GetArea(vecResult);
		bool bMinsValid = minsArea < MAX_MAP_AREAS && minsArea > 0; // Area 0 is also invalid, thats why > 0

		pCollision->CollisionToWorldSpace( maxs, &vecResult );
		int maxsArea = Util::engineserver->GetArea(vecResult);
		bool bMaxsValid = maxsArea < MAX_MAP_AREAS && maxsArea > 0;

		if (!bMinsValid && !bMaxsValid)
		{
			Vector obbCenter;
			VectorLerp( mins, maxs, 0.5f, obbCenter );

			// Test center, in case you have things like a vent door where the mins/maxs may be out of bounds but not the center
			pCollision->CollisionToWorldSpace( obbCenter, &vecResult );
			minsArea = Util::engineserver->GetArea(vecResult);
			bMinsValid = minsArea < MAX_MAP_AREAS && minsArea > 0;
		}

		if (!bMinsValid && !bMaxsValid)
		{
			pPVSEntityList[++nPVSEdictCount] = pEntity;
			return;
		}

		if (bMinsValid)
		{
			AddEntityToArea(minsArea, pEntity);
		}

		// IMPORTANT: This entity stretches across to another area! Probably a door with an areaportal!
		//            If it crosses more than two areas, GG. Not our problem
		if (bMaxsValid && minsArea != maxsArea)
		{
			// We add them here too, an entity can be in two areas at once for our case
			AddEntityToArea(maxsArea, pEntity);
		}
	}

	inline void AddEntityToArea(int areaNum, CBaseEntity* pEntity)
	{
		// We do -1 since nArea 0 is not actually an valid area and we shifted all by 1
		AreaCache& pArea = nAreaEntities[areaNum-1];
		if (pArea.nCount >= nMaxEntitiesPerArea)
		{
			pPVSEntityList[++nPVSEdictCount] = pEntity;
			return;
		}

		pArea.pEntities[pArea.nCount++] = pEntity;
	}

	inline void FinishNetworking()
	{
		m_bIsActivelyNetworking = false;
	}

	bool m_bIsActivelyNetworking = false;
	bool m_bAreaCacheFilled = false; // Whether nAreaEntities holds anything worth clearing (see UpdateEntities).

	CBitVec<MAX_EDICTS> pAlwaysTransmitBits;
	CBitVec<MAX_EDICTS> pNeverTransmitBits;
	CBitVec<MAX_EDICTS> pPVSTransmitBits;
	CBitVec<MAX_EDICTS> pFullTransmitBits;

	// Edict indices of players that are the move parent of an always transmitted entity, see MarkAlwaysTransmit.
	int nAlwaysTransmitPlayerCount = 0;
	int pAlwaysTransmitPlayers[MAX_PLAYERS] = {0};

	// int nEntityCluster[MAX_EDICTS] = {0};
	// CBitVec<MAX_EDICTS> bDirtyEntities = {false}; // Their Cluster changed compared to last tick.

	// NOTE: They are preincrement, use <= in for loops!
	int nPVSEdictCount = -1; // NOTE: This will only contain entities that were unable to be fitted into an AreaCache! (only happens on overflow)
	int nFullEdictCount = -1;
	CBaseEntity* pPVSEntityList[MAX_EDICTS] = {nullptr};
	CBaseEntity* pFullEntityList[MAX_EDICTS] = {nullptr};

	// Full check entities sent like always transmitted ones, see IsManipulatorOfAlwaysTransmitted. Preincrement as well.
	int nAlwaysManipulatorCount = -1;
	CBaseEntity* pAlwaysManipulatorList[MAX_EDICTS] = {nullptr};

	/*
		If holylib_networking_areasplit is enabled
		we split all PVS entities into Areas
		This allows us to first check if an area even connects to our and only if so, we check the entities inside
		this save a LOT of performance as we heavily reduce PVS checks by checking areas first

		ToDo: Think about if we should move Areas with 1-2 entities into the nPVSEntityList simply because its probably quicker to do a PVS check than area? (verify)
	*/
	static constexpr int nMaxEntitiesPerArea = 512; // Max entities per area before we move them to PVS checks
	struct AreaCache
	{
		// Not preincremented - use < in for loops
		int nCount = 0;
		CBaseEntity* pEntities[nMaxEntitiesPerArea];
	};

	AreaCache nAreaEntities[MAX_MAP_AREAS-1]; // -1 since Area 0 is not a valid one so we save some bytes
};
static EntityTransmitCache g_nEntityTransmitCache;

static CBitVec<MAX_EDICTS> g_pForceWeaponTransmitIndexes;
void Networking_ForceWeaponTransmit(int entIndex, bool bForceTransmit) // Exposed for pvs.ForceWeaponTransmit
{
	if (bForceTransmit) {
		g_pForceWeaponTransmitIndexes.Set(entIndex);
	} else {
		g_pForceWeaponTransmitIndexes.Clear(entIndex);
	}
}

// The weapons a player holds this tick, in slot order (holylib_networking_transmit_weaponlist).
// hook_CBaseCombatCharacter_SetTransmit runs for every recipient that receives the player, and walking all MAX_WEAPONS
// handles each time costs far more than this. The list is only used while the player's weapon handles are byte for byte
// what they were when it was built: a weapon picked up, dropped or stripped later in the same tick sends every caller
// back to the slot scan. A deleted weapon keeps its handle, so OnEntityDeleted invalidates the lists instead.
struct PlayerWeaponList
{
	inline void Begin(const CBaseEntity* pPlayer, const int nTick)
	{
		nCount = 0;
		pOwner = pPlayer;
		nBuildTick = nTick;
		memcpy(pHandles, GetMyWeaponHandles(pPlayer), sizeof(pHandles));
	}

	inline void Add(CBaseEntity* pWeapon)
	{
		pWeapons[nCount++] = pWeapon;
	}

	inline bool IsValid(const CBaseEntity* pPlayer, const int nTick) const
	{
		return pOwner && pOwner == pPlayer && nBuildTick == nTick && memcmp(pHandles, GetMyWeaponHandles(pPlayer), sizeof(pHandles)) == 0;
	}

	inline void Invalidate()
	{
		pOwner = nullptr;
	}

	const CBaseEntity* pOwner = nullptr;
	int nBuildTick = 0;
	int nCount = 0;
	CBaseEntity* pWeapons[MAX_WEAPONS] = {nullptr};
	unsigned char pHandles[nMyWeaponHandlesSize] = {0};
};

// Full cache persisting across ticks, reset only when the player disconnects.
static ConVar* sv_stressbots = nullptr;
static ConVar networking_transmit_newweapons("holylib_networking_transmit_newweapons", "1", 0, "If enabled, weapons that a player equipped/was given are networked for the first x ticks");
static ConVar networking_transmit_ticks("holylib_networking_transmit_ticks", "-1", 0, "How many ticks to use for transmit_newweapons & transmit_onfullupdate. -1 will instead ensure they are networked until the client acknowledges them");
static ConVar networking_transmit_onfullupdate("holylib_networking_transmit_onfullupdate", "1", 0, "If enabled, players and their own weapons are transmitted for the first x ticks when they had a full update");
static ConVar networking_transmit_onfullupdate_networktoothers("holylib_networking_transmit_onfullupdate_networktoothers", "1", 0, "If enabled, any player that has a full update will be networked to everyone");
struct PlayerTransmitCache
{
	inline void NextTick(const CBaseEntity* pPlayer, const int nTick)
	{
		if (!sv_stressbots && g_pCVar)
			sv_stressbots = g_pCVar->FindVar("sv_stressbots");

		int nTransmitTicks = networking_transmit_ticks.GetInt();
		if (nTransmitTicks == -1) {
			CBaseClient* pClient = Util::GetClientByPlayer((const CBasePlayer*)pPlayer);
			if (pClient) {
				nLastAcknowledgedTick = pClient->GetMaxAckTickCount(); // pClient->m_nDeltaTick;
				// Verify: GetMaxAckTickCount may be inaccurate for our use case since we need m_nDeltaTick?
				// if (pClient->m_nDeltaTick != pClient->GetMaxAckTickCount())
				// 	DevMsg(PROJECT_NAME " - networking: Interesting... for client %i (ent index) the delta tick %i differs from the MaxAckTick %i (%i)\n", pPlayer->edict()->m_EdictIndex, pClient->m_nDeltaTick, pClient->GetMaxAckTickCount(), nFullUpdateTick);
				if (pClient->IsFakeClient() && sv_stressbots && !sv_stressbots->GetBool())
					nLastAcknowledgedTick = gpGlobals->tickcount;
			} else {
				DevMsg(PROJECT_NAME " - networking: Failed to get CBaseClient for player %i (ent index)\n", pPlayer->edict()->m_EdictIndex);
				nLastAcknowledgedTick = nTick - 100; // Fallback though should never happen
			}
		} else {
			nLastAcknowledgedTick = nTick - nTransmitTicks;
		}

		pWeaponList.Begin(pPlayer, nTick);
		for (int i=0; i<MAX_WEAPONS; ++i)
		{
			WeaponSlot& pSlot = pWeapons[i];
			CBaseEntity *pWeapon = GetMyWeapon(pPlayer, i);
			if (pWeapon)
				pWeaponList.Add(pWeapon);

			if (pWeapon && pWeapon->edict())
			{
				if (!pSlot.bIsValid || pWeapon != pSlot.pWeapon)
				{
					pSlot.bIsValid = true;
					pSlot.bIsNew = true;
					pSlot.nCreationTick = nTick;
					pSlot.pWeapon = pWeapon;
				} else if (pSlot.nCreationTick < nLastAcknowledgedTick) {
					pSlot.bIsNew = false;
				}

				pSlot.bAlwaysNetwork = g_pForceWeaponTransmitIndexes.IsBitSet(pWeapon->edict()->m_EdictIndex);

				nHighestWeaponSlot = i;
			} else {
				pSlot.bIsValid = false;
				pSlot.bIsNew = false;
				pSlot.bAlwaysNetwork = false;
			}
		}

		// If you have less weapons, they will be transmitted more frequently
		if (++nNextWeaponSlot >= nHighestWeaponSlot)
			nNextWeaponSlot = 0;
	}

	// After an entity was deleted, callers scan all weapon slots for the rest of the tick (see PlayerWeaponList).
	inline void InvalidateWeaponList()
	{
		pWeaponList.Invalidate();
	}

	void Reset()
	{
		Plat_FastMemset(this, 0, sizeof(PlayerTransmitCache));
	}

	void MarkFullUpdate()
	{
		// DevMsg(PROJECT_NAME " - networking: Triggered fullupdate %i\n", gpGlobals->tickcount);
		nFullUpdateTick = gpGlobals->tickcount;
	}

	bool InFullUpdate(int nTick) const
	{
		return nFullUpdateTick > nTick;
	}

	// If the client relative to his own last acknowledged tick
	bool InFullUpdate() const
	{
		// DevMsg(PROJECT_NAME " - networking: InFullUpdate %i - %i\n", nFullUpdateTick, nLastAcknowledgedTick);
		return nFullUpdateTick >= nLastAcknowledgedTick;
	}

	bool bIsValid = false;

	int nFullUpdateTick = 0;
	int nLastAreaNum = 0;
	int nLastAcknowledgedTick = 0;
	// CBitVec<MAX_EDICTS> pLastTransmitBits; // No use yet
	// CBitVec<MAX_EDICTS> pWeaponTransmitBits; // These are ALWAYS transmitted (unused)

	struct WeaponSlot
	{
		bool bIsNew = false; // Exists for quick checking to not have to compare numbers
		bool bIsValid = false;
		// Transmit state - in case an offhand weapon insists on being an ass requesting to be networked
		// NOTE: For this to take effect, a weapon must return TRANSMIT_ALWAYS inside Entity:UpdateTransmitState
		bool bAlwaysNetwork = false;
		int nCreationTick = 0; // For how many ticks a weapon is considered new
		CBaseEntity* pWeapon = nullptr; // in case a weapon is removed/given onto the same slot in a tick
	};

	// Used for rotating weapon slots when networking
	int nNextWeaponSlot = 0;
	int nHighestWeaponSlot = 0;
	WeaponSlot pWeapons[MAX_WEAPONS];

	PlayerWeaponList pWeaponList;
};
// NOTE: Index is playerslot / entindex - 1
static PlayerTransmitCache g_pPlayerTransmitCache[MAX_PLAYERS];

class NetworkingGameEventListener : public IGameEventListener2
{
public:
	NetworkingGameEventListener() = default;

	void FireGameEvent(IGameEvent* pEvent)
	{
		if (V_stricmp(pEvent->GetName(), "OnRequestFullUpdate") != 0)
			return;
		
		int nPlayerSlot = pEvent->GetInt("index");
		if (nPlayerSlot >= gpGlobals->maxClients || nPlayerSlot < 0)
		{
			Warning(PROJECT_NAME " - networking: Invalid OnRequestFullUpdate event! (Index: %i)\n", nPlayerSlot);
			return;
		}

		g_pPlayerTransmitCache[nPlayerSlot].MarkFullUpdate();
	}
};
static NetworkingGameEventListener g_pNetworkGameEventListener;

// Bounded storage shared only by recipients with byte-identical PVS data. Reset on map activation.
using TransmitPVSCache = Networking::PVSCache<sizeof(CCheckTransmitInfo::m_PVS), MAX_PLAYERS>;
static TransmitPVSCache g_pTransmitPVSCache;

struct TransmitPVSQuery
{
	explicit TransmitPVSQuery(bool bEnabled) : bPending(bEnabled) {}

	bool CheckHeadnode(const CCheckTransmitInfo* pInfo, int headnode)
	{
		// Most small entities use direct cluster bit tests. Do not hash/copy a
		// recipient's PVS unless an expensive headnode query actually needs it.
		if (bPending)
		{
			pContext = g_pTransmitPVSCache.FindContext(pInfo->m_PVS, pInfo->m_nPVSSize);
			bPending = false;
		}

		// Answer misses from the context's own copy, so a stored answer always belongs to the PVS it is stored under,
		// even if something changes the recipient's PVS buffer during the transmit.
		const unsigned char* pPVS = pContext ? pContext->pvs.data() : pInfo->m_PVS;
		const int nPVSSize = pContext ? pContext->size : pInfo->m_nPVSSize;
		return g_pTransmitPVSCache.CheckHeadnode(pContext, headnode, [pPVS, nPVSSize, headnode]() {
			return engine->CheckHeadnodeVisible(headnode, pPVS, nPVSSize) != 0;
		});
	}

	bool bPending; // The context is still to be looked up.
	TransmitPVSCache::Context* pContext = nullptr;
};

struct GlobalTransmitTickCache
{
	inline bool IsNewTick(int nTick)
	{
		return g_iLastCheckTransmit != nTick;
	}

	inline void NewTick(int nTick)
	{
		g_iLastCheckTransmit = nTick;

		g_bWasSeenByPlayer.ClearAll();
		// g_pAlwaysTransmitCacheBitVec.ClearAll();
		// Plat_FastMemset(g_bFilledDontTransmitWeaponCache, 0, sizeof(g_bFilledDontTransmitWeaponCache));
	}

	int g_iLastCheckTransmit = -1;
	// CBitVec<MAX_EDICTS> g_pAlwaysTransmitCacheBitVec;
	CBitVec<MAX_EDICTS> g_bWasSeenByPlayer;
	// bool g_bFilledDontTransmitWeaponCache[MAX_PLAYERS] = {0};
};
static GlobalTransmitTickCache g_pGlobalTransmitTickCache;

#if 0 // Would be needed for pvs.AddEntitiesToTransmit / this would need to be called after the HolyLib:PostCheckTransmit hook if we'd were to allow entity additions in there
void Networking_DoPostTransmitCheck(CCheckTransmitInfo* pInfo)
{
	// Needed as else in PackEntities_Normal the entity may not be packed
	// Causing a crash in CBaseServer::WriteDeltas as its missing the packed data then
	pInfo->m_pTransmitEdict->Or(g_pGlobalTransmitTickCache.g_bWasSeenByPlayer, &g_pGlobalTransmitTickCache.g_bWasSeenByPlayer);
}
#endif

static CBitVec<MAX_EDICTS> g_pDontTransmitCache; // Reset on every CServerGameEnts::CheckTransmit call
static Detouring::Hook detour_CBaseCombatCharacter_SetTransmit;
static Symbols::CBaseCombatCharacter_SetTransmit func_CBaseAnimating_SetTransmit;
static ConVar networking_transmit_all_weapons("holylib_networking_transmit_all_weapons", "1", 0, "By default all weapons are networked based on their PVS, though normally if they have an owner you might only want the active weapon to be networked");
static ConVar networking_transmit_all_weapons_to_owner("holylib_networking_transmit_all_weapons_to_owner", "1", 0, "By default all weapons are networked to the owner");
static ConVar networking_transmit_one_per_tick("holylib_networking_transmit_one_per_tick", "0", 0, "If enabled, one additional weapon is networked per tick");
static ConVar networking_fasttransmit("holylib_networking_fasttransmit", "1", 0, "Replaces CServerGameEnts::CheckTransmit with our own implementation");
static ConVar networking_transmit_profile("holylib_networking_transmit_profile", "0", 0, "If enabled, time our CheckTransmit by phase; read the result with holylib_networking_transmit_stats");
static ConVar networking_transmit_weaponlist("holylib_networking_transmit_weaponlist", "1", 0, "If enabled, a player's weapons are sent from a list built once per tick instead of checking every weapon slot for every recipient");
static ConVar networking_pvssnapshot("holylib_networking_pvssnapshot", "0", 0, "Experimental - Copy the PVS data of all PVS checked entities once per tick and check every recipient against that copy");

// Transmit profiling: timings are summed over all recipient passes since the last reset.
struct TransmitProfile
{
	double fTickSetup = 0.0;
	double fRecipientSetup = 0.0;
	double fFullCheck = 0.0;
	double fPVSCheck = 0.0;
	double fFinish = 0.0;
	double fCharacter = 0.0; // Time inside hook_CBaseCombatCharacter_SetTransmit, already part of the phases above.
	std::uint64_t nTicks = 0;
	std::uint64_t nPasses = 0;
	std::uint64_t nFullEntities = 0;
	std::uint64_t nAlwaysManipulators = 0;
	std::uint64_t nPVSEntities = 0;
	std::uint64_t nCharacterCalls = 0;
	std::uint64_t nCharacterListCalls = 0;
	std::uint64_t nCharacterWeapons = 0;
	std::uint64_t nCharacterSlotScans = 0;
	int nCharacterDepth = 0;

	void Reset() { *this = TransmitProfile(); }
};
static TransmitProfile g_pTransmitProfile;

static inline double TransmitProfileNow()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Times the outermost character SetTransmit call; weapons and viewmodels can re-enter it through their move parent.
struct TransmitProfileCharacterScope
{
	explicit TransmitProfileCharacterScope(bool bProfile) : bEnabled(bProfile)
	{
		if (bEnabled && g_pTransmitProfile.nCharacterDepth++ == 0)
		{
			bOuter = true;
			fStart = TransmitProfileNow();
		}
	}

	~TransmitProfileCharacterScope()
	{
		if (!bEnabled)
			return;

		--g_pTransmitProfile.nCharacterDepth;
		if (bOuter)
		{
			g_pTransmitProfile.fCharacter += TransmitProfileNow() - fStart;
			++g_pTransmitProfile.nCharacterCalls;
		}
	}

	bool bEnabled;
	bool bOuter = false;
	double fStart = 0.0;
};
// End of transmit profiling

static void hook_CBaseCombatCharacter_SetTransmit(CBaseCombatCharacter* pCharacter, CCheckTransmitInfo *pInfo, bool bAlways)
{
	// IMPORANT: Apparently CBaseCombatCharacter is inherited by more than just the player, so we MUST check m_EdictIndex!
	// The !pCharacterEdict test has to come first - it is what makes the m_EdictIndex reads below safe.
	edict_t* pCharacterEdict = pCharacter->edict();
	if (!pCharacterEdict || !func_CBaseAnimating_SetTransmit || !networking_fasttransmit.GetBool() || !networking_fastcharactertransmit.GetBool() || pCharacterEdict->m_EdictIndex <= 0 || pCharacterEdict->m_EdictIndex > gpGlobals->maxClients)
	{
		// Without it we won't do shit, simply because possibly missing a transmit can cause quite the issues.
		detour_CBaseCombatCharacter_SetTransmit.GetTrampoline<Symbols::CBaseCombatCharacter_SetTransmit>()(pCharacter, pInfo, bAlways);
		return;
	}

	if (pInfo->m_pTransmitEdict->Get(pCharacterEdict->m_EdictIndex)) // Already being networked!
		return;

	const TransmitProfileCharacterScope pProfileScope(networking_transmit_profile.GetBool());
	func_CBaseAnimating_SetTransmit(pCharacter, pInfo, bAlways); // Base transmit

	const bool bLocalPlayer = pInfo->m_pClientEnt == pCharacterEdict;
	if (networking_bind_gmodhands_to_player.GetBool())
	{
		CBaseEntity* pGMODHands = GetGMODPlayerHands(pCharacter);
		if (pGMODHands) {
			if (bLocalPlayer)
				pGMODHands->SetTransmit(pInfo, bAlways);
		}
	}

	if (networking_bind_viewmodels_to_player.GetBool())
	{
		for (int i=0; i < MAX_VIEWMODELS; ++i)
		{
			CBaseEntity* pViewModel = GetViewModel(pCharacter, i);
			if (pViewModel) {
				if (bLocalPlayer)
					pViewModel->SetTransmit(pInfo, bAlways);
			}
		}
	}

	if (networking_transmit_all_weapons.GetBool() || (bLocalPlayer && networking_transmit_all_weapons_to_owner.GetBool()))
	{
		const PlayerWeaponList& pWeaponList = g_pPlayerTransmitCache[pCharacterEdict->m_EdictIndex-1].pWeaponList;
		bool bListed = networking_transmit_weaponlist.GetBool() && pWeaponList.IsValid(pCharacter, gpGlobals->tickcount);
		if (bListed)
		{
			// Same weapons in the same order as the slot scan below, resolved once this tick.
			for (int i=0; i < pWeaponList.nCount; ++i)
			{
				pWeaponList.pWeapons[i]->SetTransmit(pInfo, bAlways);
				if (!pWeaponList.pOwner)
				{
					// An entity was deleted from inside SetTransmit, the remaining pointers may be gone.
					bListed = false;
					break;
				}
			}
		}

		if (bListed)
		{
			if (pProfileScope.bEnabled)
			{
				++g_pTransmitProfile.nCharacterListCalls;
				g_pTransmitProfile.nCharacterWeapons += pWeaponList.nCount;
			}
		} else {
			// Also finishes a list that was dropped halfway, SetTransmit returns early for what was already sent.
			for (int i=0; i < MAX_WEAPONS; ++i)
			{
				CBaseEntity *pWeapon = GetMyWeapon(pCharacter, i);
				if (!pWeapon)
					continue;

				// The local player is sent all of his weapons.
				pWeapon->SetTransmit(pInfo, bAlways);
			}

			if (pProfileScope.bEnabled)
				++g_pTransmitProfile.nCharacterSlotScans;
		}
	} else {
		CBaseEntity* pActiveWeapon = GetActiveWeapon(pCharacter);
		if (pActiveWeapon)
			pActiveWeapon->SetTransmit(pInfo, bAlways);

		/*
			If you're out for performance without wanting to lose default behavior, the following settings can improve performance while having just a little downside

			holylib_networking_transmit_all_weapons 0
			holylib_networking_transmit_all_weapons_to_owner 0
			holylib_networking_transmit_one_per_tick 1

			This way, ONLY the active weapon is always networked and 1 weapon every tick is additionally networked
			so after 255 Ticks a client will always have received an update for every weapon slot.

			NOTE:
			If you pick up a weapon and it does not become your active weapon
			it may happen that the client won't receive an update till that slot is reached leaving them unable to select the weapon

			Newer NOTE:
			If you don't need constant weapon updates, you can always keep networking_transmit_one_per_tick disabled and instead rely on
			holylib_networking_transmit_newweapons which will ensure players always know which weapons they have but won't receive constant updates about them.
		*/
		if (networking_transmit_one_per_tick.GetInt() == 1 || (networking_transmit_one_per_tick.GetInt() == 2 && bLocalPlayer))
		{
			int nSlot = g_pPlayerTransmitCache[pCharacterEdict->m_EdictIndex-1].nNextWeaponSlot;
			CBaseEntity *pWeapon = GetMyWeapon(pCharacter, nSlot);
			if ( pWeapon )
			{
				pWeapon->SetTransmit(pInfo, bAlways);
			}
		}

		const PlayerTransmitCache& pCache = g_pPlayerTransmitCache[pCharacterEdict->m_EdictIndex-1];
		if (bLocalPlayer)
		{
			if (networking_transmit_onfullupdate.GetBool() && pCache.InFullUpdate())
			{
				// DevMsg(PROJECT_NAME " - networking: Doing full weapon transmit for %i...\n", pCharacterEdict->m_EdictIndex);
				for (int i=0; i < MAX_WEAPONS; ++i)
				{
					CBaseEntity *pWeapon = GetMyWeapon(pCharacter, i);
					if (!pWeapon)
						continue;

					pWeapon->SetTransmit(pInfo, bAlways);
				}
				return; // We don't gotta do the thing below
			}

			if (networking_transmit_newweapons.GetBool())
			{
				for (int i=0; i < MAX_WEAPONS; ++i)
				{
					const PlayerTransmitCache::WeaponSlot& pWeaponSlot = pCache.pWeapons[i];
					if (pWeaponSlot.bIsNew)
					{
						CBaseEntity *pWeapon = GetMyWeapon(pCharacter, i);
						if (!pWeapon)
							continue;

						pWeapon->SetTransmit(pInfo, bAlways);
					}
				}
			}
		}

		// In some cases offhand weapons may want to be transmitted!
		// This normally is shit! Why would anyone need offhand weapons? Idk, some do!
		for (int i=0; i < MAX_WEAPONS; ++i)
		{
			const PlayerTransmitCache::WeaponSlot& pWeaponSlot = pCache.pWeapons[i];
			if (pWeaponSlot.bAlwaysNetwork)
			{
				CBaseEntity *pWeapon = GetMyWeapon(pCharacter, i);
				if (!pWeapon)
					continue;

				pWeapon->SetTransmit(pInfo, bAlways);
			}
		}
	}
}

static inline bool IsInPVS(CCServerNetworkProperty* netProp, const CCheckTransmitInfo* pInfo,
	TransmitPVSQuery& pPVSQuery)
{
	return netProp->IsInPVS(pInfo, [pInfo, &pPVSQuery](int headnode) {
		return pPVSQuery.CheckHeadnode(pInfo, headnode);
	});
}

static vec_t g_nTransmitRange = -1.0f;
// Will only work for ONE transmit!
void Networking_SetNextTransmitRange(vec_t nRange)
{
	g_nTransmitRange = nRange;
}

// Per-tick copy of the PVS data of every entity in the PVS list (holylib_networking_pvssnapshot).
// Most PVS checked entities aren't visible to a given recipient. Checking them against this contiguous copy
// avoids touching every entity object once per recipient; the entity is only used to send it or walk its parents.
// An entry is only used while the entity's PVS data is unchanged since Build(): a dirty entity, or one whose data
// was recomputed during this tick's transmit (pStale), goes through DoTransmitPVSCheck like before.
// The copy owns everything it reads, and an entity deleted during the transmit only loses its entry (EntityRemoved).
// Not noticed until the next tick: an entity that code running during the transmit moves and that something other
// than our checks then recomputes, and an entity that gets its first parent during the transmit.
struct PVSSnapshot
{
	static constexpr int nInlineClusters = 4;
	enum : unsigned short
	{
		FLAG_LIVE = 1 << 0, // No network property or an unusual area: always use DoTransmitPVSCheck.
		FLAG_HAS_PARENT = 1 << 1,
		FLAG_REMOVED = 1 << 2, // Deleted after Build(), the entity pointers of this entry are gone.
	};

	struct Entry
	{
		edict_t* pEdict;
		short nAreaNum;
		short nAreaNum2;
		short nClusterCount; // Negative: use the headnode.
		short nHeadNode;
		unsigned short nEdict;
		unsigned short nFlags;
		unsigned short pClusters[nInlineClusters];
	};

	void Build(int nTick)
	{
		nCount = 0;
		pStale.ClearAll();
		pClusterData.clear();
		memset(pEntryOfEdict, -1, sizeof(pEntryOfEdict));
		const EntityTransmitCache& pCache = g_nEntityTransmitCache;
		for (int i=0; i<=pCache.nPVSEdictCount; ++i)
		{
			CBaseEntity* pEnt = pCache.pPVSEntityList[i];
			edict_t* pEdict = pEnt ? pEnt->edict() : nullptr;
			if (!pEdict) // Skipped by the PVS loop as well.
				continue;

			const int nIndex = nCount++;
			Entry& pEntry = pEntries[nIndex];
			pEntry.pEdict = pEdict;
			pEntry.nEdict = static_cast<unsigned short>(pEdict->m_EdictIndex);
			pEntry.nFlags = 0;
			pEntity[nIndex] = pEnt;
			pEntryOfEdict[pEdict->m_EdictIndex] = static_cast<short>(nIndex);
			pClusterOffset[nIndex] = 0;

			CCServerNetworkProperty* pNetProp = static_cast<CCServerNetworkProperty*>(pEdict->GetNetworkable());
			pNetworkProperty[nIndex] = pNetProp;
			if (!pNetProp)
			{
				pEntry.nFlags = FLAG_LIVE;
				continue;
			}

			pNetProp->RecomputePVSInformation();
			const auto& pInfo = pNetProp->m_PVSInfo;
			pEntry.nAreaNum = static_cast<short>(pInfo.m_nAreaNum);
			pEntry.nAreaNum2 = static_cast<short>(pInfo.m_nAreaNum2);
			pEntry.nClusterCount = static_cast<short>(pInfo.m_nClusterCount);
			pEntry.nHeadNode = static_cast<short>(pInfo.m_nHeadNode);
			if (pEntry.nClusterCount > nInlineClusters)
			{
				// The engine frees and replaces this list when it recomputes the entity, so it is copied as well.
				pClusterOffset[nIndex] = static_cast<int>(pClusterData.size());
				pClusterData.insert(pClusterData.end(), pInfo.m_pClusters, pInfo.m_pClusters + pEntry.nClusterCount);
			} else {
				for (int nCluster=0; nCluster<pEntry.nClusterCount; ++nCluster)
					pEntry.pClusters[nCluster] = pInfo.m_pClusters[nCluster];
			}

			if (pEntry.nAreaNum < 0 || pEntry.nAreaNum >= MAX_MAP_AREAS || pEntry.nAreaNum2 < 0 || pEntry.nAreaNum2 >= MAX_MAP_AREAS)
				pEntry.nFlags |= FLAG_LIVE;

			if (GetNetworkParentSafe(pNetProp))
				pEntry.nFlags |= FLAG_HAS_PARENT;
		}

		nBuildTick = nTick;
		bValid = true;
	}

	inline bool IsValid(int nTick) const
	{
		return bValid && nBuildTick == nTick;
	}

	inline void Invalidate()
	{
		bValid = false;
	}

	// Called before PVS data is recomputed during the transmit, so that the entry isn't used anymore.
	inline void MarkStale(const edict_t* pEdict)
	{
		if ((pEdict->m_fStateFlags & FL_EDICT_DIRTY_PVS_INFORMATION) != 0)
			pStale.Set(pEdict->m_EdictIndex);
	}

	// The entity was deleted. A loop that is walking the entries sees this too: a ShouldTransmit callback of one
	// entity can delete another one that comes later.
	inline void EntityRemoved(const edict_t* pEdict)
	{
		const int nEdict = pEdict->m_EdictIndex;
		if (!bValid || nEdict < 0 || nEdict >= MAX_EDICTS)
			return;

		const int nIndex = pEntryOfEdict[nEdict];
		if (nIndex < 0)
			return;

		pEntryOfEdict[nEdict] = -1;
		pEntries[nIndex].nFlags |= FLAG_REMOVED;
		pEntity[nIndex] = nullptr;
		pNetworkProperty[nIndex] = nullptr;
	}

	bool bValid = false;
	int nBuildTick = -1;
	int nCount = 0;
	CBitVec<MAX_EDICTS> pStale;
	Entry pEntries[MAX_EDICTS];
	short pEntryOfEdict[MAX_EDICTS]; // Entry of an edict index, -1 without one.
	// Only needed when an entity is sent, has parents or needs the live check.
	CBaseEntity* pEntity[MAX_EDICTS];
	CCServerNetworkProperty* pNetworkProperty[MAX_EDICTS];
	// Cluster lists that don't fit into their entry, one after another.
	int pClusterOffset[MAX_EDICTS];
	std::vector<unsigned short> pClusterData;
};
static PVSSnapshot g_pPVSSnapshot;

// Counts the ShouldTransmit calls made from the PVS checks. Such a call can run Lua and change the world, which
// TransmitPVSSnapshot looks for before it reuses what it looked up earlier in the same pass.
static unsigned int g_nTransmitCallbacks = 0;

// If the entity is marked "check PVS" but it's in hierarchy, walk up the hierarchy looking for
// any parent which is also in the PVS. If none are found, then we don't need to worry about sending ourself.
static inline void TransmitIfParentVisible(CCServerNetworkProperty* netProp, CBaseEntity* pEnt, CCheckTransmitInfo *pInfo, TransmitPVSQuery& pPVSQuery)
{
	CCServerNetworkProperty *check = GetNetworkParentSafe(netProp);

	// BUG BUG:  I think it might be better to build up a list of edict indices which "depend" on other answers and then
	// resolve them in a second pass.  Not sure what happens if an entity has two parents who both request PVS check?
	while ( check )
	{
		edict_t *checkEdict = check->edict();
		if ( !checkEdict ) // RaphelIT7: This can happen when a server-only entity is parented to a networked entity!
			return;

		int checkIndex = checkEdict->m_EdictIndex;
		if (checkIndex < 0 || checkIndex >= MAX_EDICTS)
			return; // Garbage edict - indexing the cache or the bitvec with this reads out of bounds.

		// Parent already being sent
		if ( pInfo->m_pTransmitEdict->Get( checkIndex ) )
		{
			pEnt->SetTransmit( pInfo, true );
			return;
		}

		// Parent isn't transmitted, so we also shouldn't be transmitted.
		// if ( g_pDontTransmitCache.Get( checkIndex ) )
		//	break;

		const int checkFlags = checkEdict->m_fStateFlags & (FL_EDICT_DONTSEND|FL_EDICT_ALWAYS|FL_EDICT_PVSCHECK|FL_EDICT_FULLCHECK);
		if ( checkFlags & FL_EDICT_DONTSEND )
		{
			return;
		}

		if ( checkFlags & FL_EDICT_ALWAYS )
		{
			pEnt->SetTransmit( pInfo, true );
			return;
		}

		if ( checkFlags == FL_EDICT_FULLCHECK )
		{
			// do a full ShouldTransmit() check, may return FL_EDICT_CHECKPVS
			CBaseEntity *pCheckEntity = g_pEntityCache[checkIndex];
			if (!pCheckEntity)
				return; // RaphaelIT7: ABORT! We got a garbage edict?

			const int nFlags = pCheckEntity->ShouldTransmit( pInfo );
			++g_nTransmitCallbacks;
			// Assert( !(nFlags & FL_EDICT_FULLCHECK) );
			if ( nFlags & FL_EDICT_ALWAYS )
			{
				pCheckEntity->SetTransmit( pInfo, true );
				pEnt->SetTransmit( pInfo, true );
			}
			// Msg("Fucking shit, why did this happen! (%i - %i)\n", checkIndex, pEdict->m_EdictIndex);
			return;
		}

		// RaphaelIT7: ToDo - We can make an assumption here - if the entIndex of the parent is smaller than ours
		// we can assume it isn't in the PVS since it goes by ent index order
		// Else we can also mark the parent as being networked to save PVS checks
		if ( checkFlags & FL_EDICT_PVSCHECK )
		{
			// Check pvs
			g_pPVSSnapshot.MarkStale(checkEdict);
			check->RecomputePVSInformation();
			const bool bMoveParentInPVS = IsInPVS(check, pInfo, pPVSQuery);
			if ( bMoveParentInPVS )
			{
				pEnt->SetTransmit( pInfo, true );
				return;
			}
		}

		// Continue up chain just in case the parent itself has a parent that's in the PVS...
		check = GetNetworkParentSafe(check);
	}
}

// Very expensive!
static inline void DoTransmitPVSCheck(
	edict_t* pEdict, CBaseEntity* pEnt, const bool bIsHLTV, CCheckTransmitInfo *pInfo,
	const bool bForceTransmit, const int skyBoxArea, const Vector& clientPosition, const vec_t maxTransmitRange,
	TransmitPVSQuery& pPVSQuery
)
{
	CCServerNetworkProperty *netProp = static_cast<CCServerNetworkProperty*>( pEdict->GetNetworkable() );
	if ( !netProp )
	{
		Warning(PROJECT_NAME " - networking: Somehow CCServerNetworkProperty was NULL!\n");
		return;
	}

	g_pPVSSnapshot.MarkStale(pEdict); // AreaNum() below recomputes dirty PVS data.

	if ( bIsHLTV )
	{
		// for the HLTV/Replay we don't cull against PVS
		pEnt->SetTransmit( pInfo, netProp->AreaNum() == skyBoxArea );
		return;
	}

	// Always send entities in the player's 3d skybox.
	// Sidenote: call of AreaNum() ensures that PVS data is up to date for this entity
	const bool bSameAreaAsSky = netProp->AreaNum() == skyBoxArea;
	if ( bSameAreaAsSky )
	{
		pEnt->SetTransmit( pInfo, true );
		return;
	}

	// Check if we have a range set and if so skip transmit
	if (maxTransmitRange != -1.0f)
	{
		CCollisionProperty* pCollision = GetEntityCollisionProperty(pEnt);
		if (pCollision)
		{
			const vec_t dist = pCollision->WorldSpaceCenter().DistTo(clientPosition);
			float radius = pCollision->BoundingRadius();
			if ((dist - radius) > maxTransmitRange)
				return;
		}
	}

	const bool bInPVS = IsInPVS(netProp, pInfo, pPVSQuery);
	if ( bInPVS || bForceTransmit )
	{
		// only send if entity is in PVS
		pEnt->SetTransmit( pInfo, false );
		return;
	}

	TransmitIfParentVisible(netProp, pEnt, pInfo, pPVSQuery);
}

// A full check entity answers for every recipient, and may ask for the PVS check.
static inline void TransmitFullCheckEntity(
	CBaseEntity* pEnt, const bool bIsHLTV, CCheckTransmitInfo *pInfo,
	const bool bForceTransmit, const int skyBoxArea, const Vector& clientPosition, const vec_t maxTransmitRange,
	TransmitPVSQuery& pPVSQuery
)
{
	// do a full ShouldTransmit() check, may return FL_EDICT_CHECKPVS
	const int nFlags = pEnt->ShouldTransmit(pInfo);
	if (nFlags & FL_EDICT_ALWAYS)
	{
		pEnt->SetTransmit(pInfo, true);
		// g_pAlwaysTransmitCacheBitVec.Set( iEdict ); We do NOT do this since view models and such would also be included.
		return;
	}

	if (!(nFlags & FL_EDICT_PVSCHECK))
		return;

	// Now only PVS remains
	DoTransmitPVSCheck(pEnt->edict(), pEnt, bIsHLTV, pInfo, bForceTransmit, skyBoxArea, clientPosition, maxTransmitRange, pPVSQuery);
}

// The PVS entity loop of CheckTransmit using g_pPVSSnapshot, for a recipient that isn't HLTV and has no transmit range.
// Makes the same decisions in the same order as DoTransmitPVSCheck over pPVSEntityList.
static void TransmitPVSSnapshot(CCheckTransmitInfo *pInfo, const bool bForceTransmit, const int skyBoxArea,
	const Vector& clientPosition, TransmitPVSQuery& pPVSQuery)
{
	// Whether an area is connected to one of the recipient's areas, filled in for the areas entities are in.
	// Looked up again after a ShouldTransmit call, which can open or close an area portal.
	signed char pAreaConnected[MAX_MAP_AREAS];
	memset(pAreaConnected, -1, sizeof(pAreaConnected));
	unsigned int nCallbacks = g_nTransmitCallbacks;
	const auto IsAreaConnected = [pInfo, &pAreaConnected](int nArea) {
		signed char& nState = pAreaConnected[nArea];
		if (nState < 0)
		{
			nState = 0;
			for (int i=0; i<pInfo->m_AreasNetworked; ++i)
			{
				const int clientArea = pInfo->m_Areas[i];
				if (clientArea == nArea || CheckAreasConnected(clientArea, nArea))
				{
					nState = 1;
					break;
				}
			}
		}

		return nState != 0;
	};

	const PVSSnapshot& pSnapshot = g_pPVSSnapshot;
	for (int i=0; i<pSnapshot.nCount; ++i)
	{
		const PVSSnapshot::Entry& pEntry = pSnapshot.pEntries[i];
		const int iEdict = pEntry.nEdict;
		if ((pEntry.nFlags & PVSSnapshot::FLAG_REMOVED) || pInfo->m_pTransmitEdict->Get(iEdict) || g_pDontTransmitCache.Get(iEdict))
			continue;

		if (nCallbacks != g_nTransmitCallbacks)
		{
			memset(pAreaConnected, -1, sizeof(pAreaConnected));
			nCallbacks = g_nTransmitCallbacks;
		}

		CBaseEntity* pEnt = pSnapshot.pEntity[i];
		if ((pEntry.nFlags & PVSSnapshot::FLAG_LIVE) || (pEntry.pEdict->m_fStateFlags & FL_EDICT_DIRTY_PVS_INFORMATION) || pSnapshot.pStale.Get(iEdict))
		{
			DoTransmitPVSCheck(pEntry.pEdict, pEnt, false, pInfo, bForceTransmit, skyBoxArea, clientPosition, -1.0f, pPVSQuery);
			continue;
		}

		// Always send entities in the player's 3d skybox.
		if (pEntry.nAreaNum == skyBoxArea)
		{
			pEnt->SetTransmit(pInfo, true);
			continue;
		}

		// CCServerNetworkProperty::IsInPVS
		bool bInPVS = false;
		if (IsAreaConnected(pEntry.nAreaNum) || (pEntry.nAreaNum2 != 0 && IsAreaConnected(pEntry.nAreaNum2)))
		{
			if (pEntry.nClusterCount < 0)
			{
				bInPVS = pPVSQuery.CheckHeadnode(pInfo, pEntry.nHeadNode);
			} else {
				const unsigned short* pClusters = pEntry.nClusterCount <= PVSSnapshot::nInlineClusters ? pEntry.pClusters : &pSnapshot.pClusterData[pSnapshot.pClusterOffset[i]];
				const unsigned char* pPVS = pInfo->m_PVS;
				for (int nCluster = pEntry.nClusterCount; --nCluster >= 0; )
				{
					const int nClusterIndex = pClusters[nCluster];
					if (((int)(pPVS[nClusterIndex >> 3])) & BitVec_BitInByte(nClusterIndex))
					{
						bInPVS = true;
						break;
					}
				}
			}
		}

		if (bInPVS || bForceTransmit)
		{
			pEnt->SetTransmit(pInfo, false);
			continue;
		}

		if (pEntry.nFlags & PVSSnapshot::FLAG_HAS_PARENT)
			TransmitIfParentVisible(pSnapshot.pNetworkProperty[i], pEnt, pInfo, pPVSQuery);
	}
}

// The engine's CBaseViewModel::ShouldTransmit sends a player's viewmodels to the owner, to spectators watching
// that player in first person and to HLTV. Binding moves viewmodels and hands out of the ordinary entity checks
// and hook_CBaseCombatCharacter_SetTransmit only adds them for their owner, so this adds the other two viewers.
static void TransmitBoundAttachmentsToViewers(CBasePlayer* pRecipientPlayer, CCheckTransmitInfo* pInfo, int clientIndex)
{
	const bool bBindViewModels = networking_bind_viewmodels_to_player.GetBool();
	const bool bBindHands = networking_bind_gmodhands_to_player.GetBool();
	if (!networking_fastcharactertransmit.GetBool() || (!bBindViewModels && !bBindHands))
		return; // Nothing is bound, everything went through the ordinary checks.

	const CBitVec<MAX_EDICTS>& preventTransmit = g_pShouldPrevent[clientIndex];
	const auto TransmitAttachment = [pInfo, &preventTransmit](CBaseEntity* pEntity) {
		edict_t* pEdict = pEntity ? pEntity->edict() : nullptr;
		if (!pEdict || pEdict->m_EdictIndex <= 0 || pEdict->m_EdictIndex >= MAX_EDICTS ||
			(pEdict->m_fStateFlags & FL_EDICT_DONTSEND) || preventTransmit.Get(pEdict->m_EdictIndex))
			return;

		// Like in the engine, this also sends the move parent, so the owner comes along with its viewmodel.
		pEntity->SetTransmit(pInfo, true);
	};
	const auto TransmitAttachmentsOf = [&](CBaseEntity* pOwner) {
		if (bBindViewModels)
		{
			for (int i = 0; i < MAX_VIEWMODELS; ++i)
				TransmitAttachment(GetViewModel(pOwner, i));
		}

		if (bBindHands)
			TransmitAttachment(GetGMODPlayerHands(pOwner));
	};

	if (pInfo->m_pTransmitAlways) // HLTV gets them like everything else, it doesn't cull by PVS.
	{
		for (int iPlayerIndex = 1; iPlayerIndex <= gpGlobals->maxClients; ++iPlayerIndex)
		{
			if (g_pEntityCache[iPlayerIndex])
				TransmitAttachmentsOf(g_pEntityCache[iPlayerIndex]);
		}
		return;
	}

	if (GetObserverMode(pRecipientPlayer) != OBS_MODE_IN_EYE)
		return;

	CBaseEntity* pTarget = GetObserverTarget(pRecipientPlayer);
	edict_t* pTargetEdict = pTarget ? pTarget->edict() : nullptr;
	if (!pTargetEdict || pTarget == pRecipientPlayer || !pTarget->IsPlayer())
		return;

	// A target hidden from this spectator must not come back as the move parent of its attachments.
	if (preventTransmit.Get(pTargetEdict->m_EdictIndex))
		return;

	TransmitAttachmentsOf(pTarget);
}

static ConVar networking_fastpath("holylib_networking_fastpath", "0", 0, "Experimental - Cache BSP headnode visibility results per exact PVS until the map changes; all recipient transmit decisions still run");
static ConVar networking_fastpath_usecluster("holylib_networking_fastpath_usecluster", "1", 0, "Deprecated compatibility setting; fastpath always matches exact PVS data, never just an area or cluster");
static void NetworkingFastPathStats(const CCommand&)
{
	const auto& stats = g_pTransmitPVSCache.GetStats();
	Msg("HolyLib networking fastpath: contexts hit=%llu miss=%llu evicted=%llu bypass=%llu; headnodes hit=%llu miss=%llu\n",
		static_cast<unsigned long long>(stats.contextHits), static_cast<unsigned long long>(stats.contextMisses),
		static_cast<unsigned long long>(stats.contextEvictions), static_cast<unsigned long long>(stats.contextBypasses),
		static_cast<unsigned long long>(stats.nodeHits), static_cast<unsigned long long>(stats.nodeMisses));
}
static ConCommand networking_fastpath_stats("holylib_networking_fastpath_stats", NetworkingFastPathStats,
	"Show fastpath cache counters since module initialization or map activation", 0);

static void NetworkingTransmitStats(const CCommand& args)
{
	TransmitProfile& pProfile = g_pTransmitProfile;
	if (args.ArgC() > 1 && V_stricmp(args.Arg(1), "reset") == 0)
	{
		const int nDepth = pProfile.nCharacterDepth; // Keep an active scope balanced.
		pProfile.Reset();
		pProfile.nCharacterDepth = nDepth;
		Msg("HolyLib transmit profile: reset\n");
		return;
	}

	const double fTicks = pProfile.nTicks > 0 ? static_cast<double>(pProfile.nTicks) : 1.0;
	const double fPasses = pProfile.nPasses > 0 ? static_cast<double>(pProfile.nPasses) : 1.0;
	const double fTotal = pProfile.fTickSetup + pProfile.fRecipientSetup + pProfile.fFullCheck + pProfile.fPVSCheck + pProfile.fFinish;
	Msg("HolyLib transmit profile (%s): %llu ticks, %llu recipient passes, %.3f ms per tick in our CheckTransmit\n",
		networking_transmit_profile.GetBool() ? "recording" : "stopped",
		static_cast<unsigned long long>(pProfile.nTicks), static_cast<unsigned long long>(pProfile.nPasses), fTotal * 1000.0 / fTicks);
	Msg("  tick setup (entity lists, once per tick)  %8.3f ms/tick\n", pProfile.fTickSetup * 1000.0 / fTicks);
	Msg("  recipient setup (prevent/always bits)     %8.3f ms/tick\n", pProfile.fRecipientSetup * 1000.0 / fTicks);
	Msg("  full-check entities (ShouldTransmit)      %8.3f ms/tick, %.0f entities per pass, %.0f manipulators sent with their always transmitted parent\n",
		pProfile.fFullCheck * 1000.0 / fTicks, static_cast<double>(pProfile.nFullEntities) / fPasses,
		static_cast<double>(pProfile.nAlwaysManipulators) / fPasses);
	Msg("  PVS entities                              %8.3f ms/tick, %.0f entities per pass\n",
		pProfile.fPVSCheck * 1000.0 / fTicks, static_cast<double>(pProfile.nPVSEntities) / fPasses);
	Msg("  full updates and bound attachments        %8.3f ms/tick\n", pProfile.fFinish * 1000.0 / fTicks);
	Msg("  of which player SetTransmit hook          %8.3f ms/tick, %.0f calls per tick, %.1f weapons per listed call, %llu slot scans\n",
		pProfile.fCharacter * 1000.0 / fTicks, static_cast<double>(pProfile.nCharacterCalls) / fTicks,
		pProfile.nCharacterListCalls > 0 ? static_cast<double>(pProfile.nCharacterWeapons) / static_cast<double>(pProfile.nCharacterListCalls) : 0.0,
		static_cast<unsigned long long>(pProfile.nCharacterSlotScans));
}
static ConCommand networking_transmit_stats("holylib_networking_transmit_stats", NetworkingTransmitStats,
	"Show the timings recorded while holylib_networking_transmit_profile is 1; 'holylib_networking_transmit_stats reset' clears them", 0);
bool New_CServerGameEnts_CheckTransmit(IServerGameEnts* gameents, CCheckTransmitInfo *pInfo, const unsigned short *pEdictIndices, int nEdicts)
{
	vec_t maxTransmitRange = g_nTransmitRange;
	g_nTransmitRange = -1.0f;

	if (!networking_fasttransmit.GetBool() || !gpGlobals || !engine || !func_CBaseAnimating_SetTransmit || !world_edict || gpGlobals->maxClients > MAX_PLAYERS)
		return false; // Fail-safe: the engine's own CheckTransmit runs instead. (mdlcache is deliberately not
		              //            required anymore, see the MDLCACHE_CRITICAL_SECTION note further down.)

	// get recipient player's skybox: 3670181
	CBaseEntity *pRecipientEntity = Util::servergameents->EdictToBaseEntity(pInfo->m_pClientEnt);
	if (!pRecipientEntity)
		return true;
	
	CBasePlayer *pRecipientPlayer = static_cast<CBasePlayer*>(pRecipientEntity);
	const int skyBoxArea = GetSkybox3DArea(pRecipientPlayer);
	// current transmit client | player index / entindex - 1
	const int clientIndex = pInfo->m_pClientEnt->m_EdictIndex - 1;

	// BUG: Can this even happen? Probably, when people screw with the gameserver module & disable spawn safety
	if (clientIndex >= gpGlobals->maxClients || clientIndex < 0)
		return true; // We don't return false since we never want to transmit anything to a player in a invalid slot!

	const bool bProfile = networking_transmit_profile.GetBool();
	const double fProfileStart = bProfile ? TransmitProfileNow() : 0.0;

	CBaseEntity* pViewEntity = GetViewEntity(pRecipientPlayer);
	const Vector& clientPosition = pViewEntity ? pViewEntity->EyePosition() : pRecipientPlayer->EyePosition();
	// ToDo: Bring over's CS:GO code for InitialSpawnTime
	//const bool bIsFreshlySpawned = pRecipientPlayer->GetInitialSpawnTime()+3.0f > gpGlobals->curtime;

	// Deliberately NO MDLCACHE_CRITICAL_SECTION() here.
	// IMDLCache as declared in sourcesdk-minimal puts BeginLock/EndLock at vtable slots 33/34, but GMod's
	// shipped datacache.so - linux32 and linux64 alike - has them at 41/42. Going through the header therefore
	// dispatched EndLock into CMDLCache's real slot 34, a method taking (MDLHandle_t, int, float): it indexed
	// m_MDLDict with whatever was left in rsi and segfaulted inside datacache.so on every call. Slot 33 is
	// worse still - it looks like a lock acquire, so we were taking a lock we then never released.
	// Nothing is lost by dropping it: GMod's own BeginLock/EndLock are empty stubs in both binaries, and the
	// engine already wraps the whole CheckTransmit pass in its own critical section in SV_ComputeClientPacks.

	// pRecipientPlayer->IsHLTV(); Why do we not use IsHLTV()? Because its NOT a virtual function & the variables are fked
	const int nCurrentTick = gpGlobals->tickcount;
	const bool bIsHLTV = pInfo->m_pTransmitAlways != nullptr;
	const bool bFirstTransmit = g_pGlobalTransmitTickCache.IsNewTick(nCurrentTick);
	if (bFirstTransmit)
	{
		// Has to happen before anything reads g_pEntityCache. If gEntList resolution failed, the cache is
		// only ever as fresh as the last rebuild. Doing this inside UpdateEntities (below) left the player
		// loop reading entries a tick out of date and could hand a removed player to NextTick() freed.
		RebuildEntityCacheForTick();

		for (int iPlayerIndex = 1; iPlayerIndex <= gpGlobals->maxClients; ++iPlayerIndex)
		{
			CBaseEntity* pPlayer = g_pEntityCache[iPlayerIndex];
			if (!pPlayer)
				continue;

			g_pPlayerTransmitCache[iPlayerIndex-1].NextTick(pPlayer, nCurrentTick);
		}

		g_nEntityTransmitCache.UpdateEntities(pEdictIndices, nEdicts);
		g_pGlobalTransmitTickCache.NewTick(nCurrentTick);

		if (networking_pvssnapshot.GetBool() && !networking_areasplit.GetBool())
			g_pPVSSnapshot.Build(nCurrentTick);
		else
			g_pPVSSnapshot.Invalidate();
	}

	const double fProfileTick = bProfile ? TransmitProfileNow() : 0.0;

	// Sharing a final transmit bitset skips ShouldTransmit, SetTransmit and
	// full-update handling. Share only pure BSP queries; HLTV never queries the PVS.
	TransmitPVSQuery pPVSQuery(networking_fastpath.GetBool());

	g_pShouldPrevent[clientIndex].CopyTo(&g_pDontTransmitCache); // We combine Gmod's prevent transmit with also our things to remove unessesary checks.
	g_nEntityTransmitCache.pNeverTransmitBits.Or(g_pDontTransmitCache, &g_pDontTransmitCache);

	const bool bForceTransmit = sv_force_transmit_ents && sv_force_transmit_ents->GetBool(); // Only set when g_pCVar existed at ServerActivate
	// pInfo->m_pTransmitEdict->Or(g_pGlobalTransmitTickCache.g_pAlwaysTransmitCacheBitVec, pInfo->m_pTransmitEdict);
	pInfo->m_pTransmitEdict->Or(g_nEntityTransmitCache.pAlwaysTransmitBits, pInfo->m_pTransmitEdict);
	if (bIsHLTV)
		pInfo->m_pTransmitAlways->Or(g_nEntityTransmitCache.pAlwaysTransmitBits, pInfo->m_pTransmitAlways);

	// Like the engine forcing an always transmitted child's parents, but through SetTransmit so that the player's
	// attachments and weapons come along (see EntityTransmitCache::MarkAlwaysTransmit).
	for (int i=0; i<g_nEntityTransmitCache.nAlwaysTransmitPlayerCount; ++i)
	{
		CBaseEntity* pPlayer = g_pEntityCache[g_nEntityTransmitCache.pAlwaysTransmitPlayers[i]];
		if (pPlayer)
			pPlayer->SetTransmit(pInfo, true);
	}

	const double fProfileFull = bProfile ? TransmitProfileNow() : 0.0;
	// Their ShouldTransmit would answer FL_EDICT_ALWAYS, see IsManipulatorOfAlwaysTransmitted.
	for (int i=0; i<=g_nEntityTransmitCache.nAlwaysManipulatorCount; ++i)
	{
		CBaseEntity* pEnt = g_nEntityTransmitCache.pAlwaysManipulatorList[i];

		edict_t* pEntEdict = pEnt ? pEnt->edict() : nullptr;
		if (!pEntEdict)
			continue;

		const int iEdict = pEntEdict->m_EdictIndex;
		if (pInfo->m_pTransmitEdict->Get(iEdict) || g_pDontTransmitCache.Get(iEdict))
			continue;

		CCServerNetworkProperty* pNetProp = static_cast<CCServerNetworkProperty*>(pEntEdict->GetNetworkable());
		if (pNetProp && HasAlwaysTransmittedParent(pNetProp))
		{
			pEnt->SetTransmit(pInfo, true);
			continue;
		}

		// Detached, or the parent's transmit state changed, after the list was built this tick: ask it like before.
		TransmitFullCheckEntity(pEnt, bIsHLTV, pInfo, bForceTransmit, skyBoxArea, clientPosition, maxTransmitRange, pPVSQuery);
	}

	for (int i=0; i<=g_nEntityTransmitCache.nFullEdictCount; ++i)
	{
		CBaseEntity* pEnt = g_nEntityTransmitCache.pFullEntityList[i];

		edict_t* pEntEdict = pEnt ? pEnt->edict() : nullptr;
		if (!pEntEdict)
			continue;

		const int iEdict = pEntEdict->m_EdictIndex;
		if (pInfo->m_pTransmitEdict->Get(iEdict) || g_pDontTransmitCache.Get(iEdict))
			continue;

		TransmitFullCheckEntity(pEnt, bIsHLTV, pInfo, bForceTransmit, skyBoxArea, clientPosition, maxTransmitRange, pPVSQuery);
	}

	const double fProfilePVS = bProfile ? TransmitProfileNow() : 0.0;
	if (networking_areasplit.GetBool())
	{
		const int nClientArea = Util::engineserver->GetArea(clientPosition);
		for (int nArea = 0; nArea<MAX_MAP_AREAS-1; ++nArea)
		{
			EntityTransmitCache::AreaCache& pArea = g_nEntityTransmitCache.nAreaEntities[nArea];
			if (pArea.nCount == 0)
				continue;

			// +1 since we shifted everything by 1 to remove Area0 saving some KB
			if (!Util::engineserver->CheckAreasConnected(nClientArea, nArea+1))
				continue;

			for (int i=0; i<pArea.nCount; ++i)
			{
				CBaseEntity* pEnt = pArea.pEntities[i];

				edict_t* pEdict = pEnt->edict();
				const int iEdict = pEdict->m_EdictIndex;
				if (pInfo->m_pTransmitEdict->Get(iEdict) || g_pDontTransmitCache.Get(iEdict))
					continue;

				// Now only PVS remains
				DoTransmitPVSCheck(pEdict, pEnt, bIsHLTV, pInfo, bForceTransmit, skyBoxArea, clientPosition, maxTransmitRange, pPVSQuery);
			}
		}
	}

	if (g_pPVSSnapshot.IsValid(nCurrentTick) && !bIsHLTV && maxTransmitRange == -1.0f && !networking_areasplit.GetBool())
	{
		TransmitPVSSnapshot(pInfo, bForceTransmit, skyBoxArea, clientPosition, pPVSQuery);
	} else {
		for (int i=0; i<=g_nEntityTransmitCache.nPVSEdictCount; ++i)
		{
			CBaseEntity* pEnt = g_nEntityTransmitCache.pPVSEntityList[i];

			// EntityRemoved() only runs off the entity listener. Keep the guard for the resolution-failure fallback,
			// where an entity removed while we are still networking this tick can stay in the list.
			edict_t* pEdict = pEnt ? pEnt->edict() : nullptr;
			if (!pEdict)
				continue;

			const int iEdict = pEdict->m_EdictIndex;
			if (pInfo->m_pTransmitEdict->Get(iEdict) || g_pDontTransmitCache.Get(iEdict))
				continue;

			// Now only PVS remains
			DoTransmitPVSCheck(pEdict, pEnt, bIsHLTV, pInfo, bForceTransmit, skyBoxArea, clientPosition, maxTransmitRange, pPVSQuery);
		}
	}

	const double fProfileFinish = bProfile ? TransmitProfileNow() : 0.0;
	if (networking_transmit_onfullupdate.GetBool())
	{
		if (g_pPlayerTransmitCache[clientIndex].InFullUpdate())
		{
			for (int iPlayerIndex = 1; iPlayerIndex <= gpGlobals->maxClients; ++iPlayerIndex)
			{
				// Lets avoid trying to network invalid entity slots as else we trigger a crash/engine error in CBaseServer::WriteDeltaEntities
				if (!g_pEntityCache[iPlayerIndex])
					continue;

				// We mark all to transmit to they will receive the CBasePlayer's
				// but not all their weapon since that could cause a overflow due to the amount of data that could be sent at once
				pInfo->m_pTransmitEdict->Set(iPlayerIndex);
				if (bIsHLTV)
					pInfo->m_pTransmitAlways->Set(iPlayerIndex);
			}
		} else if (networking_transmit_onfullupdate_networktoothers.GetBool()) {
			// In this case, if any other player is having a full update, we network them to all others
			// simply because this ensures every player knows of every other players existence
			int nLastAcknowledgedTick = g_pPlayerTransmitCache[clientIndex].nLastAcknowledgedTick;
			for (int iPlayerIndex = 1; iPlayerIndex <= gpGlobals->maxClients; ++iPlayerIndex)
			{
				if (g_pEntityCache[iPlayerIndex] && g_pPlayerTransmitCache[iPlayerIndex-1].InFullUpdate(nLastAcknowledgedTick))
				{
					pInfo->m_pTransmitEdict->Set(iPlayerIndex);
					if (bIsHLTV)
						pInfo->m_pTransmitAlways->Set(iPlayerIndex);
				}
			}
		}
	}

	TransmitBoundAttachmentsToViewers(pRecipientPlayer, pInfo, clientIndex);
	pInfo->m_pTransmitEdict->Or(g_pGlobalTransmitTickCache.g_bWasSeenByPlayer, &g_pGlobalTransmitTickCache.g_bWasSeenByPlayer);

	if (bProfile)
	{
		TransmitProfile& pProfile = g_pTransmitProfile;
		if (bFirstTransmit)
		{
			pProfile.fTickSetup += fProfileTick - fProfileStart;
			++pProfile.nTicks;
		} else {
			pProfile.fRecipientSetup += fProfileTick - fProfileStart;
		}

		pProfile.fRecipientSetup += fProfileFull - fProfileTick;
		pProfile.fFullCheck += fProfilePVS - fProfileFull;
		pProfile.fPVSCheck += fProfileFinish - fProfilePVS;
		pProfile.fFinish += TransmitProfileNow() - fProfileFinish;
		pProfile.nFullEntities += g_nEntityTransmitCache.nFullEdictCount + 1;
		pProfile.nAlwaysManipulators += g_nEntityTransmitCache.nAlwaysManipulatorCount + 1;
		pProfile.nPVSEntities += g_nEntityTransmitCache.nPVSEdictCount + 1;
		++pProfile.nPasses;
	}

	return true;
}

void SV_FillHLTVData( CFrameSnapshot *pSnapshot, edict_t *edict, int iValidEdict )
{
	if ( pSnapshot->m_pHLTVEntityData && edict )
	{
		CHLTVEntityData *pHLTVData = &pSnapshot->m_pHLTVEntityData[iValidEdict];
		PVSInfo_t *pvsInfo = edict->GetNetworkable()->GetPVSInfo();
		if ( pvsInfo->m_nClusterCount == 1 ) {
			// store cluster, if entity spawns only over one cluster
			pHLTVData->m_nNodeCluster = pvsInfo->m_pClusters[0];
		} else {
			// otherwise save PVS head node for larger entities
			pHLTVData->m_nNodeCluster = pvsInfo->m_nHeadNode | (1<<31);
		}

		// remember origin
		pHLTVData->origin[0] = pvsInfo->m_vCenter[0];
		pHLTVData->origin[1] = pvsInfo->m_vCenter[1];
		pHLTVData->origin[2] = pvsInfo->m_vCenter[2];
	}
}

#if MODULE_EXISTS_NETWORKINGREPLACEMENT
extern bool g_bRedirectPackEntity;
extern void NWR_SV_PackEntity(int edictIdx, edict_t* edict, ServerClass* pServerClass, CFrameSnapshot *pSnapshot);
#endif
static Symbols::PackWork_t_Process func_PackWork_t_Process;
static Symbols::SV_PackEntity func_SV_PackEntity;
struct PackWork_t
{
	int				nIdx;
	edict_t			*pEdict;
	CFrameSnapshot	*pSnapshot;

	static void Process( PackWork_t &item )
	{
#if MODULE_EXISTS_NETWORKINGREPLACEMENT
		if (g_bRedirectPackEntity)
		{
			NWR_SV_PackEntity( item.nIdx, item.pEdict, item.pSnapshot->m_pEntities[ item.nIdx ].m_pClass, item.pSnapshot );
			return;
		}
#endif

#if SYSTEM_LINUX
		func_PackWork_t_Process(item);
#else
		func_SV_PackEntity( item.nIdx, item.pEdict, item.pSnapshot->m_pEntities[ item.nIdx ].m_pClass, item.pSnapshot );
#endif
	}
};

static Symbols::InvalidateSharedEdictChangeInfos func_InvalidateSharedEdictChangeInfos;
static ConVar* sv_parallel_packentities;
static Detouring::Hook detour_PackEntities_Normal;
static ConVar networking_fastpacking("holylib_networking_fastpacking", "1", 0, "Makes PackEntities_Normal slightly faster");
void PackEntities_Normal(int clientCount, CGameClient **clients, CFrameSnapshot *snapshot)
{
	g_nEntityTransmitCache.FinishNetworking();

	if (!networking_fasttransmit.GetBool() || !networking_fastpacking.GetBool())
	{
		detour_PackEntities_Normal.GetTrampoline<Symbols::PackEntities_Normal>()(clientCount, clients, snapshot);
		return;
	}

	Assert( snapshot->m_nValidEntities >= 0 && snapshot->m_nValidEntities <= MAX_EDICTS );
	// tmZoneFiltered( TELEMETRY_LEVEL0, 50, TMZF_NONE, "%s %d", __FUNCTION__, snapshot->m_nValidEntities );

	int workItemCount = 0;
	static PackWork_t workItems[MAX_EDICTS];
	/*
		Formerly used CUtlVectorFixed< PackWork_t, MAX_EDICTS > workItems(0, snapshot->m_nValidEntities);
		But there is no point in allocating the entire thing every time we call, instead we can keep it static and keep track of the count.

		Entries from previous frames will remain but that shouldn't be a issue,
		since we use workItemCount to keep track of how many entries we actually have for this update.
	*/

	if (!gpGlobals || (g_pGlobalTransmitTickCache.IsNewTick(gpGlobals->tickcount)))
	{
		bool seen[MAX_EDICTS] = {false};
		for (int iClient = 0; iClient < clientCount; ++iClient)
		{
			if (!clients[iClient])
				continue;

			const CClientFrame *frame = clients[iClient]->m_pCurrentFrame;
			if (!frame)
				continue;

			for (int iValidEdict = 0; iValidEdict < snapshot->m_nValidEntities; ++iValidEdict)
			{
				const int index = snapshot->m_pValidEntities[iValidEdict];
				if (!seen[index] && frame->transmit_entity.Get(index))
				{
					seen[index] = true;
				}
			}
		}

		for (int iValidEdict = 0; iValidEdict < snapshot->m_nValidEntities; ++iValidEdict)
		{
			const int index = snapshot->m_pValidEntities[iValidEdict];

			edict_t* edict = &world_edict[index];
			SV_FillHLTVData(snapshot, edict, iValidEdict);
			if (!seen[index])
				continue;

			PackWork_t& w = workItems[workItemCount++];
			w.nIdx = index;
			w.pEdict = edict;
			w.pSnapshot = snapshot;
		}
	} else {
		for (int iValidEdict = 0; iValidEdict < snapshot->m_nValidEntities; ++iValidEdict)
		{
			int index = snapshot->m_pValidEntities[iValidEdict];

			edict_t* edict = &world_edict[index];
			SV_FillHLTVData(snapshot, edict, iValidEdict);
			if (!g_pGlobalTransmitTickCache.g_bWasSeenByPlayer.IsBitSet(index))
				continue;

			PackWork_t& w = workItems[workItemCount++];
			w.nIdx = index;
			w.pEdict = edict;
			w.pSnapshot = snapshot;
		}
	}

	if (!sv_parallel_packentities)
		sv_parallel_packentities = g_pCVar->FindVar("sv_parallel_packentities");

	// Process work
	if ( sv_parallel_packentities && sv_parallel_packentities->GetBool() )
	{
#if !defined(GMOD_X86_64)
		ParallelProcess( "PackWork_t::Process", workItems, workItemCount, &PackWork_t::Process );
#else
		ParallelProcess( workItems, workItemCount, &PackWork_t::Process );
#endif
	}
	else
	{
		intp c = workItemCount;
		for ( intp i = 0; i < c; ++i )
		{
			PackWork_t &w = workItems[ i ];
			PackWork_t::Process(w);
		}
	}

	func_InvalidateSharedEdictChangeInfos();
}

static Detouring::Hook detour_CServerGameEnts_CheckTransmit;
void hook_CServerGameEnts_CheckTransmit(IServerGameEnts* gameents, CCheckTransmitInfo *pInfo, const unsigned short *pEdictIndices, int nEdicts)
{
	if (!New_CServerGameEnts_CheckTransmit(gameents, pInfo, pEdictIndices, nEdicts))
		detour_CServerGameEnts_CheckTransmit.GetTrampoline<Symbols::CServerGameEnts_CheckTransmit>()(gameents, pInfo, pEdictIndices, nEdicts);
}

void CNetworkingModule::OnEntityDeleted(CBaseEntity* pEntity)
{
	edict_t* pEdict = pEntity->edict();
	if (!pEdict)
		return;

	g_nEntityTransmitCache.EntityRemoved(pEntity, pEdict);
	CleanupSetPreventTransmit(pEntity);
	g_pEntityCache[pEdict->m_EdictIndex] = nullptr;
	g_pForceWeaponTransmitIndexes.Clear(pEdict->m_EdictIndex);

	g_pPVSSnapshot.EntityRemoved(pEdict);

	// Deleted after this tick's weapon lists were built? One of them may hold it, so scan the slots again for the
	// rest of the tick. Deletions earlier in the frame only touch lists of a previous tick, which aren't used.
	if (gpGlobals && !g_pGlobalTransmitTickCache.IsNewTick(gpGlobals->tickcount))
	{
		for (PlayerTransmitCache& pCache : g_pPlayerTransmitCache)
			pCache.InvalidateWeaponList();
	}
}

void CNetworkingModule::OnEntityCreated(CBaseEntity* pEntity)
{
	const edict_t* pEdict = pEntity->edict();
	if (pEdict)
		g_pEntityCache[pEdict->m_EdictIndex] = pEntity;
}

void CNetworkingModule::ClientDisconnect(edict_t* pPlayer)
{
	if (pPlayer->m_EdictIndex > gpGlobals->maxClients)
		return;

	g_pPlayerTransmitCache[pPlayer->m_EdictIndex-1].Reset();
}

#if MODULE_EXISTS_PVS
void Networking_SwitchToPVSTransmit()
{
	if (!detour_CServerGameEnts_CheckTransmit.IsEnabled())
		return;

	Detour::DisableHook(&detour_CServerGameEnts_CheckTransmit);
}

void Networking_SwitchToOURTransmit()
{
	IModuleWrapper* pNetworking = g_pModuleManager.GetModuleByID(HOLYLIB_MODULEID_NETWORKING);
	if (detour_CServerGameEnts_CheckTransmit.IsEnabled() || (!pNetworking || !pNetworking->IsEnabled()))
		return;

	Detour::EnableHook(&detour_CServerGameEnts_CheckTransmit);
}
#endif

#if SYSTEM_WINDOWS
DETOUR_THISCALL_START()
	DETOUR_THISCALL_ADDFUNC2( hook_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer, GMOD_SetShouldPreventTransmitToPlayer, CBaseEntity*, CBasePlayer*, bool );
	DETOUR_THISCALL_ADDRETFUNC1( hook_CBaseEntity_GMOD_ShouldPreventTransmitToPlayer, bool, GMOD_ShouldPreventTransmitToPlayer, CBaseEntity*, CBasePlayer* );
	DETOUR_THISCALL_ADDFUNC1( hook_CGMOD_Player_CreateViewModel, CreateViewModel, CBasePlayer*, int );
	DETOUR_THISCALL_ADDFUNC2( hook_CBaseCombatCharacter_SetTransmit, SetTransmit, CBaseCombatCharacter*, CCheckTransmitInfo*, bool );
	DETOUR_THISCALL_ADDFUNC3( hook_CServerGameEnts_CheckTransmit, CheckTransmit, IServerGameEnts*, CCheckTransmitInfo*, const unsigned short*, int );
DETOUR_THISCALL_FINISH();
#endif

void CNetworkingModule::Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn)
{
	Util::gameeventmanager->AddListener(&g_pNetworkGameEventListener, "OnRequestFullUpdate", true);
}

static SendTable* playerSendTable;
static ServerClass* playerServerClass;
static CFrameSnapshotManager* framesnapshotmanager = nullptr;
static CSharedEdictChangeInfo* g_SharedEdictChangeInfo = nullptr;
static ServerClassCache *player_class_cache = nullptr;
#if ARCHITECTURE_X86_64
static ConVar networking_enableunsafe64x("holylib_networking_enableunsafe64x", "0", 0, "(only affects 64x) Enables 64x the full module code though it may crash on 64x.");
static ConVar networking_enabletransmit64x("holylib_networking_enabletransmit64x", "1", 0, "(only affects 64x) Enables the CheckTransmit fast path on 64x. Unlike enableunsafe64x this only covers the transmit code, which does not touch the broken CGameClient offsets.");
#endif
void CNetworkingModule::InitDetour(bool bPreServer)
{
	if (bPreServer)
		return;

	g_pTransmitPVSCache.Reset();
	g_pGlobalTransmitTickCache.g_iLastCheckTransmit = -1;
	Plat_FastMemset(g_pEntityCache, 0, sizeof(g_pEntityCache));
	g_pReplaceCServerGameEnts_CheckTransmit = false;
#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)
	g_bEntityCacheSeeded = false;
#endif
	for (int i=0; i<MAX_PLAYERS; ++i)
		g_pShouldPrevent[i].ClearAll();

	SourceSDK::FactoryLoader engine_loader("engine");
#if !defined(GMOD_X86_64) // x86-64 does not have this function / ChangeFrameLists don't use virtual functions!
	Detour::Create(
		&detour_AllocChangeFrameList, "AllocChangeFrameList",
		engine_loader.GetModule(), Symbols::AllocChangeFrameListSym,
		(void*)hook_AllocChangeFrameList, m_pID
	);
#endif

	DETOUR_PREPARE_THISCALL();
	SourceSDK::FactoryLoader server_loader("server");

	/*
	 * The transmit fast path (CServerGameEnts::CheckTransmit and everything it reads) does not touch
	 * CGameClient::m_pCurrentFrame, which is the member the 64x CBaseClient offsets get wrong. That
	 * single broken read lives in PackEntities_Normal, below the enableunsafe64x gate, so the transmit
	 * side can be installed on 64x independently of it.
	 *
	 * The GMOD_*ShouldPreventTransmitToPlayer pair must stay with it: they maintain g_pShouldPrevent,
	 * which New_CServerGameEnts_CheckTransmit reads. Installing the transmit path without them would
	 * silently leak entities that Lua hid via Entity:SetPreventTransmit.
	 */
#if ARCHITECTURE_X86_64
	if (networking_enabletransmit64x.GetBool() || networking_enableunsafe64x.GetBool())
#endif
	{
		bool bPreventTransmitHooksActive = false;

		/*
		 * These two MUST be installed as a pair or not at all.
		 *
		 * hook_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer does NOT call the trampoline: it
		 * records into g_pShouldPrevent and fully replaces the engine's own bookkeeping. If the
		 * matching reader (GMOD_ShouldPreventTransmitToPlayer) is not hooked, the engine keeps
		 * consulting its own state, which we then never write -> Entity:SetPreventTransmit silently
		 * becomes a no-op. Resolve both up-front and only hook when both are available, so a stale
		 * signature degrades to stock behaviour instead of a silent correctness bug.
		 */
		void* pGMODShouldPrevent = Detour::GetFunction(server_loader.GetModule(), Symbols::CBaseEntity_GMOD_ShouldPreventTransmitToPlayerSym);
		void* pGMODSetShouldPrevent = Detour::GetFunction(server_loader.GetModule(), Symbols::CBaseEntity_GMOD_SetShouldPreventTransmitToPlayerSym);
		if (pGMODShouldPrevent && pGMODSetShouldPrevent)
		{
			// Preserve the original entry address before detouring changes its prologue.
			// Other modules must reuse this address rather than scan patched code.
			func_GMODSetShouldPrevent = (Symbols::CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer)pGMODSetShouldPrevent;
			Detour::Create(
				&detour_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer, "CBaseEntity::GMOD_SetShouldPreventTransmitToPlayer",
				server_loader.GetModule(), Symbols::CBaseEntity_GMOD_SetShouldPreventTransmitToPlayerSym,
				(void*)DETOUR_THISCALL(hook_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer, GMOD_SetShouldPreventTransmitToPlayer), m_pID
			);

			Detour::Create(
				&detour_CBaseEntity_GMOD_ShouldPreventTransmitToPlayer, "CBaseEntity::GMOD_ShouldPreventTransmitToPlayer",
				server_loader.GetModule(), Symbols::CBaseEntity_GMOD_ShouldPreventTransmitToPlayerSym,
				(void*)DETOUR_THISCALL(hook_CBaseEntity_GMOD_ShouldPreventTransmitToPlayer, GMOD_ShouldPreventTransmitToPlayer), m_pID
			);

			bPreventTransmitHooksActive =
				detour_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer.IsEnabled() &&
				detour_CBaseEntity_GMOD_ShouldPreventTransmitToPlayer.IsEnabled();
			if (!bPreventTransmitHooksActive)
			{
				const bool bSetHookEnabled = detour_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer.IsEnabled();
				const bool bShouldHookEnabled = detour_CBaseEntity_GMOD_ShouldPreventTransmitToPlayer.IsEnabled();
				if (bSetHookEnabled)
					Detour::DisableHook(&detour_CBaseEntity_GMOD_SetShouldPreventTransmitToPlayer);
				if (bShouldHookEnabled)
					Detour::DisableHook(&detour_CBaseEntity_GMOD_ShouldPreventTransmitToPlayer);

				Warning(PROJECT_NAME " - networking: GMOD_(Set)ShouldPreventTransmitToPlayer did not install "
					"as a pair (set=%i should=%i) - leaving the entire transmit path to the engine.\n",
					bSetHookEnabled, bShouldHookEnabled);
			}
		} else {
			Warning(PROJECT_NAME " - networking: GMOD_(Set)ShouldPreventTransmitToPlayer did not both resolve "
				"(set=%p should=%p) - leaving the entire transmit path to the engine.\n",
				pGMODSetShouldPrevent, pGMODShouldPrevent);
		}

		if (bPreventTransmitHooksActive)
		{
			Detour::Create(
				&detour_CBaseCombatCharacter_SetTransmit, "CBaseCombatCharacter::SetTransmit",
				server_loader.GetModule(), Symbols::CBaseCombatCharacter_SetTransmitSym,
				(void*)DETOUR_THISCALL(hook_CBaseCombatCharacter_SetTransmit, SetTransmit), m_pID
			);

#if MODULE_EXISTS_PVS
			IModuleWrapper* pPVS = g_pModuleManager.GetModuleByID(HOLYLIB_MODULEID_PVS);
			if (pPVS && !pPVS->IsEnabled())
#endif
			{
				Detour::Create(
					&detour_CServerGameEnts_CheckTransmit, "CServerGameEnts::CheckTransmit",
					server_loader.GetModule(), Symbols::CServerGameEnts_CheckTransmitSym,
					(void*)DETOUR_THISCALL(hook_CServerGameEnts_CheckTransmit, CheckTransmit), m_pID
				);
			}

			SourceSDK::FactoryLoader datacache_loader("datacache");
			mdlcache = datacache_loader.GetInterface<IMDLCache>(MDLCACHE_INTERFACE_VERSION);

			func_CBaseAnimating_SetTransmit = (Symbols::CBaseCombatCharacter_SetTransmit)Detour::GetFunction(server_loader.GetModule(), Symbols::CBaseAnimating_SetTransmitSym);
			Detour::CheckFunction((void*)func_CBaseAnimating_SetTransmit, "CBaseAnimating::SetTransmit");

			g_pReplaceCServerGameEnts_CheckTransmit = true;
		}
	}

	// Everything below still touches the 64x-broken CBaseClient/CGameClient layout or has no 64x
	// signature at all (InvalidateSharedEdictChangeInfos is called unguarded by the packing path).
#if ARCHITECTURE_X86_64
	if (!networking_enableunsafe64x.GetBool()) // It exists so that when I get to working on it, I can easily test it.
		return;
#endif

	Detour::Create(
		&detour_SendTable_CullPropsFromProxies, "SendTable_CullPropsFromProxies",
		engine_loader.GetModule(), Symbols::SendTable_CullPropsFromProxiesSym,
		(void*)hook_SendTable_CullPropsFromProxies, m_pID
	);

	Detour::Create(
		&detour_CGMOD_Player_CreateViewModel, "CGMOD_Player::CreateViewModel",
		server_loader.GetModule(), Symbols::CGMOD_Player_CreateViewModelSym,
		(void*)DETOUR_THISCALL(hook_CGMOD_Player_CreateViewModel, CreateViewModel), m_pID
	);

#if SYSTEM_LINUX
	Detour::Create(
		&detour_PackEntities_Normal, "PackEntities_Normal",
		engine_loader.GetModule(), Symbols::PackEntities_NormalSym,
		(void*)PackEntities_Normal, m_pID
	);
#endif

	framesnapshotmanager = Detour::ResolveSymbol<CFrameSnapshotManager>(engine_loader, Symbols::g_FrameSnapshotManagerSym);
	Detour::CheckValue("get class", "framesnapshotmanager", framesnapshotmanager != nullptr);

#if defined(ARCHITECTURE_X86) && defined(SYSTEM_LINUX)
	g_BSPData = Detour::ResolveSymbol<CCollisionBSPData>(engine_loader, Symbols::g_BSPDataSym);
#else
	g_BSPData = Detour::ResolveSymbolWithOffset<CCollisionBSPData>(engine_loader.GetModule(), Symbols::g_BSPDataSym);
#endif
	Detour::CheckValue("get class", "CCollisionBSPData", g_BSPData != nullptr);
	
#if defined(ARCHITECTURE_X86) && defined(SYSTEM_LINUX)
	PropTypeFns* pPropTypeFns = Detour::ResolveSymbol<PropTypeFns>(engine_loader, Symbols::g_PropTypeFnsSym);
#else
	PropTypeFns* pPropTypeFns = Detour::ResolveSymbolWithOffset<PropTypeFns>(engine_loader.GetModule(), Symbols::g_PropTypeFnsSym);
#endif
	Detour::CheckValue("get class", "pPropTypeFns", pPropTypeFns != nullptr);

	if (pPropTypeFns)
	{
		for (size_t i = 0; i < DPT_NUMSendPropTypes; ++i)
			g_PropTypeFns[i] = pPropTypeFns[i]; // Crash any% speed run. I don't believe this will work
	}

#if SYSTEM_WINDOWS
	func_SV_PackEntity = (Symbols::SV_PackEntity)Detour::GetFunction(engine_loader.GetModule(), Symbols::SV_PackEntitySym);
	Detour::CheckFunction((void*)func_SV_PackEntity, "SV_PackEntity");
#endif

	func_InvalidateSharedEdictChangeInfos = (Symbols::InvalidateSharedEdictChangeInfos)Detour::GetFunction(engine_loader.GetModule(), Symbols::InvalidateSharedEdictChangeInfosSym);
	Detour::CheckFunction((void*)func_InvalidateSharedEdictChangeInfos, "InvalidateSharedEdictChangeInfos");

#if SYSTEM_LINUX
	func_PackWork_t_Process = (Symbols::PackWork_t_Process)Detour::GetFunction(engine_loader.GetModule(), Symbols::PackWork_t_ProcessSym);
	Detour::CheckFunction((void*)func_PackWork_t_Process, "PackWork_t::Process");
#endif

#if SYSTEM_WINDOWS // BUG: On Windows IModule::ServerActivate is not called if HolyLib gets loaded using: require("holylib")
	world_edict = Util::engineserver->PEntityOfEntIndex(0);
#endif
}

void CNetworkingModule::ServerActivate(edict_t* pEdictList, int edictCount, int clientMax)
{
	g_pTransmitPVSCache.Reset();
	g_pGlobalTransmitTickCache.g_iLastCheckTransmit = -1;
	if (pEdictList)
	{
		for (int i=0; i<edictCount; ++i)
			g_pEntityCache[i] = Util::GetCBaseEntityFromEdict(&pEdictList[i]);
	}

#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)
	g_bEntityCacheSeeded = pEdictList != nullptr && g_pEntityList != nullptr;
#endif

	if (g_pCVar)
		sv_force_transmit_ents = g_pCVar->FindVar("sv_force_transmit_ents");

	// Find player class (has DT_BasePlayer as a baseclass table)
	// We do this in ServerActivate since the engine only now hooked into the ServerClass allowing us to safely use them now.
	g_SharedEdictChangeInfo = Util::engineserver->GetSharedEdictChangeInfo();
	for(ServerClass *serverclass = Util::servergamedll->GetAllServerClasses(); serverclass->m_pNext != nullptr; serverclass = serverclass->m_pNext) {
		for (int i = 0; i < serverclass->m_pTable->GetNumProps(); ++i) {
			if (serverclass->m_pTable->GetProp(i)->GetDataTable() != nullptr && strcmp(serverclass->m_pTable->GetProp(i)->GetDataTable()->GetName(), "DT_BasePlayer") == 0 ) {
				playerSendTable = serverclass->m_pTable;
				playerServerClass = serverclass;
			}
		}
	}

	world_edict = Util::engineserver->PEntityOfEntIndex(0);
	CStandardSendProxies* sendproxies = Util::servergamedll->GetStandardSendProxies();
	local_sendtable_proxy = sendproxies->m_SendLocalDataTable;
	player_local_exclusive_send_proxy = new bool[playerSendTable->m_pPrecalc ? playerSendTable->m_pPrecalc->m_nDataTableProxies: 255];
	for(ServerClass *serverclass = Util::servergamedll->GetAllServerClasses(); serverclass->m_pNext != nullptr; serverclass = serverclass->m_pNext)
	{
		SendTable *sendTable = serverclass->m_pTable;
		auto serverClassCache = new ServerClassCache();
		if (sendTable == playerSendTable) 
			player_class_cache = serverClassCache;

		// Reuse unused variable
		sendTable->m_pPrecalc->m_pDTITable = (CDTISendTable*)serverClassCache;
		int propcount = sendTable->m_pPrecalc->m_Props.Count();
				
		CPropMapStack pmStack( sendTable->m_pPrecalc, sendproxies );
		serverClassCache->prop_offsets = new unsigned short[propcount];
		pmStack.Init();

		int t = 0;
		PropScan(0,sendTable, t);
		unsigned char proxyStack[256];

		RecurseStack(*serverClassCache, proxyStack, &sendTable->m_pPrecalc->m_Root , sendTable->m_pPrecalc);
		serverClassCache->prop_cull = new unsigned char[sendTable->m_pPrecalc->m_Props.Count()];
		serverClassCache->prop_propproxy_first = new unsigned short[sendTable->m_pPrecalc->m_nDataTableProxies];
		for (int i = 0; i < sendTable->m_pPrecalc->m_nDataTableProxies; ++i)
			serverClassCache->prop_propproxy_first[i] = INVALID_PROP_INDEX;

		for (int iToProp = 0; iToProp < sendTable->m_pPrecalc->m_Props.Count(); iToProp++)
		{ 
			const SendProp *pProp = sendTable->m_pPrecalc->m_Props[iToProp];

			pmStack.SeekToProp( iToProp );

			auto dataTableIndex = proxyStack[sendTable->m_pPrecalc->m_PropProxyIndices[iToProp]];
			serverClassCache->prop_cull[iToProp] = dataTableIndex;
			if (dataTableIndex < sendTable->m_pPrecalc->m_nDataTableProxies)
				serverClassCache->prop_propproxy_first[dataTableIndex] = iToProp;

			if ((intptr_t)pmStack.GetCurStructBase() != 0)
			{
				int offset = pProp->GetOffset() + (intptr_t)pmStack.GetCurStructBase() - 1;
						
				int elementCount = 1;
				int elementStride = 0;
				int propIdToUse = iToProp;
				int offsetToUse = offset;
				if ( pProp->GetType() == DPT_Array )
				{
					offset = pProp->GetArrayProp()->GetOffset() + (intptr_t)pmStack.GetCurStructBase() - 1;
					elementCount = pProp->m_nElements;
					elementStride = pProp->m_ElementStride;
					pProp = pProp->GetArrayProp();
					offsetToUse = (intptr_t)pmStack.GetCurStructBase() - 1;
				}

				serverClassCache->prop_offsets[propIdToUse] = offsetToUse;
				if (offset != 0)
				{
					if (offset != 0)
					{
						int offset_off = offset;
						for ( int j = 0; j < elementCount; j++ )
						{
							AddOffsetToList(*serverClassCache, offset_off, propIdToUse, j);
							if (pProp->GetType() == DPT_Vector) {
								AddOffsetToList(*serverClassCache, offset_off + 4, propIdToUse, j);
								AddOffsetToList(*serverClassCache, offset_off + 8, propIdToUse, j);
							} else if (pProp->GetType() == DPT_VectorXY) {
								AddOffsetToList(*serverClassCache, offset_off + 4, propIdToUse, j);
							}
							offset_off += elementStride;
						}
					}
				} else {
					serverClassCache->prop_special.push_back({propIdToUse});
				}
			} else {
				auto &datatableSpecial = serverClassCache->datatable_special[sendTable->m_pPrecalc->m_DatatableProps[pmStack.m_pIsPointerModifyingProxy[sendTable->m_pPrecalc->m_PropProxyIndices[iToProp]]->m_iDatatableProp]];
						
				datatableSpecial.propIndexes.push_back(iToProp);
				datatableSpecial.baseOffset = pmStack.m_iBaseOffset[sendTable->m_pPrecalc->m_PropProxyIndices[iToProp]];

				serverClassCache->prop_offsets[iToProp] = pProp->GetOffset();
			}
		}
	}
}

extern CGlobalVars *gpGlobals;
void CNetworkingModule::Shutdown()
{
	g_pTransmitPVSCache.Reset();
	g_pReplaceCServerGameEnts_CheckTransmit = false;

	// The default 64-bit transmit path does not resolve the snapshot manager.
	// Listener teardown is independent of the disabled snapshot restoration below.
	if (Util::gameeventmanager)
		Util::gameeventmanager->RemoveListener(&g_pNetworkGameEventListener);

	/*
	 * The code below to unload also belongs to sigsegv
	 * Source: https://github.com/rafradek/sigsegv-mvm/blob/e6a6cee305023f36e5b914872500ef8319317d71/src/mod/perf/sendprop_optimize.cpp#L1981-L2002
	 */
	/*for (CFrameSnapshot* pSnapshot : framesnapshotmanager->m_FrameSnapshots)
	{
		for (int i=0; i<pSnapshot->m_nNumEntities; ++i)
		{
			CFrameSnapshotEntry* pSnapshotEntry = pSnapshot->m_pEntities + i;
			if (!pSnapshotEntry)
				continue;

			PackedEntity* pPackedEntity = reinterpret_cast<PackedEntity*>(pSnapshotEntry->m_pPackedData);
			if (!pPackedEntity || !pPackedEntity->m_pChangeFrameList)
				continue;

			pPackedEntity->m_pChangeFrameList->Release();
			pPackedEntity->m_pChangeFrameList = detour_AllocChangeFrameList.GetTrampoline<Symbols::AllocChangeFrameList>()(pPackedEntity->m_pServerClass->m_pTable->m_pPrecalc->m_Props.Count(), gpGlobals->tickcount);
		}
	}

	// ToDo: Fix this crash. pPackedEntity will be invalid and it crashes when trying to access it's member.
	for (int i=0; i<MAX_EDICTS; ++i)
	{
		PackedEntity* pPackedEntity = reinterpret_cast<PackedEntity*>(framesnapshotmanager->m_pPackedData[i]);
		if (!pPackedEntity || !pPackedEntity->m_pChangeFrameList)
			continue;

		pPackedEntity->m_pChangeFrameList->Release();
		pPackedEntity->m_pChangeFrameList = detour_AllocChangeFrameList.GetTrampoline<Symbols::AllocChangeFrameList>()(pPackedEntity->m_pServerClass->m_pTable->m_pPrecalc->m_Props.Count(), gpGlobals->tickcount);
	}*/
}

static char strIndent = '\t';
static char strNewLine = '\n';
static void WriteString(std::string str, int nIndent, FileHandle_t pHandle)
{
	for (int i=0; i<nIndent; ++i)
	{
		g_pFullFileSystem->Write(&strIndent, 1, pHandle);
	}

	g_pFullFileSystem->Write(str.c_str(), str.length(), pHandle);
	g_pFullFileSystem->Write(&strNewLine, 1, pHandle);
}

#define APPEND_IF_PFLAGS_CONTAINS_SPROP(sprop) if(flags & SPROP_##sprop) pFlags.append(" " #sprop)
extern void WriteSendTable(SendTable* pTable, unordered_set<SendTable*>& pWrittenTables);
void WriteSendProp(SendProp* pProp, int nIndex, int nIndent, FileHandle_t pHandle, unordered_set<SendTable*>& pWrittenTables)
{
	std::string pIndex = "Index: ";
	pIndex.append(std::to_string(nIndex));
	WriteString(pIndex, nIndent, pHandle);

	std::string pName = "PropName: ";
	pName.append(pProp->GetName());
	WriteString(pName, nIndent, pHandle);

	std::string pExcludeDTName = "ExcludeName: ";
	pExcludeDTName.append(pProp->GetExcludeDTName() != nullptr ? pProp->GetExcludeDTName() : "NULL");
	WriteString(pExcludeDTName, nIndent, pHandle);

	std::string pOffset = "Offset: ";
	pOffset.append(std::to_string(pProp->GetOffset()));
	WriteString(pOffset, nIndent, pHandle);

	std::string pFlags = "Flags:";
	int flags = pProp->GetFlags();
	if (flags == 0) {
		pFlags.append(" None");
	} else {
		APPEND_IF_PFLAGS_CONTAINS_SPROP(UNSIGNED);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(COORD);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(NOSCALE);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(ROUNDDOWN);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(ROUNDUP);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(NORMAL);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(EXCLUDE);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(XYZE);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(INSIDEARRAY);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(PROXY_ALWAYS_YES);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(CHANGES_OFTEN);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(IS_A_VECTOR_ELEM);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(COORD_MP);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(COORD_MP_LOWPRECISION);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(COORD_MP_INTEGRAL);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(VARINT);
		APPEND_IF_PFLAGS_CONTAINS_SPROP(ENCODED_AGAINST_TICKCOUNT);
	}
	WriteString(pFlags, nIndent, pHandle);

	std::string pDataTableName = "Inherited: ";
	pDataTableName.append((pProp->GetType() == SendPropType::DPT_DataTable && pProp->GetDataTable()) ? pProp->GetDataTable()->GetName() : "NONE");
	

	if (pProp->GetDataTable())
		WriteSendTable(pProp->GetDataTable(), pWrittenTables);
	WriteString(pDataTableName, nIndent, pHandle);

	std::string pType = "Type: ";
	switch (pProp->GetType())
	{
	case SendPropType::DPT_Int:
		pType.append("DPT_Int");
		break;
	case SendPropType::DPT_Float:
		pType.append("DPT_Float");
		break;
	case SendPropType::DPT_Vector:
		pType.append("DPT_Vector");
		break;
	case SendPropType::DPT_VectorXY:
		// IMPORTANT: GMod uses this to network doubles! See SendPropTime64 which just redirects to SendPropVectorXY
		pType.append("DPT_VectorXY");
		break;
	case SendPropType::DPT_String:
		pType.append("DPT_String");
		break;
	case SendPropType::DPT_Array:
		pType.append("DPT_Array");
		break;
	case SendPropType::DPT_DataTable:
		pType.append("DPT_DataTable");
		break;
	case SendPropType::DPT_GMODTable:
		pType.append("DPT_GMODTable");
		break;
	default:
		pType.append("UNKNOWN(");
		pType.append(std::to_string(pProp->GetType()));
		pType.append(")");
	}
	WriteString(pType, nIndent, pHandle);

	std::string pElements = "NumElement: ";
	pElements.append(std::to_string(pProp->GetNumElements()));
	WriteString(pElements, nIndent, pHandle);

	std::string pBits = "Bits: ";
	pBits.append(std::to_string(pProp->m_nBits));
	WriteString(pBits, nIndent, pHandle);

	std::string pHighValue = "HighValue: ";
	pHighValue.append(std::to_string(pProp->m_fHighValue));
	WriteString(pHighValue, nIndent, pHandle);

	std::string pLowValue = "LowValue: ";
	pLowValue.append(std::to_string(pProp->m_fLowValue));
	WriteString(pLowValue, nIndent, pHandle);

	if (pProp->GetArrayProp())
	{
		std::string pArrayProp = "ArrayProp: ";

		WriteString(pArrayProp, nIndent, pHandle);
		WriteSendProp(pProp->GetArrayProp(), nIndex, nIndent + 1, pHandle, pWrittenTables);
	}
}

static void WriteSendTable(SendTable* pTable, FileHandle_t pHandle, unordered_set<SendTable*>& pWrittenTables)
{
	for (int i = 0; i < pTable->GetNumProps(); ++i) {
		SendProp* pProp = pTable->GetProp(i);
		
		WriteSendProp(pProp, i, 0, pHandle, pWrittenTables);

		g_pFullFileSystem->Write(&strNewLine, 1, pHandle);

		if (pWrittenTables.find(pTable) == pWrittenTables.end())
		{
			pWrittenTables.insert(pTable);
		}
	}
}

static std::string baseDTDumpFilePath = "holylib/dump/dt/";
void WriteSendTable(SendTable* pTable, unordered_set<SendTable*>& pWrittenTables)
{
	if (pWrittenTables.find(pTable) != pWrittenTables.end())
		return; // Already wrote it. Skipping...

	std::string fileName = baseDTDumpFilePath;
	fileName.append(pTable->GetName());
	fileName.append(".txt");

	FileHandle_t pHandle = g_pFullFileSystem->Open(fileName.c_str(), "wb", "MOD");
	if (!pHandle)
	{
		Warning(PROJECT_NAME " - DumpDT: Failed to open \"%s\" for dump!\n", fileName.c_str());
		return;
	}

	for (int i = 0; i < pTable->GetNumProps(); ++i) {
		SendProp* pProp = pTable->GetProp(i);
		
		WriteSendProp(pProp, i, 0, pHandle, pWrittenTables);

		g_pFullFileSystem->Write(&strNewLine, 1, pHandle);

		if (pWrittenTables.find(pTable) == pWrittenTables.end())
		{
			pWrittenTables.insert(pTable);
		}
	}

	g_pFullFileSystem->Close(pHandle);
}

static void DumpDT(const CCommand &args)
{
	g_pFullFileSystem->CreateDirHierarchy(baseDTDumpFilePath.c_str(), "MOD");

	unordered_set<SendTable*> pWrittenTables;
	int nClassIndex = 0;
	for(ServerClass *serverclass = Util::servergamedll->GetAllServerClasses(); serverclass->m_pNext != nullptr; serverclass = serverclass->m_pNext) {
		std::string fileName = baseDTDumpFilePath;
		fileName.append(std::to_string(nClassIndex++));
		fileName.append("_");
		fileName.append(serverclass->GetName());
		fileName.append("-");
		fileName.append(serverclass->m_pTable->GetName());
		fileName.append(".txt");

		FileHandle_t pHandle = g_pFullFileSystem->Open(fileName.c_str(), "wb", "MOD");
		if (!pHandle)
		{
			Warning(PROJECT_NAME " - DumpDT: Failed to open \"%s\" for dump!\n", fileName.c_str());
			continue;
		}

		WriteSendTable(serverclass->m_pTable, pHandle, pWrittenTables);

		g_pFullFileSystem->Close(pHandle);
	}

	FileHandle_t pFullList = g_pFullFileSystem->Open("holylib/dump/dt/fulllist.dt", "wb", "MOD");
	if (pFullList)
	{
		nClassIndex = 0;
		for(ServerClass *serverclass = Util::servergamedll->GetAllServerClasses(); serverclass->m_pNext != nullptr; serverclass = serverclass->m_pNext) {
			std::string pName = serverclass->GetName();
			pName.append(" = ");
			pName.append(std::to_string(nClassIndex++));
			g_pFullFileSystem->Write(pName.c_str(), pName.length(), pFullList);
			g_pFullFileSystem->Write(&strNewLine, 1, pFullList);
		}

		g_pFullFileSystem->Close(pFullList);
	}
}
static ConCommand dumpdt("holylib_networking_dumpdt", DumpDT, "Dumps a lot of DT into into the holylib/dumps/dt.txt file", 0);
