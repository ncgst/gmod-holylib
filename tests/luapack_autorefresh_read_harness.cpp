#include "modules/gmoddatapack_luapack_policy.h"
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#define PROJECT_NAME "holylib"
#define SYSTEM_LINUX 1
#define MODULE_EXISTS_AUTOREFRESH 1
constexpr int INVALID_STRING_INDEX = -1;
constexpr size_t MAX_TRACKED_LUA_FILES = 8192;
using FileHandle_t = int;
constexpr FileHandle_t FILESYSTEM_INVALID_HANDLE = 0;

static double mockTime;
static double Plat_FloatTime() { return mockTime; }
static bool enabled, moduleEnabled, canonical, skipWatcher, baseSnapshot;
static int engineCalls, preCalls, postCalls;
static std::function<void()> engineAction;
static std::vector<std::string> warnings;
static void Warning(const char* format, ...)
{
	char message[2048];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	warnings.emplace_back(message);
}
static void Msg(const char*, ...) {}
static bool IsGModDataPackModuleEnabled() { return moduleEnabled; }

struct MockFilesystem
{
	std::map<std::pair<std::string, std::string>, std::string> files;
	std::vector<std::pair<std::string, std::string>> opens;
	std::string body;
	bool shortRead = false, oversized = false;
	int handles = 0;
	FileHandle_t Open(const char* path, const char*, const char* pathID)
	{
		opens.emplace_back(pathID, path);
		const auto found = files.find({pathID, path});
		if (found == files.end()) return FILESYSTEM_INVALID_HANDLE;
		body = found->second;
		++handles;
		return 1;
	}
	unsigned int Size(FileHandle_t)
	{
		return oversized ? (std::numeric_limits<unsigned int>::max)() :
			static_cast<unsigned int>(body.size());
	}
	int Read(char* output, int size, FileHandle_t)
	{
		const int read = shortRead ? size - 1 : size;
		std::memcpy(output, body.data(), read);
		return read;
	}
	void Close(FileHandle_t) { --handles; }
} filesystem;
static MockFilesystem* g_pFullFileSystem = &filesystem;

struct MockBuffer { int written = 100, position = 12; };
struct NativeBufferAdapter
{
	bool Ready() const { return true; }
	void setWritten(MockBuffer* buffer, int value) { buffer->written = value; }
	void setPosition(MockBuffer* buffer, int value) { buffer->position = value; }
} g_nativeLuaBuffer;

namespace GarrysMod::Lua
{
	struct LuaFile
	{
		std::string contents = "old";
		MockBuffer compressed;
		int changes = 0;
		void SetContents(const std::string& value) { contents = value; ++changes; }
	};
}
struct MockShared
{
	std::map<std::string, GarrysMod::Lua::LuaFile> cache;
	GarrysMod::Lua::LuaFile* GetCache(const std::string& path)
	{
		const auto found = cache.find(path);
		return found == cache.end() ? nullptr : &found->second;
	}
} shared;
static bool sharedAvailable;
namespace Lua { static MockShared* GetShared() { return sharedAvailable ? &shared : nullptr; } }

struct MockTable
{
	std::map<std::string, int> registrations;
	int FindStringIndex(const char* name)
	{
		const auto found = registrations.find(name);
		return found == registrations.end() ? INVALID_STRING_INDEX : found->second;
	}
	int AddString(bool, const char* name)
	{
		const int id = static_cast<int>(registrations.size()) + 1;
		registrations.emplace(name, id);
		return id;
	}
} table;
struct MockDataPack { MockTable* m_pClientLuaFiles = &table; } dataPack;
static MockDataPack* g_pDataPack = &dataPack;
struct Capture { std::string name, body; bool forced; };
static std::vector<Capture> packCaptures, updates;
static std::map<std::string, std::string> capturedSources;
static unsigned int invalidations;
static void InvalidatePinnedCanonicalFileForAllClients(int) { ++invalidations; }
struct MockSetting { bool GetBool() const { return false; } };
static MockSetting gmoddatapack_removeserverif, gmoddatapack_removecomments;
static struct { bool InDebug() const { return false; } } g_pGModDataPackModule;

using ClientLuaHash = std::array<unsigned char, 32>;
static ClientLuaHash HashClientLuaString(const std::string& source)
{
	ClientLuaHash hash{};
	for (size_t i = 0; i < source.size(); ++i)
		hash[i % hash.size()] ^= static_cast<unsigned char>(source[i]);
	return hash;
}

namespace Bootil
{
	struct AutoBuffer
	{
		std::vector<unsigned char> data;
		void Write(const void* value, size_t bytes)
		{
			const auto* first = static_cast<const unsigned char*>(value);
			data.insert(data.end(), first, first + bytes);
		}
		size_t GetWritten() const { return data.size(); }
		const void* GetBase() const { return data.data(); }
		void Clear() { data.clear(); }
	};
	namespace Compression::LZMA
	{
		static bool Compress(const char* data, size_t bytes, AutoBuffer& output, int level)
		{
			assert(data[bytes - 1] == '\0' && level == 5);
			// Model a compressor that returns an incompressible body of the given
			// length. The production guard must measure the output plus the hash.
			output.Write(data, bytes);
			return true;
		}
	}
}

class LuaDataPack
{
public:
	struct LuaPackEntry
	{
		std::shared_mutex mutex;
		std::string registrationName;
		std::uint64_t registrationEpoch = 0;
		bool hasSourceContent = false;
		std::string sourceContent, content;
		Bootil::AutoBuffer compressed;
		std::shared_ptr<const Bootil::AutoBuffer> activeRefreshCompressed;
		ClientLuaHash sourceHash{}, contentHash{};
		bool activeRefreshRequested = false, activeRefreshFailed = false;
		bool processed = false, hashPublished = false;
		bool sourceHashReady = false, contentHashReady = false;
		bool removeServerCode = false, removeComments = false;
		bool luapackCanonical = false, luapackPassthrough = false;
		bool activeHashRefreshPending = false, forceActiveHashRefreshPending = false;
		bool IsContentReady() const { return processed; }
	};
	std::map<int, LuaPackEntry> m_pLuaFileCache;
	std::vector<int> m_pCompressQueue;
	std::mutex m_pCompressQueueMutex;
	LuaPackEntry* GetPackEntry(int id)
	{
		return &m_pLuaFileCache[id];
	}
	void EnsureSourceHash(LuaPackEntry* entry) { entry->sourceHash = HashClientLuaString(entry->sourceContent); }
	void PublishEntryHash(int, LuaPackEntry& entry) { entry.hashPublished = true; }
	bool CommitFileContents(std::string name, std::string body, bool forced,
		GarrysMod::Lua::LuaFile* file, bool capture);
	bool AddFileContents(const std::string& name, const std::string& body, bool forced = false,
		GarrysMod::Lua::LuaFile* file = nullptr, bool capture = true)
	{
		if (!CommitFileContents(name, body, forced, file, capture))
			return false;
		updates.push_back({name, body, forced});
		return true;
	}
} g_pLuaDataPack;
namespace HolyLib::LuaPack
{
	static bool IsEnabled() { return enabled; }
	static bool SupportsCanonicalRegistration() { return canonical; }
	static bool HasMapBaseSnapshot() { return baseSnapshot; }
	static bool IsInitFile(const std::string& name) { return name == "includes/init.lua"; }
	static std::string PrepareVanillaFile(const std::string& name, const std::string& body)
	{
		return !enabled || !canonical ? body : IsInitFile(name) ? "-- bootstrap\n" + body : "-- stub";
	}
	static void CaptureFileContents(const std::string& name, const std::string& body)
	{
		if (capturedSources[name] == body)
			return;
		capturedSources[name] = body;
		packCaptures.push_back({name, body, false});
	}
}
namespace HolyLib::AutoRefresh
{
	static bool RunPreLuaChange(const std::string*, const std::string*, const std::string*)
	{
		++preCalls;
		return skipWatcher;
	}
	static void RunPostLuaChange(const std::string*, const std::string*, const std::string*) { ++postCalls; }
}
namespace Symbols
{
	using GarrysMod_AutoRefresh_HandleChange_Lua =
		bool (*)(const std::string*, const std::string*, const std::string*);
}
static const std::string* g_engineLuaRefreshName;
static bool g_engineLuaRefreshRejected;
static bool EngineWatcher(const std::string*, const std::string*, const std::string*)
{
	++engineCalls;
	if (engineAction) engineAction();
	return false;
}
struct MockDetour
{
	template <typename T> T GetTrampoline() { return &EngineWatcher; }
} detour_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack;

// INSERT_PRODUCTION_METHODS

static const std::string addon = "addons/ndoc/lua/autorun/sh_load_comm_computer.lua";
static void Reset()
{
	mockTime = 10.0;
	enabled = moduleEnabled = canonical = sharedAvailable = true;
	baseSnapshot = false;
	skipWatcher = false;
	engineCalls = preCalls = postCalls = 0;
	engineAction = nullptr;
	g_engineLuaRefreshName = nullptr;
	filesystem = {};
	g_pFullFileSystem = &filesystem;
	g_pDataPack = &dataPack;
	dataPack.m_pClientLuaFiles = &table;
	table.registrations.clear(); shared.cache.clear(); g_pLuaDataPack.m_pLuaFileCache.clear();
	g_pLuaDataPack.m_pCompressQueue.clear();
	updates.clear(); packCaptures.clear(); warnings.clear();
	capturedSources = {{addon, "old"}};
	invalidations = 0;
	g_pendingLuaAutoRefreshReads.clear();
	g_rejectedLateLuaFiles.clear();
	table.registrations[addon] = 1;
	shared.cache[addon];
	auto& old = *g_pLuaDataPack.GetPackEntry(1);
	old.hasSourceContent = old.processed = old.hashPublished = true;
	old.registrationName = addon;
	old.registrationEpoch = g_clientLuaRegistrationEpoch;
	old.luapackCanonical = old.luapackPassthrough = true;
	old.sourceContent = "old";
	old.content = "-- stub";
	old.contentHash = HashClientLuaString(old.content);
	old.contentHashReady = true;
}
static LuaPackDiskRefreshResult Auto(const std::string& path = addon)
{
	return CaptureExistingLuaPackDiskRefresh(path, path, LuaPackDiskRefreshMode::Automatic);
}
static bool Watch()
{
	const std::string directory = "addons/ndoc/lua/autorun/";
	const std::string stem = "sh_load_comm_computer", extension = "lua";
	return hook_GarrysMod_AutoRefresh_HandleChange_Lua_LuaPack(&directory, &stem, &extension);
}
static void Advance(double time) { mockTime = time; DrainLuaAutoRefreshReads(); }

int main()
{
	using Result = LuaPackDiskRefreshResult;
	using Mode = LuaPackDiskRefreshMode;
	std::string output;
	Reset();
	filesystem.files[{"MOD", addon}] = "correct";
	filesystem.files[{"GAME", "lua/autorun/sh_load_comm_computer.lua"}] = "shadow";
	assert(ReadLuaAutoRefreshSource("autorun/sh_load_comm_computer.lua", addon, output));
	assert(output == "correct" && filesystem.opens.size() == 1);
	filesystem.files.erase({"MOD", addon});
	assert(!ReadLuaAutoRefreshSource(addon, addon, output));
	assert(output.empty() && filesystem.opens.back() == std::make_pair(std::string("MOD"), addon));
	assert(!ReadLuaAutoRefreshSource(addon, "autorun/sh_load_comm_computer.lua", output));
	assert(ReadLuaAutoRefreshSource("autorun/sh_load_comm_computer.lua", "autorun/sh_load_comm_computer.lua", output));
	assert(output == "shadow");
	for (const auto& path : {std::string("lua/test.lua"), std::string("gamemodes/test/gamemode/shared.lua")})
	{
		filesystem.files[{"MOD", path}] = path;
		assert(ReadLuaAutoRefreshSource(path, path, output) && output == path);
		assert(filesystem.opens.back() == std::make_pair(std::string("MOD"), path));
		filesystem.files.erase({"MOD", path});
		filesystem.files[{"GAME", "lua/" + path}] = "wrong root";
		assert(!ReadLuaAutoRefreshSource(path, path, output));
		assert(filesystem.opens.back() == std::make_pair(std::string("MOD"), path));
	}
	filesystem.files[{"MOD", addon}] = "normalized";
	assert(ReadLuaAutoRefreshSource("addons\\ndoc\\lua\\autorun\\sh_load_comm_computer.lua", addon, output));
	assert(output == "normalized");
	const auto opens = filesystem.opens.size();
	for (const auto* invalid : {"../bad.lua", "/tmp/bad.lua", "C:\\bad.lua"})
		assert(!ReadLuaAutoRefreshSource(invalid, invalid, output));
	assert(filesystem.opens.size() == opens);
	std::cout << "PASS: exact source paths, shadow rejection, legacy paths, normalization and traversal\n";

	Reset();
	filesystem.files[{"MOD", addon}] = "complete";
	filesystem.shortRead = true;
	assert(Auto() == Result::ReadQueued);
	assert(updates.empty() && shared.cache[addon].contents == "old" && filesystem.handles == 0);
	filesystem.shortRead = false; filesystem.oversized = true;
	assert(!ReadLuaAutoRefreshSource(addon, addon, output));
	assert(output.empty() && filesystem.handles == 0);
	filesystem.oversized = false; filesystem.files[{"MOD", addon}] = "";
	assert(ReadLuaAutoRefreshSource(addon, addon, output) && output.empty());
	assert(filesystem.handles == 0);
	std::cout << "PASS: short/oversized reads preserve caches, close handles; empty Lua remains valid\n";

	Reset();
	assert(Watch() && g_pendingLuaAutoRefreshReads.size() == 1);
	assert(engineCalls == 1 && preCalls == 1 && postCalls == 1 && !g_engineLuaRefreshName);
	Advance(10.24);
	assert(filesystem.opens.size() == 1 && updates.empty());
	filesystem.files[{"MOD", addon}] = "new";
	Advance(10.25);
	assert(g_pendingLuaAutoRefreshReads.empty() && updates.size() == 1 && packCaptures.size() == 1);
	assert(updates[0].name == addon && updates[0].body == "new" && !updates[0].forced);
	assert(shared.cache[addon].contents == "new" && shared.cache[addon].changes == 1);
	assert(shared.cache[addon].compressed.written == 0 && shared.cache[addon].compressed.position == 0);
	Advance(20.0);
	assert(engineCalls == 1 && preCalls == 1 && postCalls == 1 && updates.size() == 1);
	std::cout << "PASS: failed watcher read recovers exact bytes once without replaying engine callbacks\n";

	Reset();
	assert(Auto() == Result::ReadQueued);
	mockTime = 10.1;
	for (int i = 0; i < 100; ++i) assert(Auto() == Result::ReadQueued);
	assert(g_pendingLuaAutoRefreshReads.size() == 1 && warnings.size() == 1);
	assert(g_pendingLuaAutoRefreshReads.front().deadline == 15.0);
	assert(g_pendingLuaAutoRefreshReads.front().nextAttempt == 10.25);
	for (double time : {10.25, 10.75, 11.75, 13.75}) Advance(time);
	assert(g_pendingLuaAutoRefreshReads.empty() && updates.empty() && warnings.size() == 2);
	assert(warnings.back().find("attempts exhausted") != std::string::npos);
	Reset(); Auto(); Advance(15.0);
	assert(g_pendingLuaAutoRefreshReads.empty() && filesystem.opens.size() == 1);
	assert(warnings.back().find("deadline expired") != std::string::npos);
	std::cout << "PASS: repeated notifications coalesce without extending retry attempts or deadline\n";

	Reset(); Auto();
	filesystem.files[{"MOD", addon}] = "latest";
	assert(Auto() == Result::Captured && g_pendingLuaAutoRefreshReads.empty());
	Advance(10.25);
	assert(updates.size() == 1 && updates[0].body == "latest");
	Reset(); Auto();
	filesystem.files[{"MOD", addon}] = "already captured";
	g_pLuaDataPack.GetPackEntry(1)->sourceContent = "already captured";
	shared.cache[addon].contents = "already captured";
	Advance(10.25);
	assert(updates.empty() && packCaptures.empty() && g_pendingLuaAutoRefreshReads.empty());
	Reset();
	assert(CaptureExistingLuaPackDiskRefresh(addon, addon, Mode::Explicit) == Result::Unreadable);
	assert(g_pendingLuaAutoRefreshReads.empty());
	Auto(); filesystem.files[{"MOD", addon}] = "old";
	assert(CaptureExistingLuaPackDiskRefresh(addon, addon, Mode::Explicit) == Result::RefreshQueued);
	assert(updates.size() == 1 && updates[0].forced && g_pendingLuaAutoRefreshReads.empty());
	std::cout << "PASS: newer captures cancel retries; unchanged retries and explicit API retain their contracts\n";

	for (int mode = 0; mode < 7; ++mode)
	{
		Reset(); Auto();
		switch (mode)
		{
			case 0: enabled = false; break;
			case 1: moduleEnabled = false; break;
			case 2: canonical = false; break;
			case 3: sharedAvailable = false; break;
			case 4: g_pFullFileSystem = nullptr; break;
			case 5: g_pDataPack = nullptr; break;
			case 6: dataPack.m_pClientLuaFiles = nullptr; break;
		}
		Advance(10.25);
		assert(g_pendingLuaAutoRefreshReads.empty() && filesystem.opens.size() == 1);
	}
	Reset(); Auto(); table.registrations[addon] = 2; Advance(10.25);
	assert(g_pendingLuaAutoRefreshReads.empty() && filesystem.opens.size() == 1);
	Reset(); Auto(); table.registrations.clear(); Advance(10.25);
	assert(g_pendingLuaAutoRefreshReads.empty() && filesystem.opens.size() == 1);
	Reset(); Auto(); shared.cache.clear(); Advance(10.25);
	assert(g_pendingLuaAutoRefreshReads.empty() && filesystem.opens.size() == 1);
	Reset();
	assert(Auto("unknown.lua") == Result::UnknownRegistration);
	assert(Auto("../bad.lua") == Result::InvalidPath);
	assert(g_pendingLuaAutoRefreshReads.empty() && filesystem.opens.empty());
	skipWatcher = true;
	assert(Watch() && engineCalls == 0 && preCalls == 1 && postCalls == 0);
	std::cout << "PASS: disabled features, lost registration/cache, invalid paths and pre-hook cancellation\n";

	Reset();
	for (std::size_t i = 0; i < luaAutoRefreshReadLimit + 1; ++i)
	{
		const auto path = "addons/test/lua/file" + std::to_string(i) + ".lua";
		table.registrations[path] = static_cast<int>(i + 10);
		shared.cache[path];
		const auto result = Auto(path);
		assert(result == (i < luaAutoRefreshReadLimit ? Result::ReadQueued : Result::Unreadable));
	}
	assert(g_pendingLuaAutoRefreshReads.size() == luaAutoRefreshReadLimit);
	const auto before = filesystem.opens.size();
	Advance(10.25);
	assert(filesystem.opens.size() == before + luaAutoRefreshReadBudget);
	assert(g_pendingLuaAutoRefreshReads.front().fileID == 10 + static_cast<int>(luaAutoRefreshReadBudget));
	Advance(10.25);
	assert(filesystem.opens.size() == before + 2 * luaAutoRefreshReadBudget);
	assert(g_pendingLuaAutoRefreshReads.front().fileID == 10 + 2 * static_cast<int>(luaAutoRefreshReadBudget));
	assert(updates.empty());
	std::cout << "PASS: bounded queue admission and fair per-frame disk-read budget\n";

	Reset(); Auto();
	filesystem.files[{"MOD", addon}] = "engine refresh";
	engineAction = [] {
		assert(g_engineLuaRefreshName && *g_engineLuaRefreshName == addon);
		g_pLuaDataPack.AddFileContents(addon, "engine refresh", false, &shared.cache[addon], false);
		shared.cache[addon].contents = "engine refresh";
	};
	assert(Watch()); Advance(10.25);
	assert(engineCalls == 1 && updates.size() == 1 && packCaptures.empty());
	assert(g_pendingLuaAutoRefreshReads.empty() && !g_engineLuaRefreshName);
	std::cout << "PASS: original engine capture cancels deferred work without duplicate publication\n";

	Reset();
	using namespace HolyLib::LuaPack::Policy;
	const auto limit = ActiveLuaRefreshMaximumBodyBytes(addon.size());
	const std::string lastGood(limit - ClientLuaHashBytes - 1, 'a');
	const std::string oversized = lastGood + 'b';
	auto& native = shared.cache[addon];
	filesystem.files[{"MOD", addon}] = lastGood;
	assert(Auto() == Result::Captured);
	auto& entry = *g_pLuaDataPack.GetPackEntry(1);
	const auto oldBody = entry.activeRefreshCompressed;
	const auto oldHash = entry.sourceHash;
	assert(oldBody && oldBody->GetWritten() == limit);
	const auto committed = invalidations;
	filesystem.files[{"MOD", addon}] = oversized;
	assert(CaptureExistingLuaPackDiskRefresh(addon, addon, Mode::Explicit) == Result::Rejected);
	// The original watcher can change the shared cache before AddOrUpdateFile.
	engineAction = [&] {
		native.contents = oversized;
		assert(!g_pLuaDataPack.AddFileContents(addon, oversized, false, &native));
	};
	Watch();
	assert(native.contents == lastGood && entry.sourceContent == lastGood);
	assert(entry.sourceHash == oldHash && entry.activeRefreshCompressed == oldBody);
	assert(capturedSources[addon] == lastGood && invalidations == committed);
	assert(warnings.back().find(addon) != std::string::npos);
	assert(warnings.back().find(std::to_string(limit + 1)) != std::string::npos);
	assert(warnings.back().find(std::to_string(limit)) != std::string::npos);
	engineAction = nullptr;
	filesystem.files[{"MOD", addon}] = "restored";
	assert(Auto() == Result::Captured);
	assert(native.contents == "restored" && entry.sourceContent == "restored");
	assert(entry.activeRefreshCompressed != oldBody && capturedSources[addon] == "restored");

	const std::string late = "lua/late.lua";
	const std::string tooLarge(NativeLuaMaximumBodyBytes, 'z');
	baseSnapshot = true;
	shared.cache[late].contents = tooLarge;
	assert(!g_pLuaDataPack.AddFileContents(late, tooLarge, false, &shared.cache[late]));
	assert(table.FindStringIndex(late.c_str()) == INVALID_STRING_INDEX && !capturedSources.count(late));
	filesystem.files[{"MOD", late}] = "fixed";
	assert(Auto(late) == Result::Captured);
	assert(shared.cache[late].contents == "fixed");
	assert(table.FindStringIndex(late.c_str()) != INVALID_STRING_INDEX);
	baseSnapshot = false;
	assert(g_pLuaDataPack.AddFileContents("lua/base.lua", tooLarge));
	assert(table.FindStringIndex("lua/base.lua") != INVALID_STRING_INDEX);
	assert(!g_pLuaDataPack.AddFileContents("includes/init.lua", tooLarge));
	assert(g_pLuaDataPack.AddFileContents("includes/init.lua", ""));
	enabled = false;
	assert(g_pLuaDataPack.AddFileContents(addon, tooLarge));
	std::cout << "PASS: oversized changes preserve registration, source hash, and body; restoration and base capture work\n";

	Reset();
	table.registrations.clear();
	table.registrations["lua/reused.lua"] = 1;
	assert(g_pLuaDataPack.AddFileContents("lua/reused.lua", tooLarge));
	Reset();
	++g_clientLuaRegistrationEpoch;
	assert(g_pLuaDataPack.AddFileContents(addon, tooLarge));
	std::cout << "PASS: retained cache entries don't reject new base files after registration IDs are reused\n";
}
