// SDK-free fixture; networking_fastpath_integration_tests.py inserts the
// production transmit/PVS functions at the markers before compiling this file.
#include "networking_pvs_cache.h"
#include <array>
#include <bitset>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string.h> // Production code calls memmove unqualified.
#include <vector>

constexpr int MAX_PLAYERS = 128, MAX_EDICTS = 1024, MAX_MAP_AREAS = 4;
constexpr int MAX_VIEWMODELS = 3, MAX_WEAPONS = 4;
constexpr int OBS_MODE_NONE = 0, OBS_MODE_IN_EYE = 4, OBS_MODE_CHASE = 5;
constexpr int FL_EDICT_FULLCHECK = 0, FL_EDICT_ALWAYS = 8, FL_EDICT_DONTSEND = 16, FL_EDICT_PVSCHECK = 32;
constexpr int FL_EDICT_DIRTY_PVS_INFORMATION = 128;
#define PROJECT_NAME "fixture"
#define Warning(...) ((void)0)
#define DevMsg(...) ((void)0)
#define BitVec_BitInByte(n) (1u << ((n) & 7))

template <std::size_t N> struct CBitVec
{
	std::bitset<N> bits;
	void ClearAll() { bits.reset(); }
	void Set(int index) { bits.set(index); }
	void Clear(int index) { bits.reset(index); }
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
struct ServerClass
{
	const char* m_pNetworkName = "CFixture";
	const char* GetName() const { return m_pNetworkName; }
};
struct CCServerNetworkProperty
{
	PVSInfo m_PVSInfo;
	ServerClass* m_pServerClass = nullptr;
	edict_t* owner = nullptr;
	CCServerNetworkProperty* parent = nullptr;
	// Like the engine: AreaNum() brings dirty PVS data up to date, which clears the dirty flag.
	int AreaNum() { RecomputePVSInformation(); return m_PVSInfo.m_nAreaNum; }
	edict_t* edict() { return owner; }
	void RecomputePVSInformation()
	{
		if (owner)
			owner->m_fStateFlags &= ~FL_EDICT_DIRTY_PVS_INFORMATION;
	}
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
	bool manipulator = false; // GMod's manipulate_bone/manipulate_flex: ShouldTransmit asks the parent.
	int observerMode = OBS_MODE_NONE;
	int othersTransmit = FL_EDICT_DONTSEND; // A player's ShouldTransmit result for everyone but itself.
	int transmitState = -1; // >= 0: the base ShouldTransmit's answer from the entity's transmit state.
	int skybox = 3;
	CBaseEntity() { entry.entity = this; entry.property = &property; property.owner = &entry; }
	edict_t* edict() { return hasEdict ? &entry : nullptr; }
	bool IsPlayer() const { return player; }
	const char* GetClassname() const { return "fixture"; }
	// No GetObserverMode()/GetObserverTarget() members: production must read the networked fields instead of
	// calling CBasePlayer virtuals, and calling them would no longer compile here.
	const Vector& EyePosition() const { return collision.position; }
	int ShouldTransmit(CCheckTransmitInfo* info)
	{
		++shouldTransmitCalls;
		if (manipulator)
		{
			CBaseEntity* parent = property.parent && property.parent->owner ? property.parent->owner->entity : nullptr;
			return parent ? parent->ShouldTransmit(info) : FL_EDICT_DONTSEND;
		}
		if (transmitState >= 0)
			return transmitState;
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
static ConVar networking_transmit_profile, networking_transmit_weaponlist{true}, networking_pvssnapshot;
static ConVar networking_bind_manipulators{true};
static ConVar forceTransmit;
static ConVar* sv_force_transmit_ents = &forceTransmit;
static bool fillWeaponLists = false; // Whether NextTick builds the per-tick weapon list as production does.
struct PlayerTransmitCache
{
	struct WeaponSlot { bool bIsNew = false, bAlwaysNetwork = false; };
	WeaponSlot pWeapons[MAX_WEAPONS];
	int nNextWeaponSlot = 0;
	bool full = false;
	int nLastAcknowledgedTick = 0;
	const CBaseEntity* pWeaponListOwner = nullptr;
	int nWeaponListTick = 0, nWeaponCount = 0;
	CBaseEntity* pWeaponList[MAX_WEAPONS]{};
	void NextTick(CBaseEntity* player, int tick)
	{
		if (!fillWeaponLists)
			return;
		nWeaponCount = 0;
		for (int i = 0; i < MAX_WEAPONS; ++i)
			if (CBaseEntity* weapon = GetMyWeapon(player, i))
				pWeaponList[nWeaponCount++] = weapon;
		pWeaponListOwner = player;
		nWeaponListTick = tick;
	}
	bool HasWeaponList(const CBaseEntity* player, int tick) const
	{
		return pWeaponListOwner && pWeaponListOwner == player && nWeaponListTick == tick;
	}
	void InvalidateWeaponList() { pWeaponListOwner = nullptr; }
	bool InFullUpdate(int = 0) const { return full; }
};
static PlayerTransmitCache g_pPlayerTransmitCache[MAX_PLAYERS];
// PRODUCTION_TRANSMIT_PROFILE
// PRODUCTION_CHARACTER_TRANSMIT
void CBaseEntity::SetTransmit(CCheckTransmitInfo* info, bool always)
{
	if (player)
		hook_CBaseCombatCharacter_SetTransmit(this, info, always);
	else
		BaseSetTransmit(info, always);
}
static CCServerNetworkProperty* GetNetworkParentSafe(CCServerNetworkProperty* property) { return property->parent; }
// PRODUCTION_MANIPULATOR
static std::vector<edict_t*> alwaysEdicts; // FL_EDICT_ALWAYS edicts, marked on every tick's first transmit.
struct EntityTransmitCache
{
	struct AreaCache { int nCount = 0; CBaseEntity* pEntities[64]{}; };
	bool m_bIsActivelyNetworking = false;
	CBitVec<MAX_EDICTS> pNeverTransmitBits, pAlwaysTransmitBits, pPVSTransmitBits, pFullTransmitBits;
	int nAlwaysTransmitPlayerCount = 0;
	int pAlwaysTransmitPlayers[MAX_PLAYERS]{};
	int nFullEdictCount = -1, nPVSEdictCount = -1, nAlwaysManipulatorCount = -1;
	CBaseEntity* pFullEntityList[256]{};
	CBaseEntity* pPVSEntityList[64]{};
	CBaseEntity* pAlwaysManipulatorList[64]{};
	AreaCache nAreaEntities[MAX_MAP_AREAS - 1];
	void UpdateEntities(const unsigned short*, int)
	{
		pNeverTransmitBits.ClearAll();
		pAlwaysTransmitBits.ClearAll();
		nAlwaysTransmitPlayerCount = 0;
		for (edict_t* always : alwaysEdicts)
			MarkAlwaysTransmit(always, always->m_EdictIndex);
		// PRODUCTION_ATTACHMENT_EXCLUSION
	}
	// PRODUCTION_ALWAYS_TRANSMIT
	// PRODUCTION_ENTITY_REMOVED
};
static EntityTransmitCache g_nEntityTransmitCache;
// PRODUCTION_GLOBAL_CACHE
static void RebuildEntityCacheForTick() {}
static CBaseEntity* GetViewEntity(CBasePlayer* player) { return player->view; }
static int GetSkybox3DArea(CBasePlayer* player) { return player->skybox; }
static CCollisionProperty* GetEntityCollisionProperty(CBaseEntity* ent) { return &ent->collision; }
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
	entities[320].property.m_PVSInfo.m_nHeadNode = 20; // Moved during the previous run.
	entities[325].property.m_PVSInfo.m_nHeadNode = 25;
	Result result;
	for (int recipient = 1; recipient <= 120; ++recipient)
	{
		// Change recipient context and live portal state within the same tick.
		portalOpen = recipient % 9 != 0;
		forceTransmit.value = recipient % 13 == 0;
		entities[recipient].collision.position.x = recipient % 2 == 0 ? 0.0f : 100.0f;
		entities[recipient].view = recipient % 7 == 0 ? &entities[1] : nullptr;
		entities[recipient].skybox = recipient % 11 == 0 ? 2 : 3;
		// Moving an entity changes its PVS data and marks it dirty, as the engine does.
		entities[310].property.m_PVSInfo.m_nHeadNode = recipient % 2 == 0 ? 1 : 0;
		entities[310].entry.m_fStateFlags |= FL_EDICT_DIRTY_PVS_INFORMATION;
		if (recipient == 61)
		{
			// 320, the parent of 305, moves. 305 comes first and isn't visible to 61, so its parent walk recomputes
			// 320 and clears the dirty flag before 320's own check; that check must still use the new data.
			entities[320].property.m_PVSInfo.m_nHeadNode = 7;
			entities[320].entry.m_fStateFlags |= FL_EDICT_DIRTY_PVS_INFORMATION;
		}
		if (recipient == 62)
		{
			// 325 moves; its own check recomputes it, later recipients must keep using the new data.
			entities[325].property.m_PVSInfo.m_nHeadNode = 7;
			entities[325].entry.m_fStateFlags |= FL_EDICT_DIRTY_PVS_INFORMATION;
		}
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
	alwaysEdicts.clear();
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

static CBitVec<MAX_EDICTS> ObserverTransmit(int recipient, bool preMarked = false, bool hltv = false,
	CBitVec<MAX_EDICTS>* alwaysOut = nullptr)
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
	if (alwaysOut)
		*alwaysOut = always;
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

// An always transmitted child of a player (a projected texture, a TRANSMIT_ALWAYS SENT) forces that player for
// every recipient. Its SetTransmit hook must still run, or the owner loses its viewmodels, hands and weapons.
static void CheckAlwaysTransmitParents()
{
	for (bool fast : {true, false})
	{
		ResetObserverWorld(fast);
		entities[2].observerMode = OBS_MODE_NONE;
		entities[2].observerTarget = nullptr;
		entities[450].property.parent = &entities[1].property;
		alwaysEdicts.push_back(&entities[450].entry);
		const auto owner = ObserverTransmit(1);
		assert(owner.Get(450) && owner.Get(1) && owner.Get(510) && owner.Get(601));
		assert(owner.Get(710) && owner.Get(711));
		entities[1].othersTransmit = FL_EDICT_DONTSEND; // Hidden, but forced by its always transmitted child.
		const auto other = ObserverTransmit(3);
		assert(other.Get(450) && other.Get(1) && other.Get(710));
		assert(!other.Get(510) && !other.Get(601) && !other.Get(711));
		assert(!g_nEntityTransmitCache.pAlwaysTransmitBits.Get(1));
	}
	std::cout << "Always transmitted children: their player parent still sends its attachments and weapons, "
		"both cache settings passed\n";
}

// A weapon list is used for the rest of its tick; once invalidated (CNetworkingModule::OnEntityDeleted does this
// when an entity is deleted mid-tick) the character hook scans the weapon slots again.
static void CheckWeaponListInvalidation()
{
	fillWeaponLists = true;
	ResetObserverWorld(false);
	networking_transmit_all_weapons.value = true;
	const auto first = ObserverTransmit(1);
	assert(first.Get(710) && first.Get(711) && !first.Get(712));
	entities[1].weapons[2] = &entities[712]; // Changed after this tick's lists were built.
	assert(!ObserverTransmit(1).Get(712)); // The list is in use for this tick.
	g_pPlayerTransmitCache[0].InvalidateWeaponList();
	assert(ObserverTransmit(1).Get(712));
	++globals.tickcount;
	const auto nextTick = ObserverTransmit(1); // A new tick rebuilds the list, which now holds the new weapon.
	assert(nextTick.Get(712) && g_pPlayerTransmitCache[0].HasWeaponList(&entities[1], globals.tickcount));
	fillWeaponLists = false;
	std::cout << "Weapon lists: used within their tick, slot scan after invalidation, rebuilt on the next tick\n";
}

// Removing an entity from a full area list during networking must stay inside that list.
static void CheckEntityRemovedFromFullArea()
{
	static EntityTransmitCache cache;
	cache.m_bIsActivelyNetworking = true;
	auto& area = cache.nAreaEntities[0];
	for (auto*& entity : area.pEntities)
		entity = &entities[300];
	area.nCount = 64;
	area.pEntities[10] = &entities[301];
	cache.nAreaEntities[1].nCount = 5;
	cache.EntityRemoved(&entities[301], &entities[301].entry);
	assert(area.nCount == 63 && area.pEntities[62] == &entities[300] && area.pEntities[63] == nullptr);
	assert(cache.nAreaEntities[1].nCount == 5);
	std::cout << "EntityRemoved: removal from a full area list stays in bounds\n";
}

// GMod's manipulators answer with their parent's ShouldTransmit. Sending those of always transmitted parents from their
// own list must give every recipient, a prevented one and HLTV included, what the full check gives, minus the calls.
static void CheckManipulators()
{
	const int manipulators[] = {451, 452, 453, 454, 455};
	const int parents[] = {450, 450, 1, 460, 0}; // 455 has no parent.
	static ServerClass boneClass{"CBoneManipulate"}, flexClass{"CFlexManipulate"}, propClass{"CDynamicProp"};
	for (bool fast : {true, false})
	{
		std::vector<std::bitset<MAX_EDICTS>> sent[2], always[2];
		int calls[2] = {};
		for (int bound = 0; bound < 2; ++bound)
		{
			ResetObserverWorld(fast);
			networking_bind_manipulators.value = bound;
			entities[450].entry.m_fStateFlags = FL_EDICT_ALWAYS;
			entities[450].transmitState = FL_EDICT_ALWAYS;
			alwaysEdicts.push_back(&entities[450].entry);
			g_nEntityTransmitCache.pPVSEntityList[++g_nEntityTransmitCache.nPVSEdictCount] = &entities[460];
			for (int i = 0; i < 5; ++i)
			{
				CBaseEntity& entity = entities[manipulators[i]];
				entity.manipulator = true;
				entity.property.m_pServerClass = i == 1 ? &flexClass : &boneClass;
				entity.entry.m_fStateFlags = FL_EDICT_FULLCHECK;
				if (parents[i])
				{
					entity.property.parent = &entities[parents[i]].property;
					entity.transmitDependency = &entities[parents[i]]; // SetTransmit also sends the move parent.
				}
			}

			// Production classification, as UpdateEntities applies it.
			entities[456].property.m_pServerClass = &propClass;
			entities[456].property.parent = &entities[450].property;
			assert(!IsManipulatorOfAlwaysTransmitted(&entities[456].entry));
			entities[457].property.parent = &entities[450].property; // No server class.
			assert(!IsManipulatorOfAlwaysTransmitted(&entities[457].entry));
			entities[450].entry.m_fStateFlags = FL_EDICT_ALWAYS | FL_EDICT_DONTSEND;
			assert(!IsManipulatorOfAlwaysTransmitted(&entities[451].entry));
			entities[450].entry.m_fStateFlags = FL_EDICT_ALWAYS;
			const int playerFlags = entities[1].entry.m_fStateFlags;
			entities[1].entry.m_fStateFlags = FL_EDICT_ALWAYS; // A player's answer depends on the recipient regardless.
			assert(!IsManipulatorOfAlwaysTransmitted(&entities[453].entry));
			entities[1].entry.m_fStateFlags = playerFlags;
			auto& cache = g_nEntityTransmitCache;
			for (int i : manipulators)
			{
				const bool withAlwaysParent = IsManipulatorOfAlwaysTransmitted(&entities[i].entry);
				assert(withAlwaysParent == (i == 451 || i == 452));
				if (bound && withAlwaysParent)
					cache.pAlwaysManipulatorList[++cache.nAlwaysManipulatorCount] = &entities[i];
				else
					cache.pFullEntityList[++cache.nFullEdictCount] = &entities[i];
			}

			g_pShouldPrevent[2].Set(451); // Recipient 3 must not receive this manipulator...
			g_pShouldPrevent[0].Set(450); // ...while a prevented always transmitted parent changes nothing.
			shouldTransmitCalls = 0;
			for (int recipient : {1, 2, 3, 2})
			{
				const bool hltv = always[bound].size() == 3;
				CBitVec<MAX_EDICTS> alwaysBits;
				sent[bound].push_back(ObserverTransmit(recipient, false, hltv, &alwaysBits).bits);
				always[bound].push_back(alwaysBits.bits);
			}
			calls[bound] = shouldTransmitCalls;

			const auto& first = sent[bound][0];
			assert(first.test(450) && first.test(451) && first.test(452) && first.test(453) && first.test(454));
			assert(!first.test(455));
			assert(!sent[bound][2].test(451) && sent[bound][2].test(452));
			assert(always[bound][3].test(451) && always[bound][3].test(452));
		}
		assert(sent[0] == sent[1] && always[0] == always[1]);
		assert(calls[0] - calls[1] == 4 * 2 + 3 * 2); // Manipulator and parent call, minus the prevented recipient.

		auto& cache = g_nEntityTransmitCache;
		cache.m_bIsActivelyNetworking = true;
		cache.EntityRemoved(&entities[451], &entities[451].entry);
		assert(cache.nAlwaysManipulatorCount == 0 && cache.pAlwaysManipulatorList[0] == &entities[452]);
		assert(cache.pAlwaysManipulatorList[1] == nullptr);
	}
	networking_bind_manipulators.value = true;
	std::cout << "Manipulators of always transmitted parents: same recipients, prevent bits and HLTV always bits "
		"as the full check, without its ShouldTransmit calls; removal mid-tick\n";
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
	entities[305].property.parent = &entities[320].property; // A parent later in the PVS list.
	unsigned short clusters[] = {1, 2};
	entities[330].property.m_PVSInfo.m_nClusterCount = 2;
	entities[330].property.m_PVSInfo.m_pClusters = clusters;
	alwaysEdicts.push_back(&entities[400].entry);

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

		// The per-tick weapon list must send what the slot scan sends, and timing must not change any decision.
		fillWeaponLists = true;
		networking_transmit_profile.value = true;
		g_pTransmitProfile.Reset();
		const Result listed = Run(false, 12, split);
		networking_transmit_profile.value = false;
		fillWeaponLists = false;
		assert(listed.bits == baseline.bits && listed.always == baseline.always && listed.packed == baseline.packed);
		assert(listed.callbacks == baseline.callbacks && listed.queries == baseline.queries);
		assert(g_pTransmitProfile.nTicks == 1 && g_pTransmitProfile.nPasses == 120);
		assert(g_pTransmitProfile.nCharacterListCalls > 0 && g_pTransmitProfile.nCharacterSlotScans == 0);
		assert(g_pTransmitProfile.nCharacterDepth == 0);
		std::cout << "Per-tick weapon lists with profiling, areaSplit=" << split << ": 120 recipients matched the slot scan, "
			<< g_pTransmitProfile.nCharacterListCalls << " listed player transmits\n";

		// Turning the list off at runtime goes back to the slot scan.
		fillWeaponLists = true;
		networking_transmit_weaponlist.value = false;
		networking_transmit_profile.value = true;
		g_pTransmitProfile.Reset();
		const Result unlisted = Run(false, 13, split);
		networking_transmit_profile.value = false;
		networking_transmit_weaponlist.value = true;
		fillWeaponLists = false;
		assert(unlisted.bits == baseline.bits && unlisted.always == baseline.always && unlisted.packed == baseline.packed);
		assert(g_pTransmitProfile.nCharacterListCalls == 0 && g_pTransmitProfile.nCharacterSlotScans > 0);

		// The PVS snapshot must make the same decisions in the same order, including the headnode queries.
		networking_pvssnapshot.value = true;
		const Result snapshot = Run(false, 14, split);
		const Result snapshotCached = Run(true, 15, split);
		networking_pvssnapshot.value = false;
		assert(snapshot.bits == baseline.bits && snapshot.always == baseline.always && snapshot.packed == baseline.packed);
		assert(snapshot.callbacks == baseline.callbacks && snapshot.queries == baseline.queries);
		assert(snapshotCached.bits == baseline.bits && snapshotCached.always == baseline.always);
		assert(snapshotCached.packed == baseline.packed && snapshotCached.queries == cached.queries);
		assert(g_pPVSSnapshot.IsValid(15) == !split);
		if (!split)
			assert(g_pPVSSnapshot.pStale.Get(310) && g_pPVSSnapshot.pStale.Get(320) && g_pPVSSnapshot.pStale.Get(325) &&
				!g_pPVSSnapshot.pStale.Get(300));
		std::cout << "PVS snapshot, areaSplit=" << split << ": 120 recipients matched, moved entities checked live\n";
	}

	// The cheap cluster branch must not pay for a PVS hash or context copy.
	g_pTransmitPVSCache.Reset();
	CCheckTransmitInfo info;
	info.m_PVS[0] = 0xFF;
	portalOpen = true;
	TransmitPVSQuery query(true);
	assert(IsInPVS(&entities[330].property, &info, query));
	assert(query.bPending && g_pTransmitPVSCache.GetStats().contextMisses == 0);

	// A disabled cache still performs every underlying headnode query.
	TransmitPVSQuery disabled(false);
	const int before = engine->headnodeCalls;
	assert(IsInPVS(&entities[300].property, &info, disabled));
	assert(IsInPVS(&entities[300].property, &info, disabled));
	assert(engine->headnodeCalls == before + 2 && !disabled.bPending && !disabled.pContext);

	// Misses are answered from the context's copy. Bits that a callback adds to the live PVS buffer later in the
	// same transmit must not be stored under the original PVS and served to another recipient.
	g_pTransmitPVSCache.Reset();
	CCheckTransmitInfo changing, unchanged;
	changing.m_PVS[0] = unchanged.m_PVS[0] = 0x01; // Headnode 0 is visible, headnode 5 is not.
	TransmitPVSQuery changingQuery(true), unchangedQuery(true);
	assert(IsInPVS(&entities[300].property, &changing, changingQuery));
	changing.m_PVS[0] |= 0x20; // For example AddOriginToPVS from an entity callback.
	(void)IsInPVS(&entities[305].property, &changing, changingQuery);
	assert(!IsInPVS(&entities[305].property, &unchanged, unchangedQuery));
	assert(g_pTransmitPVSCache.GetStats().contextHits == 1);

	globals.maxClients = MAX_PLAYERS + 1;
	assert(!New_CServerGameEnts_CheckTransmit(Util::servergameents, &info, nullptr, 0));
	for (bool lists : {false, true})
	{
		fillWeaponLists = lists;
		if (!CheckObserverAttachments())
			return 1;
		CheckAlwaysTransmitParents();
	}
	fillWeaponLists = false;
	CheckWeaponListInvalidation();
	CheckEntityRemovedFromFullArea();
	CheckManipulators();
}
