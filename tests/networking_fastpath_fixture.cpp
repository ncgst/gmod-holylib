// SDK-free fixture; networking_fastpath_integration_tests.py inserts the
// production transmit/PVS functions at the markers before compiling this file.
#include "networking_pvs_cache.h"
#include <array>
#include <bitset>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

constexpr int MAX_PLAYERS = 128, MAX_EDICTS = 1024, MAX_MAP_AREAS = 4;
constexpr int FL_EDICT_FULLCHECK = 0, FL_EDICT_ALWAYS = 8, FL_EDICT_DONTSEND = 16, FL_EDICT_PVSCHECK = 32;
#define PROJECT_NAME "fixture"
#define Warning(...) ((void)0)
#define BitVec_BitInByte(n) (1u << ((n) & 7))

template <std::size_t N> struct CBitVec
{
	std::bitset<N> bits;
	void ClearAll() { bits.reset(); }
	void Set(int index) { bits.set(index); }
	bool Get(int index) const { return bits.test(index); }
	void CopyTo(CBitVec* out) const { *out = *this; }
	void Or(const CBitVec& other, CBitVec* out) const { out->bits = bits | other.bits; }
};
using vec_t = float;
struct Vector
{
	float x = 0;
	float DistTo(const Vector& other) const { return std::fabs(x - other.x); }
};
struct CCollisionProperty
{
	Vector position;
	const Vector& WorldSpaceCenter() const { return position; }
	float BoundingRadius() const { return 1; }
};
struct CBaseEntity;
struct CCServerNetworkProperty;
struct edict_t
{
	int m_EdictIndex = 0, m_fStateFlags = FL_EDICT_PVSCHECK;
	CBaseEntity* entity = nullptr;
	CCServerNetworkProperty* property = nullptr;
	CCServerNetworkProperty* GetNetworkable() { return property; }
};
struct CCheckTransmitInfo
{
	edict_t* m_pClientEnt = nullptr;
	unsigned char m_PVS[64]{};
	int m_nPVSSize = 64;
	CBitVec<MAX_EDICTS>* m_pTransmitEdict = nullptr;
	CBitVec<MAX_EDICTS>* m_pTransmitAlways = nullptr;
	int m_AreasNetworked = 1;
	int m_Areas[4] = {1};
};
struct PVSInfo
{
	int m_nAreaNum = 2, m_nAreaNum2 = 0, m_nClusterCount = -1, m_nHeadNode = 0;
	unsigned short* m_pClusters = nullptr;
};
struct CCServerNetworkProperty
{
	PVSInfo m_PVSInfo;
	edict_t* owner = nullptr;
	CCServerNetworkProperty* parent = nullptr;
	int AreaNum() const { return m_PVSInfo.m_nAreaNum; }
	edict_t* edict() { return owner; }
	void RecomputePVSInformation() {}
	template <typename HeadnodeQuery>
	bool IsInPVS(const CCheckTransmitInfo* pInfo, HeadnodeQuery&& headnodeQuery);
};
static bool portalOpen = true;
static bool CheckAreasConnected(int a, int b) { return a == b || portalOpen; }
// PRODUCTION_PVS

static int shouldTransmitCalls = 0;
struct CBaseEntity
{
	edict_t entry;
	CCServerNetworkProperty property;
	CCollisionProperty collision;
	CBaseEntity* view = nullptr;
	bool player = false, evenOnly = false;
	int skybox = 3;
	CBaseEntity() { entry.entity = this; entry.property = &property; property.owner = &entry; }
	edict_t* edict() { return &entry; }
	const Vector& EyePosition() const { return collision.position; }
	int ShouldTransmit(CCheckTransmitInfo* info)
	{
		++shouldTransmitCalls;
		const int recipient = info->m_pClientEnt->m_EdictIndex;
		if (player)
			return recipient == entry.m_EdictIndex ? FL_EDICT_ALWAYS : FL_EDICT_DONTSEND;
		if (evenOnly && recipient % 2 != 0)
			return FL_EDICT_DONTSEND;
		return FL_EDICT_PVSCHECK;
	}
	void SetTransmit(CCheckTransmitInfo* info, bool always)
	{
		info->m_pTransmitEdict->Set(entry.m_EdictIndex);
		if (always && info->m_pTransmitAlways)
			info->m_pTransmitAlways->Set(entry.m_EdictIndex);
		if (player && info->m_pClientEnt == &entry)
		{
			// Recipient-only attachments model viewmodels, hands and inventory.
			info->m_pTransmitEdict->Set(500 + entry.m_EdictIndex);
			info->m_pTransmitEdict->Set(700 + entry.m_EdictIndex);
		}
	}
};
using CBasePlayer = CBaseEntity;
struct IServerGameEnts
{
	CBaseEntity* EdictToBaseEntity(edict_t* edict) { return edict->entity; }
};
struct Engine
{
	int headnodeCalls = 0;
	int CheckHeadnodeVisible(int node, const unsigned char* pvs, int size)
	{
		++headnodeCalls;
		assert(node >= 0 && node < size * 8);
		return (pvs[node / 8] & (1 << (node % 8))) != 0;
	}
	int GetArea(const Vector&) { return 1; }
	bool CheckAreasConnected(int a, int b) { return ::CheckAreasConnected(a, b); }
};
static Engine engineObject;
static Engine* engine = &engineObject;
namespace Util
{
static IServerGameEnts gameents;
static IServerGameEnts* servergameents = &gameents;
static Engine* engineserver = engine;
}
struct Globals { int maxClients = 120, tickcount = 0; };
static Globals globals;
static Globals* gpGlobals = &globals;
static edict_t world;
static edict_t* world_edict = &world;
static bool func_CBaseAnimating_SetTransmit = true;
static CBaseEntity* g_pEntityCache[MAX_EDICTS]{};
static CBitVec<MAX_EDICTS> g_pShouldPrevent[MAX_PLAYERS], g_pDontTransmitCache;
struct ConVar
{
	bool value = false;
	bool GetBool() const { return value; }
};
static ConVar networking_fastpath, networking_fasttransmit{true}, networking_areasplit;
static ConVar networking_transmit_onfullupdate{true}, networking_transmit_onfullupdate_networktoothers{true};
static ConVar forceTransmit;
static ConVar* sv_force_transmit_ents = &forceTransmit;
struct PlayerCache
{
	bool full = false;
	int nLastAcknowledgedTick = 0;
	void NextTick(CBaseEntity*, int) {}
	bool InFullUpdate(int = 0) const { return full; }
};
static PlayerCache g_pPlayerTransmitCache[MAX_PLAYERS];
struct EntityTransmitCache
{
	struct AreaCache { int nCount = 0; CBaseEntity* pEntities[64]{}; };
	CBitVec<MAX_EDICTS> pNeverTransmitBits, pAlwaysTransmitBits;
	int nFullEdictCount = -1, nPVSEdictCount = -1;
	CBaseEntity* pFullEntityList[256]{};
	CBaseEntity* pPVSEntityList[64]{};
	AreaCache nAreaEntities[MAX_MAP_AREAS - 1];
	void UpdateEntities(const unsigned short*, int) {}
};
static EntityTransmitCache g_nEntityTransmitCache;
// PRODUCTION_GLOBAL_CACHE
static void RebuildEntityCacheForTick() {}
static CBaseEntity* GetViewEntity(CBasePlayer* player) { return player->view; }
static int GetSkybox3DArea(CBasePlayer* player) { return player->skybox; }
static CCollisionProperty* GetEntityCollisionProperty(CBaseEntity* ent) { return &ent->collision; }
static CCServerNetworkProperty* GetNetworkParentSafe(CCServerNetworkProperty* property) { return property->parent; }
// PRODUCTION_IS_IN_PVS
static vec_t g_nTransmitRange = -1.0f;
// PRODUCTION_DO_TRANSMIT
// PRODUCTION_CHECK_TRANSMIT

struct Result
{
	std::vector<std::bitset<MAX_EDICTS>> bits, always;
	std::bitset<MAX_EDICTS> packed;
	int callbacks = 0, queries = 0;
};
static std::array<CBaseEntity, MAX_EDICTS> entities;

static Result Run(bool fast, int tick, bool areaSplit)
{
	g_pTransmitPVSCache.Reset();
	g_pGlobalTransmitTickCache.g_iLastCheckTransmit = -1;
	globals.tickcount = tick;
	networking_fastpath.value = fast;
	networking_areasplit.value = areaSplit;
	shouldTransmitCalls = engine->headnodeCalls = 0;
	Result result;
	for (int recipient = 1; recipient <= 120; ++recipient)
	{
		// Change recipient context and live portal state within the same tick.
		portalOpen = recipient % 9 != 0;
		forceTransmit.value = recipient % 13 == 0;
		entities[recipient].collision.position.x = recipient % 2 == 0 ? 0.0f : 100.0f;
		entities[recipient].view = recipient % 7 == 0 ? &entities[1] : nullptr;
		entities[recipient].skybox = recipient % 11 == 0 ? 2 : 3;
		entities[310].property.m_PVSInfo.m_nHeadNode = recipient % 2 == 0 ? 1 : 0;
		g_pPlayerTransmitCache[recipient - 1].full = recipient % 17 == 0;
		g_pShouldPrevent[recipient - 1].ClearAll();
		if (recipient % 3 == 0)
			g_pShouldPrevent[recipient - 1].Set(301);

		CBitVec<MAX_EDICTS> transmit, always;
		transmit.Set(900 + recipient); // Bits inserted by PreCheckTransmit must survive.
		CCheckTransmitInfo info;
		info.m_pClientEnt = &entities[recipient].entry;
		info.m_pTransmitEdict = &transmit;
		if (recipient % 19 == 0)
			info.m_pTransmitAlways = &always;
		for (unsigned char& byte : info.m_PVS)
			byte = 0x55;
		if (recipient % 5 == 0)
			info.m_PVS[0] = 0xAA; // Same area, different camera/extra visibility origin.
		g_nTransmitRange = recipient % 4 == 0 ? 25.0f : -1.0f;
		assert(New_CServerGameEnts_CheckTransmit(Util::servergameents, &info, nullptr, 0));
		assert(g_nTransmitRange == -1.0f);
		assert(transmit.Get(recipient) && transmit.Get(500 + recipient) && transmit.Get(700 + recipient));
		assert(transmit.Get(900 + recipient));
		assert(transmit.Get(501) == (recipient == 1));
		if (recipient % 3 == 0)
			assert(!transmit.Get(301));
		if (recipient % 2 != 0)
			assert(!transmit.Get(200)); // ShouldTransmit must run for every recipient.
		if (recipient % 17 == 0)
			for (int player = 1; player <= 120; ++player)
				assert(transmit.Get(player));
		result.bits.push_back(transmit.bits);
		result.always.push_back(always.bits);
	}
	result.packed = g_pGlobalTransmitTickCache.g_bWasSeenByPlayer.bits;
	result.callbacks = shouldTransmitCalls;
	result.queries = engine->headnodeCalls;
	for (auto& player : g_pPlayerTransmitCache)
		player.full = false;
	return result;
}

int main()
{
	for (int i = 0; i < MAX_EDICTS; ++i)
		entities[i].entry.m_EdictIndex = i;
	for (int i = 1; i <= 120; ++i)
	{
		entities[i].player = true;
		g_pEntityCache[i] = &entities[i];
		g_nEntityTransmitCache.pFullEntityList[++g_nEntityTransmitCache.nFullEdictCount] = &entities[i];
	}
	entities[200].evenOnly = true;
	g_nEntityTransmitCache.pFullEntityList[++g_nEntityTransmitCache.nFullEdictCount] = &entities[200];
	for (int i = 300; i < 332; ++i)
	{
		entities[i].property.m_PVSInfo.m_nHeadNode = i - 300;
		entities[i].collision.position.x = 50;
		g_nEntityTransmitCache.pPVSEntityList[++g_nEntityTransmitCache.nPVSEdictCount] = &entities[i];
	}
	// Parent visibility and ordinary cluster checks use the same production path.
	entities[331].property.parent = &entities[300].property;
	unsigned short clusters[] = {1, 2};
	entities[330].property.m_PVSInfo.m_nClusterCount = 2;
	entities[330].property.m_PVSInfo.m_pClusters = clusters;
	g_nEntityTransmitCache.pAlwaysTransmitBits.Set(400);

	for (bool split : {false, true})
	{
		if (split)
		{
			auto& area = g_nEntityTransmitCache.nAreaEntities[1];
			area.nCount = 32;
			for (int i = 0; i < area.nCount; ++i)
				area.pEntities[i] = &entities[300 + i];
			g_nEntityTransmitCache.nPVSEdictCount = -1;
		}
		const Result baseline = Run(false, 10, split);
		const Result cached = Run(true, 11, split);
		assert(cached.bits == baseline.bits);
		assert(cached.always == baseline.always);
		assert(cached.packed == baseline.packed);
		assert(cached.callbacks == baseline.callbacks && cached.callbacks == 120 * 121);
		assert(cached.queries < baseline.queries);
		std::cout << "Production transmit fixture, areaSplit=" << split
			<< ": 120 recipients matched, headnode queries " << baseline.queries << " -> " << cached.queries << '\n';
	}

	// The cheap cluster branch must not pay for a PVS hash or context copy.
	g_pTransmitPVSCache.Reset();
	g_pTransmitPVSCache.BeginTick(12);
	CCheckTransmitInfo info;
	info.m_PVS[0] = 0xFF;
	portalOpen = true;
	TransmitPVSQuery query(true);
	assert(IsInPVS(&entities[330].property, &info, query));
	assert(!query.bInitialized && g_pTransmitPVSCache.GetStats().contextMisses == 0);

	// A disabled cache still performs every underlying headnode query.
	TransmitPVSQuery disabled(false);
	const int before = engine->headnodeCalls;
	assert(IsInPVS(&entities[300].property, &info, disabled));
	assert(IsInPVS(&entities[300].property, &info, disabled));
	assert(engine->headnodeCalls == before + 2 && !disabled.bInitialized);

	globals.maxClients = MAX_PLAYERS + 1;
	assert(!New_CServerGameEnts_CheckTransmit(Util::servergameents, &info, nullptr, 0));
}
