#include "modules/gmoddatapack_luapack_policy.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <vector>

#define PROJECT_NAME "holylib"
#define SYSTEM_LINUX 1
#define MODULE_EXISTS_AUTOREFRESH 1
constexpr int INVALID_STRING_INDEX = -1;
using FileHandle_t = int;
constexpr FileHandle_t FILESYSTEM_INVALID_HANDLE = 0;

static double mockTime;
static double Plat_FloatTime() { return mockTime; }
static bool enabled, moduleEnabled, canonical, skipWatcher;
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
} table;
struct MockDataPack { MockTable* m_pClientLuaFiles = &table; } dataPack;
static MockDataPack* g_pDataPack = &dataPack;
struct Capture { std::string name, body; bool forced; };
static std::vector<Capture> packCaptures, updates;

class LuaDataPack
{
public:
	struct LuaPackEntry
	{
		std::shared_mutex mutex;
		bool hasSourceContent = true;
		std::string sourceContent = "old";
	};
	std::map<int, LuaPackEntry> entries;
	LuaPackEntry* GetPackEntry(int id)
	{
		const auto found = entries.find(id);
		return found == entries.end() ? nullptr : &found->second;
	}
	void AddFileContents(const std::string& name, const std::string& body, bool forced = false)
	{
		updates.push_back({name, body, forced});
		entries[table.FindStringIndex(name.c_str())].sourceContent = body;
	}
} g_pLuaDataPack;
namespace HolyLib::LuaPack
{
	static bool IsEnabled() { return enabled; }
	static bool SupportsCanonicalRegistration() { return canonical; }
	static void CaptureFileContents(const std::string& name, const std::string& body)
	{
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
	skipWatcher = false;
	engineCalls = preCalls = postCalls = 0;
	engineAction = nullptr;
	g_engineLuaRefreshName = nullptr;
	filesystem = {};
	g_pFullFileSystem = &filesystem;
	g_pDataPack = &dataPack;
	dataPack.m_pClientLuaFiles = &table;
	table.registrations.clear(); shared.cache.clear(); g_pLuaDataPack.entries.clear();
	updates.clear(); packCaptures.clear(); warnings.clear();
	g_pendingLuaAutoRefreshReads.clear();
	table.registrations[addon] = 1;
	shared.cache[addon]; g_pLuaDataPack.entries[1];
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
	g_pLuaDataPack.entries[1].sourceContent = "already captured";
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
		g_pLuaDataPack.AddFileContents(addon, "engine refresh");
		shared.cache[addon].contents = "engine refresh";
	};
	assert(Watch()); Advance(10.25);
	assert(engineCalls == 1 && updates.size() == 1 && packCaptures.empty());
	assert(g_pendingLuaAutoRefreshReads.empty() && !g_engineLuaRefreshName);
	std::cout << "PASS: original engine capture cancels deferred work without duplicate publication\n";
}
