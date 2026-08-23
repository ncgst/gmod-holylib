#include "LuaInterface.h"
#include "detours.h"
#include "module.h"
#include "lua.h"
#include <sstream>
#include "eiface.h"
#include "sourcesdk/GModDataPack.h"
#include "sourcesdk/iluashared.h"
#include "sourcesdk/baseclient.h"
#include "sourcesdk/net_chan.h"
#include "sourcesdk/netmessages.h"
#include "modules/gmoddatapack_luapack.h"
#include "modules/gmoddatapack_luapack_policy.h"
#include "modules/autorefresh_shared.h"
#include "networkstringtable.h"
#include "networkstringtableitem.h"
#include "picosha2/picosha2.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <deque>
#include <unordered_map>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Were doing it the Lumi way, eat all of it
#undef isalnum // 64x loves to shit on this one AGAIN
#undef isalpha // 64x loves to shit on this one AGAIN FUCKING HELL
#undef isspace // 64x SHAT AGAIN! OMFG
#undef isdigit // 64x really wants to be fucking ass

class CGModDataPackModule : public IModule
{
public:
	void Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn) override;
	void LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit) override;
	void LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua) override;
	void Shutdown() override;
	void LevelShutdown() override;
	void Think(bool bSimulating) override;
	void InitDetour(bool bPreServer) override;
	void ClientActive(edict_t* pClient) override;
	void ClientDisconnect(edict_t* pClient) override;
	MODULE_RESULT ClientConnect(bool* bAllowConnect, edict_t* pClient, const char* pszName, const char* pszAddress, char* reject, int maxrejectlen) override;
	MODULE_RESULT ClientCommand(edict_t* pClient, const CCommand* args) override;
	void OnClientDisconnect(CBaseClient* pClient) override;
	const char* Name() override { return "gmoddatapack"; };
	int Compatibility() override { return LINUX32 | LINUX64 | WINDOWS32 | WINDOWS64; };
	bool IsEnabledByDefault() override { return false; }; // ToDo: Figure out what inside of here is behaving very randomly
	// client_lua_files may contain LuaPack canonical identities. Runtime removal would
	// detach the body-selection hooks without an atomic native baseline replacement.
	bool CanDisableAtRuntime() override { return false; };
};

static ConVar gmoddatapack_removeserverif("holylib_gmoddatapack_removeserverif", "0", 0, "If enabled, \"if SERVER then\" code blocks are removed from client files");
static ConVar gmoddatapack_removecomments("holylib_gmoddatapack_removecomments", "0", 0, "If enabled, comments are removed from client files");
static ConVar gmoddatapack_fastnetworking("holylib_gmoddatapack_fastnetworking", "0", 0, "(Very Experimental) If enabled, it'll do funky stuff to the networking");
static ConVar gmoddatapack_luapack_required_stub_budget(
	"holylib_gmoddatapack_luapack_required_stub_budget", "32", FCVAR_ARCHIVE,
	"Maximum required canonical Lua placeholders staged globally per server frame",
	true, 1.0f, true, 1024.0f);
static ConVar gmoddatapack_luapack_baseline_warn_ms(
	"holylib_gmoddatapack_luapack_baseline_warn_ms", "10", FCVAR_ARCHIVE,
	"Log LuaPack SendServerInfo baselines whose preparation plus engine serialization reaches this many milliseconds",
	true, 0.0f, true, 60000.0f);
static ConVar gmoddatapack_luapack_serverinfo_budget(
	"holylib_gmoddatapack_luapack_serverinfo_budget", "1", FCVAR_ARCHIVE,
	"Maximum LuaPack SendServerInfo baselines executed globally per server frame",
	true, 1.0f, true, 16.0f);
static ConVar gmoddatapack_luapack_registration_refresh_budget(
	"holylib_gmoddatapack_luapack_registration_refresh_budget", "64", FCVAR_ARCHIVE,
	"Maximum Lua registration entries or active-client hot-refresh hash updates processed per server frame",
	true, 1.0f, true, 1024.0f);

static CGModDataPackModule g_pGModDataPackModule;
IModule* pGModDataPackModule = &g_pGModDataPackModule;

#if MODULE_EXISTS_GAMESERVER
extern bool Gameserver_HasExactGModSender();
extern CBaseClient* Gameserver_GetClientBySlot(int slot);
#endif

// Linux connections begin from globally canonical hashes. Remember the exact
// per-client native identities for every lane so initial native baselines do not
// receive duplicate updates while later hot refreshes can repair their identity.
using ClientLuaHash = std::array<unsigned char, 32>;
static std::array<std::unordered_map<int, ClientLuaHash>, ABSOLUTE_PLAYER_LIMIT> g_clientNativeLuaHashes;
static std::array<std::unordered_map<int, ClientLuaHash>, ABSOLUTE_PLAYER_LIMIT> g_clientHashUpdatesPending;
static unsigned int g_activeHashRefreshNativeAcknowledgements = 0;
static unsigned int g_activeHashRefreshCanonicalAcknowledgements = 0;
static constexpr std::size_t MAX_TRACKED_LUA_FILES = 1u << 13u;
using PinnedCanonicalFiles = HolyLib::LuaPack::Policy::PinnedCanonicalFileSet<MAX_TRACKED_LUA_FILES>;
static std::array<PinnedCanonicalFiles, ABSOLUTE_PLAYER_LIMIT> g_clientPinnedCanonicalFiles;
static std::array<const Bootil::AutoBuffer*, ABSOLUTE_PLAYER_LIMIT> g_clientPinnedCanonicalPayloads{};

struct ClientRequiredStubQueue
{
	HolyLib::LuaPack::Policy::RequiredStubQueue<MAX_TRACKED_LUA_FILES> pending;
	unsigned int accepted = 0;
	unsigned int sent = 0;
	unsigned int duplicates = 0;
	unsigned int drainFrames = 0;
	unsigned int requestBatches = 0;
	unsigned int requestedFiles = 0;
	std::size_t peakDepth = 0;
	double requestBatchMilliseconds = 0.0;
	double peakRequestBatchMilliseconds = 0.0;
	unsigned int baselineFiles = 0;
	unsigned int baselinePublishedHashes = 0;
	unsigned int baselineCachedHashes = 0;
	unsigned int baselineComputedHashes = 0;
	unsigned int baselineOverrides = 0;
	double baselinePreparationMilliseconds = 0.0;
	double engineServerInfoMilliseconds = 0.0;

	void Reset()
	{
		pending.Reset();
		accepted = 0;
		sent = 0;
		duplicates = 0;
		drainFrames = 0;
		requestBatches = 0;
		requestedFiles = 0;
		peakDepth = 0;
		requestBatchMilliseconds = 0.0;
		peakRequestBatchMilliseconds = 0.0;
		baselineFiles = 0;
		baselinePublishedHashes = 0;
		baselineCachedHashes = 0;
		baselineComputedHashes = 0;
		baselineOverrides = 0;
		baselinePreparationMilliseconds = 0.0;
		engineServerInfoMilliseconds = 0.0;
	}
};

static std::array<ClientRequiredStubQueue, ABSOLUTE_PLAYER_LIMIT> g_clientRequiredStubQueues;
static HolyLib::LuaPack::Policy::RequiredStubScheduler<ABSOLUTE_PLAYER_LIMIT>
	g_requiredStubScheduler;

struct PendingLuaPackServerInfo
{
	CBaseClient* client = nullptr;
	INetChannel* channel = nullptr;
	int userID = 0;
	int clientChallenge = 0;
	double queuedAt = 0.0;
	unsigned int coalescedPolls = 0;

	void Reset()
	{
		client = nullptr;
		channel = nullptr;
		userID = 0;
		clientChallenge = 0;
		queuedAt = 0.0;
		coalescedPolls = 0;
	}
};

// This is a work scheduler, not another connection-admission queue. The engine's
// m_bSendServerInfo remains authoritative, while one fixed-capacity slot record
// prevents a join flood from running every non-preemptible baseline in one frame.
static std::array<PendingLuaPackServerInfo, ABSOLUTE_PLAYER_LIMIT>
	g_pendingLuaPackServerInfos;
static HolyLib::LuaPack::Policy::RoundRobinSlotScheduler<ABSOLUTE_PLAYER_LIMIT>
	g_luaPackServerInfoScheduler;

struct LuaPackRegistrationRefresh
{
	bool active = false;
	bool captureForMapBase = false;
	int nextFileID = 0;
	int targetFileCount = 0;
	std::string registeredInitName;
	bool initRefreshed = false;
	bool initialPassComplete = false;
	bool waitingWarningEmitted = false;
	std::vector<int> unresolvedFileIDs;
	std::vector<int> retryFileIDs;
	std::size_t nextUnresolved = 0;
	std::vector<int> pendingSourceHashFileIDs;
	std::vector<int> retrySourceHashFileIDs;
	std::size_t nextSourceHash = 0;
	unsigned int refreshed = 0;
	unsigned int frames = 0;
	double startedAt = 0.0;

	void Begin(int fileCount, bool capture)
	{
		active = true;
		captureForMapBase = capture;
		targetFileCount = (std::max)(0, fileCount);
		nextFileID = static_cast<int>(HolyLib::LuaPack::Policy::FirstClientLuaRegistration(
			static_cast<std::size_t>(targetFileCount)));
		registeredInitName.clear();
		initRefreshed = false;
		initialPassComplete = false;
		waitingWarningEmitted = false;
		unresolvedFileIDs.clear();
		retryFileIDs.clear();
		nextUnresolved = 0;
		pendingSourceHashFileIDs.clear();
		retrySourceHashFileIDs.clear();
		nextSourceHash = 0;
		refreshed = 0;
		frames = 0;
		startedAt = Plat_FloatTime();
	}

	void Reset()
	{
		active = false;
		captureForMapBase = false;
		nextFileID = 0;
		targetFileCount = 0;
		registeredInitName.clear();
		initRefreshed = false;
		initialPassComplete = false;
		waitingWarningEmitted = false;
		unresolvedFileIDs.clear();
		retryFileIDs.clear();
		nextUnresolved = 0;
		pendingSourceHashFileIDs.clear();
		retrySourceHashFileIDs.clear();
		nextSourceHash = 0;
		refreshed = 0;
		frames = 0;
		startedAt = 0.0;
	}
};

static LuaPackRegistrationRefresh g_luaPackRegistrationRefresh;

static void ClearQueuedLuaPackServerInfo(int slot)
{
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
		return;

	g_luaPackServerInfoScheduler.Unschedule(slot);
	g_pendingLuaPackServerInfos[slot].Reset();
}

static void ClearClientRequiredStubQueue(int slot)
{
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
		return;

	g_requiredStubScheduler.Unschedule(slot);
	g_clientRequiredStubQueues[slot].Reset();
}

static void ClearClientPinnedRequiredDeliveryState(int slot)
{
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
		return;

	ClearClientRequiredStubQueue(slot);
	g_clientPinnedCanonicalFiles[slot].Reset();
	g_clientPinnedCanonicalPayloads[slot] = nullptr;
}

static bool EnqueuePinnedRequiredStub(int slot, int fileID)
{
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
		return false;

	ClientRequiredStubQueue& queue = g_clientRequiredStubQueues[slot];
	using HolyLib::LuaPack::Policy::RequiredStubEnqueueResult;
	const RequiredStubEnqueueResult result = queue.pending.Enqueue(fileID);
	if (result == RequiredStubEnqueueResult::OutOfRange)
		return false;
	if (result == RequiredStubEnqueueResult::AlreadyQueued)
	{
		++queue.duplicates;
		return true;
	}

	++queue.accepted;
	if (queue.accepted == 1)
		Msg(PROJECT_NAME " - luapack: client slot %i queued its pinned required placeholders for bounded per-frame delivery\n", slot);
	queue.peakDepth = (std::max)(queue.peakDepth, queue.pending.Size());
	if (!g_requiredStubScheduler.IsScheduled(slot))
		g_requiredStubScheduler.Schedule(slot);
	return true;
}

static bool HasPendingRequiredStubs(int slot)
{
	return slot >= 0 && slot < ABSOLUTE_PLAYER_LIMIT &&
		!g_clientRequiredStubQueues[slot].pending.Empty();
}

static void ReportRequiredStubQueue(int slot)
{
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
		return;

	const ClientRequiredStubQueue& queue = g_clientRequiredStubQueues[slot];
	if (queue.accepted == 0)
		return;

	Msg(PROJECT_NAME " - luapack: required stub queue slot %i staged %u/%u canonical placeholders over %u frame(s), peak depth %u, coalesced %u duplicate request(s); decoded %u unique request(s) in %u batch(es), %.3f ms total / %.3f ms peak; baseline %u file(s), %u published + %u cached + %u computed hash(es), %u override(s), %.3f ms prepare / %.3f ms engine ServerInfo\n",
		slot, queue.sent, queue.accepted, queue.drainFrames,
		static_cast<unsigned int>(queue.peakDepth), queue.duplicates,
		queue.requestedFiles, queue.requestBatches,
		queue.requestBatchMilliseconds, queue.peakRequestBatchMilliseconds,
		queue.baselineFiles, queue.baselinePublishedHashes,
		queue.baselineCachedHashes, queue.baselineComputedHashes,
		queue.baselineOverrides, queue.baselinePreparationMilliseconds,
		queue.engineServerInfoMilliseconds);
}

static void DrainRequiredStubQueues();
#if defined(SYSTEM_LINUX)
static void DrainQueuedLuaPackServerInfos();
#endif

static void ClearClientLuaDeliveryState(int slot)
{
	if (slot >= 0 && slot < ABSOLUTE_PLAYER_LIMIT)
	{
		ClearQueuedLuaPackServerInfo(slot);
		g_clientNativeLuaHashes[slot].clear();
		g_clientHashUpdatesPending[slot].clear();
		ClearClientPinnedRequiredDeliveryState(slot);
	}
}

static void InvalidatePinnedCanonicalFileForAllClients(int fileID)
{
	for (int slot = 0; slot < ABSOLUTE_PLAYER_LIMIT; ++slot)
		g_clientPinnedCanonicalFiles[slot].Invalidate(fileID);
}

static void ClearClientNativeLuaHashForAllClients(int fileID, const ClientLuaHash* publishedHash = nullptr)
{
	for (int slot = 0; slot < ABSOLUTE_PLAYER_LIMIT; ++slot)
	{
		auto& pendingHashes = g_clientHashUpdatesPending[slot];
		const bool nativeMarkerErased = g_clientNativeLuaHashes[slot].erase(fileID) > 0;
		if (nativeMarkerErased || pendingHashes.find(fileID) != pendingHashes.end())
		{
			if (publishedHash)
				pendingHashes[fileID] = *publishedHash;
			else
				pendingHashes.erase(fileID);
		}
	}
}

static int ClientSlotFromEdict(edict_t* pClient)
{
	if (!pClient)
		return -1;

	return pClient->m_EdictIndex - 1;
}

#define TK_LIST \
	TK(TK_INVALID) \
	TK(TK_IF) \
	TK(TK_FUNCTION) \
	TK(TK_RETURN) \
	TK(TK_THEN) \
	TK(TK_END) \
	TK(TK_DO) \
	TK(TK_OR) \
	TK(TK_AND) \
	TK(TK_NOT) \
	TK(TK_EQUAL) \
	TK(TK_NOT_EQUAL) \
	TK(TK_GREATER_OR_EQUAL) \
	TK(TK_GREATER) \
	TK(TK_LESS_OR_EQUAL) \
	TK(TK_PARENTHESIS) \
	TK(TK_LESS) \
	TK(TK_ELSE) \
	TK(TK_ELSEIF) \
	TK(TK_COMMENT) \
	TK(TK_STRING) \
	TK(TK_NUMBER) \
	TK(TK_TRUE) \
	TK(TK_FALSE) \
	TK(TK_SOMETHING) \
	TK(TK_ADD) \
	TK(TK_SUB) \
	TK(TK_DIV) \
	TK(TK_MUL) \
	TK(TK_LOCAL) \
	TK(TK_LINEEND) \
	TK(TK_EOF)

enum TokenType
{
#define TK(token) token,
	TK_LIST
#undef TK
};

static const char* g_TokenNames[] =
{
#define TK(token) #token,
	TK_LIST
#undef TK
};

struct Token {
	TokenType type;
	std::string content;
	bool isSpace = false;
};

static inline TokenType KeywordType(const std::string& strWord)
{
	if (strWord == "if") return TK_IF;
	if (strWord == "function") return TK_FUNCTION;
	if (strWord == "return") return TK_RETURN;
	if (strWord == "then") return TK_THEN;
	if (strWord == "end") return TK_END;
	if (strWord == "not") return TK_NOT;
	if (strWord == "do") return TK_DO;
	if (strWord == "elseif") return TK_ELSEIF;
	if (strWord == "else") return TK_ELSE;
	if (strWord == "true") return TK_TRUE;
	if (strWord == "false") return TK_FALSE;
	if (strWord == "local") return TK_LOCAL;
	return TK_SOMETHING;
}

// NOTE: I kinda want to keep this function synconized with HolyScript's Tokenizer
static std::vector<Token> TokenizeContent(const std::string& content)
{
	std::vector<Token> tokens;

	size_t scope = 0;
	size_t i = 0;
	while (i < content.size())
	{
		char c = content[i];
		if (c == '\n')
		{
			tokens.push_back({TK_LINEEND, std::string(1, c), true});
			i++;
			continue;
		}

		if (c == ' ' || c == '\t' || c == '\r')
		{
			tokens.push_back({TK_SOMETHING, std::string(1, c), true});
			i++;
			continue;
		}

		if (c == '(' || c == ')')
		{
			tokens.push_back({TK_PARENTHESIS, std::string(1, c)});
			i++;
			continue;
		}

		if (c == '>' && i+1 < content.size() && content[i+1] == '=')
		{
			tokens.push_back({TK_GREATER_OR_EQUAL, content.substr(i, 2)});
			i+=2;
			continue;
		}

		if (c == '>')
		{
			tokens.push_back({TK_GREATER, std::string(1, c)});
			i++;
			continue;
		}

		if (c == '<' && i+1 < content.size() && content[i+1] == '=')
		{
			tokens.push_back({TK_LESS_OR_EQUAL, content.substr(i, 2)});
			i+=2;
			continue;
		}

		if (c == '<')
		{
			tokens.push_back({TK_LESS, std::string(1, c)});
			i++;
			continue;
		}

		if (i+1 < content.size() && ((c == '|' && content[i+1] == '|') || (c == 'o' && content[i+1] == 'r')))
		{
			tokens.push_back({TK_OR, content.substr(i, 2)});
			i+=2;
			continue;
		}

		if (i+1 < content.size() && ((c == '!' && content[i+1] == '=') || (c == '~' && content[i+1] == '=')))
		{
			tokens.push_back({TK_NOT_EQUAL, content.substr(i, 2)});
			i+= 2;
			continue;
		}

		if (i+1 < content.size() && ((c == '=' && content[i+1] == '=')))
		{
			tokens.push_back({TK_EQUAL, content.substr(i, 2)});
			i+= 2;
			continue;
		}

		if (i+1 < content.size() && ((c == '&' && content[i+1] == '&') || (c == 'a' && content[i+1] == 'n' && i+2 < content.size() && content[i+2] == 'd')))
		{
			tokens.push_back({TK_AND, content.substr(i, c == '&' ? 2 : 3)});
			i+= c == '&' ? 2 : 3;
			continue;
		}

		if (c == '+')
		{
			tokens.push_back({TK_ADD, std::string(1, c)});
			i++;
			continue;
		}

		if (c == '*')
		{
			tokens.push_back({TK_MUL, std::string(1, c)});
			i++;
			continue;
		}

		if (c == '-' && i+1 < content.size() && content[i+1] == '-')
		{
			size_t start = i;
			i += 2;

			if (i+1 < content.size() && content[i] == '[')
			{
				i += 1;
				int count = 0;
				while (i+1 < content.size() && content[i]=='=')
				{
					i++;
					count++;
				}

				if (content[i] == '[')
				{
					while (i+1 < content.size())
					{
						i++;
						if (content[i] == ']')
						{
							if (count == 0 && i+1 < content.size() && content[i+1] == ']')
								break;

							i++;
							int nextCount = 0;
							while (i+1 < content.size() && content[i]=='=')
							{
								i++;
								nextCount++;
							}

							if (nextCount == count && content[i] == ']')
								break;

							i--;
						}
					}

					if (content[i] == ']') {
						if (count > 0)
							i++; // Skip second ]
						else
							i=i+2;
					}
				} else
				{
					while (i < content.size() && content[i] != '\n')
						i++;
				}
			} else
			{
				while (i < content.size() && content[i] != '\n')
					i++;
			}

			tokens.push_back({TK_COMMENT, content.substr(start, i-start)});
			continue;
		}

		if (c == '/' && i+1 < content.size() && (content[i+1]=='/' || content[i+1]=='*'))
		{
			size_t start = i;
			if (content[i+1] == '/')
			{
				i += 2;
				while (i < content.size() && content[i] != '\n')
					i++;
			} else
			{
				i += 2;
				while (i+1 < content.size() && !(content[i]=='*' && content[i+1]=='/'))
					i++;

				if (i+1 < content.size())
					i += 2;
			}

			tokens.push_back({TK_COMMENT, content.substr(start, i-start)});
			continue;
		}

		if ((c == '\'' || c == '"') && (i == 0 || (content[i-1] != '\\' || (i >= 2 && content[i-2] == '\\'))))
		{
			int start = i;
			i++;
			while (i < content.size() && (content[i] != c || (content[i-1] == '\\' && content[i-2] != '\\')))
				i++;

			// String
			i++;
			tokens.push_back({TK_STRING, content.substr(start, i-start)});
			continue;
		}

		if (c == '[' && (i == 0 || content[i-1] != '\\') && i+1 < content.size())
		{
			int start = i;
			i += 1;
			int count = 0;
			while (i+1 < content.size() && content[i]=='=')
			{
				i++;
				count++;
			}

			if (content[i]=='[')
			{
				while (i+1 < content.size())
				{
					i++;
					if (content[i] == ']')
					{
						if (count == 0 && i+1 < content.size() && content[i+1] == ']')
							break;

						i++;
						int nextCount = 0;
						while (i+1 < content.size() && content[i]=='=')
						{
							i++;
							nextCount++;
						}

						if (nextCount == count && content[i] == ']')
							break;

						i--;
					}
				}

				if (content[i] == ']') {
					if (count > 0)
						i++; // Skip second ]
					else
						i=i+2;
				}

				// Long String
				tokens.push_back({TK_STRING, content.substr(start, i-start)});
				continue;
			}
			i = start;
		}

		// if it starts with a number, it is a number.
		if (std::isdigit(c))
		{
			size_t start = i;
			while (i < content.size() && std::isdigit(content[i]))
				i++;

			std::string strWord = content.substr(start, i - start);
			tokens.push_back({TK_NUMBER, strWord});
			continue;
		}

		if (std::isalpha(c) || c == '_')
		{
			size_t start = i;
			while (i < content.size() && (std::isalnum(content[i]) || content[i]=='_'))
				i++;

			std::string strWord = content.substr(start, i - start);
			tokens.push_back({KeywordType(strWord), strWord});
			continue;
		}

		// NOTE: We check - and / down here since they may else conflict with -- or //
		if (c == '-')
		{
			tokens.push_back({TK_SUB, std::string(1, c)});
			i++;
			continue;
		}

		if (c == '/')
		{
			tokens.push_back({TK_DIV, std::string(1, c)});
			i++;
			continue;
		}

		if (c == '!')
		{
			tokens.push_back({TK_NOT, std::string(1, c)});
			i++;
			continue;
		}

		tokens.push_back({TK_SOMETHING, std::string(1, c)});
		i++;
	}

	tokens.push_back({TK_EOF, ""});
	return tokens;
}

static bool IsNotTK(const std::vector<Token>& tokens, size_t i)
{
	TokenType tk = tokens[i].type;
	if (tk == TK_NOT) // Backtracking time... yay
	{
		while (i > 0)
		{
			--i;
			if (tokens[i].isSpace)
				continue;

			if (tokens[i].type != TK_NOT)
				break;

			tk = tk == TK_NOT ? TK_INVALID : TK_NOT;
		}
	}

	return tk == TK_NOT;
}

struct ScopeEvalInfo
{
	bool isEmpty = true;
	TokenType evalToken = TokenType::TK_INVALID;
	size_t start;
	size_t end;
	std::vector<bool> isServer = {false}; // if we got an or we must check every side
};

// Input start must be advanced by 1!
// We expect that the start argument was moved to the TK_THEN or TK_DO when were done!
static bool CanServerConditionBeRemoved(const std::vector<Token> &tokens, size_t& start)
{
	std::deque<size_t> pScopeIDs;
	std::vector<ScopeEvalInfo> pScopes;
	pScopes.emplace_back();
	pScopes[0].start = start;
	pScopeIDs.push_back(0);

	int lastNonEmpty = 0; // To avoid backtracking! (Now it just is a very small save in backtracking I guess)
	while (tokens[start].type != TK_THEN && tokens[start].type != TK_DO && start < tokens.size())
	{
		if (!tokens[start].isSpace && tokens[start].type != TK_PARENTHESIS)
			pScopes[pScopeIDs.back()].isEmpty = false;

		if (tokens[start].type == TK_SOMETHING)
		{
			if (tokens[start].content == "SERVER" && !IsNotTK(tokens, lastNonEmpty))
			{
				pScopes[pScopeIDs.back()].isServer[pScopes[pScopeIDs.back()].isServer.size()-1] = true;
			} else if (tokens[start].content == "CLIENT" && IsNotTK(tokens, lastNonEmpty))
			{
				pScopes[pScopeIDs.back()].isServer[pScopes[pScopeIDs.back()].isServer.size()-1] = true;
			}
		}

		if (tokens[start].type == TK_OR)
		{
			pScopes[pScopeIDs.back()].isServer.push_back(false);
		}

		if (tokens[start].type == TK_PARENTHESIS)
		{
			if (tokens[start].content == "(") {
				pScopes.emplace_back();
				pScopeIDs.push_back(pScopes.size()-1);
				pScopes[pScopeIDs.back()].start = start;

				pScopes[pScopeIDs.back()].evalToken = IsNotTK(tokens, lastNonEmpty) ? TK_NOT : TK_INVALID;
			} else {
				size_t prevID = pScopeIDs.back();
				pScopes[prevID].end = start;
				
				bool wasServer = pScopes[prevID].evalToken != TK_NOT;
				for (bool bServer : pScopes[prevID].isServer)
				{
					if (bServer)
						continue;

					wasServer = false;
					break;
				}

				pScopeIDs.pop_back();
				pScopes[pScopeIDs.back()].isServer[pScopes[pScopeIDs.back()].isServer.size()-1] = wasServer;
			}
		}

		if (!tokens[start].isSpace)
			lastNonEmpty = start;

		start++;
	}
	pScopes[0].end = start;
	pScopeIDs.pop_back();

	bool isServer = true;
	for (const ScopeEvalInfo& info : pScopes)
	{
		if (info.isEmpty)
			continue;

		for (bool bServer : info.isServer)
		{
			if (bServer)
				continue;

			isServer = false;
			break;
		}

		break;
	}

	return isServer;
}

// The goal is to remove parts of a Lua script without changing the line number (so that errors remain easy to debug!)
static size_t RemoveScoped(size_t i, std::vector<Token> &tokens, std::stringstream& ss, TokenType tok, bool& hasLineBreaks)
{
	int depth = 1;
	++i;
	hasLineBreaks = false; // If it's a one line if -> "if x then x else x end" then we won't restore spaces
	while (i < tokens.size() && depth > 0)
	{
		if (tokens[i].type == TK_THEN || tokens[i].type == TK_DO || tokens[i].type == TK_FUNCTION)
			depth++;
		else if (tokens[i].type == TK_END)
		{
			depth--;
			if (tok == TK_ELSEIF && depth <= 0)
				continue;
		}
		else if (tokens[i].type == TK_LINEEND)
		{
			hasLineBreaks = true;
			ss << '\n';
		}
		else if (tokens[i].type == TK_COMMENT)
		{
			for (char c : tokens[i].content)
				if (c=='\n') ss << '\n';
		}
		else if (tokens[i].type == TK_ELSE && depth == 1)
		{
			depth--;
			continue;
		}
		else if (tokens[i].type == TK_ELSEIF)
		{
			depth--;
			if (depth <= 0)
				continue;
		}

		i++;
	}

	return i;
}

static size_t RemoveServerScoped(size_t j, std::vector<Token> &tokens, std::stringstream& ss, TokenType tok)
{
	bool hasLineBreaks = false;
	size_t i = RemoveScoped(j, tokens, ss, tok, hasLineBreaks);

	if (tokens[i].type == TK_ELSEIF)
		tokens[i].content = tok == TK_IF ? "if" : "elseif";
	else if (tokens[i].type == TK_ELSE && tok == TK_IF)
		tokens[i].content = "do";

	// If we had for example "	elseif xx then" we want to restore the space before it.
	while (hasLineBreaks && i-1 > 0 && tokens[i-1].isSpace && tokens[i-1].type != TK_LINEEND)
		i--;

	return i;
}

std::string ProcessTokens(std::vector<Token> &tokens, bool bRemoveServerCode, bool bRemoveComments)
{
	std::stringstream ss;
	size_t i = 0;

	while (i < tokens.size()) {
		const Token &tok = tokens[i];
		if (bRemoveComments && tok.type == TK_COMMENT)
		{
			for (char c : tok.content)
				if (c == '\n') ss << '\n';

			i++;
			continue;
		}

		if (bRemoveServerCode && tok.type == TK_IF)
		{
			size_t j = i+1;
			if (CanServerConditionBeRemoved(tokens, j))
			{
				if (j < tokens.size() && tokens[j].type == TK_THEN)
				{
					i = RemoveServerScoped(j, tokens, ss, TK_IF);
					continue;
				}
			}
		}

		if (bRemoveServerCode && tok.type == TK_ELSEIF)
		{
			size_t j = i+1;
			if (CanServerConditionBeRemoved(tokens, j))
			{
				if (j < tokens.size() && tokens[j].type == TK_THEN)
				{
					i = RemoveServerScoped(j, tokens, ss, TK_ELSEIF);
					continue;
				}
			}
		}

		if (tok.type != TK_EOF)
			ss << tok.content;

		i++;
	}

	std::string pCode = ss.str();
	bool bHasCode = false;
	for (char c : pCode)
	{
		if (!std::isspace(static_cast<unsigned char>(c)))
		{
			bHasCode = true;
			break;
		}
	}

	if (!bHasCode)
		pCode = "--"; // Nothing to see :3

	return pCode;
}

// Copies what GMod does in GModDataPack::GetHashFromString.
static ClientLuaHash HashClientLuaString(const std::string& content)
{
	ClientLuaHash hash{};
	picosha2::hash256_one_by_one hasher;
	hasher.process(content.c_str(), content.c_str() + content.length() + 1);
	hasher.finish();
	hasher.get_hash_bytes(hash.begin(), hash.end());
	return hash;
}

static bool PublishedLuaHashMatches(const CNetworkStringTableItem& item,
	const ClientLuaHash& expected)
{
	return item.m_pUserData &&
		item.m_nUserDataLength == static_cast<int>(expected.size()) &&
		std::equal(expected.begin(), expected.end(), item.m_pUserData);
}

static inline void CallLuaTokenizeContent(GarrysMod::Lua::ILuaInterface* LUA, std::vector<Token>& tokens, int fileID, bool bIsHook)
{
	LUA->PreCreateTable(tokens.size(), 0);
	int idx = 0;
	for (const Token& tok : tokens)
	{
		LUA->PreCreateTable(0, 3);

		LUA->PushString("isSpace");
		LUA->PushBool(tok.isSpace);
		LUA->RawSet(-3);

		LUA->PushString("type");
		LUA->PushNumber(tok.type);
		LUA->RawSet(-3);

		LUA->PushString("content");
		LUA->PushString(tok.content.c_str(), tok.content.length());
		LUA->RawSet(-3);

		Util::RawSetI(LUA, -2, ++idx);
	}

	LUA->PushNumber(fileID);

	if (LUA->CallFunctionProtected(2 + (bIsHook ? 1 : 0), 1, true))
	{
		if (LUA->IsType(-1, GarrysMod::Lua::Type::Table))
		{
			bool bInvalid = false;
			std::vector<Token> pNewTokens;

			LUA->Push(-1);
			LUA->PushNil();
			while (LUA->Next(-2))
			{
				if (!LUA->IsType(-1, GarrysMod::Lua::Type::Table))
				{
					bInvalid = true;
					LUA->Pop(2);
					break;
				}

				Token newToken;

				LUA->PushString("isSpace");
				LUA->RawGet(-2);
				newToken.isSpace = LUA->GetBool(-1);
				LUA->Pop(1);

				LUA->PushString("type");
				LUA->RawGet(-2);
				newToken.type = (TokenType)LUA->GetNumber(-1);
				LUA->Pop(1);
				if (newToken.type <= TK_INVALID || newToken.type > TK_EOF)
				{
					bInvalid = true;
					LUA->Pop(2);
					break;
				}

				LUA->PushString("content");
				LUA->RawGet(-2);

				unsigned int nLength;
				const char* pContent = LUA->GetString(-1, &nLength);
				LUA->Pop(1);
				if (!pContent)
				{
					bInvalid = true;
					LUA->Pop(2);
					break;
				}

				newToken.content = std::string(pContent, nLength);
				pNewTokens.push_back(newToken);
				LUA->Pop(1);
			}
			LUA->Pop(1);

			if (!bInvalid)
			{
				tokens.clear();
				tokens = std::move(pNewTokens);
			}
		}
		LUA->Pop(1);
	}
}

/*
	The goal of our LuaDataPack is to avoid compressing files on the main thread.
	Especially if you got many files & huge ones too, then it can easily take ages for them all to compres
	and GMod "conveniently" does it only when the player requests them when joining, so it normally would add up to the loading times
*/
static GModDataPack* g_pDataPack;
class LuaDataPack
{
public:
	class LuaPackEntry
	{
	public:
		inline bool IsReady() const
		{
			return compressed.GetWritten() != 0;
		}

		inline bool IsContentReady()
		{
			return processed;
		}

		inline bool Compress()
		{
			// Leading 32 bytes are the SHA256, used by the client to verify if the content matches the hash in the fileID's string userdata!
			if (!contentHashReady)
			{
				contentHash = HashClientLuaString(content);
				contentHashReady = true;
			}
			compressed.Write(contentHash.data(), contentHash.size());

			return Bootil::Compression::LZMA::Compress(content.data(), content.length() + 1, compressed, 9);
		}

		inline void Clear()
		{
			hasSourceContent = false;
			sourceContent = "";
			content = "";
			compressed.Clear();
			processed = false;
			hashPublished = false;
			sourceHashReady = false;
			contentHashReady = false;
			removeServerCode = false;
			removeComments = false;
			luapackCanonical = false;
			luapackPassthrough = false;
			activeHashRefreshPending = false;
			forceActiveHashRefreshPending = false;
		}

		std::string sourceContent = "";
		std::string content = "";
		Bootil::AutoBuffer compressed;
		ClientLuaHash sourceHash{};
		ClientLuaHash contentHash{};
		std::shared_mutex mutex; // Per entry instead of a global mutex to avoid blocking the main thread for other entries while compressing
		bool hasSourceContent = false;
		bool processed = false;
		bool hashPublished = false;
		bool sourceHashReady = false;
		bool contentHashReady = false;
		bool removeServerCode = false;
		bool removeComments = false;
		bool luapackCanonical = false;
		bool luapackPassthrough = false;
		bool activeHashRefreshPending = false;
		bool forceActiveHashRefreshPending = false;
	};

	void QueueActiveHashRefresh(int fileID)
	{
		std::lock_guard<std::mutex> lock(m_pActiveHashRefreshQueueMutex);
		m_pActiveHashRefreshQueue.push_back(fileID);
	}

	void PublishEntryHash(int fileID, LuaPackEntry& entry)
	{
		INetworkStringTable* table = g_pDataPack->m_pClientLuaFiles;
		int publishedLength = 0;
		const void* publishedData = table->GetStringUserData(fileID, &publishedLength);
		const bool publishedHashChanged = publishedLength != static_cast<int>(entry.contentHash.size()) ||
			!publishedData || !std::equal(entry.contentHash.begin(), entry.contentHash.end(),
				static_cast<const unsigned char*>(publishedData));

		// Source only marks a string-table entry changed when its userdata bytes differ.
		// Clear per-client native identities only when that real global notification will
		// happen. A byte-identical canonical publication is repaired explicitly below.
		if (publishedHashChanged)
			ClearClientNativeLuaHashForAllClients(fileID, &entry.contentHash);
		table->SetStringUserData(fileID, entry.contentHash.size(), entry.contentHash.data());
		entry.hashPublished = true;

		const char* fileName = table->GetString(fileID);
		if (HolyLib::LuaPack::Policy::ShouldQueueActiveHashRefresh(
			HolyLib::LuaPack::IsEnabled(), HolyLib::LuaPack::SupportsCanonicalRegistration(),
			entry.activeHashRefreshPending, publishedHashChanged,
			HolyLib::LuaPack::IsInitFile(fileName ? fileName : ""),
			entry.forceActiveHashRefreshPending))
		{
			QueueActiveHashRefresh(fileID);
		}
		entry.activeHashRefreshPending = false;
		entry.forceActiveHashRefreshPending = false;
	}

	void Initialize();
	void Shutdown();
	bool PublishRegistrationHash(int fileID)
	{
		if (!g_pDataPack || !g_pDataPack->m_pClientLuaFiles)
			return false;
		LuaPackEntry* entry = GetPackEntry(fileID);
		if (!entry)
			return false;

		std::lock_guard<std::shared_mutex> lock(entry->mutex);
		if (!entry->IsContentReady())
		{
			// Publish the replacement identity on the main thread before the new mode can
			// serve a body. The worker may still compress it asynchronously afterwards.
			ProcessContent(entry, fileID);
		}
		if (entry->IsContentReady() && !entry->hashPublished)
		{
			if (!entry->contentHashReady)
			{
				entry->contentHash = HashClientLuaString(entry->content);
				entry->contentHashReady = true;
			}
			PublishEntryHash(fileID, *entry);
		}
		return entry->IsContentReady() && entry->hashPublished;
	}

	void AddFileContents(std::string fileName, std::string content,
		bool forceActiveHashRefresh = false)
	{
		if (!g_pDataPack)
			return;

		int fileID = g_pDataPack->m_pClientLuaFiles->FindStringIndex(fileName.c_str());
		if (fileID == INVALID_STRING_INDEX)
			fileID = g_pDataPack->m_pClientLuaFiles->AddString(true, fileName.c_str());

		if (fileID == INVALID_STRING_INDEX)
		{
			Warning(PROJECT_NAME " - gmoddatapack: Failed to add string \"%s\" to client_lua_files table? Are we full?\n", fileName.c_str());
			return;
		}

		LuaPackEntry& pEntry = m_pLuaFileCache[fileID];
		std::lock_guard<std::shared_mutex> lock(pEntry.mutex);
		bool bRemoveServerCode = gmoddatapack_removeserverif.GetBool();
		bool bRemoveComments = gmoddatapack_removecomments.GetBool();
		const bool bLuaPackEnabled = HolyLib::LuaPack::IsEnabled();
		const bool bCanonicalRegistration = HolyLib::LuaPack::SupportsCanonicalRegistration();
		const bool bLuaPackCanonical = HolyLib::LuaPack::Policy::UsesCanonicalRegistration(
			bLuaPackEnabled, bCanonicalRegistration);
		if (bLuaPackEnabled && (bRemoveServerCode || bRemoveComments))
		{
			// Luapack requires one canonical byte stream: the stringtable hash comes from this
			// processed content, but pack capture and the engine-native send path both carry the
			// raw file bytes. Stripping here would make those three disagree, so the strip flags
			// are inert while luapack is enabled.
			static bool s_bWarnedStripConflict = false;
			if (!s_bWarnedStripConflict)
			{
				s_bWarnedStripConflict = true;
				Warning(PROJECT_NAME " - gmoddatapack: removeserverif/removecomments are ignored while luapack is enabled (stripped bytes would diverge from pack and native delivery)\n");
			}
			bRemoveServerCode = false;
			bRemoveComments = false;
		}
		bool bSameSource = pEntry.hasSourceContent && pEntry.sourceContent == content;
		bool bSameProcessConfig = pEntry.removeServerCode == bRemoveServerCode && pEntry.removeComments == bRemoveComments &&
			HolyLib::LuaPack::Policy::RegistrationModeMatches(pEntry.luapackCanonical,
				pEntry.luapackPassthrough,
				bLuaPackEnabled, bCanonicalRegistration);
		if (bSameSource && pEntry.IsContentReady() && bSameProcessConfig) // Nothing changed
		{
			if (forceActiveHashRefresh)
			{
				pEntry.activeHashRefreshPending = true;
				pEntry.forceActiveHashRefreshPending = true;
			}
			if (!pEntry.contentHashReady)
			{
				pEntry.contentHash = HashClientLuaString(pEntry.content);
				pEntry.contentHashReady = true;
			}
			PublishEntryHash(fileID, pEntry);

			return;
		}

		// The baseline plan is keyed by file ID and the exact immutable-base identity.
		// Any source or registration-mode transition must leave the O(1) request path
		// before LuaPack can decide whether this client now needs a native delta or an
		// ordered canonical restoration.
		InvalidatePinnedCanonicalFileForAllClients(fileID);
		pEntry.compressed.Clear();
		pEntry.hasSourceContent = true;
		pEntry.sourceContent = content;
		pEntry.content = HolyLib::LuaPack::PrepareVanillaFile(fileName, content);
		if (bLuaPackEnabled)
		{
			// Publish the small canonical body immediately, but leave full native-source
			// hashing to the compression worker. Hashing an entire large registration set
			// inside one transition frame can trip the server freeze watchdog.
			pEntry.contentHash = HashClientLuaString(pEntry.content);
			pEntry.sourceHashReady = false;
			pEntry.contentHashReady = true;
		}
		else
		{
			// Ordinary gmoddatapack processing may still strip/tokenize content on the
			// worker. Defer its final hash there instead of hashing twice on the caller.
			pEntry.sourceHashReady = false;
			pEntry.contentHashReady = false;
		}
		pEntry.removeServerCode = bRemoveServerCode;
		pEntry.removeComments = bRemoveComments;
		pEntry.luapackCanonical = bLuaPackCanonical;
		pEntry.luapackPassthrough = HolyLib::LuaPack::Policy::UsesPassthroughProcessing(bLuaPackEnabled);
		pEntry.activeHashRefreshPending = true;
		pEntry.forceActiveHashRefreshPending = forceActiveHashRefresh;
		pEntry.processed = false;
		pEntry.hashPublished = false;

		if (g_pGModDataPackModule.InDebug())
			Msg(PROJECT_NAME " - gmoddatapack: Added fileID %i into compression queue!\n", fileID);

		std::lock_guard<std::mutex> queueLock(m_pCompressQueueMutex);
		m_pCompressQueue.push_back(fileID);
	}

	// We only strip it, if it's valid lua (yes my token stuff is highly sensitive!)
	std::string StripContent(std::string content, bool* bError, int fileID, bool bRemoveServerCode, bool bRemoveComments)
	{
		*bError = false;
		lua_State* L = luaL_newstate();
		if (luaL_loadbuffer(L, content.c_str(), content.length(), "StripContent") != LUA_OK)
		{
			std::string pError = "";
			size_t nLength;
			const char* err = lua_tolstring(L, -1, &nLength);
			if (err)
				pError = std::string(err, nLength);

			lua_close(L);

			*bError = true;
			return pError;
		}
		lua_close(L);

		std::vector<Token> tokens = TokenizeContent(content);

		Lua::ScopedThreadAccess pThreadScope;
		auto LUA = pInterface.GetLua();
		if (LUA)
		{
			Lua::StateAccess pScope(LUA);
			if (pScope.IsValid() && Lua::PushHook("HolyLib:OnTokenizeContent", LUA))
			{
				CallLuaTokenizeContent(LUA, tokens, fileID, true);
			}
		}

		return ProcessTokens(tokens, bRemoveServerCode, bRemoveComments);
	}

	void ProcessContent(LuaPackEntry* pEntry, int fileID)
	{
		if (!pEntry->luapackPassthrough)
		{
			bool bError;
			std::string strippedContent = StripContent(pEntry->content, &bError, fileID,
				pEntry->removeServerCode, pEntry->removeComments);
			if (bError)
			{
				Warning(PROJECT_NAME " - gmoddatapack: Failed to strip fileID \"%i\" due to lua errors! (%s)", fileID, strippedContent.c_str());
			} else {
				pEntry->content = strippedContent;
			}
		}
		// LuaPack passthrough deliberately skips tokenization and Lua hook access. This
		// keeps the raw registered bytes identical to pack capture/native delivery and
		// lets transition publication remain independent of the worker's Lua-state lock.
		if (!pEntry->luapackPassthrough || !pEntry->contentHashReady)
		{
			pEntry->contentHash = HashClientLuaString(pEntry->content);
			pEntry->contentHashReady = true;
		}
		pEntry->processed = true;

		if (ThreadInMainThread()) // SetStringUserData is NOT thread safe!
		{
			PublishEntryHash(fileID, *pEntry);
		} else {
			pEntry->hashPublished = false;
			std::lock_guard<std::mutex> lock(m_pStringTableUpdateQueueMutex);
			m_pStringTableUpdateQueue.push_back(fileID);
		}
	}

	bool CompressFile(LuaPackEntry* pEntry, int fileID)
	{
		if (pEntry->luapackPassthrough && pEntry->hasSourceContent &&
			!pEntry->sourceHashReady)
		{
			pEntry->sourceHash = HashClientLuaString(pEntry->sourceContent);
			pEntry->sourceHashReady = true;
		}
		bool bSuccess = pEntry->Compress();
		if (!bSuccess)
			Warning(PROJECT_NAME " - gmoddatapack: Failed to compress lua file %i\n", fileID);
		else if (g_pGModDataPackModule.InDebug())
			Msg(PROJECT_NAME " - gmoddatapack: Compressed lua file %i (compressed/uncompressed: %i/%i)\n", fileID, pEntry->compressed.GetWritten(), pEntry->content.length());

		return bSuccess;
	}

	inline LuaPackEntry* GetPackEntry(int fileID)
	{
		if (fileID < 0 || fileID >= MAX_LUA_FILES)
			return nullptr;

		return &m_pLuaFileCache[fileID];
	}

	static constexpr size_t MIN_TRANSFER_RATE = 1024 * 64; // If we cannot achieve this speed we just let GMod handle it since it'll be faster at that point
	static constexpr size_t MAX_TRANSFER_RATE = 1024 * 512;
	struct PlayerQueue
	{
		std::vector<int> pQueue;
		size_t sentFiles = 0; // This connection count (Since they are reconnected each time)
		size_t requestCount = 0; // Total file count of this attempt
		size_t previousCount = 0; // Total file count of this attempt
		size_t totalSentFiles = 0; // Total sent count
		size_t targetRate = MAX_TRANSFER_RATE; // Default rate which we will attempt to hit (and go a bit over) - this rate has no effect when useReliable = true
		// If we sent anything unreliable at any point through this attempt we must reconnect at the end!
		// I hate this but GMod doesn't provide a way to request which files are left & Rubat never answered in the binary-modules channel :(
		bool usedUnreliable = false;
		bool usedReliable = false; // We don't need to reconnect them once were done since we used the reliable stream
		bool useReliable = false; // We may force reliable networking if the loss through unreliable is too great
		float successRate = -1.0f;
		double reconnectTime = -1;

		void Clear()
		{
			pQueue.clear();
			sentFiles = 0;
			requestCount = 0;
			previousCount = 0;
			totalSentFiles = 0;
			targetRate = MAX_TRANSFER_RATE;
			usedUnreliable = false;
			usedReliable = false;
			useReliable = false;
			reconnectTime = -1;
			successRate = -1.0f;
		}

		// Player was reconnected... prepare for a new attempt
		void Reconnect()
		{
			reconnectTime = -1;
			successRate = -1.0f;
			useReliable = false,
			usedReliable = false;
			usedUnreliable = false;
			previousCount = requestCount;
			requestCount = 0;
		}

		// Before sending we always recalculate
		void RecalculateRate()
		{
			if (successRate != -1.0f)
				return;

			successRate = ((float)requestCount / (float)previousCount);
			if (successRate > 0.9f)
			{
				// We always have some loss, to avoid issues we sent the last few through reliable to avoid wasting time with reconnecting just to find the few lost files
				if (requestCount < 50)
					useReliable = true;

				return; // No adjustment needed
			}

			// If we got only a few files left it's not worth with this success rate to keep using the unrelibale method
			if (requestCount < 150)
			{
				useReliable = true;
				return;
			}

			// GG, loss is too high
			if (targetRate == MIN_TRANSFER_RATE)
			{
				useReliable = true;
				return;
			}

			targetRate = targetRate * successRate;
			if (targetRate < MIN_TRANSFER_RATE)
				targetRate = MIN_TRANSFER_RATE;
		}
	};

public:
	static constexpr int MAX_LUA_FILES = static_cast<int>(MAX_TRACKED_LUA_FILES);
	LuaPackEntry m_pLuaFileCache[MAX_LUA_FILES];

	PlayerQueue m_pPlayerQueue[ABSOLUTE_PLAYER_LIMIT];

	std::vector<int> m_pCompressQueue;
	std::mutex m_pCompressQueueMutex;

	std::vector<int> m_pStringTableUpdateQueue;
	std::mutex m_pStringTableUpdateQueueMutex;
	std::vector<int> m_pActiveHashRefreshQueue;
	std::mutex m_pActiveHashRefreshQueueMutex;

	std::atomic<ThreadState> m_pWorkerThreadState;
	ThreadHandle_t m_pWorkerThread = nullptr;

	Lua::ILuaInterfaceReference pInterface;
};
static LuaDataPack g_pLuaDataPack;

static SIMPLETHREAD_RETURNVALUE WorkerThread(void* pData)
{
	while (g_pLuaDataPack.m_pWorkerThreadState.load() == ThreadState::STATE_RUNNING)
	{
		ThreadSleep(50);

		std::vector<int> pWorkEntires;
		{
			std::lock_guard<std::mutex> lock(g_pLuaDataPack.m_pCompressQueueMutex);
			if (!g_pLuaDataPack.m_pCompressQueue.empty())
			{
				pWorkEntires = std::move(g_pLuaDataPack.m_pCompressQueue);
				g_pLuaDataPack.m_pCompressQueue.clear();
			}
		}

		// First pass
		// We do this first to have less overhead by being able to faster call from this thread to the main thread
		if (pWorkEntires.empty())
		{
			if (g_pGModDataPackModule.InDebug() >= 2)
				Msg(PROJECT_NAME " - gmoddatapack: Worker thread got no work!\n");

			continue;
		}

		{
			// We lock the lua state if we have one set.
			// We mainly split this into two passes to avoid blocking the main thread for long!
			Lua::ScopedThreadAccess pThreadScope;
			
			// Let's try to avoid an deadlock, we check here due to the main thread possibly shutting down so we cannot expect the StateAccess below to work.
			if (g_pLuaDataPack.m_pWorkerThreadState.load() != ThreadState::STATE_RUNNING)
				break;

			auto LUA = g_pLuaDataPack.pInterface.GetLua();
			Lua::StateAccess pState(LUA);
			for (int fileID : pWorkEntires)
			{
				if (g_pLuaDataPack.m_pWorkerThreadState.load() != ThreadState::STATE_RUNNING)
					break;

				LuaDataPack::LuaPackEntry* pEntry = &g_pLuaDataPack.m_pLuaFileCache[fileID];
				std::lock_guard<std::shared_mutex> lock(pEntry->mutex);
				if (pEntry->IsContentReady()) // Already done? Either we did it, or the main thread.
					continue;
				
				g_pLuaDataPack.ProcessContent(pEntry, fileID);
			}
		}

		// Second pass
		for (int fileID : pWorkEntires)
		{
			if (g_pLuaDataPack.m_pWorkerThreadState.load() != ThreadState::STATE_RUNNING)
				break;

			LuaDataPack::LuaPackEntry* pEntry = &g_pLuaDataPack.m_pLuaFileCache[fileID];
			std::lock_guard<std::shared_mutex> lock(pEntry->mutex);
			if (pEntry->IsReady() || !pEntry->IsContentReady()) // Already done? Either we did it, or the main thread.
				continue;
				
			g_pLuaDataPack.CompressFile(pEntry, fileID);
		}
	}

	g_pLuaDataPack.m_pWorkerThreadState.store(ThreadState::STATE_NOTRUNNING);
	return 0;
}

void LuaDataPack::Initialize()
{
	Shutdown();

	// If we load AFTER the server already started, we build our cache ourselves again.
	if (g_pDataPack)
	{
		for (int fileID=0; fileID<g_pDataPack->m_pClientLuaFiles->GetNumStrings(); ++fileID)
		{
			std::string fileName = g_pDataPack->m_pClientLuaFiles->GetString(fileID);
			GarrysMod::Lua::LuaFile* luaFile = Lua::GetShared()->GetCache(fileName);
			if (!luaFile)
				continue;

			HolyLib::LuaPack::CaptureFileContents(fileName, luaFile->contents);
			// Re-feed ready entries too. AddFileContents reuses an exact source/mode match,
			// but invalidates stale native/canonical bodies after a module reload or hook change.
			AddFileContents(fileName, luaFile->contents);
		}
	}

	m_pWorkerThreadState = ThreadState::STATE_RUNNING;
	m_pWorkerThread = CreateSimpleThread((ThreadFunc_t)WorkerThread, this);
}

void LuaDataPack::Shutdown()
{
	g_pLuaDataPack.pInterface.InvalidateInterface();
	if (m_pWorkerThread)
	{
		if (m_pWorkerThreadState.load() != ThreadState::STATE_NOTRUNNING)
		{
			m_pWorkerThreadState.store(ThreadState::STATE_SHOULD_SHUTDOWN);
			while (m_pWorkerThreadState.load() != ThreadState::STATE_NOTRUNNING) // Wait for shutdown
				ThreadSleep(0);
		}

		ReleaseThreadHandle(m_pWorkerThread);
		m_pWorkerThread = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(m_pActiveHashRefreshQueueMutex);
		m_pActiveHashRefreshQueue.clear();
	}
	for (LuaPackEntry& entry : m_pLuaFileCache)
	{
		std::lock_guard<std::shared_mutex> lock(entry.mutex);
		entry.activeHashRefreshPending = false;
	}

	// We keep it, simply because then we can re-use stuff
	//for (int i=0; i<MAX_LUA_FILES; ++i)
	//	m_pLuaFileCache[i].Clear();
}

#if defined(SYSTEM_LINUX)
enum class ClientLuaBaselineHashOrigin
{
	Cached,
	Computed,
};

static bool ResolveClientLuaBaselineHash(int fileID, const char* fileName,
	HolyLib::LuaPack::BaselineAction fileAction,
	const Bootil::AutoBuffer* requiredPayload, ClientLuaHash& output,
	ClientLuaBaselineHashOrigin& origin)
{
	const bool initFile = HolyLib::LuaPack::IsInitFile(fileName ? fileName : "");
	if (fileAction == HolyLib::LuaPack::BaselineAction::CanonicalStub &&
		!initFile && requiredPayload && requiredPayload->GetWritten() >= output.size())
	{
		const auto* hash = static_cast<const unsigned char*>(requiredPayload->GetBase());
		std::copy(hash, hash + output.size(), output.begin());
		origin = ClientLuaBaselineHashOrigin::Cached;
		return true;
	}

	LuaDataPack::LuaPackEntry* entry = g_pLuaDataPack.GetPackEntry(fileID);
	if (entry)
	{
		std::shared_lock<std::shared_mutex> lock(entry->mutex);
		if (fileAction == HolyLib::LuaPack::BaselineAction::NativeSource &&
			!initFile && entry->hasSourceContent && entry->sourceHashReady)
		{
			output = entry->sourceHash;
			origin = ClientLuaBaselineHashOrigin::Cached;
			return true;
		}
		if (initFile && entry->hasSourceContent && entry->luapackCanonical &&
			entry->contentHashReady)
		{
			// includes/init.lua is the one real globally registered LuaPack body. Its
			// canonical content hash already covers bootstrap + native source bytes.
			output = entry->contentHash;
			origin = ClientLuaBaselineHashOrigin::Cached;
			return true;
		}
		if (fileAction == HolyLib::LuaPack::BaselineAction::CanonicalStub &&
			entry->luapackCanonical && entry->contentHashReady)
		{
			output = entry->contentHash;
			origin = ClientLuaBaselineHashOrigin::Cached;
			return true;
		}
	}

	std::string source;
	bool hasSource = false;
	if (entry)
	{
		std::shared_lock<std::shared_mutex> lock(entry->mutex);
		if (entry->hasSourceContent)
		{
			source = entry->sourceContent;
			hasSource = true;
		}
	}
	if (!hasSource)
	{
		GarrysMod::Lua::LuaFile* luaFile = Lua::GetShared() && fileName
			? Lua::GetShared()->GetCache(fileName) : nullptr;
		if (luaFile)
		{
			source = luaFile->contents;
			hasSource = true;
		}
	}

	std::string baselineSource;
	if (fileAction == HolyLib::LuaPack::BaselineAction::CanonicalStub && !initFile)
		baselineSource = HolyLib::LuaPack::PrepareVanillaFile(fileName ? fileName : "", "");
	else if (hasSource)
		baselineSource = initFile
			? HolyLib::LuaPack::PrepareVanillaFile(fileName ? fileName : "", source)
			: source;
	else
		return false;

	output = HashClientLuaString(baselineSource);
	origin = ClientLuaBaselineHashOrigin::Computed;
	return true;
}

struct ClientLuaBaselineHashOverride
{
	CNetworkStringTableItem* item = nullptr;
	unsigned char* originalData = nullptr;
	int originalLength = 0;
	std::array<unsigned char, 32> replacementHash{};
};

class ScopedClientLuaBaseline
{
public:
	explicit ScopedClientLuaBaseline(int slot, HolyLib::LuaPack::BaselineAction action)
	{
		VPROF_BUDGET("HolyLib - LuaPack baseline preparation", VPROF_BUDGETGROUP_HOLYLIB);
		const bool validSlot = slot >= 0 && slot < ABSOLUTE_PLAYER_LIMIT;
		if (validSlot)
		{
			g_clientPinnedCanonicalFiles[slot].Reset();
			g_clientPinnedCanonicalPayloads[slot] = nullptr;
		}
		if (action != HolyLib::LuaPack::BaselineAction::BasePlusDelta &&
			action != HolyLib::LuaPack::BaselineAction::CanonicalStub &&
			action != HolyLib::LuaPack::BaselineAction::NativeSource)
		{
			failure = "unsupported Lua baseline action";
			return;
		}

		if (!g_pDataPack || !g_pDataPack->m_pClientLuaFiles)
		{
			failure = "client_lua_files is unavailable";
			return;
		}
		const bool trackNativeBaseline = validSlot &&
			(action == HolyLib::LuaPack::BaselineAction::BasePlusDelta ||
				(action == HolyLib::LuaPack::BaselineAction::NativeSource &&
					HolyLib::LuaPack::NeedsNativeHashUpdate(slot)));
		if (trackNativeBaseline)
		{
			g_clientNativeLuaHashes[slot].clear();
		}

		INetworkStringTable* networkTable = g_pDataPack->m_pClientLuaFiles;
		CNetworkStringTable* table = static_cast<CNetworkStringTable*>(networkTable);
		if (!table->m_pItems)
		{
			failure = "client_lua_files has no item dictionary";
			return;
		}

		if (validSlot && action == HolyLib::LuaPack::BaselineAction::BasePlusDelta)
		{
			requiredPayload = HolyLib::LuaPack::RequiredStubPayloadForClient(slot);
			if (!requiredPayload || requiredPayload->GetWritten() < 32)
			{
				failure = "the pinned canonical placeholder payload is unavailable";
				return;
			}
		}

		const int fileCount = networkTable->GetNumStrings();
		for (int fileID = 1; fileID < fileCount; ++fileID)
		{
			const char* fileName = networkTable->GetString(fileID);
			if (!fileName)
				continue;
			++baselineFiles;
			HolyLib::LuaPack::BaselineAction fileAction = action;
			if (action == HolyLib::LuaPack::BaselineAction::BasePlusDelta)
			{
				const HolyLib::LuaPack::BaselineDecision fileBaseline =
					HolyLib::LuaPack::DecidePinnedFileBaselineForClient(slot, fileName);
				if (fileBaseline.action == HolyLib::LuaPack::BaselineAction::Reject)
				{
					failure = std::string(fileBaseline.failure ? fileBaseline.failure : "map-base selection failed") +
						" for " + fileName;
					Restore();
					return;
				}
				fileAction = fileBaseline.action;
			}

			if (fileAction == HolyLib::LuaPack::BaselineAction::CanonicalStub &&
				!HolyLib::LuaPack::IsInitFile(fileName))
			{
				pinnedCanonicalFiles.Mark(fileID);
			}

			ClientLuaHash hash{};
			ClientLuaBaselineHashOrigin hashOrigin = ClientLuaBaselineHashOrigin::Computed;
			if (!ResolveClientLuaBaselineHash(fileID, fileName, fileAction,
				requiredPayload, hash, hashOrigin))
			{
				failure = std::string("source identity is unavailable for ") + fileName;
				Restore();
				return;
			}

			CNetworkStringTableItem& item = table->m_pItems->Element(fileID);
			using HolyLib::LuaPack::Policy::BaselineHashDisposition;
			const BaselineHashDisposition hashDisposition =
				HolyLib::LuaPack::Policy::SelectBaselineHashDisposition(
					PublishedLuaHashMatches(item, hash),
					hashOrigin == ClientLuaBaselineHashOrigin::Cached);
			if (hashDisposition == BaselineHashDisposition::ReusePublished)
			{
				++publishedHashes;
			}
			else
			{
				if (hashDisposition == BaselineHashDisposition::OverrideCached)
					++cachedHashes;
				else
					++computedHashes;

				overrides.emplace_back();
				ClientLuaBaselineHashOverride& replacement = overrides.back();
				replacement.item = &item;
				replacement.originalData = item.m_pUserData;
				replacement.originalLength = item.m_nUserDataLength;
				replacement.replacementHash = hash;
				item.m_pUserData = replacement.replacementHash.data();
				item.m_nUserDataLength = static_cast<int>(replacement.replacementHash.size());
			}
			if (trackNativeBaseline &&
				fileAction == HolyLib::LuaPack::BaselineAction::NativeSource &&
				!HolyLib::LuaPack::IsInitFile(fileName))
			{
				nativeBaselineHashes.emplace(fileID, hash);
			}
		}

		if (trackNativeBaseline)
		{
			auto& nativeHashes = g_clientNativeLuaHashes[slot];
			nativeHashes.insert(nativeBaselineHashes.begin(), nativeBaselineHashes.end());
		}
		if (requiredPayload)
		{
			g_clientPinnedCanonicalFiles[slot] = pinnedCanonicalFiles;
			g_clientPinnedCanonicalPayloads[slot] = requiredPayload;
		}
		valid = true;
	}

	~ScopedClientLuaBaseline()
	{
		Restore();
	}

	bool IsValid() const { return valid; }
	const char* Failure() const { return failure.c_str(); }
	unsigned int FileCount() const { return baselineFiles; }
	unsigned int PublishedHashCount() const { return publishedHashes; }
	unsigned int CachedHashCount() const { return cachedHashes; }
	unsigned int ComputedHashCount() const { return computedHashes; }
	unsigned int OverrideCount() const { return static_cast<unsigned int>(overrides.size()); }

private:
	void Restore()
	{
		VPROF_BUDGET("HolyLib - LuaPack baseline restoration", VPROF_BUDGETGROUP_HOLYLIB);
		for (ClientLuaBaselineHashOverride& replacement : overrides)
		{
			if (!replacement.item)
				continue;
			replacement.item->m_pUserData = replacement.originalData;
			replacement.item->m_nUserDataLength = replacement.originalLength;
			replacement.item = nullptr;
		}
		overrides.clear();
	}

	std::deque<ClientLuaBaselineHashOverride> overrides;
	std::unordered_map<int, ClientLuaHash> nativeBaselineHashes;
	PinnedCanonicalFiles pinnedCanonicalFiles;
	const Bootil::AutoBuffer* requiredPayload = nullptr;
	std::string failure;
	unsigned int baselineFiles = 0;
	unsigned int publishedHashes = 0;
	unsigned int cachedHashes = 0;
	unsigned int computedHashes = 0;
	bool valid = false;
};

static Detouring::Hook detour_CBaseClient_SendServerInfo;

static bool EnsureRequiredLuaReliableCapacity(CBaseClient* client, int slot,
	HolyLib::LuaPack::BaselineAction action, std::string& failure)
{
	using namespace HolyLib::LuaPack::Policy;
	if (action != HolyLib::LuaPack::BaselineAction::BasePlusDelta)
		return true;
	if (!client || !g_pDataPack || !g_pDataPack->m_pClientLuaFiles)
	{
		failure = "the client Lua string table is unavailable";
		return false;
	}

	INetChannel* engineChannel = client->GetNetChannel();
	CNetChan* channel = static_cast<CNetChan*>(engineChannel);
	if (!engineChannel || !channel)
	{
		failure = "the client netchannel is unavailable";
		return false;
	}

	const int registeredFiles = g_pDataPack->m_pClientLuaFiles->GetNumStrings();
	const std::size_t registeredStubCount = registeredFiles > 1
		? static_cast<std::size_t>(registeredFiles - 1) : 0u;
	const std::size_t perFrameStubCount = (std::min)(registeredStubCount,
		static_cast<std::size_t>((std::max)(1,
			gmoddatapack_luapack_required_stub_budget.GetInt())));
	const std::size_t compressedStubBytes =
		HolyLib::LuaPack::RequiredStubCompressedBytesForClient(slot);
	if (compressedStubBytes == 0)
	{
		failure = "the canonical placeholder body is unavailable";
		return false;
	}

	const std::size_t minimumRequiredBytes = RequiredStubReliableCapacityBytes(
		perFrameStubCount, compressedStubBytes);
	if (minimumRequiredBytes == (std::numeric_limits<std::size_t>::max)() ||
		minimumRequiredBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
	{
		failure = "the canonical placeholder burst exceeds the addressable reliable buffer";
		return false;
	}

	const int previousBits = channel->m_StreamReliable.GetMaxNumBits();
	if (previousBits <= 0)
	{
		failure = "the engine reported an invalid reliable buffer capacity";
		return false;
	}
	const std::size_t previousBytes =
		(static_cast<std::size_t>(previousBits) + 7u) / 8u;
	if (previousBytes >= minimumRequiredBytes)
		return true;

	// When growth is necessary, ask to preserve the channel's entire previous capacity
	// as sign-on headroom. The engine may clamp that preference, but the verified result
	// must still retain the 64 KiB minimum used for the no-growth decision.
	const std::size_t preferredBytes = RequiredStubReliableCapacityBytes(
		perFrameStubCount, compressedStubBytes, previousBytes);
	const std::size_t requestedBytes = preferredBytes !=
		(std::numeric_limits<std::size_t>::max)() && preferredBytes <=
		static_cast<std::size_t>((std::numeric_limits<int>::max)())
		? preferredBytes : minimumRequiredBytes;

	// Resizing an occupied scratch stream can invalidate bytes already staged by the
	// engine. SendServerInfo is normally entered with an empty scratch stream; if that
	// invariant changes, fail closed instead of copying through an unproven layout.
	if (channel->m_StreamReliable.GetNumBitsWritten() != 0)
	{
		failure = "reliable sign-on bytes were already staged before capacity reservation";
		return false;
	}

	engineChannel->SetMaxBufferSize(true, static_cast<int>(requestedBytes), false);
	const int actualBits = channel->m_StreamReliable.GetMaxNumBits();
	const std::size_t actualBytes = actualBits > 0
		? (static_cast<std::size_t>(actualBits) + 7u) / 8u : 0u;
	if (actualBytes < minimumRequiredBytes)
	{
		failure = "the engine refused the required reliable buffer capacity";
		return false;
	}

	Msg(PROJECT_NAME " - luapack: client slot %i reserved %u reliable bytes (previously %u) for up to %u canonical placeholders per frame\n",
		slot, static_cast<unsigned int>(actualBytes),
		static_cast<unsigned int>(previousBytes),
		static_cast<unsigned int>(perFrameStubCount));
	return true;
}

static bool QueueLuaPackServerInfo(CBaseClient* client)
{
	if (!client)
		return false;

	const int slot = client->GetPlayerSlot();
	INetChannel* channel = client->GetNetChannel();
	if (!HolyLib::LuaPack::Policy::ShouldScheduleLuaPackServerInfo(
		HolyLib::LuaPack::IsEnabled(),
		HolyLib::LuaPack::SupportsCanonicalRegistration(),
		HolyLib::LuaPack::RegistrationRefreshPending(),
		client->m_bSendServerInfo,
		client->m_nSignonState == SIGNONSTATE_CONNECTED,
		slot >= 0 && slot < ABSOLUTE_PLAYER_LIMIT, channel != nullptr))
	{
		return false;
	}

	PendingLuaPackServerInfo& pending = g_pendingLuaPackServerInfos[slot];
	if (g_luaPackServerInfoScheduler.IsScheduled(slot) &&
		pending.client == client && pending.channel == channel &&
		pending.userID == client->m_UserID &&
		pending.clientChallenge == client->m_clientChallenge)
	{
		++pending.coalescedPolls;
		return true;
	}

	// A slot can be reused before every late callback has run. Cancel the predecessor
	// and append the successor at the tail so a churned slot cannot inherit priority.
	if (g_luaPackServerInfoScheduler.IsScheduled(slot))
		g_luaPackServerInfoScheduler.Unschedule(slot);
	pending.client = client;
	pending.channel = channel;
	pending.userID = client->m_UserID;
	pending.clientChallenge = client->m_clientChallenge;
	pending.queuedAt = Plat_FloatTime();
	pending.coalescedPolls = 0;
	if (!g_luaPackServerInfoScheduler.Schedule(slot))
	{
		pending.Reset();
		return false;
	}
	return true;
}

static bool SendLuaPackServerInfoNow(CBaseClient* client,
	double queuedMilliseconds, unsigned int coalescedPolls)
{
	VPROF_BUDGET("HolyLib - LuaPack SendServerInfo", VPROF_BUDGETGROUP_HOLYLIB);
	const double totalStartedAt = Plat_FloatTime();
	const int slot = client ? client->GetPlayerSlot() : -1;
	// SendServerInfo is the ownership boundary for a new Lua baseline. A prior
	// connection or reconnect epoch must not retain queued bodies or a pinned-ID plan.
	ClearClientPinnedRequiredDeliveryState(slot);
	const HolyLib::LuaPack::BaselineDecision baseline = HolyLib::LuaPack::DecideBaselineForClient(slot);
	if (baseline.action == HolyLib::LuaPack::BaselineAction::Reject)
	{
		if (client)
			client->Disconnect("%s", baseline.failure ? baseline.failure : "required LuaPack baseline was rejected");
		return false;
	}

	std::string capacityFailure;
	if (!EnsureRequiredLuaReliableCapacity(client, slot, baseline.action, capacityFailure))
	{
		if (client)
			client->Disconnect("Required Lua delivery could not reserve its engine buffer: %s", capacityFailure.c_str());
		return false;
	}

	if (baseline.action == HolyLib::LuaPack::BaselineAction::Unchanged)
	{
		VPROF_BUDGET("HolyLib - engine SendServerInfo", VPROF_BUDGETGROUP_OTHER_NETWORKING);
		return detour_CBaseClient_SendServerInfo.GetTrampoline<Symbols::CBaseClient_SendServerInfo>()(client);
	}

	const double preparationStartedAt = Plat_FloatTime();
	ScopedClientLuaBaseline clientBaseline(slot, baseline.action);
	const double preparationMilliseconds = (Plat_FloatTime() - preparationStartedAt) * 1000.0;
	ClientRequiredStubQueue* telemetry = slot >= 0 && slot < ABSOLUTE_PLAYER_LIMIT
		? &g_clientRequiredStubQueues[slot] : nullptr;
	if (telemetry)
	{
		telemetry->baselineFiles = clientBaseline.FileCount();
		telemetry->baselinePublishedHashes = clientBaseline.PublishedHashCount();
		telemetry->baselineCachedHashes = clientBaseline.CachedHashCount();
		telemetry->baselineComputedHashes = clientBaseline.ComputedHashCount();
		telemetry->baselineOverrides = clientBaseline.OverrideCount();
		telemetry->baselinePreparationMilliseconds = preparationMilliseconds;
	}
	if (!clientBaseline.IsValid())
	{
		if (client)
			client->Disconnect("Lua delivery could not prepare a consistent file baseline: %s", clientBaseline.Failure());
		return false;
	}

	VPROF_BUDGET("HolyLib - engine SendServerInfo", VPROF_BUDGETGROUP_OTHER_NETWORKING);
	const double engineStartedAt = Plat_FloatTime();
	const bool result = detour_CBaseClient_SendServerInfo.GetTrampoline<Symbols::CBaseClient_SendServerInfo>()(client);
	const double engineMilliseconds = (Plat_FloatTime() - engineStartedAt) * 1000.0;
	if (telemetry)
		telemetry->engineServerInfoMilliseconds = engineMilliseconds;

	const double totalMilliseconds = (Plat_FloatTime() - totalStartedAt) * 1000.0;
	if (totalMilliseconds >= gmoddatapack_luapack_baseline_warn_ms.GetFloat())
	{
		Warning(PROJECT_NAME " - luapack: slow SendServerInfo baseline slot %i action %i: %u file(s), %u published + %u cached + %u computed hash(es), %u override(s), %.3f ms prepare / %.3f ms engine / %.3f ms total after %.3f ms queued (%u coalesced poll(s))\n",
			slot, static_cast<int>(baseline.action), clientBaseline.FileCount(),
			clientBaseline.PublishedHashCount(), clientBaseline.CachedHashCount(),
			clientBaseline.ComputedHashCount(), clientBaseline.OverrideCount(),
			preparationMilliseconds, engineMilliseconds, totalMilliseconds,
			queuedMilliseconds, coalescedPolls);
	}
	return result;
}

static bool hook_CBaseClient_SendServerInfo(CBaseClient* client)
{
	VPROF_BUDGET("HolyLib - LuaPack queue SendServerInfo", VPROF_BUDGETGROUP_HOLYLIB);
	if (QueueLuaPackServerInfo(client))
	{
		// The engine clears m_bSendServerInfo only inside the real call, and all known
		// callers ignore this return or interpret false as terminal failure. Report the
		// deferred request as accepted while leaving its authoritative flag untouched.
		return true;
	}
	return SendLuaPackServerInfoNow(client, 0.0, 0);
}

static CBaseClient* ResolveQueuedLuaPackServerInfoClient(int slot)
{
#if MODULE_EXISTS_GAMESERVER
	return Gameserver_GetClientBySlot(slot);
#else
	return Util::server ? Util::GetClientByIndex(slot) : nullptr;
#endif
}

static void DrainQueuedLuaPackServerInfos()
{
	if (HolyLib::LuaPack::BaselinePreparationPending())
		return;

	const unsigned int budget = static_cast<unsigned int>(
		(std::max)(1, gmoddatapack_luapack_serverinfo_budget.GetInt()));
	const std::size_t queuedAtFrameStart = g_luaPackServerInfoScheduler.Size();
	std::size_t inspected = 0;
	unsigned int serviced = 0;

	while (serviced < budget && inspected < queuedAtFrameStart &&
		!g_luaPackServerInfoScheduler.Empty())
	{
		const int slot = g_luaPackServerInfoScheduler.TakeNext();
		++inspected;
		if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
			continue;

		const PendingLuaPackServerInfo pending = g_pendingLuaPackServerInfos[slot];
		g_pendingLuaPackServerInfos[slot].Reset();
		CBaseClient* current = ResolveQueuedLuaPackServerInfoClient(slot);
		if (!current ||
			!HolyLib::LuaPack::Policy::QueuedServerInfoIdentityMatches(
				current == pending.client,
				current->GetNetChannel() == pending.channel,
				current->GetPlayerSlot() == slot,
				current->m_UserID == pending.userID,
				current->m_clientChallenge == pending.clientChallenge,
				current->m_nSignonState == SIGNONSTATE_CONNECTED,
				current->m_bSendServerInfo))
		{
			continue;
		}

		++serviced;
		const double queuedMilliseconds = pending.queuedAt > 0.0
			? (Plat_FloatTime() - pending.queuedAt) * 1000.0 : 0.0;
		// The token was removed before execution. A false result disconnects inside
		// the engine/current LuaPack path and is deliberately terminal, not retried.
		(void)SendLuaPackServerInfoNow(current, queuedMilliseconds,
			pending.coalescedPolls);
	}
}
#endif

static Detouring::Hook detour_GModDataPack_AddOrUpdateFile;
static Detouring::Hook detour_GModDataPack_SendFileToClient;
static Detouring::Hook detour_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack;
#if defined(SYSTEM_LINUX)
static Detouring::Hook detour_GModDataPack_OnFilesRequested;
static thread_local int g_requiredRequestProbeSlot = -1;
static thread_local bool g_requiredRequestProbeAccepted = false;
#endif

static bool IsGModDataPackModuleEnabled()
{
	IModuleWrapper* module = g_pModuleManager.GetModuleByID(HOLYLIB_MODULEID_GMODDATAPACK);
	return module && module->IsEnabled();
}

static bool ReadLuaAutoRefreshSource(const std::string& fileRelPath,
	const std::string& fileName, std::string& output)
{
	auto readPath = [&](const std::string& path, const char* pathID) -> bool
	{
		FileHandle_t handle = g_pFullFileSystem->Open(path.c_str(), "rb", pathID);
		if (handle == FILESYSTEM_INVALID_HANDLE)
			return false;

		const unsigned int size = g_pFullFileSystem->Size(handle);
		if (size > static_cast<unsigned int>((std::numeric_limits<int>::max)()))
		{
			g_pFullFileSystem->Close(handle);
			return false;
		}

		output.assign(size, '\0');
		const int read = size == 0 ? 0 :
			g_pFullFileSystem->Read(output.data(), static_cast<int>(size), handle);
		g_pFullFileSystem->Close(handle);
		return read == static_cast<int>(size);
	};

	if (!fileRelPath.empty() && readPath(fileRelPath, "MOD"))
		return true;
	return !fileName.empty() && readPath("lua/" + fileName, "GAME");
}

enum class LuaPackDiskRefreshResult
{
	NotEligible,
	InvalidPath,
	UnknownRegistration,
	Resolved,
	Unreadable,
	Unchanged,
	RescanQueued,
	Captured,
};

static LuaPackDiskRefreshResult ResolveExistingLuaRegistration(
	const std::string& fileRelPath, const std::string& fileName,
	int& fileID, std::string& registeredName)
{
	if (!g_pDataPack || !g_pDataPack->m_pClientLuaFiles)
		return LuaPackDiskRefreshResult::NotEligible;

	const HolyLib::LuaPack::Policy::LuaRefreshPathResolution resolution =
		HolyLib::LuaPack::Policy::ResolveExistingLuaRefreshPath(
			fileRelPath, fileName,
			[&](const std::string& candidate) -> bool
			{
				const int candidateID = g_pDataPack->m_pClientLuaFiles->FindStringIndex(
					candidate.c_str());
				if (candidateID <= 0 || candidateID == INVALID_STRING_INDEX)
					return false;
				fileID = candidateID;
				return true;
			},
			registeredName);
	if (resolution == HolyLib::LuaPack::Policy::LuaRefreshPathResolution::ExistingRegistration)
		return LuaPackDiskRefreshResult::Resolved;
	if (resolution == HolyLib::LuaPack::Policy::LuaRefreshPathResolution::UnknownRegistration)
		return LuaPackDiskRefreshResult::UnknownRegistration;
	return LuaPackDiskRefreshResult::InvalidPath;
}

static LuaPackDiskRefreshResult CaptureExistingLuaPackDiskRefresh(
	const std::string& fileRelPath, const std::string& fileName,
	const char* telemetryKind, bool recoverUnchanged)
{
	if (!IsGModDataPackModuleEnabled() || !g_pFullFileSystem || !g_pDataPack ||
		!g_pDataPack->m_pClientLuaFiles || !HolyLib::LuaPack::IsEnabled() ||
		!HolyLib::LuaPack::SupportsCanonicalRegistration())
	{
		return LuaPackDiskRefreshResult::NotEligible;
	}

	int fileID = INVALID_STRING_INDEX;
	std::string registeredName;
	const LuaPackDiskRefreshResult resolved = ResolveExistingLuaRegistration(
		fileRelPath, fileName, fileID, registeredName);
	if (resolved != LuaPackDiskRefreshResult::Resolved)
		return resolved;

	std::string source;
	const bool sourceReadable = ReadLuaAutoRefreshSource(
		fileRelPath, registeredName, source);
	bool sourceChanged = true;
	if (sourceReadable)
	{
		if (LuaDataPack::LuaPackEntry* entry = g_pLuaDataPack.GetPackEntry(fileID))
		{
			std::shared_lock<std::shared_mutex> lock(entry->mutex);
			sourceChanged = !entry->hasSourceContent || entry->sourceContent != source;
		}
	}
	const bool captureAndRescan = HolyLib::LuaPack::Policy::ShouldCaptureAutoRefresh(
		true, true, true, sourceReadable, sourceChanged);
	if (!captureAndRescan)
	{
		if (!sourceReadable)
		{
			Warning(PROJECT_NAME " - luapack: could not read %s refresh for existing client Lua registration \"%s\"\n",
				telemetryKind ? telemetryKind : "disk", registeredName.c_str());
			return LuaPackDiskRefreshResult::Unreadable;
		}
		if (HolyLib::LuaPack::Policy::ShouldQueueExplicitRefreshRecovery(
			recoverUnchanged, true, true, true, sourceReadable, sourceChanged))
		{
			g_pLuaDataPack.AddFileContents(registeredName, source, true);
			Msg(PROJECT_NAME " - luapack: queued explicit refresh recovery for existing client Lua registration \"%s\"\n",
				registeredName.c_str());
			return LuaPackDiskRefreshResult::RescanQueued;
		}
		return LuaPackDiskRefreshResult::Unchanged;
	}

	HolyLib::LuaPack::CaptureFileContents(registeredName, source);
	// A current server-side shared cache does not prove that a connected client
	// received or executed the changed bytes. If the trampoline already reached
	// AddOrUpdateFile, sourceChanged above is false because that hook updated the
	// LuaPack entry; otherwise this bypass path must always stage an active rescan.
	g_pLuaDataPack.AddFileContents(registeredName, source);
	Msg(PROJECT_NAME " - luapack: captured %s refresh for existing client Lua registration \"%s\"\n",
		telemetryKind ? telemetryKind : "disk", registeredName.c_str());
	return LuaPackDiskRefreshResult::Captured;
}

static bool hook_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack(
	const std::string* fileRelPath, const std::string* fileName,
	const std::string* fileExt)
{
	auto trampoline = detour_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack.GetTrampoline<
		Symbols::GarrysMod_AutoRefresh_HandleChange_Lua>();
#if defined(MODULE_EXISTS_AUTOREFRESH)
	if (HolyLib::AutoRefresh::RunPreLuaChange(fileRelPath, fileName, fileExt))
		return true;
#endif

	const bool originalHandled = trampoline(fileRelPath, fileName, fileExt);
	LuaPackDiskRefreshResult luaPackResult = LuaPackDiskRefreshResult::NotEligible;
	if (fileRelPath && fileName && fileExt && fileExt->compare(0, 3, "lua") == 0)
	{
		luaPackResult = CaptureExistingLuaPackDiskRefresh(
			*fileRelPath, *fileName, "auto", false);
	}
	const bool luaPackHandled = luaPackResult == LuaPackDiskRefreshResult::Unchanged ||
		luaPackResult == LuaPackDiskRefreshResult::Captured;

#if defined(MODULE_EXISTS_AUTOREFRESH)
	HolyLib::AutoRefresh::RunPostLuaChange(fileRelPath, fileName, fileExt);
#endif
	return originalHandled || luaPackHandled;
}

void HolyLib::GModDataPack::InstallLuaAutoRefreshDetour()
{
	if (DETOUR_ISVALID(detour_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack))
		return;

	SourceSDK::FactoryLoader serverLoader("server");
	// Category zero is deliberate: this is a shared ingress used by two optional
	// modules and must not disappear when either module alone is toggled off.
	Detour::Create(
		&detour_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack,
		"GarrysMod::AutoRefresh::HandleChange_Lua (shared)",
		serverLoader.GetModule(), Symbols::GarrysMod_AutoRefresh_HandleChange_LuaSym,
		(void*)hook_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack, 0
	);
}

bool HolyLib::LuaPack::SupportsCanonicalRegistration()
{
	// Required delivery needs the complete hook set: global canonical registration,
	// the per-connection baseline override, canonical/native body selection, and the
	// exact queue-client sender. Platform support alone is not enough; any disabled,
	// unresolved, or failed detour must keep required admission closed.
#if defined(SYSTEM_LINUX) && MODULE_EXISTS_GAMESERVER
	return DETOUR_ISVALID(detour_CBaseClient_SendServerInfo) &&
		DETOUR_ISENABLED(detour_CBaseClient_SendServerInfo) &&
		DETOUR_ISVALID(detour_GModDataPack_OnFilesRequested) &&
		DETOUR_ISENABLED(detour_GModDataPack_OnFilesRequested) &&
		DETOUR_ISVALID(detour_GModDataPack_AddOrUpdateFile) &&
		DETOUR_ISENABLED(detour_GModDataPack_AddOrUpdateFile) &&
		DETOUR_ISVALID(detour_GModDataPack_SendFileToClient) &&
		DETOUR_ISENABLED(detour_GModDataPack_SendFileToClient) &&
		DETOUR_ISVALID(detour_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack) &&
		DETOUR_ISENABLED(detour_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack) &&
		Gameserver_HasExactGModSender();
#else
	return false;
#endif
}

static void hook_GModDataPack_AddOrUpdateFile(GModDataPack* pDataPack, GarrysMod::Lua::LuaFile* file, bool bReCompress)
{
	g_pDataPack = pDataPack;
	HolyLib::LuaPack::CaptureFile(file);
	g_pLuaDataPack.AddFileContents(file->GetName(), file->GetContents());
	/*
	if (g_Lua && Lua::PushHook("HolyLib:AddOrUpdateFileToDataPack")) // Allows one to override the clientside content
	{
		g_Lua->PushString(file->GetName());
		g_Lua->PushString(file->GetSource());
		g_Lua->PushString(file->GetContents());
		g_Lua->CallFunctionProtected(4, 0, true);
	}

	lua_State* L = luaL_newstate();
	if (luaL_loadbuffer(L, file->contents.c_str(), file->contents.length(), file->GetName()) != LUA_OK)
	{
		const char* pError = lua_tolstring(L, -1, nullptr);
		Warning(PROJECT_NAME " - gmoddatapack: File \"%s\" contains invalid lua code! (%s)\n", file->GetName(), pError);

		detour_GModDataPack_AddOrUpdateFile.GetTrampoline<Symbols::GModDataPack_AddOrUpdateFile>()(pDataPack, file, bReCompress);
	} else {
		std::string content = file->GetContents();
		std::vector<Token> tokens = TokenizeContent(content);
		std::string finalCode = ProcessTokens(tokens);
		file->SetContents(finalCode.c_str());

		detour_GModDataPack_AddOrUpdateFile.GetTrampoline<Symbols::GModDataPack_AddOrUpdateFile>()(pDataPack, file, bReCompress);

		// file->SetContents(content);
		// We restore the original content in case it's a shared file.
		// Since we forced it to re-compress the file buffer now contains our processed content, which will be sent to clients.
	}
	lua_close(L);*/

	//if (!pDataPack->m_pClientLuaFiels)
	//	return;

	// NOTE:
	// GMod stores the SHA256 hash in the string userdata for the client to compare against
	// The SHA256 is generated by GModDataPack::GetHashFromString
}

static void SendCompressedLuaFile(int clientIdx, int fileID, const Bootil::AutoBuffer& compressed)
{
	char pBuffer[1 << 16];
	bf_write msg(pBuffer, sizeof(pBuffer));

	msg.WriteByte(GarrysMod::NetworkMessage::LuaFileDownload);
	msg.WriteUBitLong(fileID, 16);
	msg.WriteBytes(compressed.GetBase(), compressed.GetWritten());

	Util::engineserver->GMOD_SendToClient(clientIdx, msg.GetData(), msg.GetNumBitsWritten());

	if (g_pGModDataPackModule.InDebug())
		Msg(PROJECT_NAME " - gmoddatapack: Sent FileID %i though reliable stream!\n", fileID);
}

static HolyLib::LuaPack::Policy::RequiredStubDrainAction PrepareCanonicalLuaStubAppend(
	int clientIdx, const Bootil::AutoBuffer& compressed,
	bool enabled, bool canonicalRegistration, bool filePinned,
	CNetChan*& channel)
{
	using namespace HolyLib::LuaPack::Policy;
	static_assert(NETMSG_TYPE_BITS == RequiredStubServiceTypeBits,
		"LuaPack reliable capacity math must match the engine message type width");
	channel = nullptr;

	CBaseClient* client = nullptr;
#if MODULE_EXISTS_GAMESERVER
	client = Gameserver_GetClientBySlot(clientIdx);
#else
	client = Util::server ? Util::GetClientByIndex(clientIdx) : nullptr;
#endif
	INetChannel* engineChannel = client ? client->GetNetChannel() : nullptr;
	CNetChan* candidate = static_cast<CNetChan*>(engineChannel);
	const bool clientConnected = client && engineChannel && candidate &&
		client->IsConnected() && client->GetPlayerSlot() == clientIdx;
	if (clientConnected && candidate->m_StreamReliable.GetNumBitsLeft() < 0)
		return RequiredStubDrainAction::Reject;

	const RequiredStubDrainAction action = SelectRequiredStubDrainAction(
		enabled, canonicalRegistration, filePinned, true, compressed.GetWritten(),
		clientConnected, clientConnected && candidate->m_StreamReliable.IsOverflowed(),
		clientConnected ? static_cast<std::size_t>(candidate->m_StreamReliable.GetNumBitsLeft()) : 0u);
	if (action == RequiredStubDrainAction::Append)
		channel = candidate;
	return action;
}

static bool AppendCanonicalLuaStubToChannel(CNetChan* channel, int clientIdx,
	int fileID, const Bootil::AutoBuffer& compressed)
{
	using namespace HolyLib::LuaPack::Policy;
	if (!channel)
		return false;

	bf_write& reliable = channel->m_StreamReliable;
	const std::size_t compressedBytes = compressed.GetWritten();
	// CNetChan::SendNetMsg ultimately invokes the GMod message's WriteToBuffer,
	// which appends exactly this envelope to m_StreamReliable. This helper writes
	// the identical record; its caller decides whether that happens immediately for
	// an exceptional slow path or during the bounded required-placeholder drain.
	const bool appended = AppendRequiredStubWire(reliable, svc_GMod_ServerToClient,
		GarrysMod::NetworkMessage::LuaFileDownload,
		static_cast<std::uint32_t>(fileID), compressed.GetBase(), compressedBytes);
	if (appended && g_pGModDataPackModule.InDebug())
		Msg(PROJECT_NAME " - gmoddatapack: Appended canonical FileID %i to the reliable stream\n", fileID);
	return appended;
}

static bool AppendCanonicalLuaStub(int clientIdx, int fileID,
	const Bootil::AutoBuffer& compressed)
{
	VPROF_BUDGET("HolyLib - LuaPack append canonical stub", VPROF_BUDGETGROUP_OTHER_NETWORKING);
	CNetChan* channel = nullptr;
	if (PrepareCanonicalLuaStubAppend(clientIdx, compressed, true, true, true, channel) !=
		HolyLib::LuaPack::Policy::RequiredStubDrainAction::Append)
	{
		return false;
	}
	return AppendCanonicalLuaStubToChannel(channel, clientIdx, fileID, compressed);
}

static void SendLuaFile(int clientIdx, int fileID, LuaDataPack::LuaPackEntry* pEntry, bool bNoFastTransmit = false)
{
	if (!bNoFastTransmit && gmoddatapack_fastnetworking.GetBool() && clientIdx < ABSOLUTE_PLAYER_LIMIT)
	{
		if (g_pGModDataPackModule.InDebug())
			Msg(PROJECT_NAME " - gmoddatapack: Client requested fileID %i! adding to queue...\n", fileID);

		LuaDataPack::PlayerQueue& pQueue = g_pLuaDataPack.m_pPlayerQueue[clientIdx];
		pQueue.pQueue.push_back(fileID);
		++pQueue.requestCount;
		return;
	}

	SendCompressedLuaFile(clientIdx, fileID, pEntry->compressed);
}

static void SendOriginalLuaFile(GModDataPack* pDataPack, int clientIdx, int fileID)
{
	detour_GModDataPack_SendFileToClient.GetTrampoline<Symbols::GModDataPack_SendFileToClient>()(pDataPack, clientIdx, fileID);
}

static bool SendClientLuaHashUpdate(int clientIdx, int fileID, const unsigned char* hash, size_t hashLength)
{
	if (!g_pDataPack || !g_pDataPack->m_pClientLuaFiles || !hash || hashLength != 32)
		return false;

	CBaseClient* client = Util::server ? Util::GetClientByIndex(clientIdx) : nullptr;
	if (!client || !client->GetNetChannel())
		return false;

	INetworkStringTable* table = g_pDataPack->m_pClientLuaFiles;
	CNetworkStringTable* concreteTable = static_cast<CNetworkStringTable*>(table);
	if (fileID <= 0 || fileID >= table->GetNumStrings())
		return false;
	if (!concreteTable->m_bUserDataFixedSize ||
		concreteTable->m_nUserDataSizeBits != static_cast<int>(hashLength * 8))
	{
		return false;
	}

	char updateBuffer[64];
	SVC_UpdateStringTable update;
	update.m_DataOut.StartWriting(updateBuffer, sizeof(updateBuffer));
	update.m_nTableID = table->GetTableId();
	update.m_nChangedEntries = 1;

	if (!HolyLib::LuaPack::Policy::AppendClientLuaHashUpdate(update.m_DataOut,
		static_cast<std::uint32_t>(fileID), table->GetEntryBits(), hash, hashLength))
	{
		return false;
	}

	return !update.m_DataOut.IsOverflowed() && client->SendNetMsg(update, true);
}

static bool RequestActiveClientLuaFiles(int clientIdx)
{
	CBaseClient* client = nullptr;
#if MODULE_EXISTS_GAMESERVER
	client = Gameserver_GetClientBySlot(clientIdx);
#else
	client = Util::server ? Util::GetClientByIndex(clientIdx) : nullptr;
#endif
	if (!Util::engineserver || !client || !client->IsActive() || !client->GetNetChannel())
		return false;

	unsigned char requestBuffer[1]{};
	bf_write request(requestBuffer, sizeof(requestBuffer));
	request.WriteByte(GarrysMod::NetworkMessage::RequestLuaFiles);
	if (request.IsOverflowed())
		return false;

	// A string-table update changes the advertised identity, but active GMod clients
	// do not rescan client_lua_files until this message asks them to compare the table.
	// It shares the reliable stream with the preceding hash updates, preserving order.
	Util::engineserver->GMOD_SendToClient(clientIdx, request.GetData(),
		request.GetNumBitsWritten());
	return true;
}

static void DrainActiveLuaHashRefreshes()
{
	std::vector<int> pending;
	{
		std::lock_guard<std::mutex> lock(g_pLuaDataPack.m_pActiveHashRefreshQueueMutex);
		pending = std::move(g_pLuaDataPack.m_pActiveHashRefreshQueue);
		g_pLuaDataPack.m_pActiveHashRefreshQueue.clear();
	}
	if (pending.empty())
		return;

	std::sort(pending.begin(), pending.end());
	pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
	if (!HolyLib::LuaPack::IsEnabled() || !HolyLib::LuaPack::SupportsCanonicalRegistration() ||
		!g_pDataPack || !g_pDataPack->m_pClientLuaFiles)
	{
		return;
	}

	const std::size_t budget = static_cast<std::size_t>((std::max)(1,
		gmoddatapack_luapack_registration_refresh_budget.GetInt()));
	std::vector<int> retry;
	unsigned int nativeUpdates = 0;
	unsigned int canonicalUpdates = 0;
	unsigned int updatedFiles = 0;
	unsigned int requestedScans = 0;
	std::size_t sentUpdates = 0;
	std::size_t processed = 0;
	std::array<std::size_t, ABSOLUTE_PLAYER_LIMIT> stagedUpdatesBySlot{};
	for (std::size_t index = 0; index < pending.size(); ++index)
	{
		if (processed >= budget || sentUpdates >= budget)
		{
			retry.insert(retry.end(), pending.begin() + index, pending.end());
			break;
		}
		++processed;

		const int fileID = pending[index];
		LuaDataPack::LuaPackEntry* entry = g_pLuaDataPack.GetPackEntry(fileID);
		if (!entry || fileID <= 0 || fileID >= g_pDataPack->m_pClientLuaFiles->GetNumStrings())
			continue;

		const char* registeredName = g_pDataPack->m_pClientLuaFiles->GetString(fileID);
		const std::string fileName = registeredName ? registeredName : "";
		if (fileName.empty() || HolyLib::LuaPack::IsInitFile(fileName))
			continue;

		ClientLuaHash sourceHash{};
		ClientLuaHash canonicalHash{};
		{
			std::shared_lock<std::shared_mutex> lock(entry->mutex);
			if (!entry->hasSourceContent || !entry->sourceHashReady ||
				!entry->contentHashReady || !entry->hashPublished)
			{
				retry.push_back(fileID);
				continue;
			}
			sourceHash = entry->sourceHash;
			canonicalHash = entry->contentHash;
		}

		bool retryFile = false;
		bool updatedFile = false;
		for (int slot = 0; slot < ABSOLUTE_PLAYER_LIMIT; ++slot)
		{
			CBaseClient* client = nullptr;
#if MODULE_EXISTS_GAMESERVER
			client = Gameserver_GetClientBySlot(slot);
#else
			client = Util::server ? Util::GetClientByIndex(slot) : nullptr;
#endif
			if (!client || !client->IsActive() || !client->GetNetChannel())
				continue;

			const HolyLib::LuaPack::BaselineDecision baseline =
				HolyLib::LuaPack::DecideFileBaselineForClient(slot, fileName);
			HolyLib::LuaPack::Policy::Action fileAction = HolyLib::LuaPack::Policy::Action::Reject;
			if (baseline.action == HolyLib::LuaPack::BaselineAction::NativeSource)
				fileAction = HolyLib::LuaPack::Policy::Action::Native;
			else if (baseline.action == HolyLib::LuaPack::BaselineAction::CanonicalStub)
				fileAction = HolyLib::LuaPack::Policy::Action::CanonicalStub;

			auto& nativeHashes = g_clientNativeLuaHashes[slot];
			auto& pendingHashes = g_clientHashUpdatesPending[slot];
			const bool nativeHashKnown = nativeHashes.find(fileID) != nativeHashes.end();
			const bool nativeHashMatches = HolyLib::LuaPack::Policy::NativeHashMatches(
				nativeHashes, fileID, sourceHash);
			const auto refresh = HolyLib::LuaPack::Policy::SelectActiveHashRefresh(
				true, fileAction, nativeHashKnown, nativeHashMatches);
			const ClientLuaHash* targetHash = nullptr;
			if (refresh == HolyLib::LuaPack::Policy::ActiveHashRefreshAction::Native)
				targetHash = &sourceHash;
			else if (refresh == HolyLib::LuaPack::Policy::ActiveHashRefreshAction::Canonical)
				targetHash = &canonicalHash;
			const bool targetHashAlreadyPending = targetHash &&
				HolyLib::LuaPack::Policy::NativeHashMatches(pendingHashes, fileID, *targetHash);
			if (!HolyLib::LuaPack::Policy::ShouldStageActiveHashRefresh(
				refresh, targetHashAlreadyPending))
			{
				continue;
			}
			if (refresh != HolyLib::LuaPack::Policy::ActiveHashRefreshAction::None &&
				sentUpdates >= budget)
			{
				retryFile = true;
				break;
			}
			if (refresh == HolyLib::LuaPack::Policy::ActiveHashRefreshAction::Native)
			{
				if (!SendClientLuaHashUpdate(slot, fileID, sourceHash.data(), sourceHash.size()))
				{
					retryFile = true;
					continue;
				}
				HolyLib::LuaPack::Policy::RememberNativeHash(pendingHashes, fileID, sourceHash);
				++nativeUpdates;
				++sentUpdates;
				++stagedUpdatesBySlot[slot];
				updatedFile = true;
			}
			else if (refresh == HolyLib::LuaPack::Policy::ActiveHashRefreshAction::Canonical)
			{
				if (!SendClientLuaHashUpdate(slot, fileID,
					canonicalHash.data(), canonicalHash.size()))
				{
					retryFile = true;
					continue;
				}
				HolyLib::LuaPack::Policy::RememberNativeHash(pendingHashes, fileID, canonicalHash);
				++canonicalUpdates;
				++sentUpdates;
				++stagedUpdatesBySlot[slot];
				updatedFile = true;
			}
		}
		if (retryFile)
			retry.push_back(fileID);
		if (updatedFile)
			++updatedFiles;
	}
	for (int slot = 0; slot < ABSOLUTE_PLAYER_LIMIT; ++slot)
	{
		if (HolyLib::LuaPack::Policy::ShouldRequestActiveLuaScan(
			stagedUpdatesBySlot[slot]) && RequestActiveClientLuaFiles(slot))
		{
			++requestedScans;
		}
	}

	if (!retry.empty())
	{
		std::lock_guard<std::mutex> lock(g_pLuaDataPack.m_pActiveHashRefreshQueueMutex);
		g_pLuaDataPack.m_pActiveHashRefreshQueue.insert(
			g_pLuaDataPack.m_pActiveHashRefreshQueue.end(), retry.begin(), retry.end());
	}
	if (nativeUpdates != 0 || canonicalUpdates != 0)
	{
		Msg(PROJECT_NAME " - luapack: active hot refresh staged %u native and %u canonical per-client hash update(s) across %u file(s), then requested %u client Lua rescan(s)\n",
			nativeUpdates, canonicalUpdates, updatedFiles, requestedScans);
	}
}

static void ReportActiveLuaHashRefreshAcknowledgements()
{
	if (g_activeHashRefreshNativeAcknowledgements == 0 &&
		g_activeHashRefreshCanonicalAcknowledgements == 0)
	{
		return;
	}

	Msg(PROJECT_NAME " - luapack: active hot refresh acknowledged %u native and %u canonical file request(s)\n",
		g_activeHashRefreshNativeAcknowledgements,
		g_activeHashRefreshCanonicalAcknowledgements);
	g_activeHashRefreshNativeAcknowledgements = 0;
	g_activeHashRefreshCanonicalAcknowledgements = 0;
}

static bool GetNativeLuaHash(int fileID, GarrysMod::Lua::LuaFile* luaFile, std::array<unsigned char, 32>& output)
{
	LuaDataPack::LuaPackEntry* entry = g_pLuaDataPack.GetPackEntry(fileID);
	if (entry)
	{
		std::shared_lock<std::shared_mutex> lock(entry->mutex);
		if (entry->hasSourceContent && entry->sourceHashReady &&
			(!luaFile || entry->sourceContent == luaFile->contents))
		{
			output = entry->sourceHash;
			return true;
		}
	}

	std::string source;
	bool hasSource = false;
	if (luaFile)
	{
		source = luaFile->contents;
		hasSource = true;
	}
	else
	{
		if (entry)
		{
			std::shared_lock<std::shared_mutex> lock(entry->mutex);
			if (entry->hasSourceContent)
			{
				source = entry->sourceContent;
				hasSource = true;
			}
		}
	}

	if (!hasSource)
		return false;
	output = HashClientLuaString(source);
	return true;
}

static void DisconnectLuaHashFailure(int clientIdx, const char* fileName, const char* failure)
{
	Warning(PROJECT_NAME " - luapack: disconnecting client slot %i because %s for %s\n",
		clientIdx, failure ? failure : "Lua hash identity failed", fileName ? fileName : "?");
	CBaseClient* client = nullptr;
#if MODULE_EXISTS_GAMESERVER
	client = Gameserver_GetClientBySlot(clientIdx);
#else
	client = Util::server ? Util::GetClientByIndex(clientIdx) : nullptr;
#endif
	if (client)
		client->Disconnect("Lua delivery identity failed for %s", fileName ? fileName : "an unknown file");
}

static void DrainRequiredStubQueues()
{
	using namespace HolyLib::LuaPack::Policy;
	const unsigned int budget = static_cast<unsigned int>(
		(std::max)(1, gmoddatapack_luapack_required_stub_budget.GetInt()));
	unsigned int sentThisFrame = 0;
	std::size_t consecutiveNoProgress = 0;
	std::array<bool, ABSOLUTE_PLAYER_LIMIT> advancedThisFrame{};

	while (sentThisFrame < budget && !g_requiredStubScheduler.Empty())
	{
		const int slot = g_requiredStubScheduler.TakeNext();
		if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
			continue;

		ClientRequiredStubQueue& queue = g_clientRequiredStubQueues[slot];
		if (queue.pending.Empty())
			continue;

		const int fileID = queue.pending.Front();
		const Bootil::AutoBuffer* payload = g_clientPinnedCanonicalPayloads[slot];
		CNetChan* channel = nullptr;
		const RequiredStubDrainAction action = payload
			? PrepareCanonicalLuaStubAppend(slot, *payload,
				HolyLib::LuaPack::IsEnabled(),
				HolyLib::LuaPack::SupportsCanonicalRegistration(),
				g_clientPinnedCanonicalFiles[slot].Contains(fileID), channel)
			: RequiredStubDrainAction::Reject;

		if (action == RequiredStubDrainAction::WaitForReliableSpace)
		{
			g_requiredStubScheduler.Schedule(slot);
			++consecutiveNoProgress;
			if (consecutiveNoProgress >= g_requiredStubScheduler.Size())
				break;
			continue;
		}

		if (action == RequiredStubDrainAction::Reject ||
			!HolyLib::LuaPack::RecordPinnedRequiredStubForClient(slot) ||
			!AppendCanonicalLuaStubToChannel(channel, slot, fileID, *payload))
		{
			const char* fileName = g_pDataPack && g_pDataPack->m_pClientLuaFiles &&
				fileID > 0 && fileID < g_pDataPack->m_pClientLuaFiles->GetNumStrings()
				? g_pDataPack->m_pClientLuaFiles->GetString(fileID) : "?";
			DisconnectLuaHashFailure(slot, fileName,
				"a queued required placeholder lost its pinned identity or reliable channel");
			ClearClientRequiredStubQueue(slot);
			consecutiveNoProgress = 0;
			continue;
		}

		if (!advancedThisFrame[slot])
		{
			advancedThisFrame[slot] = true;
			++queue.drainFrames;
		}
		queue.pending.Pop();
		++queue.sent;
		++sentThisFrame;
		consecutiveNoProgress = 0;
		if (queue.pending.Empty())
			continue;
		g_requiredStubScheduler.Schedule(slot);
	}
}

static bool RefreshRegistrationModeForRequest(int fileID, const std::string& fileName,
	GarrysMod::Lua::LuaFile* luaFile)
{
	LuaDataPack::LuaPackEntry* entry = g_pLuaDataPack.GetPackEntry(fileID);
	if (!entry)
		return true;

	const bool enabled = HolyLib::LuaPack::IsEnabled();
	const bool canonicalRegistration = HolyLib::LuaPack::SupportsCanonicalRegistration();
	std::string source;
	bool hasSource = false;
	bool modeMatches = false;
	bool cachedHasSource = false;
	{
		std::shared_lock<std::shared_mutex> lock(entry->mutex);
		cachedHasSource = entry->hasSourceContent;
		modeMatches = HolyLib::LuaPack::Policy::RegistrationModeMatches(
			entry->luapackCanonical, entry->luapackPassthrough,
			enabled, canonicalRegistration);
		if (!HolyLib::LuaPack::Policy::RegistrationNeedsHashPublication(
			entry->luapackCanonical, entry->luapackPassthrough,
			entry->hasSourceContent, entry->IsContentReady(), entry->hashPublished,
			enabled, canonicalRegistration))
			return true;
		if ((!modeMatches || !cachedHasSource) && entry->hasSourceContent)
		{
			source = entry->sourceContent;
			hasSource = true;
		}
	}

	// A cvar or hook transition can occur after the previous Think. Repair this exact
	// request before consulting IsReady so the stale mode never sends one body/hash pair.
	if ((!modeMatches || !cachedHasSource) && luaFile)
	{
		source = luaFile->contents;
		hasSource = true;
	}
	if (!modeMatches || !cachedHasSource)
	{
		if (!hasSource)
			return false;
		if (enabled)
			HolyLib::LuaPack::CaptureFileContents(fileName, source);
		g_pLuaDataPack.AddFileContents(fileName, source);
	}
	// A matching mode can be pending publication without needing the source again.
	// Mode changes, however, must never reuse the old transformed body as source.
	return g_pLuaDataPack.PublishRegistrationHash(fileID);
}

static bool SendNativeLuaFile(GModDataPack* pDataPack, int clientIdx, int fileID,
	const std::string& fileName, GarrysMod::Lua::LuaFile* luaFile)
{
	if (HolyLib::LuaPack::NeedsNativeHashUpdate(clientIdx) && !HolyLib::LuaPack::IsInitFile(fileName))
	{
		std::array<unsigned char, 32> nativeHash{};
		if (!GetNativeLuaHash(fileID, luaFile, nativeHash))
		{
			DisconnectLuaHashFailure(clientIdx, fileName.c_str(), "the native hash could not be built");
			return false;
		}

		auto& nativeHashes = g_clientNativeLuaHashes[clientIdx];
		auto& pendingHashes = g_clientHashUpdatesPending[clientIdx];
		auto pendingHash = pendingHashes.find(fileID);
		const bool requestedHashMatchesNative = pendingHash != pendingHashes.end() &&
			pendingHash->second == nativeHash;
		if (requestedHashMatchesNative)
			++g_activeHashRefreshNativeAcknowledgements;
		if (!HolyLib::LuaPack::Policy::NativeHashMatches(nativeHashes, fileID, nativeHash))
		{
			// Receipt of this body request proves the client processed the staged hash
			// and rescan. Only send another ordered update when the requested identity
			// is stale or came from another publication path.
			if (!requestedHashMatchesNative &&
				!SendClientLuaHashUpdate(clientIdx, fileID, nativeHash.data(), nativeHash.size()))
			{
				DisconnectLuaHashFailure(clientIdx, fileName.c_str(), "the native hash update could not be sent");
				return false;
			}
			HolyLib::LuaPack::Policy::RememberNativeHash(nativeHashes, fileID, nativeHash);
		}
		if (pendingHash != pendingHashes.end())
			pendingHashes.erase(pendingHash);
	}
	else if (clientIdx >= 0 && clientIdx < ABSOLUTE_PLAYER_LIMIT)
	{
		g_clientHashUpdatesPending[clientIdx].erase(fileID);
	}

	SendOriginalLuaFile(pDataPack, clientIdx, fileID);
	return true;
}

static void hook_GModDataPack_SendFileToClient(GModDataPack* pDataPack, int clientIdx, int fileID)
{
#if defined(SYSTEM_LINUX)
	// The required batch decoder asks the engine parser to consume its normal request
	// allowance with one harmless valid ID. Intercept only that thread-local probe;
	// every real body request continues below.
	if (g_requiredRequestProbeSlot == clientIdx)
	{
		g_requiredRequestProbeAccepted = true;
		return;
	}
#endif
	VPROF_BUDGET("HolyLib - GModDataPack Lua request", VPROF_BUDGETGROUP_HOLYLIB);
	if (!pDataPack || !pDataPack->m_pClientLuaFiles)
	{
		Warning(PROJECT_NAME " - gmoddatapack: Invalid datapack or missing client_lua_files stringtable?\n");
		return;
	}
	g_pDataPack = pDataPack;

	INetworkStringTable* clientFiles = pDataPack->m_pClientLuaFiles;
	if (fileID <= 0 || fileID >= clientFiles->GetNumStrings()) // NOTE: GMod only checks < 0 BUT the index 0 is used for paths... too lazy to report it rn
	{
		// Deliberately NOT forwarded to the engine when luapack is enabled: a legitimate
		// client never requests an out-of-range ID, and index 0 reaches engine code that
		// treats it as the path table. Dropping it cannot strand a real join.
		Warning(PROJECT_NAME " - gmoddatapack: Client requesting crazy file number (%i)\n", fileID);
		return;
	}

	// SendServerInfo already resolved every required base/delta decision against the
	// exact string-table baseline advertised to this connection. Most cold joins then
	// request all canonical IDs in one CNetChan::ProcessMessages call. Repeating the Lua
	// cache lookup, path normalization, registry lock, and base/current identity lookup
	// for each unchanged ID makes that one packet scale linearly into a frame stall.
	// Source or registration-mode changes invalidate the affected bit before publication,
	// so only the unchanged baseline-owned IDs may take this O(1) body path.
	if (clientIdx >= 0 && clientIdx < ABSOLUTE_PLAYER_LIMIT)
	{
		const bool enabled = HolyLib::LuaPack::IsEnabled();
		const bool canonicalRegistration = HolyLib::LuaPack::SupportsCanonicalRegistration();
		const bool filePinned = g_clientPinnedCanonicalFiles[clientIdx].Contains(fileID);
		if (enabled && canonicalRegistration && filePinned)
		{
			const Bootil::AutoBuffer* pinnedPayload = g_clientPinnedCanonicalPayloads[clientIdx];
			const std::size_t compressedBytes = pinnedPayload ? pinnedPayload->GetWritten() : 0u;
			if (HolyLib::LuaPack::Policy::CanUsePinnedRequiredStub(
				enabled, canonicalRegistration, filePinned,
				pinnedPayload != nullptr, compressedBytes))
			{
				// Do not stage a complete cold baseline while CNetChan is processing the
				// client's single batched request. The main-thread drain revalidates the
				// exact pinned ID/lane and appends a globally bounded number each frame.
				if (!EnqueuePinnedRequiredStub(clientIdx, fileID))
				{
					DisconnectLuaHashFailure(clientIdx, clientFiles->GetString(fileID),
						"the pinned canonical placeholder could not enter its bounded queue");
				}
				return;
			}
		}
	}

	std::string fileName = clientFiles->GetString(fileID);
	GarrysMod::Lua::LuaFile* luaFile = Lua::GetShared()->GetCache(fileName);
	bool registrationReady = false;
	{
		VPROF_BUDGET("HolyLib - LuaPack registration check", VPROF_BUDGETGROUP_HOLYLIB);
		registrationReady = RefreshRegistrationModeForRequest(fileID, fileName, luaFile);
	}
	if (!registrationReady)
	{
		DisconnectLuaHashFailure(clientIdx, fileName.c_str(),
			"the cached Lua registration mode changed but its native source is unavailable");
		return;
	}
	HolyLib::LuaPack::DeliveryDecision delivery;
	{
		VPROF_BUDGET("HolyLib - LuaPack delivery selection", VPROF_BUDGETGROUP_HOLYLIB);
		delivery = HolyLib::LuaPack::DecideDeliveryForClient(
			clientIdx, fileName, luaFile ? luaFile->contents.length() : 0);
	}
	if (delivery.action == HolyLib::LuaPack::DeliveryAction::Stub && delivery.compressed)
	{
		// A stub is a normal, reliably delivered LuaFileDownload. It preserves the engine's
		// per-file barrier contract while the multi-megabyte payload stays exclusively on FastDL.
		// Its identity is already canonical in this connection's server-info baseline, so a
		// same-hash string-table update here would only make the client process 1 update per file.
		if (delivery.compressed->GetWritten() < 32)
		{
			DisconnectLuaHashFailure(clientIdx, fileName.c_str(), "the canonical placeholder payload is incomplete");
			return;
		}

		if (clientIdx >= 0 && clientIdx < ABSOLUTE_PLAYER_LIMIT)
		{
			auto& nativeHashes = g_clientNativeLuaHashes[clientIdx];
			auto nativeHash = nativeHashes.find(fileID);
			CBaseClient* client = Util::server ? Util::GetClientByIndex(clientIdx) : nullptr;
			const bool clientActive = client && client->IsActive();
			auto& pendingHashes = g_clientHashUpdatesPending[clientIdx];
			auto pendingHash = pendingHashes.find(fileID);
			const bool publishedHashMatchesCanonical = pendingHash != pendingHashes.end() &&
				std::equal(pendingHash->second.begin(), pendingHash->second.end(),
					static_cast<const unsigned char*>(delivery.compressed->GetBase()));
			if (publishedHashMatchesCanonical)
				++g_activeHashRefreshCanonicalAcknowledgements;
			if (HolyLib::LuaPack::Policy::NeedsOrderedCanonicalHash(clientActive,
				nativeHash != nativeHashes.end(), publishedHashMatchesCanonical))
			{
				if (!SendClientLuaHashUpdate(clientIdx, fileID,
					static_cast<const unsigned char*>(delivery.compressed->GetBase()), 32))
				{
					DisconnectLuaHashFailure(clientIdx, fileName.c_str(), "the canonical placeholder hash could not be restored");
					return;
				}
				HolyLib::LuaPack::Policy::RestoreCanonicalHash(nativeHashes, fileID);
			}
			if (pendingHash != pendingHashes.end())
				pendingHashes.erase(pendingHash);
		}
		if (!AppendCanonicalLuaStub(clientIdx, fileID, *delivery.compressed))
		{
			DisconnectLuaHashFailure(clientIdx, fileName.c_str(),
				"the canonical placeholder could not be appended to the reserved reliable buffer");
		}
		return;
	}
	if (delivery.action == HolyLib::LuaPack::DeliveryAction::Reject)
	{
		HolyLib::LuaPack::DisconnectRequiredClient(clientIdx, delivery.failure);
		return;
	}

	if (!luaFile)
	{
		DevWarning(PROJECT_NAME " - gmoddatapack: Client requested file but doesn't exist! \"%s\"\n", fileName.c_str());
		if (HolyLib::LuaPack::IsEnabled())
		{
			if (HolyLib::LuaPack::IsInitFile(fileName))
				DisconnectLuaHashFailure(clientIdx, fileName.c_str(), "the bootstrap source is unavailable");
			else
				SendNativeLuaFile(pDataPack, clientIdx, fileID, fileName, nullptr);
		}
		return;
	}

	if (HolyLib::LuaPack::IsEnabled() && !HolyLib::LuaPack::IsInitFile(fileName))
	{
		// The init file must take HolyLib's normal full-file path because it carries the bootstrap.
		// Every other uncertain/non-ready request goes through the engine implementation itself;
		// this is the fail-open invariant and deliberately bypasses all luapack decisions.
		SendNativeLuaFile(pDataPack, clientIdx, fileID, fileName, luaFile);
		return;
	}

	LuaDataPack::LuaPackEntry* pEntry;
	{
		pEntry = g_pLuaDataPack.GetPackEntry(fileID);
		if (!pEntry)
		{
			Warning(PROJECT_NAME " - gmoddatapack: Client requested a file which we couldn't get an entry for! (%i)\n", fileID);
			if (HolyLib::LuaPack::IsEnabled())
			{
				if (HolyLib::LuaPack::IsInitFile(fileName))
					DisconnectLuaHashFailure(clientIdx, fileName.c_str(), "the bootstrap entry is unavailable");
				else
					SendNativeLuaFile(pDataPack, clientIdx, fileID, fileName, luaFile);
			}
			return;
		}

		std::shared_lock<std::shared_mutex> lock(pEntry->mutex);
		if (pEntry->IsReady())
		{
			SendLuaFile(clientIdx, fileID, pEntry);
			return;
		}
	}

	std::lock_guard<std::shared_mutex> lock(pEntry->mutex);
	if (pEntry->IsReady()) // In case the worker thread just finished this entry
	{
		SendLuaFile(clientIdx, fileID, pEntry);
		return;
	}

	DevMsg(PROJECT_NAME " - gmoddatapack: File \"%s\" isn't yet ready to be sent! Compressing on main thread...\n", fileName.c_str());
	if (!pEntry->IsContentReady())
		g_pLuaDataPack.ProcessContent(pEntry, fileID);

	if (pEntry->IsContentReady() && g_pLuaDataPack.CompressFile(pEntry, fileID))
	{
		SendLuaFile(clientIdx, fileID, pEntry);
		return;
	}

	// Last-resort fail-open. This is reached only when HolyLib's async cache cannot produce a
	// vanilla payload; never consume a request merely because luapack is active.
	if (HolyLib::LuaPack::IsEnabled())
		SendNativeLuaFile(pDataPack, clientIdx, fileID, fileName, luaFile);
}

#if defined(SYSTEM_LINUX)
static void hook_GModDataPack_OnFilesRequested(GModDataPack* pDataPack,
	int clientIdx, bf_read* message, int bits)
{
	auto original = detour_GModDataPack_OnFilesRequested.GetTrampoline<
		Symbols::GModDataPack_OnFilesRequested>();
	if (!pDataPack || !pDataPack->m_pClientLuaFiles || !message ||
		clientIdx < 0 || clientIdx >= ABSOLUTE_PLAYER_LIMIT)
	{
		original(pDataPack, clientIdx, message, bits);
		return;
	}

	const Bootil::AutoBuffer* payload = g_clientPinnedCanonicalPayloads[clientIdx];
	const std::size_t compressedBytes = payload ? payload->GetWritten() : 0u;
	const int registeredFiles = pDataPack->m_pClientLuaFiles->GetNumStrings();
	if (!HolyLib::LuaPack::Policy::CanDecodePinnedRequiredRequestBatch(
		HolyLib::LuaPack::IsEnabled(), HolyLib::LuaPack::SupportsCanonicalRegistration(),
		payload != nullptr, compressedBytes, bits, message->GetNumBitsLeft(),
		registeredFiles, MAX_TRACKED_LUA_FILES))
	{
		original(pDataPack, clientIdx, message, bits);
		return;
	}

	const double startedAt = Plat_FloatTime();
	// Preserve GModDataPack's private per-client request allowance without paying for
	// its red-black-tree construction over the complete cold ID batch. A one-ID probe
	// reaches our SendFile detour only when the engine accepted this request attempt.
	unsigned char probeBytes[2] = {1u, 0u};
	bf_read probe(probeBytes, sizeof(probeBytes));
	g_requiredRequestProbeSlot = clientIdx;
	g_requiredRequestProbeAccepted = false;
	original(pDataPack, clientIdx, &probe, 16);
	const bool requestAccepted = g_requiredRequestProbeAccepted;
	g_requiredRequestProbeAccepted = false;
	g_requiredRequestProbeSlot = -1;
	if (!requestAccepted)
		return;

	PinnedCanonicalFiles requested;
	std::size_t uniqueRequests = 0;
	if (!HolyLib::LuaPack::Policy::DecodeRequiredRequestIds(
		*message, bits, registeredFiles, requested, uniqueRequests))
	{
		DisconnectLuaHashFailure(clientIdx, "the required Lua request batch",
			"the bounded request decoder could not consume the advertised bits");
		return;
	}

	ClientRequiredStubQueue& queue = g_clientRequiredStubQueues[clientIdx];
	++queue.requestBatches;
	queue.requestedFiles += static_cast<unsigned int>(uniqueRequests);
	for (int fileID = 1; fileID < registeredFiles; ++fileID)
	{
		if (!requested.Contains(fileID))
			continue;

		if (g_clientPinnedCanonicalFiles[clientIdx].Contains(fileID))
		{
			if (!EnqueuePinnedRequiredStub(clientIdx, fileID))
			{
				DisconnectLuaHashFailure(clientIdx,
					pDataPack->m_pClientLuaFiles->GetString(fileID),
					"the pinned canonical placeholder could not enter its bounded queue");
				break;
			}
			continue;
		}

		// Init and exact map deltas remain on the existing identity-aware path.
		hook_GModDataPack_SendFileToClient(pDataPack, clientIdx, fileID);
	}

	const double elapsedMilliseconds = (Plat_FloatTime() - startedAt) * 1000.0;
	queue.requestBatchMilliseconds += elapsedMilliseconds;
	queue.peakRequestBatchMilliseconds = (std::max)(
		queue.peakRequestBatchMilliseconds, elapsedMilliseconds);
}
#endif


LUA_FUNCTION_STATIC(gmoddatapack_StripCode)
{
	size_t nLength = -1;
	const char* pContent = Util::CheckLString(LUA, 1, &nLength);
	bool bRemoveServerCode = Util::CheckBoolOpt(LUA, 2, gmoddatapack_removeserverif.GetBool());
	bool bRemoveComments = Util::CheckBoolOpt(LUA, 3, gmoddatapack_removecomments.GetBool());


	lua_State* L = luaL_newstate();
	if (luaL_loadbuffer(L, pContent, nLength, "gmoddatapack.StripCode") != LUA_OK)
	{
		LUA->PushNil();
		LUA->PushString(lua_tolstring(L, -1, nullptr));
		lua_close(L);
		return 2;
	} else {
		std::string strContent = pContent;
		std::vector<Token> tokens = TokenizeContent(strContent);
		if (LUA->IsType(4, GarrysMod::Lua::Type::Function))
		{
			LUA->Push(4);
			CallLuaTokenizeContent(LUA, tokens, -1, false);
		}

		std::string finalCode = ProcessTokens(tokens, bRemoveServerCode, bRemoveComments);
		LUA->PushString(finalCode.c_str(), finalCode.length());
	}
	lua_close(L);

	return 1;
}

LUA_FUNCTION_STATIC(gmoddatapack_GetStoredCode)
{
	const char* pFileName = LUA->CheckString(1);
	if (!g_pDataPack)
	{
		LUA->PushNil();
		return 1;
	}

	GarrysMod::Lua::LuaFile* luaFile = Lua::GetShared()->GetCache(pFileName);
	if (!luaFile)
	{
		LUA->PushNil();
		return 1;
	}

	int fileID = g_pDataPack->m_pClientLuaFiles->FindStringIndex(luaFile->GetName());
	if (fileID == INVALID_STRING_INDEX)
	{
		LUA->PushNil();
		return 1;
	}

	std::string content = "";
	LuaDataPack::LuaPackEntry* pEntry = g_pLuaDataPack.GetPackEntry(fileID);
	if (pEntry)
	{
		std::lock_guard<std::shared_mutex> lock(pEntry->mutex);
		content = pEntry->content;
	}

	LUA->PushString(content.c_str(), content.length());
	return 1;
}

LUA_FUNCTION_STATIC(gmoddatapack_GetCompressedSize)
{
	const char* pFileName = LUA->CheckString(1);
	if (!g_pDataPack)
	{
		LUA->PushNil();
		return 1;
	}

	GarrysMod::Lua::LuaFile* luaFile = Lua::GetShared()->GetCache(pFileName);
	if (!luaFile)
	{
		LUA->PushNil();
		return 1;
	}

	int fileID = g_pDataPack->m_pClientLuaFiles->FindStringIndex(luaFile->GetName());
	if (fileID == INVALID_STRING_INDEX)
	{
		LUA->PushNil();
		return 1;
	}

	size_t size = 0;
	LuaDataPack::LuaPackEntry* pEntry = g_pLuaDataPack.GetPackEntry(fileID);
	if (pEntry)
	{
		std::lock_guard<std::shared_mutex> lock(pEntry->mutex);
		size = pEntry->compressed.GetWritten();
	}

	LUA->PushNumber(size);
	return 1;
}

LUA_FUNCTION_STATIC(gmoddatapack_RefreshExistingLuaFile)
{
	const char* requestedPath = LUA->CheckString(1);
	const LuaPackDiskRefreshResult result = CaptureExistingLuaPackDiskRefresh(
		requestedPath ? requestedPath : "", requestedPath ? requestedPath : "",
		"explicit", true);

	const char* status = "not_eligible";
	switch (result)
	{
		case LuaPackDiskRefreshResult::InvalidPath:
			status = "invalid_path";
			break;
		case LuaPackDiskRefreshResult::UnknownRegistration:
			status = "unknown_registration";
			break;
		case LuaPackDiskRefreshResult::Unreadable:
			status = "unreadable";
			break;
		case LuaPackDiskRefreshResult::Unchanged:
			status = "unchanged";
			break;
		case LuaPackDiskRefreshResult::RescanQueued:
			status = "rescan_queued";
			break;
		case LuaPackDiskRefreshResult::Captured:
			status = "captured";
			break;
		default:
			break;
	}

	LUA->PushBool(result == LuaPackDiskRefreshResult::Captured ||
		result == LuaPackDiskRefreshResult::RescanQueued);
	LUA->PushString(status);
	return 2;
}

LUA_FUNCTION_STATIC(gmoddatapack_MarkAsTokenizeThread)
{
	Lua::ScopedThreadAccess pThreadScope;
	if (g_pLuaDataPack.pInterface.GetLua())
		Lua::RemoveLuaInterfaceReference(&g_pLuaDataPack.pInterface);

	Lua::AddLuaInterfaceReference(LUA, &g_pLuaDataPack.pInterface);
	return 0;
}

static bool SendFileThroughUnreliable(int clientIdx, LuaDataPack::LuaPackEntry* pEntry, int fileID)
{
	if (!pEntry->IsReady())
	{
		DevMsg(PROJECT_NAME " - gmoddatapack: File \"%i\" isn't yet ready to be sent! Compressing on main thread...\n", fileID);
		if (!pEntry->IsContentReady())
			g_pLuaDataPack.ProcessContent(pEntry, fileID);

		if (!pEntry->IsContentReady() || !g_pLuaDataPack.CompressFile(pEntry, fileID))
			return false;
	}

	// Idea: What if... we simply nuke the unreliable stream to send it?
	// The client wouldn't really complain if we send both reliable and unreliable... right?
	// Also, I am like 99% sure we can just send RequestLuaFiles once we assumed we sent everything and the client will tell us what is missing

	CBaseClient* pClient = Util::GetClientByIndex(clientIdx);
	if (pClient && pClient->GetNetChannel())
	{
		static constexpr int BUFFER_SIZE = (1 << 16) * 8;
		CNetChan* pChannel = (CNetChan*)pClient->GetNetChannel();

		int totalLength = 1 + 2 + pEntry->compressed.GetWritten();
		if (pChannel->m_StreamUnreliable.GetNumBytesLeft() < totalLength)
			pChannel->Transmit(false); // We got no space? Let's make some

		pChannel->m_StreamUnreliable.WriteUBitLong(svc_GMod_ServerToClient, NETMSG_TYPE_BITS);
		pChannel->m_StreamUnreliable.WriteUBitLong(totalLength * 8, 20);
		pChannel->m_StreamUnreliable.WriteByte(GarrysMod::NetworkMessage::LuaFileDownload);
		pChannel->m_StreamUnreliable.WriteUBitLong(fileID, 16);
		pChannel->m_StreamUnreliable.WriteBytes(pEntry->compressed.GetBase(), pEntry->compressed.GetWritten());

		if (g_pCVar)
		{
			// This is like the most common convar to limit us when sending out a lot of data.
			// So let's raise it and take the speed :hehe:
			ConVar* pVar = g_pCVar->FindVar("net_splitrate");
			if (pVar && V_stricmp(pVar->GetString(), pVar->GetDefault()) == 0)
				pVar->SetValue(100); // 100 was too much o.o let's lower it, I'm not trying to send 20MB all at once :skull: - nevermind, works now
		}

		if (g_pGModDataPackModule.InDebug())
			Msg(PROJECT_NAME " - gmoddatapack: Sent FileID %i though unreliable stream!\n", fileID);

		pChannel->Transmit(false);
		pChannel->m_fClearTime = pChannel->GetTime() + 10; // Screw you! We don't want to proceed into the SignOnState before were done!
	}

	return true;
}

void CGModDataPackModule::OnClientDisconnect(CBaseClient* pClient)
{
	int slot = pClient->GetPlayerSlot();
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
		return;
	const std::uint64_t steamID64 = pClient->m_SteamID.IsValid()
		? pClient->m_SteamID.ConvertToUint64() : 0;

	g_pLuaDataPack.m_pPlayerQueue[slot].Clear();
	ClearClientLuaDeliveryState(slot);
	HolyLib::LuaPack::PhysicalClientDisconnect(slot, steamID64);
}

static double g_nLastSend = 0;
void CGModDataPackModule::Think(bool bSimulating)
{
	HolyLib::LuaPack::Think();
	// Do not consume the one-shot refresh until both the engine datapack and shared Lua
	// cache can supply every registered path. Either detour can bind g_pDataPack later.
	if (g_pDataPack && g_pDataPack->m_pClientLuaFiles && Lua::GetShared() &&
		HolyLib::LuaPack::ConsumeBootstrapRefresh())
	{
		// A feature or hook-capability transition changes every registered byte identity.
		// Keep that work out of one non-preemptible frame: the 5k-file DarkRP corpus can
		// otherwise exceed the server's freeze-watchdog threshold during cold boot.
		g_luaPackRegistrationRefresh.Begin(
			g_pDataPack->m_pClientLuaFiles->GetNumStrings(),
			HolyLib::LuaPack::IsEnabled());
	}

	if (g_luaPackRegistrationRefresh.active && g_pDataPack &&
		g_pDataPack->m_pClientLuaFiles && Lua::GetShared())
	{
		LuaPackRegistrationRefresh& refresh = g_luaPackRegistrationRefresh;
		++refresh.frames;
		const std::size_t budget = static_cast<std::size_t>((std::max)(1,
			gmoddatapack_luapack_registration_refresh_budget.GetInt()));
		auto refreshFile = [&](int fileID) -> bool
		{
			const char* fileName = g_pDataPack->m_pClientLuaFiles->GetString(fileID);
			if (!fileName || fileName[0] == '\0')
				return true;
			const bool initFile = HolyLib::LuaPack::IsInitFile(fileName);
			if (initFile)
				refresh.registeredInitName = fileName;

			GarrysMod::Lua::LuaFile* file = Lua::GetShared()->GetCache(fileName);
			std::string source;
			bool hasSource = false;
			if (file)
			{
				source = file->contents;
				hasSource = true;
			}
			else if (LuaDataPack::LuaPackEntry* entry = g_pLuaDataPack.GetPackEntry(fileID))
			{
				std::shared_lock<std::shared_mutex> lock(entry->mutex);
				if (entry->hasSourceContent)
				{
					source = entry->sourceContent;
					hasSource = true;
				}
			}

			if (!hasSource)
				return false;
			if (refresh.captureForMapBase)
				HolyLib::LuaPack::CaptureFileContents(fileName, source);
			g_pLuaDataPack.AddFileContents(fileName, source);
			if (!g_pLuaDataPack.PublishRegistrationHash(fileID))
				return false;
			if (refresh.captureForMapBase)
			{
				LuaDataPack::LuaPackEntry* entry = g_pLuaDataPack.GetPackEntry(fileID);
				if (entry)
				{
					std::shared_lock<std::shared_mutex> lock(entry->mutex);
					if (entry->hasSourceContent && !entry->sourceHashReady)
						refresh.pendingSourceHashFileIDs.push_back(fileID);
				}
			}
			++refresh.refreshed;
			if (initFile)
				refresh.initRefreshed = true;
			return true;
		};

		if (!refresh.initialPassComplete)
		{
			const std::size_t end = HolyLib::LuaPack::Policy::BoundedRegistrationRefreshEnd(
				static_cast<std::size_t>(refresh.nextFileID),
				static_cast<std::size_t>(refresh.targetFileCount), budget);
			while (static_cast<std::size_t>(refresh.nextFileID) < end)
			{
				const int fileID = refresh.nextFileID++;
				if (!refreshFile(fileID))
					refresh.unresolvedFileIDs.push_back(fileID);
			}
			refresh.initialPassComplete = refresh.nextFileID >= refresh.targetFileCount;
		}
		else if (!refresh.unresolvedFileIDs.empty())
		{
			const std::size_t end = HolyLib::LuaPack::Policy::BoundedRegistrationRefreshEnd(
				refresh.nextUnresolved, refresh.unresolvedFileIDs.size(), budget);
			while (refresh.nextUnresolved < end)
			{
				const int fileID = refresh.unresolvedFileIDs[refresh.nextUnresolved++];
				if (!refreshFile(fileID))
					refresh.retryFileIDs.push_back(fileID);
			}
			if (refresh.nextUnresolved >= refresh.unresolvedFileIDs.size())
			{
				refresh.unresolvedFileIDs = std::move(refresh.retryFileIDs);
				refresh.retryFileIDs.clear();
				refresh.nextUnresolved = 0;
			}
		}

		if (refresh.initialPassComplete && !refresh.initRefreshed)
		{
			// The registered init entry may carry an addon path while the shared cache exposes
			// only the canonical alias. Preserve the string-table path when using that fallback.
			GarrysMod::Lua::LuaFile* initFile = Lua::GetShared()->GetCache("includes/init.lua");
			if (!initFile)
				initFile = Lua::GetShared()->GetCache("lua/includes/init.lua");
			if (initFile)
			{
				const std::string initPath = refresh.registeredInitName.empty()
					? initFile->GetName() : refresh.registeredInitName;
				if (refresh.captureForMapBase)
					HolyLib::LuaPack::CaptureFileContents(initPath, initFile->contents);
				g_pLuaDataPack.AddFileContents(initPath, initFile->contents);
				const int initID = g_pDataPack->m_pClientLuaFiles->FindStringIndex(initPath.c_str());
				refresh.initRefreshed = initID != INVALID_STRING_INDEX &&
					g_pLuaDataPack.PublishRegistrationHash(initID);
			}
		}

		if (refresh.initialPassComplete &&
			(!refresh.initRefreshed || !refresh.unresolvedFileIDs.empty()) &&
			!refresh.waitingWarningEmitted)
		{
			Warning(PROJECT_NAME " - luapack: registration refresh is waiting for the init file and/or %u unavailable source file(s); pending baselines remain queued\n",
				static_cast<unsigned int>(refresh.unresolvedFileIDs.size()));
			refresh.waitingWarningEmitted = true;
		}

		if (refresh.initialPassComplete && refresh.initRefreshed &&
			refresh.unresolvedFileIDs.empty() &&
			!refresh.pendingSourceHashFileIDs.empty())
		{
			const std::size_t end = HolyLib::LuaPack::Policy::BoundedRegistrationRefreshEnd(
				refresh.nextSourceHash, refresh.pendingSourceHashFileIDs.size(), budget);
			while (refresh.nextSourceHash < end)
			{
				const int fileID = refresh.pendingSourceHashFileIDs[refresh.nextSourceHash++];
				LuaDataPack::LuaPackEntry* entry = g_pLuaDataPack.GetPackEntry(fileID);
				bool ready = false;
				if (entry)
				{
					std::shared_lock<std::shared_mutex> lock(entry->mutex);
					ready = entry->hasSourceContent && entry->sourceHashReady;
				}
				if (!ready)
					refresh.retrySourceHashFileIDs.push_back(fileID);
			}
			if (refresh.nextSourceHash >= refresh.pendingSourceHashFileIDs.size())
			{
				refresh.pendingSourceHashFileIDs = std::move(refresh.retrySourceHashFileIDs);
				refresh.retrySourceHashFileIDs.clear();
				refresh.nextSourceHash = 0;
			}
		}

		if (HolyLib::LuaPack::Policy::CanCompleteRegistrationRefresh(
			refresh.initialPassComplete, refresh.initRefreshed,
			refresh.unresolvedFileIDs.size(),
			refresh.pendingSourceHashFileIDs.size()))
		{
			Msg(PROJECT_NAME " - luapack: registration refresh published %u/%u file(s) over %u frame(s) in %.3f ms wall time\n",
				refresh.refreshed, static_cast<unsigned int>(
					HolyLib::LuaPack::Policy::ClientLuaRegistrationCount(
						static_cast<std::size_t>(refresh.targetFileCount))),
				refresh.frames, (Plat_FloatTime() - refresh.startedAt) * 1000.0);
			refresh.Reset();
			HolyLib::LuaPack::CompleteRegistrationRefresh();
		}
	}

	std::vector<int> stringTableUpdates;
	{
		// Never hold the queue mutex while taking an entry mutex. The worker publishes in
		// the opposite direction (entry first, then queue), so nesting both here can
		// deadlock the main thread during a concurrent hot registration.
		std::lock_guard<std::mutex> lock(g_pLuaDataPack.m_pStringTableUpdateQueueMutex);
		stringTableUpdates = std::move(g_pLuaDataPack.m_pStringTableUpdateQueue);
		g_pLuaDataPack.m_pStringTableUpdateQueue.clear();
	}
	{
		// We do this since SetStringUserData isn't thread safe and may crash in very rare race conditions
		for (int fileID : stringTableUpdates)
		{
			LuaDataPack::LuaPackEntry* pEntry = g_pLuaDataPack.GetPackEntry(fileID);
			if (!pEntry)
				continue;
			std::lock_guard<std::shared_mutex> entryLock(pEntry->mutex);

			if (!pEntry->contentHashReady)
			{
				pEntry->contentHash = HashClientLuaString(pEntry->content);
				pEntry->contentHashReady = true;
			}
			g_pLuaDataPack.PublishEntryHash(fileID, *pEntry);
		}
	}
	DrainActiveLuaHashRefreshes();
	ReportActiveLuaHashRefreshAcknowledgements();

	// ServerInfo serializes the complete Lua string-table baseline atomically. Admit
	// only the configured number of already-accepted physical/parked clients here,
	// after registration publication and before any requested bodies are drained.
#if defined(SYSTEM_LINUX)
	DrainQueuedLuaPackServerInfos();
#endif

	// Required placeholders use the engine's reliable stream, but never all from
	// inside one inbound CNetChan::ProcessMessages call. This global fair drain is
	// deliberately independent of the experimental native-file fastnetwork queue.
	DrainRequiredStubQueues();

	double currentTime = Util::engineserver->Time();
	if (currentTime < (g_nLastSend + 0.05))
		return;

	g_nLastSend = currentTime;
	for (int i=0; i<ABSOLUTE_PLAYER_LIMIT; ++i)
	{
		LuaDataPack::PlayerQueue& pPlayerInfo = g_pLuaDataPack.m_pPlayerQueue[i];
		if (pPlayerInfo.reconnectTime != -1 && currentTime > pPlayerInfo.reconnectTime)
		{
			CBaseClient* pClient = Util::GetClientByIndex(i);
			if (pClient && pClient->GetNetChannel())
				pClient->Reconnect();

			pPlayerInfo.Reconnect();
		}

		if (pPlayerInfo.pQueue.empty())
			continue;

		pPlayerInfo.RecalculateRate();
		// As the name SendFileThroughUnreliable implies, it's not reliable, when only a few files are left, it's more efficient to send them though the slower reliable stream.
		if (pPlayerInfo.useReliable && !pPlayerInfo.usedUnreliable)
		{
			for (int fileID : pPlayerInfo.pQueue)
			{
				LuaDataPack::LuaPackEntry* pEntry = g_pLuaDataPack.GetPackEntry(fileID);
				std::lock_guard<std::shared_mutex> lock(pEntry->mutex);

				++pPlayerInfo.sentFiles;
				++pPlayerInfo.totalSentFiles;
				SendLuaFile(i, fileID, pEntry, true);
			}
			pPlayerInfo.pQueue.clear();
			pPlayerInfo.usedReliable = true;
		} else {
			pPlayerInfo.usedUnreliable = true;
			size_t networkedSize = 0;
			while (networkedSize < pPlayerInfo.targetRate)
			{
				int fileID = pPlayerInfo.pQueue.back();
				pPlayerInfo.pQueue.pop_back();

				LuaDataPack::LuaPackEntry* pEntry = g_pLuaDataPack.GetPackEntry(fileID);
				std::lock_guard<std::shared_mutex> lock(pEntry->mutex);

				// We "could" try to do some work and see if any file would fit next but meeh, not worth it.
				if (SendFileThroughUnreliable(i, pEntry, fileID))
				{
					networkedSize += pEntry->compressed.GetWritten();
					++pPlayerInfo.sentFiles;
					++pPlayerInfo.totalSentFiles;
				}

				if (pPlayerInfo.pQueue.empty())
					break;
			}

			CBaseClient* pClient = Util::GetClientByIndex(i);
			if (pClient && pClient->GetNetChannel())
			{
				CNetChan* pChannel = (CNetChan*)pClient->GetNetChannel();
				pChannel->ProcessStream();

				if (pPlayerInfo.pQueue.empty() && !pPlayerInfo.usedReliable)
				{
					//char pBuffer[1 << 13];
					//bf_write msg(pBuffer, sizeof(pBuffer));
					//msg.WriteByte(GarrysMod::NetworkMessage::RequestLuaFiles);
					//Util::engineserver->GMOD_SendToClient( i, msg.GetData(), msg.GetNumBitsWritten() );

					pPlayerInfo.reconnectTime = currentTime + 1; // Give some time for processing
					DevMsg(PROJECT_NAME " - gmoddatapack: Marked for reconnect! (%s)\n", pClient->GetClientName());
				}
			}
		}
	}
}

bool GMODDataPack_SetSignOnState(CBaseClient* cl, int state)
{
	int slot = cl->GetPlayerSlot();
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT)
		return false;

	if (state != SIGNONSTATE_PRESPAWN)
		return false;

	return HasPendingRequiredStubs(slot) ||
		!g_pLuaDataPack.m_pPlayerQueue[slot].pQueue.empty();
}

#if SYSTEM_WINDOWS
DETOUR_THISCALL_START()
	DETOUR_THISCALL_ADDFUNC2(hook_GModDataPack_AddOrUpdateFile, AddOrUpdateFile, GModDataPack*, GarrysMod::Lua::LuaFile*, bool);
	DETOUR_THISCALL_ADDFUNC2(hook_GModDataPack_SendFileToClient, SendFileToClient, GModDataPack*, int, int);
DETOUR_THISCALL_FINISH()
#endif

void CGModDataPackModule::Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn)
{
	(void)gamefn;
	HolyLib::LuaPack::Init(appfn);
}

MODULE_RESULT CGModDataPackModule::ClientConnect(bool* bAllowConnect, edict_t* pClient, const char* pszName, const char* pszAddress, char* reject, int maxrejectlen)
{
	(void)pszName;
	(void)pszAddress;

	const int slot = ClientSlotFromEdict(pClient);
	if (!HolyLib::LuaPack::ClientConnect(slot))
		ClearClientLuaDeliveryState(slot);

	const HolyLib::LuaPack::Config& config = HolyLib::LuaPack::GetConfig();
	if (HolyLib::LuaPack::Policy::RejectUnavailableRequiredAdmission(
		HolyLib::LuaPack::IsEnabled(), config.requiredStubbing,
		HolyLib::LuaPack::SupportsCanonicalRegistration()))
	{
		static const char* failure = "Required LuaPack is unavailable because its Linux delivery hooks are not active. The server operator must restore the hooks or disable required mode.";
		if (bAllowConnect)
			*bAllowConnect = false;
		if (reject && maxrejectlen > 0)
			V_strncpy(reject, failure, maxrejectlen);
		Warning(PROJECT_NAME " - luapack: rejecting client slot %i before admission because required delivery hooks are unavailable\n", slot);
		return MODULE_RESULT::STOP;
	}
	return MODULE_RESULT::CONTINUE;
}

void CGModDataPackModule::ClientActive(edict_t* pClient)
{
	const int slot = ClientSlotFromEdict(pClient);
	if (HasPendingRequiredStubs(slot))
	{
		DisconnectLuaHashFailure(slot, "the required Lua baseline",
			"PRESPAWN advanced before its bounded placeholder queue drained");
		ClearClientRequiredStubQueue(slot);
		return;
	}

	HolyLib::LuaPack::ClientActive(slot);
	ReportRequiredStubQueue(slot);
}

void CGModDataPackModule::ClientDisconnect(edict_t* pClient)
{
	const int slot = ClientSlotFromEdict(pClient);
	if (!HolyLib::LuaPack::ClientDisconnect(slot, true))
		ClearClientLuaDeliveryState(slot);
}

MODULE_RESULT CGModDataPackModule::ClientCommand(edict_t* pClient, const CCommand* args)
{
	return HolyLib::LuaPack::ClientCommand(ClientSlotFromEdict(pClient), args);
}

void CGModDataPackModule::InitDetour(bool bPreServer)
{
	if (bPreServer)
		return;

	HolyLib::GModDataPack::InstallLuaAutoRefreshDetour();

	DETOUR_PREPARE_THISCALL();
#if defined(SYSTEM_LINUX)
	SourceSDK::FactoryLoader engine_loader("engine");
	Detour::Create(
		&detour_CBaseClient_SendServerInfo, "CBaseClient::SendServerInfo",
		engine_loader.GetModule(), Symbols::CBaseClient_SendServerInfoSym,
		(void*)hook_CBaseClient_SendServerInfo, m_pID
	);
#endif

	SourceSDK::FactoryLoader server_loader("server");
	Detour::Create(
		&detour_GModDataPack_AddOrUpdateFile, "GModDataPack::AddOrUpdateFile",
		server_loader.GetModule(), Symbols::GModDataPack_AddOrUpdateFileSym,
		(void*)DETOUR_THISCALL(hook_GModDataPack_AddOrUpdateFile, AddOrUpdateFile), m_pID
	);

	Detour::Create(
		&detour_GModDataPack_SendFileToClient, "GModDataPack::SendFileToClient",
		server_loader.GetModule(), Symbols::GModDataPack_SendFileToClientSym,
		(void*)DETOUR_THISCALL(hook_GModDataPack_SendFileToClient, SendFileToClient), m_pID
	);

#if defined(SYSTEM_LINUX)
	Detour::Create(
		&detour_GModDataPack_OnFilesRequested, "GModDataPack::OnFilesRequested",
		server_loader.GetModule(), Symbols::GModDataPack_OnFilesRequestedSym,
		(void*)hook_GModDataPack_OnFilesRequested, m_pID
	);
#endif
}

void CGModDataPackModule::LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit)
{
	HolyLib::LuaPack::LuaInit(pLua, bServerInit);

	if (bServerInit)
		return;

	if (pLua == g_Lua)
		g_pLuaDataPack.Initialize();

	Util::StartTable(pLua);
		Util::AddFunc(pLua, gmoddatapack_StripCode, "StripCode");
		Util::AddFunc(pLua, gmoddatapack_GetStoredCode, "GetStoredCode");
		Util::AddFunc(pLua, gmoddatapack_GetCompressedSize, "GetCompressedSize");
		Util::AddFunc(pLua, gmoddatapack_RefreshExistingLuaFile, "RefreshExistingLuaFile");
		Util::AddFunc(pLua, gmoddatapack_MarkAsTokenizeThread, "MarkAsTokenizeThread");

		for (size_t i=0; i<(size_t)TK_EOF; ++i)
			Util::AddValue(pLua, (double)i, g_TokenNames[i]);

	Util::FinishTable(pLua, "gmoddatapack");
}

void CGModDataPackModule::LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua)
{
	Util::NukeTable(pLua, "gmoddatapack");
}

void CGModDataPackModule::LevelShutdown()
{
	for (int slot = 0; slot < ABSOLUTE_PLAYER_LIMIT; ++slot)
		ClearClientLuaDeliveryState(slot);
	g_requiredStubScheduler.Reset();
	g_luaPackServerInfoScheduler.Reset();
	g_luaPackRegistrationRefresh.Reset();
	g_activeHashRefreshNativeAcknowledgements = 0;
	g_activeHashRefreshCanonicalAcknowledgements = 0;
	HolyLib::LuaPack::LevelShutdown();
	g_pLuaDataPack.Shutdown();
}

void CGModDataPackModule::Shutdown()
{
	for (int slot = 0; slot < ABSOLUTE_PLAYER_LIMIT; ++slot)
		ClearClientLuaDeliveryState(slot);
	g_requiredStubScheduler.Reset();
	g_luaPackServerInfoScheduler.Reset();
	g_luaPackRegistrationRefresh.Reset();
	g_activeHashRefreshNativeAcknowledgements = 0;
	g_activeHashRefreshCanonicalAcknowledgements = 0;
	HolyLib::LuaPack::Shutdown();
}
