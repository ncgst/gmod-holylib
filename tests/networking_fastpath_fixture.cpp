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
constexpr int MAX_VIEWMODELS = 3, MAX_WEAPONS = 4;
constexpr int OBS_MODE_NONE = 0, OBS_MODE_IN_EYE = 4, OBS_MODE_CHASE = 5;
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
	bool IsBitSet(int index) const { return Get(index); }
	void CopyTo(CBitVec* out) const { *out = *this; }
	void Or(const CBitVec& other, CBitVec* out) const { out->bits = bits | other.bits; }
};
[[maybe_unused]] static void CBitVec_AndNot(CBitVec<MAX_EDICTS>* a, const CBitVec<MAX_EDICTS>* b)
{
	a->bits &= ~b->bits;
}
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
	CBaseEntity* observerTarget = nullptr;
	CBaseEntity* hands = nullptr;
	CBaseEntity* viewmodels[MAX_VIEWMODELS]{};
	CBaseEntity* weapons[MAX_WEAPONS]{};
	CBaseEntity* activeWeapon = nullptr;
	CBaseEntity* transmitDependency = nullptr;
	bool player = false, evenOnly = false;
	bool hasEdict = true;
	int observerMode = OBS_MODE_NONE;
	int othersTransmit = FL_EDICT_DONTSEND; // A player's ShouldTransmit result for everyone but itself.
	int skybox = 3;
	CBaseEntity() { entry.entity = this; entry.property = &property; property.owner = &entry; }
	edict_t* edict() { return hasEdict ? &entry : nullptr; }
	bool IsPlayer() const { return player; }
	// No GetObserverMode()/GetObserverTarget() members: production must read the networked fields instead of
	// calling CBasePlayer virtuals, and calling them would no longer compile here.
	const Vector& EyePosition() const { return collision.position; }
	int ShouldTransmit(CCheckTransmitInfo* info)
	{
		++shouldTransmitCalls;
		const int recipient = info->m_pClientEnt->m_EdictIndex;
		if (player)
			return recipient == entry.m_EdictIndex ? FL_EDICT_ALWAYS : othersTransmit;
		if (evenOnly && recipient % 2 != 0)
			return FL_EDICT_DONTSEND;
		return FL_EDICT_PVSCHECK;
	}
	void SetTransmit(CCheckTransmitInfo* info, bool always);
	void BaseSetTransmit(CCheckTransmitInfo* info, bool always)
	{
		info->m_pTransmitEdict->Set(entry.m_EdictIndex);
		if (always && info->m_pTransmitAlways)
			info->m_pTransmitAlways->Set(entry.m_EdictIndex);
		if (transmitDependency)
			transmitDependency->SetTransmit(info, always);
	}
};
using CBasePlayer = CBaseEntity;
using CBaseCombatCharacter = CBaseEntity;
using CBaseViewModel = CBaseEntity;
static CBaseEntity* GetGMODPlayerHands(const CBaseEntity* player) { return player->hands; }
static CBaseViewModel* GetViewModel(const CBaseEntity* player, int slot) { return player->viewmodels[slot]; }
static CBaseEntity* GetMyWeapon(const CBaseEntity* player, int slot) { return player->weapons[slot]; }
static CBaseEntity* GetActiveWeapon(const CBaseEntity* player) { return player->activeWeapon; }
static int GetObserverMode(const CBaseEntity* player) { return player->observerMode; }
static CBaseEntity* GetObserverTarget(const CBaseEntity* player) { return player->observerTarget; }
static void BaseCharacterTransmit(CBaseCombatCharacter* character, CCheckTransmitInfo* info, bool always)
{
	character->BaseSetTransmit(info, always);
}
namespace Symbols
{
using CBaseCombatCharacter_SetTransmit = void (*)(CBaseCombatCharacter*, CCheckTransmitInfo*, bool);
}
struct CharacterDetour
{
	template <typename Fn> Fn GetTrampoline() { return &BaseCharacterTransmit; }
};
static CharacterDetour detour_CBaseCombatCharacter_SetTransmit;
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
static Symbols::CBaseCombatCharacter_SetTransmit func_CBaseAnimating_SetTransmit = &BaseCharacterTransmit;
static CBaseEntity* g_pEntityCache[MAX_EDICTS]{};
static CBitVec<MAX_EDICTS> g_pShouldPrevent[MAX_PLAYERS], g_pDontTransmitCache;
struct ConVar
{
	int value = 0;
	bool GetBool() const { return value != 0; }
	int GetInt() const { return value; }
};
static ConVar networking_fastpath, networking_fasttransmit{true}, networking_areasplit;
static ConVar networking_transmit_onfullupdate{true}, networking_transmit_onfullupdate_networktoothers{true};
static ConVar networking_fastcharactertransmit{true};
static ConVar networking_bind_gmodhands_to_player{true}, networking_bind_viewmodels_to_player{true};
static ConVar networking_transmit_all_weapons{true}, networking_transmit_all_weapons_to_owner{true};
static ConVar networking_transmit_one_per_tick, networking_transmit_newweapons{true};
static ConVar forceTransmit;
static ConVar* sv_force_transmit_ents = &forceTransmit;
struct PlayerTransmitCache
{
	struct WeaponSlot { bool bIsNew = false, bAlwaysNetwork = false; };
	WeaponSlot pWeapons[MAX_WEAPONS];
	int nNextWeaponSlot = 0;
	bool full = false;
	int nLastAcknowledgedTick = 0;
	void NextTick(CBaseEntity*, int) {}
	bool InFullUpdate(int = 0) const { return full; }
};
static PlayerTransmitCache g_pPlayerTransmitCache[MAX_PLAYERS];
// PRODUCTION_CHARACTER_TRANSMIT
void CBaseEntity::SetTransmit(CCheckTransmitInfo* info, bool always)
{
	if (player)
		hook_CBaseCombatCharacter_SetTransmit(this, info, always);
	else
		BaseSetTransmit(info, always);
}
struct EntityTransmitCache
{
	struct AreaCache { int nCount = 0; CBaseEntity* pEntities[64]{}; };
	CBitVec<MAX_EDICTS> pNeverTransmitBits, pAlwaysTransmitBits;
	int nFullEdictCount = -1, nPVSEdictCount = -1;
	CBaseEntity* pFullEntityList[256]{};
	CBaseEntity* pPVSEntityList[64]{};
	AreaCache nAreaEntities[MAX_MAP_AREAS - 1];
	void UpdateEntities(const unsigned short*, int)
	{
		pNeverTransmitBits.ClearAll();
		// PRODUCTION_ATTACHMENT_EXCLUSION
	}
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
		assert(transmit.Get(501) == (recipient == 1 || recipient % 19 == 0)); // Owner and HLTV only.
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

static void ResetObserverWorld(bool fast)
{
	for (int i = 0; i < MAX_EDICTS; ++i)
	{
		entities[i] = CBaseEntity{};
		entities[i].entry.m_EdictIndex = i;
		entities[i].entry.entity = &entities[i];
		entities[i].entry.property = &entities[i].property;
		entities[i].property.owner = &entities[i].entry;
		g_pEntityCache[i] = nullptr;
	}
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		g_pPlayerTransmitCache[i] = {};
		g_pShouldPrevent[i].ClearAll();
	}
	g_nEntityTransmitCache = {};
	g_pTransmitPVSCache.Reset();
	g_pGlobalTransmitTickCache.g_iLastCheckTransmit = -1;
	globals.maxClients = 3;
	++globals.tickcount;
	portalOpen = true;
	forceTransmit.value = false;
	networking_fastpath.value = fast;
	networking_areasplit.value = false;
	networking_fastcharactertransmit.value = true;
	networking_bind_gmodhands_to_player.value = true;
	networking_bind_viewmodels_to_player.value = true;
	networking_transmit_all_weapons.value = false;
	networking_transmit_all_weapons_to_owner.value = true;
	networking_transmit_one_per_tick.value = 2; // Owner-only rotation must stay owner-only.
	networking_transmit_newweapons.value = true;
	networking_transmit_onfullupdate.value = true;
	networking_transmit_onfullupdate_networktoothers.value = false;
	for (int i = 1; i <= 3; ++i)
	{
		entities[i].player = true;
		entities[i].othersTransmit = FL_EDICT_PVSCHECK;
		g_pEntityCache[i] = &entities[i];
		g_nEntityTransmitCache.pFullEntityList[++g_nEntityTransmitCache.nFullEdictCount] = &entities[i];
	}
	for (int owner : {1, 3})
	{
		// Engine hierarchy: viewmodels follow their owner and the hands follow the first viewmodel, so
		// SetTransmit on an attachment also sends its move parents.
		for (int slot = 0; slot < MAX_VIEWMODELS; ++slot)
		{
			entities[owner].viewmodels[slot] = &entities[500 + owner * 10 + slot];
			entities[500 + owner * 10 + slot].transmitDependency = &entities[owner];
		}
		entities[owner].hands = &entities[600 + owner];
		entities[600 + owner].transmitDependency = entities[owner].viewmodels[0];
		entities[owner].weapons[0] = &entities[700 + owner * 10];
		entities[owner].weapons[1] = &entities[701 + owner * 10];
		entities[owner].activeWeapon = entities[owner].weapons[0];
		g_pPlayerTransmitCache[owner - 1].full = true;
		g_pPlayerTransmitCache[owner - 1].pWeapons[1].bIsNew = true;
		g_pPlayerTransmitCache[owner - 1].nNextWeaponSlot = 1;
	}
	entities[2].observerMode = OBS_MODE_IN_EYE;
	entities[2].observerTarget = &entities[1];
	// The spectator deliberately has no hands, viewmodels or weapons. None of
	// its own attachments can indirectly transmit the observed attachments.
	// The production exclusion block must keep these otherwise-visible entities
	// out of the ordinary PVS loop.
	for (int i : {510, 511, 512, 601, 710, 711, 530, 531, 532, 603, 730, 731, 300})
		g_nEntityTransmitCache.pPVSEntityList[++g_nEntityTransmitCache.nPVSEdictCount] = &entities[i];
}

// UpdateEntities never lists FL_EDICT_DONTSEND edicts for the ordinary checks.
static void RemoveFromPVSList(int index)
{
	auto& cache = g_nEntityTransmitCache;
	int kept = -1;
	for (int i = 0; i <= cache.nPVSEdictCount; ++i)
	{
		if (cache.pPVSEntityList[i] != &entities[index])
			cache.pPVSEntityList[++kept] = cache.pPVSEntityList[i];
	}
	cache.nPVSEdictCount = kept;
}

static CBitVec<MAX_EDICTS> ObserverTransmit(int recipient, bool preMarked = false, bool hltv = false)
{
	CBitVec<MAX_EDICTS> transmit, always;
	if (preMarked)
		transmit.Set(1);
	CCheckTransmitInfo info;
	info.m_pClientEnt = &entities[recipient].entry;
	info.m_pTransmitEdict = &transmit;
	info.m_pTransmitAlways = hltv ? &always : nullptr;
	for (auto& byte : info.m_PVS)
		byte = 0xFF;
	g_nTransmitRange = -1.0f;
	assert(New_CServerGameEnts_CheckTransmit(Util::servergameents, &info, nullptr, 0));
	assert((transmit.bits & ~g_pGlobalTransmitTickCache.g_bWasSeenByPlayer.bits).none());
	return transmit;
}

static bool CheckObserverAttachments()
{
	for (bool fast : {true, false})
	{
		for (bool sourceFirst : {true, false})
		{
			for (bool preMarked : {false, true})
			{
				ResetObserverWorld(fast);
				if (sourceFirst)
				{
					const auto owner = ObserverTransmit(1);
					assert(owner.Get(510) && owner.Get(601) && owner.Get(711));
				}
				const auto spectator = ObserverTransmit(2, preMarked);
				assert(g_nEntityTransmitCache.pNeverTransmitBits.Get(510));
				assert(g_nEntityTransmitCache.pNeverTransmitBits.Get(601));
				assert(g_nEntityTransmitCache.pNeverTransmitBits.Get(711));
				if (!spectator.Get(1) || !spectator.Get(510) || !spectator.Get(511) ||
					!spectator.Get(512) || !spectator.Get(601))
				{
					std::cerr << "In-eye regression: target=" << spectator.Get(1)
						<< " viewmodel=" << spectator.Get(510) << " hands=" << spectator.Get(601)
						<< " fastpath=" << fast << " sourceFirst=" << sourceFirst << '\n';
					return false;
				}
				assert(!spectator.Get(711)); // No owner-only inactive/new/full-update weapon.
				assert(!spectator.Get(530) && !spectator.Get(603)); // No other player's attachments.
			}
		}

		// A prevented attachment is never forced, and a prevented target keeps all of its attachments back so
		// that it can't return as their move parent.
		for (int blocked : {511, 601, 1})
		{
			ResetObserverWorld(fast);
			g_pShouldPrevent[1].Set(blocked);
			const auto spectator = ObserverTransmit(2);
			assert(!spectator.Get(blocked) && !g_pGlobalTransmitTickCache.g_bWasSeenByPlayer.Get(blocked));
			assert(!spectator.Get(711));
			if (blocked == 1)
				assert(!spectator.Get(510) && !spectator.Get(511) && !spectator.Get(512) && !spectator.Get(601));
			else
				assert(spectator.Get(1) && spectator.Get(510) && spectator.Get(blocked == 511 ? 601 : 511));
		}

		// There is no spectator-only prevent mask. A prevented parent pulled in by a visible child, and
		// full-update player bits, arrive exactly as they do for a recipient that isn't spectating.
		ResetObserverWorld(fast);
		entities[300].transmitDependency = &entities[450];
		g_pShouldPrevent[1].Set(450);
		g_pShouldPrevent[2].Set(450);
		const auto inEyeChild = ObserverTransmit(2);
		const auto playingChild = ObserverTransmit(3);
		assert(inEyeChild.Get(300) && inEyeChild.Get(450) && playingChild.Get(300) && playingChild.Get(450));
		bool fullUpdate[2] = {};
		for (int mode : {OBS_MODE_NONE, OBS_MODE_IN_EYE})
		{
			ResetObserverWorld(fast);
			entities[2].observerMode = mode;
			g_pShouldPrevent[1].Set(3);
			g_pPlayerTransmitCache[1].full = true;
			fullUpdate[mode == OBS_MODE_IN_EYE] = ObserverTransmit(2).Get(3);
		}
		assert(fullUpdate[0] == fullUpdate[1]);

		// A target hidden by its own ShouldTransmit (EF_NODRAW) still arrives as the move parent of a visible
		// viewmodel, as in the engine. Nothing forces it when all of its attachments are DONTSEND.
		ResetObserverWorld(fast);
		entities[1].othersTransmit = FL_EDICT_DONTSEND;
		const auto hiddenTarget = ObserverTransmit(2);
		assert(hiddenTarget.Get(1) && hiddenTarget.Get(510) && hiddenTarget.Get(601));
		ResetObserverWorld(fast);
		entities[1].othersTransmit = FL_EDICT_DONTSEND;
		for (int i : {510, 511, 512, 601})
			entities[i].entry.m_fStateFlags = FL_EDICT_DONTSEND;
		const auto hiddenAll = ObserverTransmit(2);
		assert(!hiddenAll.Get(1) && !hiddenAll.Get(510) && !hiddenAll.Get(511) && !hiddenAll.Get(512));
		assert(!hiddenAll.Get(601));

		// DONTSEND attachments, like unused viewmodel slots, are not forced to a spectator.
		ResetObserverWorld(fast);
		entities[512].entry.m_fStateFlags = FL_EDICT_DONTSEND;
		const auto unusedSlot = ObserverTransmit(2);
		assert(unusedSlot.Get(510) && unusedSlot.Get(511) && unusedSlot.Get(601) && !unusedSlot.Get(512));

		// Without binding, attachments keep their ordinary checks and nothing is forced.
		ResetObserverWorld(fast);
		networking_bind_viewmodels_to_player.value = false;
		networking_bind_gmodhands_to_player.value = false;
		entities[511].entry.m_fStateFlags = FL_EDICT_DONTSEND;
		RemoveFromPVSList(511);
		assert(!ObserverTransmit(2).Get(511));

		// Chase/free observers and absent, non-player or edict-less targets don't receive the in-eye additions.
		for (int scenario = 0; scenario < 5; ++scenario)
		{
			ResetObserverWorld(fast);
			if (scenario == 0) entities[2].observerMode = OBS_MODE_CHASE;
			if (scenario == 1) entities[2].observerMode = OBS_MODE_NONE;
			if (scenario == 2) entities[2].observerTarget = nullptr;
			if (scenario == 3) entities[2].observerTarget = &entities[400];
			if (scenario == 4) entities[1].hasEdict = false;
			const auto spectator = ObserverTransmit(2);
			assert(!spectator.Get(510) && !spectator.Get(601) && !spectator.Get(711));
		}

		// HLTV receives every player's visible bound attachments, as the engine's PVS rule for HLTV does, but
		// no owner-only weapons.
		ResetObserverWorld(fast);
		const auto hltv = ObserverTransmit(2, false, true);
		assert(hltv.Get(1) && hltv.Get(510) && hltv.Get(601) && hltv.Get(3) && hltv.Get(530) && hltv.Get(603));
		assert(!hltv.Get(711) && !hltv.Get(731));

		ResetObserverWorld(fast);
		const auto first = ObserverTransmit(2);
		assert(first.Get(510) && first.Get(601));
		entities[2].observerTarget = &entities[3];
		const auto switched = ObserverTransmit(2);
		assert(switched.Get(3) && switched.Get(530) && switched.Get(603));
		assert(!switched.Get(510) && !switched.Get(601) && !switched.Get(731));
		entities[2].observerTarget = nullptr;
		const auto cleared = ObserverTransmit(2);
		assert(!cleared.Get(510) && !cleared.Get(530) && !cleared.Get(601) && !cleared.Get(603));
	}
	std::cout << "Bound attachments for in-eye spectators and HLTV: production character hook and attachment "
		"exclusion, both cache settings, source orders, premarked and hidden targets, inventory privacy, "
		"prevent rules, DONTSEND attachments, unbound attachments and target changes passed\n";
	return true;
}

int main()
{
	for (int i = 0; i < MAX_EDICTS; ++i)
		entities[i].entry.m_EdictIndex = i;
	for (int i = 1; i <= 120; ++i)
	{
		entities[i].player = true;
		entities[i].viewmodels[0] = &entities[500 + i];
		entities[500 + i].transmitDependency = &entities[i]; // Viewmodels follow their owner.
		entities[i].weapons[0] = &entities[700 + i];
		entities[i].activeWeapon = entities[i].weapons[0];
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
	if (!CheckObserverAttachments())
		return 1;
}
