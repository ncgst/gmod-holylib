#include "httplib.h"
#include "modules/gmoddatapack_luapack.h"
#include "modules/gmoddatapack_luapack_policy.h"

#include "LuaInterface.h"
#include "detours.h"
#include "eiface.h"
#include "filesystem.h"
#include "lua.h"
#include "module.h"
#include "sourcesdk/baseclient.h"
#include "sourcesdk/iluashared.h"
#include "sourcesdk/tier2.h"
#include "networkstringtabledefs.h"
#include "picosha2/picosha2.h"
#include "tier1/convar.h"

#undef isalnum
#undef isalpha
#undef isspace
#undef isdigit
#undef min
#undef max

#include "util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace HolyLib::LuaPack
{
	static constexpr const char* canonicalStubSource = "return __holypack()()";

	struct FileRecord
	{
		std::string virtualPath;
		std::string sourcePath;
		std::string contents;
		std::string identity;
		unsigned long long revision = 0;
	};

	struct Generation
	{
		std::string id;
		std::string md5;
		std::string salt;
		std::string resourcePath;
		unsigned long long sourceRevision = 0;
		double publishedAt = 0.0;
		bool engineDownloadable = false;
		std::shared_ptr<Bootil::AutoBuffer> compressedStub;
		std::shared_ptr<Bootil::AutoBuffer> compressedRequiredStub;
		// Immutable SHA-256 identities for the map-base bodies. The live registry
		// keeps current contents; comparing the two selects canonical base stubs or
		// native deltas without building another downloadable generation.
		std::unordered_map<std::string, std::string> files;
		unsigned int pins = 0;
	};

	struct ClientPin
	{
		std::string generation;
		double deadline = 0.0;
		bool ready = false;
		bool everReady = false;
		bool active = false;
		bool fallback = true;
		bool holdsPin = false;
		bool deliveryLaneResolved = false;
		bool requiredLane = false;
		bool optOut = false;
		bool nativeLane = false;
		bool requiredRecovery = false;
		bool resolvedIdentity = false;
		bool authenticatedIdentity = false;
		bool recoveryStateCleared = false;
		bool disconnectIssued = false;
		std::uint64_t steamID64 = 0;
		std::uint64_t connectionSerial = 0;

		// Join delivery accounting. All of this is per-connection state; ReleasePin resets it.
		std::string networkID;
		bool nativeLatched = false;
		bool joinSummaryLogged = false;
		unsigned int joinNativeFiles = 0;
		unsigned int joinOptimisticStubs = 0;
		unsigned int joinRequiredStubs = 0;
		unsigned int joinReadyStubs = 0;
		unsigned long long joinNativeBytes = 0;
	};

	struct BuildTask
	{
		std::vector<FileRecord> files;
		std::string packDirectory;
		std::string salt;
		unsigned long long sourceRevision = 0;
		Bootil::AutoBuffer uncompressed;
		Bootil::AutoBuffer compressed;
		std::string md5;
		std::string error;
		std::atomic<bool> complete{false};
		bool success = false;
	};

	struct UploadTask
	{
		std::string url;
		std::string method;
		std::string md5;
		std::string resourcePath;
		std::string body;
		double objectRetentionSeconds = 0.0;
		std::string error;
		int status = 0;
		bool success = false;
		std::atomic<bool> complete{false};
	};

	struct State
	{
		std::mutex registryMutex;
		std::unordered_map<std::string, FileRecord> files;
		unsigned long long revision = 0;
		bool buildRequested = false;
		IThreadPool* buildPool = nullptr;
		IThreadPool* uploadPool = nullptr;
		BuildTask* activeBuild = nullptr;
		std::vector<UploadTask*> uploads;
		std::map<std::string, Generation> generations;
		std::string currentGeneration;
		std::string salt;
		INetworkStringTableContainer* stringTables = nullptr;
		INetworkStringTable* downloadables = nullptr;
		std::string lockedDownloadUrl;
		bool downloadUrlLocked = false;
		ClientPin clients[ABSOLUTE_PLAYER_LIMIT];
		// Accounts whose next connections must receive native files (a speculative stub could
		// not be resolved client-side). Keyed by engine network ID, value is the expiry time.
		// Main thread only, like the generation map.
		std::unordered_map<std::string, double> nativeLatches;
		Policy::RequiredRecoveryTracker requiredRecovery;
		Policy::RequiredRecoveryHandoff recoveryHandoffs[ABSOLUTE_PLAYER_LIMIT];
		std::uint64_t nextConnectionSerial = 0;
		bool featureEnabledLastFrame = false;
		bool canonicalRegistrationAvailableLastFrame = false;
		bool bootstrapRefresh = false;
		double lastCaptureAt = 0.0; // guarded by registryMutex
		double nextBuildAllowed = 0.0;
	};

	static State state;

	static const char* clientBootstrap = R"HOLYLUAPACK(
-- HolyLib luapack bootstrap. The server does not send pack bodies through the netchannel.
-- This chunk is prepended to lua/includes/init.lua, so it runs before ANY pure-Lua base
-- library exists (util.lua, extensions/*.lua, modules/*.lua are included later by this
-- very file). Only engine-registered C functions may be used here: Msg, CreateConVar,
-- GetConVar_Internal, file.Open, util.MD5/Decompress, CompileString, RunConsoleCommand,
-- string.*, debug.*. In particular GetConVar, Color, file.Read (an extensions/file.lua
-- wrapper) and net.Receive do NOT exist yet.
do
	local getConVar = GetConVar_Internal or GetConVar
	local mirrorFlags = (FCVAR_REPLICATED or 0) + (FCVAR_PROTECTED or 0) +
		(FCVAR_DONTRECORD or 0) + (FCVAR_UNLOGGED or 0) + (FCVAR_UNREGISTERED or 0)
	local function mirroredConVar(name, default)
		local ok, convar = pcall(CreateConVar, name, default, mirrorFlags)
		return (ok and convar) or (getConVar and getConVar(name))
	end
	local requiredConVar = mirroredConVar("holylib_gmoddatapack_luapack_required", "0")
	local allowOptOutConVar = mirroredConVar("holylib_gmoddatapack_luapack_allow_optout", "1")
	local sourceTvConVar = getConVar and getConVar("tv_nochat")
	local clientOptedOut = allowOptOutConVar and allowOptOutConVar:GetBool() and sourceTvConVar and
		sourceTvConVar:GetString() == "no_gluapack"
	local requiredMode = requiredConVar and requiredConVar:GetBool() and not clientOptedOut

	local function warn(message)
		Msg("[HolyLib luapack] " .. message .. "\n")
	end

	-- file.Read is defined later by extensions/file.lua; only file.Open exists here.
	local function readFile(path)
		local handle = file.Open(path, "rb", "GAME")
		if not handle then return nil end
		local contents = handle:Read(handle:Size())
		handle:Close()
		return contents or ""
	end

	-- Safety preamble. The server may speculatively deliver generation stubs before this
	-- client has proven it can resolve them (holylib_gmoddatapack_luapack_optimistic). When a
	-- stub executes and no pack can serve it, the session cannot be repaired in place — the
	-- affected files already yielded nothing — so the client asks the server to latch native
	-- delivery for this account and reconnects. The unready command needs at least one frame
	-- on the wire before the reconnect tears the channel down, hence the timer; the server
	-- also latches on its own when a speculated connection dies unacknowledged, so a lost
	-- unready command still converges.
	-- Required stubs pass a second true argument. They never switch this connection
	-- or execute retry locally. The exact failure command lets the authenticated server
	-- queue at most one engine-level reconnect whose new ServerInfo baseline is native.
	local recoveryTaint = false
	local recoverySignaled = false
	local requiredFailureSignaled = false
	local recoveryScheduled = false
	local recoveryRetried = false
	local function issueRetry()
		if recoveryRetried then return end
		recoveryRetried = true
		RunConsoleCommand("retry")
	end
	local function scheduleRetry()
		if recoveryScheduled or recoveryRetried then return end
		if timer and timer.Simple then
			recoveryScheduled = true
			timer.Simple(1, issueRetry)
		end
	end
	local function stubRecovery(generation, required)
		-- A cached pre-required stub has no second argument. Fall back to replicated lane
		-- policy so enabling required mode cannot resurrect the old automatic retry loop.
		if required == nil then required = requiredMode end
		recoveryTaint = true
		if required then
			if not requiredFailureSignaled then
				requiredFailureSignaled = true
				warn("required pack " .. tostring(generation) ..
					" could not resolve this Lua file; requesting one authenticated engine reconnect" ..
					" (manual opt out: +tv_nochat no_gluapack)")
				RunConsoleCommand("holylib_luapack_failed", tostring(generation or ""))
			end
			return function() end
		end
		if not recoverySignaled then
			recoverySignaled = true
			warn("cannot resolve stubbed Lua for generation " .. tostring(generation) ..
				"; requesting native redelivery and reconnecting")
			RunConsoleCommand("holylib_luapack_unready")
			scheduleRetry()
			return function() end
		end
		if not recoveryScheduled then
			-- No timer library existed on the first call (base includes not loaded yet).
			-- Try again, else reconnect right away: even if the unready command is lost in
			-- the teardown, the server-side disconnect latch makes the next join native.
			scheduleRetry()
			if not recoveryScheduled then issueRetry() end
		end
		return function() end
	end
	-- Stubs must never hit a nil global, even when the full bootstrap below cannot run
	-- (empty or unparsable manifest snapshot). installOverrides() replaces this guard.
	if not _G.__holypack then
		_G.__holypack = function(generation, required) return stubRecovery(generation, required) end
	end

	local function bootstrap()
		if _G.__holypack_bootstrapped then return end

		local manifestConVar = mirroredConVar("holylib_gmoddatapack_luapack_manifest", "")
		local snapshot = manifestConVar and manifestConVar:GetString() or ""
		if snapshot == "" then return end

		local function fromHex(value)
			if #value % 2 ~= 0 or value:find("[^0-9a-fA-F]") then return nil end
			return (value:gsub("..", function(pair) return string.char(tonumber(pair, 16)) end))
		end

		local function toHex(value)
			return (value:gsub(".", function(byte) return string.format("%02x", string.byte(byte)) end))
		end

		-- The client engine truncates replicated convar values to 255 characters, so the
		-- snapshot only carries what cannot be derived: a generation id doubles as the
		-- content MD5 and the FastDL object basename, and every generation published in
		-- one server lifecycle shares the salt.
		local function parseSnapshot(value)
			local version, currentGeneration, packDirectoryHex, saltHex, generationList =
				value:match("^(%d+)|([^|]+)|([^|]*)|([^|]*)|(.*)$")
			local packDirectory = packDirectoryHex and fromHex(packDirectoryHex)
			local salt = saltHex and fromHex(saltHex)
			if version ~= "1" or not currentGeneration or not packDirectory or not salt then return nil end

			local manifests = {}
			for entry in string.gmatch(generationList or "", "[^;]+") do
				if #entry == 32 and not entry:find("[^0-9a-fA-F]") then
					manifests[entry] = {generation = entry, md5 = string.lower(entry), salt = salt,
						resource = "data/" .. packDirectory .. "/" .. entry .. ".bsp"}
				end
			end
			return currentGeneration, manifests
		end

		local currentGeneration, manifests = parseSnapshot(snapshot)
		if not currentGeneration then
			warn("ignored an invalid manifest snapshot; no Lua pack generation can be acknowledged")
			return
		end

		local function parsePack(contents, manifest)
			if #contents < 1 or string.byte(contents, 1) ~= 1 then return nil, "unsupported pack version" end
			local pack = {vfs = {}, vfsLCL = {}, salt = manifest.salt, manifest = manifest}
			local function registerLocal(key, source)
				local existing = pack.vfsLCL[key]
				if existing == nil then
					pack.vfsLCL[key] = source
				elseif existing ~= source then
					-- Local forms are compatibility fallbacks. A divergent collision must
					-- fail lookup instead of selecting whichever full path was parsed last.
					pack.vfsLCL[key] = false
				end
			end
			local cursor = 2
			while cursor <= #contents do
				if #contents - cursor + 1 < 52 then return nil, "truncated entry header" end
				local sourceKey = toHex(string.sub(contents, cursor, cursor + 15)); cursor = cursor + 16
				local localKeyOne = toHex(string.sub(contents, cursor, cursor + 15)); cursor = cursor + 16
				local localKeyTwo = toHex(string.sub(contents, cursor, cursor + 15)); cursor = cursor + 16
				local a, b, c, d = string.byte(contents, cursor, cursor + 3); cursor = cursor + 4
				local length = a * 16777216 + b * 65536 + c * 256 + d
				if length < 0 or cursor + length - 1 > #contents then return nil, "truncated entry payload" end
				local source = string.sub(contents, cursor, cursor + length - 1); cursor = cursor + length
				if pack.vfs[sourceKey] ~= nil then return nil, "duplicate exact source key" end
				pack.vfs[sourceKey] = source
				registerLocal(localKeyOne, source)
				registerLocal(localKeyTwo, source)
			end
			return pack
		end

		local packs = {}
		local lazyMountAttempted = {}
		-- The engine can fetch FastDL downloadables in the background of the join instead of
		-- blocking the loading screen, so a generation missing right now may simply not have
		-- finished downloading. tryMount() reports failures without warning; the boot pass
		-- warns once, then a timer retries for five minutes and acknowledges late arrivals
		-- (the server accepts a matching late READY).
		local function tryMount(manifest)
			local compressed = readFile("download/" .. manifest.resource)
			if not compressed then return false, "is unavailable" end
			local contents = util.Decompress(compressed)
			if not contents then return false, "could not be decompressed" end
			if string.lower(util.MD5(contents)) ~= manifest.md5 then return false, "failed its generation MD5 check" end
			local pack, parseError = parsePack(contents, manifest)
			if not pack then return false, "is invalid (" .. tostring(parseError) .. ")" end
			packs[manifest.generation] = pack
			return true
		end

		local selectedGeneration
		local activate -- defined after the resolver below

		local function normalizedForms(path)
			path = string.gsub(path or "", "^@", "")
			path = string.gsub(path, "\\", "/")
			local first = string.gsub(path, "^addons/[^/]+/", "")
			first = string.gsub(first, "^gamemodes/[^/]+/entities/", "")
			first = string.gsub(first, "^gamemodes/", "")
			first = string.gsub(first, "^lua/", "")
			local second = string.gsub(path, "^addons/[^/]+/", "")
			second = string.gsub(second, "^gamemodes/", "")
			second = string.gsub(second, "^lua/", "")
			return path, first, second
		end
)HOLYLUAPACK"
	// MSVC caps a single string literal at 16380 characters (C2026), which the bootstrap
	// outgrew; adjacent raw literals concatenate at compile time (total cap 65535).
	R"HOLYLUAPACK(
		local function findSource(pack, path, allowAliases)
			local sourcePath, first, second = normalizedForms(path)
			local salted = function(value) return string.lower(util.MD5(pack.salt .. value)) end
			local source = pack.vfs[salted(sourcePath)]
			if source ~= nil then return source, sourcePath end
			if not allowAliases then return nil end

			local firstSource = pack.vfsLCL[salted(first)]
			if firstSource == false then return nil end
			if firstSource ~= nil then return firstSource, sourcePath end
			if second == first then return nil end
			local secondSource = pack.vfsLCL[salted(second)]
			if secondSource == false then return nil end
			if secondSource ~= nil then return secondSource, sourcePath end
			return nil
		end

		local function compilePacked(pack, path, allowAliases, showError)
			local source, resolvedPath = findSource(pack, path, allowAliases)
			if not source then return nil, false end
			local chunkName = "@" .. resolvedPath
			local compiled = CompileString(source, chunkName, showError ~= false)
			if type(compiled) ~= "function" then
				warn("failed to compile packed file " .. tostring(resolvedPath) .. ": " .. tostring(compiled))
				return nil, true
			end
			return compiled, true
		end

		local resolverInstalled = false
		local function installResolver()
			if resolverInstalled then return end
			resolverInstalled = true
			_G.__holypack_bootstrapped = true
			_G.__holypack_packs = packs

			function _G.__holypack(generation, required)
				-- Canonical placeholders are deliberately generation-independent so their
				-- SHA256 can be registered once in client_lua_files. The replicated manifest
				-- selects the immutable map-base generation for this level.
				if generation == nil then generation = selectedGeneration or currentGeneration end
				local pack = packs[generation]
				if not pack and not recoveryTaint and not lazyMountAttempted[generation] then
					-- A speculative stub can arrive while the pack's FastDL download is still
					-- finishing in the background; one synchronous mount attempt right now is
					-- far cheaper than a reconnect. Never re-attempted: a corrupt object would
					-- otherwise be re-decompressed once per stubbed file.
					lazyMountAttempted[generation] = true
					local manifest = manifests[generation]
					if manifest and tryMount(manifest) then
						pack = packs[generation]
						RunConsoleCommand("holylib_luapack_ready", generation, manifest.md5)
					end
				end
				if not pack then return stubRecovery(generation, required) end
				selectedGeneration = generation
				local info = debug.getinfo(2, "S")
				local sourcePath = info and info.source or ""
				local compiled = compilePacked(pack, string.gsub(sourcePath, "^@", ""), true, false)
				if not compiled then
					-- Mounted pack but no usable entry (or the entry failed to compile): the
					-- file cannot be produced in place either way, so recover like an
					-- unmounted generation instead of erroring the boot.
					warn("pack " .. tostring(generation) .. " has no usable entry for " .. tostring(sourcePath))
					return stubRecovery(generation, required)
				end
				return compiled
			end

		end

		activate = function()
			installResolver()
		end

		-- Boot mount pass. Acknowledge whatever is already on disk; stub delivery is gated
		-- server-side on this exact ACK and must not depend on anything loaded later in init.
		-- Map-base mode publishes one generation per level. Keep the table loop for manifest
		-- parser compatibility, but only the current base is expected.
		local pendingManifests = {}
		local downloadFilter = getConVar("cl_downloadfilter")
		local downloadsDisabled = downloadFilter and downloadFilter:GetString() == "none"
		for generation, manifest in pairs(manifests) do
			local mountedNow, failure = tryMount(manifest)
			if not mountedNow and not downloadsDisabled then
				pendingManifests[generation] = manifest
			end
			if not mountedNow and generation == currentGeneration then
				if downloadsDisabled then
					warn("pack " .. generation .. " is missing and downloads are disabled; set cl_downloadfilter to mapsonly or all. Required delivery will disconnect; fail-open delivery can use native Lua")
				else
					warn("pack " .. generation .. " " .. failure .. "; waiting for the background FastDL download before it can be acknowledged")
				end
			end
		end

		-- The resolver installs even when nothing mounted: the server may speculate with
		-- stubs before this client has acknowledged anything, and __holypack must then
		-- lazily mount a download that completed after the pass above — or recover —
		-- instead of hitting a nil global.
		activate()
		for generation, pack in pairs(packs) do
			RunConsoleCommand("holylib_luapack_ready", generation, pack.manifest.md5)
		end

		if next(pendingManifests) and timer and timer.Create then
			local attempts = 0
			local lastFailure = {}
			timer.Create("HolyLibLuaPackMount", 5, 60, function()
				attempts = attempts + 1
				for generation, manifest in pairs(pendingManifests) do
					local mountedNow, failure = tryMount(manifest)
					if mountedNow then
						pendingManifests[generation] = nil
						lastFailure[generation] = nil
						activate()
						if not recoveryTaint then
							RunConsoleCommand("holylib_luapack_ready", generation, manifest.md5)
						end
						warn("pack " .. generation .. " mounted after its FastDL download completed")
					else
						lastFailure[generation] = failure
					end
				end
				if not next(pendingManifests) then
					timer.Remove("HolyLibLuaPackMount")
				elseif attempts >= 60 then
					if pendingManifests[currentGeneration] then
						warn("pack " .. currentGeneration .. " could not be mounted after five minutes (last failure: " .. tostring(lastFailure[currentGeneration]) .. ") and remains unacknowledged")
					end
				end
			end)
		end
	end

	local ok, message = xpcall(bootstrap, debug.traceback)
	if not ok then Msg("[HolyLib luapack] bootstrap failed; no Lua pack generation can be acknowledged: " .. tostring(message) .. "\n") end
end
)HOLYLUAPACK";

	static const char* serverBridge = R"HLPACKSERVER(
concommand.Add("holylib_gmoddatapack_luapack_kill", function(caller)
	if IsValid(caller) and not caller:IsSuperAdmin() then
		caller:ChatPrint("HolyLib luapack kill-switch requires superadmin")
		return
	end
	RunConsoleCommand("holylib_gmoddatapack_luapack_enable", "0")
	Msg("[HolyLib luapack] kill-switch activated; all clients now use vanilla Lua delivery\n")
end, nil, "Immediately disable bundled delivery and restore per-file vanilla Lua networking", FCVAR_DONTRECORD)
)HLPACKSERVER";

	static std::string NormalizePath(std::string path)
	{
		std::replace(path.begin(), path.end(), '\\', '/');
		while (!path.empty() && (path[0] == '@' || path[0] == '/'))
			path.erase(path.begin());

		return path;
	}

	static std::string ContentIdentity(const std::string& contents)
	{
		return picosha2::hash256_hex_string(contents);
	}

	static bool StartsWith(const std::string& value, const char* prefix)
	{
		const size_t prefixLength = strlen(prefix);
		return value.length() >= prefixLength && value.compare(0, prefixLength, prefix) == 0;
	}

	static std::string StripAddonPrefix(std::string path)
	{
		if (!StartsWith(path, "addons/"))
			return path;

		const size_t slash = path.find('/', strlen("addons/"));
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	static std::string LocalKeyFormOne(std::string path)
	{
		path = StripAddonPrefix(NormalizePath(path));
		if (StartsWith(path, "gamemodes/"))
		{
			const size_t entities = path.find("/entities/", strlen("gamemodes/"));
			if (entities != std::string::npos)
				path = path.substr(entities + strlen("/entities/"));
			else
				path.erase(0, strlen("gamemodes/"));
		}

		if (StartsWith(path, "lua/"))
			path.erase(0, strlen("lua/"));

		return path;
	}

	static std::string LocalKeyFormTwo(std::string path)
	{
		path = StripAddonPrefix(NormalizePath(path));
		if (StartsWith(path, "gamemodes/"))
			path.erase(0, strlen("gamemodes/"));
		if (StartsWith(path, "lua/"))
			path.erase(0, strlen("lua/"));

		return path;
	}

	// The engine's registered init file can be provided by an addon (dash ships
	// addons/dash/lua/includes/init.lua, which shadows the base file and becomes the datapack
	// entry). Every init-file decision must therefore match by normalized key, not exact name,
	// or the client bootstrap is never injected on such servers.
	bool IsInitFile(const std::string& virtualPath)
	{
		return LocalKeyFormTwo(virtualPath) == "includes/init.lua";
	}

	static bool HexToBytes(const std::string& hex, unsigned char* output, size_t outputLength)
	{
		if (hex.length() != outputLength * 2)
			return false;

		for (size_t i = 0; i < outputLength; ++i)
		{
			const char pair[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
			char* end = nullptr;
			const unsigned long value = strtoul(pair, &end, 16);
			if (!end || *end != '\0' || value > 0xff)
				return false;

			output[i] = static_cast<unsigned char>(value);
		}

		return true;
	}

	static bool WritePackKey(Bootil::AutoBuffer& output, const std::string& salt, const std::string& value)
	{
		unsigned char key[16];
		const std::string hash = Bootil::Hasher::MD5::String(salt + value);
		if (!HexToBytes(hash, key, sizeof(key)))
			return false;

		output.Write(key, sizeof(key));
		return true;
	}

	static bool ValidatePack(const BuildTask* task)
	{
		if (!task || task->uncompressed.GetWritten() < 1)
			return false;

		const unsigned char* data = static_cast<const unsigned char*>(task->uncompressed.GetBase());
		const size_t dataLength = task->uncompressed.GetWritten();
		if (data[0] != 1)
			return false;

		size_t offset = 1;
		size_t fileIndex = 0;
		std::unordered_set<std::string> sourceKeys;
		while (offset < dataLength)
		{
			if (dataLength - offset < (16 * 3) + 4 || fileIndex >= task->files.size())
				return false;

			const std::string sourceKey(reinterpret_cast<const char*>(data + offset), 16);
			if (!Policy::RegisterExactKey(sourceKeys, sourceKey))
				return false;
			offset += 16 * 3;
			const unsigned int contentLength =
				(static_cast<unsigned int>(data[offset]) << 24) |
				(static_cast<unsigned int>(data[offset + 1]) << 16) |
				(static_cast<unsigned int>(data[offset + 2]) << 8) |
				static_cast<unsigned int>(data[offset + 3]);
			offset += 4;

			if (contentLength > dataLength - offset || contentLength != task->files[fileIndex].contents.length())
				return false;
			if (memcmp(data + offset, task->files[fileIndex].contents.data(), contentLength) != 0)
				return false;

			offset += contentLength;
			++fileIndex;
		}

		return offset == dataLength && fileIndex == task->files.size();
	}

	static void BuildPack(BuildTask*& task)
	{
		if (!task)
			return;

		const unsigned char version = 1;
		task->uncompressed.Write(&version, sizeof(version));
		for (const FileRecord& file : task->files)
		{
			if (file.contents.length() > std::numeric_limits<unsigned int>::max())
			{
				task->error = "a Lua source file exceeds the pack format's u32 length";
				task->complete.store(true);
				return;
			}

			if (!WritePackKey(task->uncompressed, task->salt, file.sourcePath) ||
				!WritePackKey(task->uncompressed, task->salt, LocalKeyFormOne(file.virtualPath)) ||
				!WritePackKey(task->uncompressed, task->salt, LocalKeyFormTwo(file.virtualPath)))
			{
				task->error = "failed to encode an MD5 index key";
				task->complete.store(true);
				return;
			}

			const unsigned int contentLength = static_cast<unsigned int>(file.contents.length());
			const unsigned char length[4] = {
				static_cast<unsigned char>((contentLength >> 24) & 0xff),
				static_cast<unsigned char>((contentLength >> 16) & 0xff),
				static_cast<unsigned char>((contentLength >> 8) & 0xff),
				static_cast<unsigned char>(contentLength & 0xff),
			};
			task->uncompressed.Write(length, sizeof(length));
			task->uncompressed.Write(file.contents.data(), file.contents.length());
		}

		if (!ValidatePack(task))
		{
			task->error = "internal build-to-parse validation failed";
			task->complete.store(true);
			return;
		}

		task->md5 = Bootil::Hasher::MD5::Easy(task->uncompressed.GetBase(), task->uncompressed.GetWritten());
		std::transform(task->md5.begin(), task->md5.end(), task->md5.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		if (!Bootil::Compression::LZMA::Compress(
			task->uncompressed.GetBase(), task->uncompressed.GetWritten(), task->compressed, 9))
		{
			task->error = "LZMA compression failed";
			task->complete.store(true);
			return;
		}

		task->success = true;
		task->complete.store(true);
	}

	static std::shared_ptr<Bootil::AutoBuffer> BuildCompressedStub()
	{
		const std::string source = canonicalStubSource;
		std::vector<unsigned char> hash(32);
		picosha2::hash256_one_by_one hasher;
		hasher.process(source.c_str(), source.c_str() + source.length() + 1);
		hasher.finish();
		hasher.get_hash_bytes(hash.begin(), hash.end());

		auto output = std::make_shared<Bootil::AutoBuffer>();
		output->Write(hash.data(), hash.size());
		if (!Bootil::Compression::LZMA::Compress(source.c_str(), source.length() + 1, *output, 9))
			return nullptr;

		return output;
	}

	static double ServerTime()
	{
		// LuaPack uses this clock only for elapsed-time gates and expirations. The
		// IVEngineServer mirror is not ABI-stable across current GMod engine builds;
		// calling Time() through a stale virtual slot produced nonsensical values
		// on Linux x64 and broke both pacing and recovery deadlines. Plat_FloatTime is
		// monotonic and does not depend on that engine vtable layout.
		return Plat_FloatTime();
	}

	static void ReleaseGenerationReference(ClientPin& client)
	{
		if (client.holdsPin && !client.generation.empty())
		{
			auto generation = state.generations.find(client.generation);
			if (generation != state.generations.end() && generation->second.pins > 0)
				--generation->second.pins;
		}
		client.holdsPin = false;
	}

	static void ReleasePin(ClientPin& client)
	{
		ReleaseGenerationReference(client);
		client = ClientPin();
	}

	static bool PinCurrentGeneration(ClientPin& client)
	{
		if (!client.generation.empty())
			return true;
		if (state.currentGeneration.empty())
			return false;

		auto generation = state.generations.find(state.currentGeneration);
		if (generation == state.generations.end())
			return false;

		client.generation = generation->first;
		client.deadline = ServerTime() + GetConfig().readyDeadlineSeconds;
		client.fallback = false;
		client.holdsPin = true;
		++generation->second.pins;
		return true;
	}

	static void MarkFallback(ClientPin& client)
	{
		ReleaseGenerationReference(client);
		client.ready = false;
		client.fallback = true;
	}

	static bool IsValidSlot(int slot)
	{
		return slot >= 0 && slot < ABSOLUTE_PLAYER_LIMIT;
	}

	static void RecordNativeJoin(ClientPin& client, size_t nativeSourceBytes)
	{
		if (client.active)
			return;

		++client.joinNativeFiles;
		client.joinNativeBytes += nativeSourceBytes;
	}

	static bool BindResolvedIdentity(int slot, ClientPin& client)
	{
		CBaseClient* baseClient = Util::server ? Util::GetClientByIndex(slot) : nullptr;
		if (!baseClient || !baseClient->m_SteamID.IsValid())
			return false;

		const std::uint64_t steamID64 = baseClient->m_SteamID.ConvertToUint64();
		if (steamID64 == 0 || (client.resolvedIdentity && client.steamID64 != steamID64))
			return false;

		client.steamID64 = steamID64;
		client.resolvedIdentity = true;
		return true;
	}

	static bool BindAuthenticatedIdentity(int slot, ClientPin& client)
	{
		CBaseClient* baseClient = Util::server ? Util::GetClientByIndex(slot) : nullptr;
		if (!baseClient || !BindResolvedIdentity(slot, client) || !baseClient->IsFullyAuthenticated())
			return false;

		client.authenticatedIdentity = true;
		return true;
	}

	static void StartClientEpoch(int slot)
	{
		ReleasePin(state.clients[slot]);
		ClientPin& client = state.clients[slot];
		++state.nextConnectionSerial;
		if (state.nextConnectionSerial == 0)
			++state.nextConnectionSerial;
		client.connectionSerial = state.nextConnectionSerial;
		if (!IsEnabled())
			return;

		CBaseClient* baseClient = Util::server ? Util::GetClientByIndex(slot) : nullptr;
		const char* networkID = baseClient ? baseClient->GetNetworkIDString() : nullptr;
		if (networkID && networkID[0] != '\0' &&
			V_stricmp(networkID, "BOT") != 0 && V_stricmp(networkID, "UNKNOWN") != 0)
			client.networkID = networkID;

		PinCurrentGeneration(client);
	}

	static const char* BeginPendingRecoveryBaseline(int slot)
	{
		CBaseClient* baseClient = Util::server ? Util::GetClientByIndex(slot) : nullptr;
		const std::uint64_t account = baseClient && baseClient->m_SteamID.IsValid()
			? baseClient->m_SteamID.ConvertToUint64() : 0;
		const bool authenticated = baseClient && baseClient->IsFullyAuthenticated();
		Policy::RequiredRecoveryHandoff* handoff = nullptr;
		for (int handoffSlot = 0; handoffSlot < ABSOLUTE_PLAYER_LIMIT; ++handoffSlot)
		{
			Policy::RequiredRecoveryHandoff& candidate = state.recoveryHandoffs[handoffSlot];
			if (!candidate.Pending() || candidate.Account() != account)
				continue;
			if (handoff)
				return "multiple recovery handoffs matched the authenticated SteamID64";
			handoff = &candidate;
		}
		if (!handoff)
		{
			if (state.recoveryHandoffs[slot].Pending())
				return "the recovery ServerInfo boundary did not have its authenticated SteamID64";
			return nullptr;
		}

		ClientPin& failedClient = state.clients[slot];
		const std::uint64_t failedConnection = failedClient.connectionSerial;
		const std::uint64_t expectedAccount = handoff->Account();
		const std::uint64_t expectedConnection = handoff->FailedConnection();
		const Policy::RecoveryHandoffResult begin = handoff->BeginBaseline(
			account, authenticated, ServerTime());
		if (begin == Policy::RecoveryHandoffResult::OwnershipMismatch)
		{
			handoff->Reset();
			Warning(PROJECT_NAME " - luapack: ignored a stale recovery handoff at ServerInfo for slot %i; expected account %llu connection %llu, got %llu connection %llu\n",
				slot, static_cast<unsigned long long>(expectedAccount),
				static_cast<unsigned long long>(expectedConnection),
				static_cast<unsigned long long>(account),
				static_cast<unsigned long long>(failedConnection));
			return nullptr;
		}
		if (begin != Policy::RecoveryHandoffResult::BeginBaseline)
			return "the engine reconnect did not begin an authenticated recovery baseline within its bounded handoff window";

		// This is the first instruction at the engine's new ServerInfo boundary. The
		// reconnect can run ClientDisconnect/ClientConnect in either order around this
		// hook, so begin a fresh epoch here unconditionally and consume the account latch
		// atomically. No old-join request can observe this new native lane.
		StartClientEpoch(slot);
		ClientPin& recovery = state.clients[slot];
		if (!BindResolvedIdentity(slot, recovery) || recovery.steamID64 != expectedAccount ||
			!baseClient->IsFullyAuthenticated())
		{
			return "SteamID64 ownership changed while the engine reconnect entered ServerInfo";
		}
		recovery.authenticatedIdentity = true;
		const Policy::RecoveryConsumeResult consumed = state.requiredRecovery.Consume(
			recovery.steamID64, recovery.connectionSerial, ServerTime());
		if (consumed != Policy::RecoveryConsumeResult::Native)
		{
			return "the one-shot recovery latch could not be consumed at the new ServerInfo baseline";
		}

		recovery.requiredLane = true;
		recovery.requiredRecovery = true;
		recovery.nativeLane = true;
		recovery.nativeLatched = true;
		recovery.deliveryLaneResolved = true;
		MarkFallback(recovery);
		Msg(PROJECT_NAME " - luapack: client slot %i (%llu) began engine reconnect epoch %llu with a wholly native initial baseline\n",
			slot, static_cast<unsigned long long>(recovery.steamID64),
			static_cast<unsigned long long>(recovery.connectionSerial));
		return nullptr;
	}

	static void ClearProvenRecoveryState(int slot, ClientPin& client)
	{
		if (client.recoveryStateCleared || !client.active || !BindAuthenticatedIdentity(slot, client))
			return;

		if (client.requiredRecovery)
		{
			if (!state.requiredRecovery.Complete(client.steamID64, client.connectionSerial))
				return;
			client.recoveryStateCleared = true;
			Msg(PROJECT_NAME " - luapack: client slot %i (%llu) completed its authenticated native recovery connection; retry state cleared\n",
				slot, static_cast<unsigned long long>(client.steamID64));
			return;
		}

		if (client.optOut || (client.requiredLane && client.ready))
		{
			const Policy::RecoveryClearResult cleared =
				state.requiredRecovery.ClearAfterSuccessfulConnection(
					client.steamID64, client.connectionSerial);
			if (cleared == Policy::RecoveryClearResult::SameFailedConnection)
			{
				Warning(PROJECT_NAME " - luapack: retained required recovery state for slot %i (%llu); the connection that armed it cannot clear its own latch\n",
					slot, static_cast<unsigned long long>(client.steamID64));
				return;
			}
			if (cleared == Policy::RecoveryClearResult::Cleared)
			{
				Msg(PROJECT_NAME " - luapack: client slot %i (%llu) reached a distinct authenticated successful connection; prior recovery state cleared\n",
					slot, static_cast<unsigned long long>(client.steamID64));
			}
			client.recoveryStateCleared = true;
		}
	}

	static bool PreserveClaimedRecoveryLifecycle(int slot, bool gameLayerCallback)
	{
		if (!IsValidSlot(slot))
			return false;

		ClientPin& client = state.clients[slot];
		CBaseClient* baseClient = Util::server ? Util::GetClientByIndex(slot) : nullptr;
		const std::uint64_t account = baseClient && baseClient->m_SteamID.IsValid()
			? baseClient->m_SteamID.ConvertToUint64() : 0;
		const bool identityMatches = account != 0 && client.resolvedIdentity &&
			account == client.steamID64;
		const bool ownsConsumedRecovery = state.requiredRecovery.OwnsConsumed(
			client.steamID64, client.connectionSerial);
		return Policy::ShouldPreserveRecoveryLifecycle(gameLayerCallback,
			client.requiredRecovery && client.nativeLane, client.active,
			client.recoveryStateCleared, identityMatches, ownsConsumedRecovery);
	}

	static void ProcessRequiredRecoveryHandoffs(double now)
	{
		for (int slot = 0; slot < ABSOLUTE_PLAYER_LIMIT; ++slot)
		{
			Policy::RequiredRecoveryHandoff& handoff = state.recoveryHandoffs[slot];
			if (!handoff.Pending())
				continue;

			ClientPin& client = state.clients[slot];
			CBaseClient* baseClient = Util::server ? Util::GetClientByIndex(slot) : nullptr;
			const std::uint64_t account = baseClient && baseClient->m_SteamID.IsValid()
				? baseClient->m_SteamID.ConvertToUint64() : 0;
			const std::uint64_t expectedAccount = handoff.Account();
			const std::uint64_t expectedConnection = handoff.FailedConnection();
			const bool authenticated = baseClient && baseClient->IsFullyAuthenticated();
			const bool channelReady = baseClient && baseClient->IsConnected() && baseClient->GetNetChannel();
			const Policy::RecoveryHandoffResult result = handoff.TryInvoke(account,
				client.connectionSerial, authenticated, channelReady, now);
			switch (result)
			{
				case Policy::RecoveryHandoffResult::Invoke:
					Msg(PROJECT_NAME " - luapack: invoking one engine reconnect for slot %i (%llu) after failed connection epoch %llu\n",
						slot, static_cast<unsigned long long>(account),
						static_cast<unsigned long long>(client.connectionSerial));
					// Virtual dispatch reaches CGameClient::Reconnect. A later frame may dispatch
					// exactly one ServerInfo only after the engine has returned to CONNECTED.
					baseClient->Reconnect();
					// Reconnect may synchronously run disconnect/connect callbacks. Re-resolve the
					// slot instead of dereferencing the pre-reconnect client pointer afterward.
					if (CBaseClient* restarted = Util::server ? Util::GetClientByIndex(slot) : nullptr)
					{
						Msg(PROJECT_NAME " - luapack: engine reconnect returned for slot %i (%llu): signon=%i send_server_info=%s channel=%s authenticated=%s\n",
							slot, static_cast<unsigned long long>(account),
							restarted->m_nSignonState, restarted->m_bSendServerInfo ? "yes" : "no",
							restarted->GetNetChannel() ? "yes" : "no",
							restarted->IsFullyAuthenticated() ? "yes" : "no");
					}
					else
					{
						Msg(PROJECT_NAME " - luapack: engine reconnect returned for slot %i (%llu) with the slot temporarily unavailable\n",
							slot, static_cast<unsigned long long>(account));
					}
					break;
				case Policy::RecoveryHandoffResult::AlreadyInvoked:
				{
					const bool signonRestarted = baseClient &&
						baseClient->m_nSignonState == SIGNONSTATE_CONNECTED;
					const Policy::RecoveryHandoffResult dispatch = handoff.TryDispatchServerInfo(
						account, authenticated, channelReady, signonRestarted, now);
					if (dispatch == Policy::RecoveryHandoffResult::DispatchServerInfo)
					{
						const bool engineQueued = baseClient->m_bSendServerInfo;
						// Reconnect resets the sign-on state but does not reliably queue the next
						// baseline on this engine branch. At exact CONNECTED, this is the engine's
						// normal pre-ServerInfo flag; the detoured call atomically consumes the
						// account latch before any Lua string-table hashes are serialized.
						baseClient->m_bSendServerInfo = true;
						Msg(PROJECT_NAME " - luapack: dispatching one native ServerInfo for slot %i (%llu) after sign-on restart (engine_queued=%s)\n",
							slot, static_cast<unsigned long long>(account),
							engineQueued ? "yes" : "no");
						if (!baseClient->SendServerInfo())
						{
							Warning(PROJECT_NAME " - luapack: native recovery ServerInfo failed for slot %i (%llu); the consumed one-shot will not retry\n",
								slot, static_cast<unsigned long long>(account));
						}
					}
					break;
				}
				case Policy::RecoveryHandoffResult::Expired:
					if (account == expectedAccount)
					{
						DisconnectRequiredClient(slot,
							"the engine reconnect did not begin its native ServerInfo baseline within the bounded handoff window");
					}
					break;
				case Policy::RecoveryHandoffResult::OwnershipMismatch:
					handoff.Reset();
					Warning(PROJECT_NAME " - luapack: cancelled stale recovery handoff for slot %i; expected account %llu connection %llu, got %llu connection %llu\n",
						slot, static_cast<unsigned long long>(expectedAccount),
						static_cast<unsigned long long>(expectedConnection),
						static_cast<unsigned long long>(account),
						static_cast<unsigned long long>(client.connectionSerial));
					break;
				case Policy::RecoveryHandoffResult::Invalid:
					handoff.Reset();
					DisconnectRequiredClient(slot,
						"the authenticated engine reconnect could not be invoked safely");
					break;
				case Policy::RecoveryHandoffResult::None:
				case Policy::RecoveryHandoffResult::NotReady:
				case Policy::RecoveryHandoffResult::DispatchServerInfo:
				case Policy::RecoveryHandoffResult::BeginBaseline:
					break;
			}
		}
	}

	static void ResolveDeliveryLane(int slot, ClientPin& client)
	{
		if (client.deliveryLaneResolved)
			return;

		const Config& currentConfig = GetConfig();
		CBaseClient* baseClient = Util::server ? Util::GetClientByIndex(slot) : nullptr;
		const char* optOutValue = baseClient ? baseClient->GetUserSetting("tv_nochat") : nullptr;
		const Policy::Lane lane = Policy::ResolveLane(currentConfig.requiredStubbing,
			currentConfig.allowOptOut, optOutValue);
		client.optOut = lane == Policy::Lane::NativeOptOut;
		client.requiredLane = lane == Policy::Lane::Required;
		client.nativeLane = lane != Policy::Lane::Required;
		client.deliveryLaneResolved = true;

		if (client.nativeLane)
		{
			// Explicit opt-out remains the highest-priority lane. Capture a resolved
			// identity when available so a successful manual recovery can heal a pending
			// automatic latch, but never reject opt-out for an authentication race.
			if (client.optOut)
				BindResolvedIdentity(slot, client);
			MarkFallback(client);
			if (client.optOut)
				Msg(PROJECT_NAME " - luapack: client slot %i selected native delivery with tv_nochat=no_gluapack\n", slot);
			else
				Msg(PROJECT_NAME " - luapack: client slot %i selected the explicit native rescue lane because required mode is disabled\n", slot);
			return;
		}

		const bool resolvedIdentity = BindResolvedIdentity(slot, client);
		if (Policy::CanConsumeRequiredRecovery(currentConfig.requiredRecovery,
			resolvedIdentity))
		{
			// The ticket-provided SteamID is resolved before ServerInfo, while Source's
			// fully-authenticated flag normally arrives later in signon. Resolution is
			// sufficient to select an account-keyed baseline; arming and successful clear
			// still require the later authenticated identity to match exactly.
			const Policy::RecoveryConsumeResult recovery = state.requiredRecovery.Consume(
				client.steamID64, client.connectionSerial, ServerTime());
			if (recovery == Policy::RecoveryConsumeResult::Native)
			{
				client.requiredRecovery = true;
				client.nativeLane = true;
				client.nativeLatched = true;
				MarkFallback(client);
				Msg(PROJECT_NAME " - luapack: client slot %i (%llu) consumed its one-shot required recovery latch; the initial baseline is wholly native\n",
					slot, static_cast<unsigned long long>(client.steamID64));
				return;
			}
		}
		// Queue clients can reach ServerInfo before Source exposes their ticket identity.
		// That makes account-owned recovery unavailable at this boundary, but it must not
		// reject an otherwise valid required base. A later authenticated failure may bind
		// ownership and arm recovery for the next connection.

		// The old optimistic-recovery latch only belongs to the fail-open lane. Required
		// connections must either resolve their pack or be kicked, regardless of prior history.
		if (!client.requiredLane && !client.networkID.empty())
		{
			auto latch = state.nativeLatches.find(client.networkID);
			if (latch != state.nativeLatches.end())
			{
				if (ServerTime() <= latch->second)
				{
					client.nativeLatched = true;
					Msg(PROJECT_NAME " - luapack: client slot %i (%s) selected fail-open delivery with a native latch from a failed speculative join\n",
						slot, client.networkID.c_str());
				}
				else
				{
					state.nativeLatches.erase(latch);
				}
			}
		}
	}

	static std::string DataDirectory(const std::string& packDirectory);

	static Policy::Lane ClientLane(const ClientPin& client)
	{
		if (client.requiredRecovery)
			return Policy::Lane::NativeRecovery;
		if (client.requiredLane)
			return Policy::Lane::Required;
		return client.optOut ? Policy::Lane::NativeOptOut : Policy::Lane::NativeRescue;
	}

	static Policy::BaseAvailability BaseAvailabilityForClient(const ClientPin& client, bool verifyLocalObject)
	{
		if (client.generation.empty())
			return Policy::BaseAvailability::Missing;

		auto generation = state.generations.find(client.generation);
		if (generation == state.generations.end())
			return Policy::BaseAvailability::Missing;

		const Generation& base = generation->second;
		const std::string expectedPath = DataDirectory(GetConfig().packDirectory) + "/" + base.id + ".bsp";
		if (!base.engineDownloadable || !base.compressedRequiredStub || base.resourcePath != expectedPath ||
			(verifyLocalObject && (!g_pFullFileSystem ||
				!g_pFullFileSystem->FileExists(base.resourcePath.c_str(), "MOD"))))
		{
			return Policy::BaseAvailability::Unusable;
		}

		return Policy::BaseAvailability::Ready;
	}

	static bool IsNativeDelta(const Generation& base, const std::string& virtualPath)
	{
		const std::string path = NormalizePath(virtualPath);
		auto baseFile = base.files.find(path);
		std::string currentIdentity;
		bool hasCurrent = false;
		{
			std::lock_guard<std::mutex> lock(state.registryMutex);
			auto current = state.files.find(path);
			if (current != state.files.end())
			{
				currentIdentity = current->second.identity;
				hasCurrent = true;
			}
		}

		return Policy::IsNativeDelta(baseFile != base.files.end() && hasCurrent,
			baseFile == base.files.end() ? std::string() : baseFile->second, currentIdentity);
	}

	static std::string DataDirectory(const std::string& packDirectory)
	{
		return "data/" + packDirectory;
	}

	static std::string ReadOrCreateSalt(const std::string& packDirectory)
	{
		const std::string directory = DataDirectory(packDirectory);
		const std::string path = directory + "/salt.txt";
		g_pFullFileSystem->CreateDirHierarchy(directory.c_str(), "MOD");

		FileHandle_t input = g_pFullFileSystem->Open(path.c_str(), "rb", "MOD");
		if (input != FILESYSTEM_INVALID_HANDLE)
		{
			const unsigned int size = g_pFullFileSystem->Size(input);
			if (size > 0 && size <= 128)
			{
				std::string salt(size, '\0');
				if (g_pFullFileSystem->Read(salt.data(), size, input) == static_cast<int>(size))
				{
					g_pFullFileSystem->Close(input);
					return salt;
				}
			}
			g_pFullFileSystem->Close(input);
		}

		const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		std::ostringstream entropy;
		entropy << packDirectory << ':' << now << ':' << &state;
		const std::string salt = Bootil::Hasher::MD5::String(entropy.str());

		FileHandle_t output = g_pFullFileSystem->Open(path.c_str(), "wb", "MOD");
		if (output != FILESYSTEM_INVALID_HANDLE)
		{
			g_pFullFileSystem->Write(salt.data(), salt.length(), output);
			g_pFullFileSystem->Close(output);
		}

		return salt;
	}

	static bool WriteImmutableObject(const BuildTask* task, std::string& resourcePath)
	{
		const std::string directory = DataDirectory(task->packDirectory);
		resourcePath = directory + "/" + task->md5 + ".bsp";
		if (g_pFullFileSystem->FileExists(resourcePath.c_str(), "MOD"))
			return true;

		g_pFullFileSystem->CreateDirHierarchy(directory.c_str(), "MOD");
		const std::string temporaryPath = resourcePath + ".tmp." + std::to_string(task->sourceRevision);
		FileHandle_t output = g_pFullFileSystem->Open(temporaryPath.c_str(), "wb", "MOD");
		if (output == FILESYSTEM_INVALID_HANDLE)
			return false;

		const int expected = static_cast<int>(task->compressed.GetWritten());
		const int written = g_pFullFileSystem->Write(task->compressed.GetBase(), expected, output);
		g_pFullFileSystem->Close(output);
		if (written != expected)
		{
			g_pFullFileSystem->RemoveFile(temporaryPath.c_str(), "MOD");
			return false;
		}

		if (g_pFullFileSystem->RenameFile(temporaryPath.c_str(), resourcePath.c_str(), "MOD"))
			return true;

		g_pFullFileSystem->RemoveFile(temporaryPath.c_str(), "MOD");
		return false;
	}

	static void EnsureBuildPool()
	{
		if (state.buildPool)
			return;

		state.buildPool = V_CreateThreadPool();
		Util::StartThreadPool(state.buildPool, 1);
	}

	static void EnsureUploadPool()
	{
		if (state.uploadPool)
			return;

		state.uploadPool = V_CreateThreadPool();
		Util::StartThreadPool(state.uploadPool, 1);
	}

	static void UploadPack(UploadTask*& task)
	{
		if (!task)
			return;

		const size_t scheme = task->url.find("://");
		const size_t pathStart = scheme == std::string::npos ? std::string::npos : task->url.find('/', scheme + 3);
		if (scheme == std::string::npos)
		{
			task->error = "ingest URL has no scheme";
			task->complete.store(true);
			return;
		}

		const std::string origin = pathStart == std::string::npos ? task->url : task->url.substr(0, pathStart);
		const std::string path = pathStart == std::string::npos ? "/" : task->url.substr(pathStart);
		if (origin.compare(0, strlen("http://"), "http://") != 0)
		{
			// cpp-httplib is not linked to OpenSSL in HolyLib. Keeping this explicit avoids silently
			// downgrading an operator-configured HTTPS ingest endpoint.
			task->error = "this build supports http:// ingest only (HTTPS is never downgraded)";
			task->complete.store(true);
			return;
		}

		httplib::Client client(origin);
		client.set_connection_timeout(10, 0);
		client.set_read_timeout(30, 0);
		client.set_write_timeout(30, 0);

		httplib::Request request;
		request.method = task->method;
		request.path = path;
		request.body = task->body;
		request.headers.emplace("Content-Type", "application/octet-stream");
		request.headers.emplace("X-HolyLib-LuaPack-MD5", task->md5);
		request.headers.emplace("X-HolyLib-LuaPack-Path", task->resourcePath);
		request.headers.emplace("X-HolyLib-LuaPack-Retention-Seconds",
			std::to_string(static_cast<unsigned long long>(task->objectRetentionSeconds)));

		auto response = client.send(request);
		if (!response)
		{
			task->error = httplib::to_string(response.error());
			task->complete.store(true);
			return;
		}

		task->status = response->status;
		task->success = response->status >= 200 && response->status < 300;
		if (!task->success)
			task->error = "HTTP status " + std::to_string(response->status);
		task->complete.store(true);
	}

	static void QueueIngest(const BuildTask* build, const Generation& generation)
	{
		const Config& currentConfig = GetConfig();
		if (currentConfig.ingestUrl.empty())
			return;

		std::string method = currentConfig.ingestMethod;
		std::transform(method.begin(), method.end(), method.begin(), [](unsigned char value) {
			return static_cast<char>(std::toupper(value));
		});
		if (method.empty() || method.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
		{
			Warning(PROJECT_NAME " - luapack: Ignoring invalid ingest method\n");
			return;
		}

		UploadTask* upload = new UploadTask;
		upload->url = currentConfig.ingestUrl;
		upload->method = method;
		upload->md5 = generation.md5;
		upload->resourcePath = generation.resourcePath;
		upload->body.assign(static_cast<const char*>(build->compressed.GetBase()), build->compressed.GetWritten());
		upload->objectRetentionSeconds = currentConfig.objectRetentionSeconds;
		EnsureUploadPool();
		state.uploads.push_back(upload);
		state.uploadPool->QueueCall(&UploadPack, upload);
	}

	static void NotifyPackBuilt(const BuildTask* build, const Generation& generation)
	{
		if (g_Lua && Lua::PushHook("HolyLib:OnLuaPackBuilt"))
		{
			g_Lua->PushString(generation.id.c_str());
			g_Lua->PushString(generation.resourcePath.c_str());
			g_Lua->PushString(generation.md5.c_str());
			g_Lua->PushNumber(build->compressed.GetWritten());
			g_Lua->CallFunctionProtected(5, 0, true);
		}
		QueueIngest(build, generation);
	}

	static ConVar* DownloadUrlConVar()
	{
		return g_pCVar ? g_pCVar->FindVar("sv_downloadurl") : nullptr;
	}

	static bool CoordinateDownloadUrl(bool publishing)
	{
		const Config& currentConfig = GetConfig();
		ConVar* downloadUrl = DownloadUrlConVar();
		if (!downloadUrl)
		{
			if (publishing)
				Warning(PROJECT_NAME " - luapack: sv_downloadurl is unavailable; refusing to publish the FastDL map base\n");
			return false;
		}

		if (V_stricmp(currentConfig.downloadUrlPolicy.c_str(), "require") == 0)
		{
			if (downloadUrl->GetString()[0] == '\0')
			{
				if (publishing)
					Warning(PROJECT_NAME " - luapack: sv_downloadurl is empty and policy=require; map base remains unpublished\n");
				return false;
			}
			return true;
		}

		if (V_stricmp(currentConfig.downloadUrlPolicy.c_str(), "lock") == 0)
		{
			if (!state.downloadUrlLocked)
			{
				state.lockedDownloadUrl = downloadUrl->GetString();
				state.downloadUrlLocked = true;
			}
			else if (state.lockedDownloadUrl != downloadUrl->GetString())
			{
				Warning(PROJECT_NAME " - luapack: restoring operator sv_downloadurl while policy=lock\n");
				downloadUrl->SetValue(state.lockedDownloadUrl.c_str());
			}
		}
		else if (V_stricmp(currentConfig.downloadUrlPolicy.c_str(), "respect") != 0)
		{
			if (publishing)
				Warning(PROJECT_NAME " - luapack: unknown download URL policy '%s'; expected respect, require, or lock\n",
					currentConfig.downloadUrlPolicy.c_str());
			return false;
		}

		return true;
	}

	static INetworkStringTable* DownloadablesTable()
	{
		if (!state.stringTables)
			return nullptr;

		if (!state.downloadables)
			state.downloadables = state.stringTables->FindTable("downloadables");
		return state.downloadables;
	}

	static bool IsGenerationObjectName(const std::string& filename)
	{
		if (filename.length() != 36 || filename.compare(32, 4, ".bsp") != 0)
			return false;
		return std::all_of(filename.begin(), filename.begin() + 32, [](unsigned char value) {
			return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
		});
	}

	enum class DownloadableRegistration
	{
		Registered,
		BudgetExhausted,
		Failed,
	};

	static unsigned int RegisteredPackObjectCount(INetworkStringTable* downloadables, const std::string& packDirectory)
	{
		if (!downloadables)
			return 0;

		const std::string prefix = DataDirectory(packDirectory) + "/";
		unsigned int count = 0;
		for (int index = 0; index < downloadables->GetNumStrings(); ++index)
		{
			const char* value = downloadables->GetString(index);
			if (!value)
				continue;

			const std::string resourcePath = value;
			if (resourcePath.compare(0, prefix.length(), prefix) != 0)
				continue;
			if (IsGenerationObjectName(resourcePath.substr(prefix.length())))
				++count;
		}
		return count;
	}

	static DownloadableRegistration RegisterDownloadable(const std::string& resourcePath,
		const std::string& packDirectory, unsigned int limit)
	{
		INetworkStringTable* downloadables = DownloadablesTable();
		if (!downloadables)
		{
			Warning(PROJECT_NAME " - luapack: downloadables string table is not available\n");
			return DownloadableRegistration::Failed;
		}

		int index = downloadables->FindStringIndex(resourcePath.c_str());
		if (index != INVALID_STRING_INDEX)
			return DownloadableRegistration::Registered;

		const unsigned int registered = RegisteredPackObjectCount(downloadables, packDirectory);
		if (registered >= limit)
		{
			Msg(PROJECT_NAME " - luapack: downloadables budget exhausted (%u/%u); map base '%s' cannot be engine-downloadable in this level lifecycle\n",
				registered, limit, resourcePath.c_str());
			return DownloadableRegistration::BudgetExhausted;
		}

		index = downloadables->AddString(true, resourcePath.c_str());

		if (index == INVALID_STRING_INDEX)
		{
			Warning(PROJECT_NAME " - luapack: failed to register '%s' in downloadables\n", resourcePath.c_str());
			return DownloadableRegistration::Failed;
		}

		return DownloadableRegistration::Registered;
	}

	static bool IsProtectedObject(const std::string& resourcePath)
	{
		for (const auto& pair : state.generations)
		{
			if (pair.second.resourcePath == resourcePath)
				return true;
		}

		INetworkStringTable* downloadables = DownloadablesTable();
		return downloadables && downloadables->FindStringIndex(resourcePath.c_str()) != INVALID_STRING_INDEX;
	}

	static void HousekeepObjects()
	{
		const Config& currentConfig = GetConfig();
		if (currentConfig.objectRetentionSeconds <= 0.0 || currentConfig.packDirectory.empty())
			return;

		const std::string directory = DataDirectory(currentConfig.packDirectory);
		const std::string wildcard = directory + "/*.bsp";
		const std::time_t now = std::time(nullptr);
		unsigned int scanned = 0;
		unsigned int protectedObjects = 0;
		unsigned int youngObjects = 0;
		unsigned int removed = 0;
		unsigned int failed = 0;
		unsigned long long removedBytes = 0;

		FileFindHandle_t findHandle;
		const char* found = g_pFullFileSystem->FindFirstEx(wildcard.c_str(), "MOD", &findHandle);
		while (found)
		{
			const std::string filename = found;
			const std::string resourcePath = directory + "/" + filename;
			if (!g_pFullFileSystem->FindIsDirectory(findHandle) && IsGenerationObjectName(filename))
			{
				++scanned;
				if (IsProtectedObject(resourcePath))
				{
					++protectedObjects;
				} else {
					const long fileTime = g_pFullFileSystem->GetFileTime(resourcePath.c_str(), "MOD");
					if (fileTime <= 0 || now < fileTime ||
						static_cast<double>(now - fileTime) < currentConfig.objectRetentionSeconds)
					{
						++youngObjects;
					} else {
						const unsigned int size = g_pFullFileSystem->Size(resourcePath.c_str(), "MOD");
						g_pFullFileSystem->RemoveFile(resourcePath.c_str(), "MOD");
						if (g_pFullFileSystem->FileExists(resourcePath.c_str(), "MOD"))
						{
							++failed;
							Warning(PROJECT_NAME " - luapack: Housekeeping failed to remove '%s'\n", resourcePath.c_str());
						} else {
							++removed;
							removedBytes += size;
						}
					}
				}
			}
			found = g_pFullFileSystem->FindNext(findHandle);
		}
		g_pFullFileSystem->FindClose(findHandle);

		Msg(PROJECT_NAME " - luapack: Housekeeping scanned %u objects, protected %u active, kept %u within TTL, removed %u (%llu bytes), failed %u\n",
			scanned, protectedObjects, youngObjects, removed, removedBytes, failed);
	}

	static void StartBuild()
	{
		// One immutable generation is admitted to Source's level-lifetime
		// downloadables table. Hot refreshes after this point are native deltas.
		if (!state.currentGeneration.empty())
			return;

		BuildTask* task = new BuildTask;
		{
			std::lock_guard<std::mutex> lock(state.registryMutex);
			if (state.files.empty())
			{
				delete task;
				return;
			}

			task->files.reserve(state.files.size());
			for (const auto& pair : state.files)
			{
				// The bootstrap must always arrive as a real file; it cannot resolve itself from the pack.
				if (IsInitFile(pair.first))
					continue;

				task->files.push_back(pair.second);
			}
			task->sourceRevision = state.revision;
			state.buildRequested = false;
		}

		std::sort(task->files.begin(), task->files.end(), [](const FileRecord& left, const FileRecord& right) {
			return left.virtualPath < right.virtualPath;
		});
		task->packDirectory = GetConfig().packDirectory;
		if (state.salt.empty())
			state.salt = ReadOrCreateSalt(task->packDirectory);
		task->salt = state.salt;

		EnsureBuildPool();
		state.activeBuild = task;
		state.buildPool->QueueCall(&BuildPack, task);
	}

	static ConVar luapack_enable(
		"holylib_gmoddatapack_luapack_enable", "0", FCVAR_ARCHIVE,
		"Bundle clientside Lua for FastDL delivery. Disabled by default; required versus fail-open delivery is configured separately");
	static ConVar luapack_packdir(
		"holylib_gmoddatapack_luapack_packdir", "holylib/luapack", FCVAR_ARCHIVE,
		"Directory below garrysmod/data used for immutable Lua pack objects");
	static ConVar luapack_downloadurl_policy(
		"holylib_gmoddatapack_luapack_downloadurl_policy", "respect", FCVAR_ARCHIVE,
		"sv_downloadurl policy: respect, require, or lock");
	static ConVar luapack_ingest_url(
		"holylib_gmoddatapack_luapack_ingest_url", "", FCVAR_ARCHIVE,
		"Optional operator-controlled HTTP ingest URL called after publishing a pack");
	static ConVar luapack_ingest_method(
		"holylib_gmoddatapack_luapack_ingest_method", "PUT", FCVAR_ARCHIVE,
		"HTTP method for the optional pack ingest hook");
	static ConVar luapack_downloadable_limit(
		"holylib_gmoddatapack_luapack_downloadable_limit", "1", FCVAR_ARCHIVE,
		"Maximum immutable pack objects appended to the level-lifetime downloadables table; LuaPack publishes only one map base and sends later changes natively",
		true, 0.0f, true, 64.0f);
	static ConVar luapack_retention_ttl(
		"holylib_gmoddatapack_luapack_retention_ttl", "300", FCVAR_ARCHIVE,
		"Compatibility floor for immutable object retention; map-base mode does not rotate generations during a level",
		true, 30.0f, true, 86400.0f);
	static ConVar luapack_object_retention_ttl(
		"holylib_gmoddatapack_luapack_object_retention_ttl", "604800", FCVAR_ARCHIVE,
		"Seconds before an unreferenced immutable pack object is eligible for local and compatible-ingest housekeeping; 0 disables housekeeping",
		true, 0.0f, true, 31536000.0f);
	static ConVar luapack_ready_deadline(
		"holylib_gmoddatapack_luapack_ready_deadline", "180", FCVAR_ARCHIVE,
		"Seconds a silent spawned client keeps its map base pinned (the clock starts at activation, not connect); a matching late acknowledgement is still accepted afterwards",
		true, 1.0f, true, 3600.0f);
	static ConVar luapack_required(
		"holylib_gmoddatapack_luapack_required", "0", FCVAR_ARCHIVE | FCVAR_REPLICATED,
		"Require connecting clients to use the pinned engine-downloaded Lua pack; failures fail closed or use the bounded authenticated recovery path instead of switching the current join to native Lua");
	static ConVar luapack_allow_optout(
		"holylib_gmoddatapack_luapack_allow_optout", "1", FCVAR_ARCHIVE | FCVAR_REPLICATED,
		"Allow a client whose tv_nochat userinfo is exactly no_gluapack to use native Lua for its entire connection");
	static ConVar luapack_required_recovery(
		"holylib_gmoddatapack_luapack_required_recovery", "1", FCVAR_ARCHIVE,
		"Allow one authenticated engine reconnect whose next ServerInfo baseline is wholly native after a required map-base failure");
	static ConVar luapack_required_recovery_ttl(
		"holylib_gmoddatapack_luapack_required_recovery_ttl", "120", FCVAR_ARCHIVE,
		"Seconds an authenticated one-shot required recovery latch remains valid",
		true, 5.0f, true, 900.0f);
	static ConVar luapack_optimistic(
		"holylib_gmoddatapack_luapack_optimistic", "0", FCVAR_ARCHIVE,
		"Speculatively deliver generation stubs to fail-open joins before the client acknowledges its pack. Ignored by connections on the required lane");
	static ConVar luapack_optimistic_prefix_files(
		"holylib_gmoddatapack_luapack_optimistic_prefix_files", "256", FCVAR_ARCHIVE,
		"Files delivered natively at the start of a join before speculative stubbing may begin",
		true, 0.0f, true, 16384.0f);
	static ConVar luapack_optimistic_prefix_bytes(
		"holylib_gmoddatapack_luapack_optimistic_prefix_bytes", "262144", FCVAR_ARCHIVE,
		"Native Lua source bytes delivered at the start of a join before speculative stubbing may begin",
		true, 0.0f, true, 134217728.0f);
	static ConVar luapack_unready_ttl(
		"holylib_gmoddatapack_luapack_unready_ttl", "900", FCVAR_ARCHIVE,
		"Seconds an account that failed to resolve speculative stubs keeps receiving native files on new connections",
		true, 60.0f, true, 86400.0f);
	// FCVAR_PROTECTED cannot be used here: Source transmits protected replicated cvars as a boolean,
	// which would destroy the manifest snapshot. The client-created mirror adds the non-server flags.
	static ConVar luapack_manifest(
		"holylib_gmoddatapack_luapack_manifest", "", FCVAR_REPLICATED | FCVAR_DONTRECORD | FCVAR_UNLOGGED,
		"Atomic immutable map-base manifest used by the client bootstrap");

	static Config config;

	static std::string HexEncode(const std::string& value)
	{
		static const char digits[] = "0123456789abcdef";
		std::string output;
		output.reserve(value.length() * 2);
		for (unsigned char byte : value)
		{
			output.push_back(digits[(byte >> 4) & 0x0f]);
			output.push_back(digits[byte & 0x0f]);
		}
		return output;
	}

	// The client engine truncates replicated convar values to 255 characters. The snapshot
	// therefore only carries what the bootstrap cannot derive: the map-base id doubles as
	// the content MD5 and FastDL object basename, plus the base directory and salt.
	static const size_t MAXIMUM_MANIFEST_LENGTH = 255;

	static void PublishManifest()
	{
		if (!IsEnabled() || state.currentGeneration.empty())
		{
			luapack_manifest.SetValue("");
			return;
		}

		auto current = state.generations.find(state.currentGeneration);
		if (current == state.generations.end())
		{
			luapack_manifest.SetValue("");
			return;
		}

		const std::string& packDirectory = GetConfig().packDirectory;
		if (current->second.resourcePath != DataDirectory(packDirectory) + "/" + state.currentGeneration + ".bsp")
		{
			// The pack directory changed after the map base was built; a derived client
			// path would point at nothing. Map-base immutability defers replacement.
			Warning(PROJECT_NAME " - luapack: pack directory changed after map base %s was built; refusing publication until level shutdown\n",
				state.currentGeneration.c_str());
			luapack_manifest.SetValue("");
			return;
		}

		std::ostringstream manifest;
		manifest << "1|" << state.currentGeneration << '|' << HexEncode(packDirectory)
			<< '|' << HexEncode(current->second.salt) << '|' << state.currentGeneration;
		const std::string serialized = manifest.str();
		if (serialized.length() > MAXIMUM_MANIFEST_LENGTH)
		{
			Warning(PROJECT_NAME " - luapack: pack directory '%s' pushes the replicated manifest over %u characters; refusing publication\n",
				packDirectory.c_str(), static_cast<unsigned>(MAXIMUM_MANIFEST_LENGTH));
			luapack_manifest.SetValue("");
			return;
		}

		// One SetValue call is the publication barrier: clients never observe a partially-updated generation.
		luapack_manifest.SetValue(serialized.c_str());
	}

	static void RefreshConfig()
	{
		config.enabled = luapack_enable.GetBool();
		config.packDirectory = luapack_packdir.GetString();
		config.downloadUrlPolicy = luapack_downloadurl_policy.GetString();
		config.ingestUrl = luapack_ingest_url.GetString();
		config.ingestMethod = luapack_ingest_method.GetString();
		config.downloadableLimit = static_cast<unsigned int>(luapack_downloadable_limit.GetInt());
		config.generationRetentionSeconds = luapack_retention_ttl.GetFloat();
		config.objectRetentionSeconds = luapack_object_retention_ttl.GetFloat();
		if (config.objectRetentionSeconds > 0.0)
			config.objectRetentionSeconds = std::max(config.objectRetentionSeconds, config.generationRetentionSeconds);
		config.readyDeadlineSeconds = luapack_ready_deadline.GetFloat();
		config.requiredStubbing = luapack_required.GetBool();
		config.allowOptOut = luapack_allow_optout.GetBool();
		config.requiredRecovery = luapack_required_recovery.GetBool();
		config.requiredRecoveryTtlSeconds = luapack_required_recovery_ttl.GetFloat();
		config.optimisticStubbing = luapack_optimistic.GetBool();
		config.optimisticPrefixFiles = static_cast<unsigned int>(luapack_optimistic_prefix_files.GetInt());
		config.optimisticPrefixBytes = static_cast<unsigned long long>(luapack_optimistic_prefix_bytes.GetInt());
		config.unreadyTtlSeconds = luapack_unready_ttl.GetFloat();
	}

	const Config& GetConfig()
	{
		RefreshConfig();
		return config;
	}

	bool IsEnabled()
	{
		return luapack_enable.GetBool();
	}

	void Init(CreateInterfaceFn* appfn)
	{
		if (appfn && appfn[0])
			state.stringTables = static_cast<INetworkStringTableContainer*>(appfn[0](INTERFACENAME_NETWORKSTRINGTABLESERVER, nullptr));
		else
		{
			SourceSDK::FactoryLoader engineLoader("engine");
			state.stringTables = engineLoader.GetInterface<INetworkStringTableContainer>(INTERFACENAME_NETWORKSTRINGTABLESERVER);
		}

		if (!state.stringTables)
			Warning(PROJECT_NAME " - luapack: INetworkStringTableContainer is unavailable; FastDL publishing cannot start\n");
		RefreshConfig();
		state.featureEnabledLastFrame = IsEnabled();
		// InitDetour runs after Init. The first Think observes the installed hook set and
		// reprocesses cached registrations if canonical delivery became available.
		state.canonicalRegistrationAvailableLastFrame = false;
	}

	void Shutdown()
	{
		if (state.buildPool)
		{
			Util::DestroyThreadPool(state.buildPool);
			state.buildPool = nullptr;
		}
		if (state.uploadPool)
		{
			Util::DestroyThreadPool(state.uploadPool);
			state.uploadPool = nullptr;
		}
		delete state.activeBuild;
		state.activeBuild = nullptr;
		for (UploadTask* upload : state.uploads)
			delete upload;
		state.uploads.clear();

		std::lock_guard<std::mutex> lock(state.registryMutex);
		state.files.clear();
		state.buildRequested = false;
		state.generations.clear();
		state.currentGeneration.clear();
		state.salt.clear();
		state.downloadables = nullptr;
		state.lockedDownloadUrl.clear();
		state.downloadUrlLocked = false;
		state.featureEnabledLastFrame = false;
		state.canonicalRegistrationAvailableLastFrame = false;
		state.bootstrapRefresh = false;
		state.lastCaptureAt = 0.0;
		state.nextBuildAllowed = 0.0;
		for (ClientPin& client : state.clients)
			client = ClientPin();
		for (Policy::RequiredRecoveryHandoff& handoff : state.recoveryHandoffs)
			handoff.Reset();
		state.nativeLatches.clear();
		if (state.requiredRecovery.Size() != 0)
			Msg(PROJECT_NAME " - luapack: clearing %llu required recovery entries at module shutdown\n",
				static_cast<unsigned long long>(state.requiredRecovery.Size()));
		state.requiredRecovery.Reset();
		state.nextConnectionSerial = 0;
		luapack_manifest.SetValue("");
	}

	void LevelShutdown()
	{
		Shutdown();
	}

	void Think()
	{
		const bool enabled = IsEnabled();
		const bool canonicalRegistrationAvailable =
			Policy::UsesCanonicalRegistration(enabled, SupportsCanonicalRegistration());
		if (enabled != state.featureEnabledLastFrame)
		{
			state.featureEnabledLastFrame = enabled;
			state.bootstrapRefresh = true;
			if (enabled)
			{
				std::lock_guard<std::mutex> lock(state.registryMutex);
				state.buildRequested = true;
			} else {
				// The disabled state must not retain a second copy of all registered Lua. Clearing
				// also prevents a runtime re-enable from publishing a stale pre-refresh snapshot.
				std::lock_guard<std::mutex> lock(state.registryMutex);
				state.files.clear();
				state.buildRequested = false;
			}
		}
		if (canonicalRegistrationAvailable != state.canonicalRegistrationAvailableLastFrame)
		{
			state.canonicalRegistrationAvailableLastFrame = canonicalRegistrationAvailable;
			// A hook-set change alters the byte identity stored in every non-init entry,
			// just like the master switch. The module refresh loop re-feeds all registrations.
			state.bootstrapRefresh = true;
		}

		for (auto upload = state.uploads.begin(); upload != state.uploads.end();)
		{
			UploadTask* task = *upload;
			if (!task->complete.load())
			{
				++upload;
				continue;
			}

			if (task->success)
				Msg(PROJECT_NAME " - luapack: Optional ingest accepted %s (HTTP %i)\n", task->md5.c_str(), task->status);
			else
				Warning(PROJECT_NAME " - luapack: Optional ingest failed for %s: %s (pack remains published locally)\n",
					task->md5.c_str(), task->error.c_str());
			delete task;
			upload = state.uploads.erase(upload);
		}

		if (state.activeBuild && state.activeBuild->complete.load())
		{
			BuildTask* task = state.activeBuild;
			state.activeBuild = nullptr;
			auto retryBaseBuild = [](double delay) {
				state.nextBuildAllowed = ServerTime() + delay;
				std::lock_guard<std::mutex> lock(state.registryMutex);
				state.buildRequested = true;
			};

			state.nextBuildAllowed = ServerTime() + 5.0;
			if (!task->success)
			{
				Warning(PROJECT_NAME " - luapack: Failed to build map-base pack: %s\n", task->error.c_str());
				if (IsEnabled() && state.currentGeneration.empty())
					retryBaseBuild(15.0);
			} else if (IsEnabled()) {
				if (!state.currentGeneration.empty())
				{
					Warning(PROJECT_NAME " - luapack: Discarding a completed replacement build because map base %s is immutable until level shutdown\n",
						state.currentGeneration.c_str());
				} else {
					std::string resourcePath;
					if (!WriteImmutableObject(task, resourcePath))
					{
						Warning(PROJECT_NAME " - luapack: Failed to atomically write map-base pack %s\n", task->md5.c_str());
						retryBaseBuild(15.0);
					} else if (!CoordinateDownloadUrl(true)) {
						Warning(PROJECT_NAME " - luapack: Map base %s exists but was not published; native lanes continue and required joins are rejected\n", task->md5.c_str());
						retryBaseBuild(15.0);
					} else {
						const Config& currentConfig = GetConfig();
						const DownloadableRegistration registration = RegisterDownloadable(resourcePath,
							currentConfig.packDirectory, currentConfig.downloadableLimit);
						if (registration == DownloadableRegistration::Failed)
						{
							Warning(PROJECT_NAME " - luapack: Map base %s could not enter the engine download queue; native lanes continue and required joins are rejected\n", task->md5.c_str());
							retryBaseBuild(15.0);
						}
						else if (registration == DownloadableRegistration::BudgetExhausted)
						{
							Warning(PROJECT_NAME " - luapack: No engine-downloadable slot remains for map base %s; refusing an HTTP-only base until level shutdown\n", task->md5.c_str());
							std::lock_guard<std::mutex> lock(state.registryMutex);
							state.buildRequested = false;
						}
						else
						{
							Generation generation;
							generation.id = task->md5;
							generation.md5 = task->md5;
							generation.salt = task->salt;
							generation.resourcePath = resourcePath;
							generation.engineDownloadable = true;
							generation.sourceRevision = task->sourceRevision;
							generation.publishedAt = ServerTime();
							generation.compressedStub = BuildCompressedStub();
							generation.compressedRequiredStub = generation.compressedStub;
							for (const FileRecord& file : task->files)
								generation.files[NormalizePath(file.virtualPath)] = file.identity;
							if (!generation.compressedStub || !generation.compressedRequiredStub)
							{
								Warning(PROJECT_NAME " - luapack: Failed to build the canonical map-base stub; required joins remain rejected\n");
								retryBaseBuild(15.0);
							}
							else
							{
								state.generations[generation.id] = generation;
								state.currentGeneration = generation.id;
								{
									std::lock_guard<std::mutex> lock(state.registryMutex);
									// Captures newer than the build snapshot stay in state.files
									// and are selected as native deltas.
									state.buildRequested = false;
								}
								PublishManifest();
								NotifyPackBuilt(task, generation);
								HousekeepObjects();
								Msg(PROJECT_NAME " - luapack: Published immutable map base %s (%u compressed bytes, %u files); subsequent changes use per-client native deltas\n",
									generation.id.c_str(), task->compressed.GetWritten(), static_cast<unsigned int>(task->files.size()));
							}
						}
					}
				}
			}

			delete task;
		}

		if (!IsEnabled())
		{
			state.downloadUrlLocked = false;
			for (ClientPin& client : state.clients)
			{
				if (!client.generation.empty())
					ReleasePin(client);
			}
			for (Policy::RequiredRecoveryHandoff& handoff : state.recoveryHandoffs)
				handoff.Reset();
			// Disabling the feature forgets speculation-failure history; nothing consults it
			// while all delivery is native anyway.
			state.nativeLatches.clear();
			if (state.requiredRecovery.Size() != 0)
				Msg(PROJECT_NAME " - luapack: clearing %llu required recovery entries because LuaPack was disabled\n",
					static_cast<unsigned long long>(state.requiredRecovery.Size()));
			state.requiredRecovery.Reset();
			if (luapack_manifest.GetString()[0] != '\0')
				luapack_manifest.SetValue("");
			return;
		}

		CoordinateDownloadUrl(false);
		const double now = ServerTime();
		ProcessRequiredRecoveryHandoffs(now);
		for (int slot = 0; slot < ABSOLUTE_PLAYER_LIMIT; ++slot)
		{
			ClientPin& client = state.clients[slot];
			// Steam authentication commonly completes after SendServerInfo. If the client
			// reached full entry first, retain the consumed tombstone until
			// the same connection's authenticated SteamID64 can be revalidated here.
			ClearProvenRecoveryState(slot, client);
			// The deadline only runs for spawned clients. A connecting client can legitimately
			// spend many minutes in map load + the Requesting-Lua burst before its Lua state even
			// exists; expiring the pin there would mark exactly the joins that matter fallback
			// before their first file request arrives. ClientActive re-arms the deadline.
			if (!client.generation.empty() && !client.ready && !client.fallback && client.active &&
				now > client.deadline)
			{
				if (client.requiredLane && !client.everReady)
				{
					DisconnectRequiredClient(slot, "the client did not acknowledge its required map base before the READY deadline");
					continue;
				}
				MarkFallback(client);
			}
		}

		for (auto latch = state.nativeLatches.begin(); latch != state.nativeLatches.end();)
		{
			if (now > latch->second)
				latch = state.nativeLatches.erase(latch);
			else
				++latch;
		}
		state.requiredRecovery.Prune(now);

		if (luapack_manifest.GetString()[0] == '\0' && !state.currentGeneration.empty())
			PublishManifest();
		if (state.activeBuild)
			return;

		bool shouldBuild = false;
		{
			std::lock_guard<std::mutex> lock(state.registryMutex);
			// The quiesce window captures the level's initial registration burst as one
			// map base; nextBuildAllowed backs off failed initial publication retries.
			shouldBuild = Policy::ShouldBuildMapBase(!state.currentGeneration.empty(), state.buildRequested) &&
				now >= state.nextBuildAllowed &&
				now - state.lastCaptureAt >= 2.0;
		}
		if (shouldBuild)
			StartBuild();
	}
	void LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit)
	{
		// bServerInit == false fires from InitLuaClasses, before includes/init.lua has run —
		// the pure-Lua concommand/hook libraries the bridge needs do not exist yet. The
		// bServerInit == true pass happens at ServerActivate, after the gamemode loaded.
		if (bServerInit && pLua == g_Lua)
			pLua->RunString("HolyLib luapack server bridge", "", serverBridge, true, true);
	}

	void CaptureFile(const GarrysMod::Lua::LuaFile* file)
	{
		if (!file)
			return;
		CaptureFileContents(file->name, file->contents);
	}

	void CaptureFileContents(const std::string& inputPath, const std::string& contents)
	{
		if (!IsEnabled())
			return;

		const std::string virtualPath = NormalizePath(inputPath);
		if (virtualPath.empty())
			return;
		const std::string identity = ContentIdentity(contents);
		const bool needsMapBase = state.currentGeneration.empty();

		{
			std::lock_guard<std::mutex> lock(state.registryMutex);
			FileRecord& record = state.files[virtualPath];
			// LuaFile::source is registration provenance (often the AddCSLuaFile caller),
			// so hundreds of unrelated entries can share it. The client executes the
			// registered string-table path; use that unique virtual path for exact lookup.
			const std::string sourcePath = virtualPath;
			if (record.contents == contents && record.sourcePath == sourcePath)
				return;

			record.virtualPath = virtualPath;
			record.sourcePath = sourcePath;
			record.contents = contents;
			record.identity = identity;
			record.revision = ++state.revision;
			if (needsMapBase)
			{
				state.buildRequested = true;
			}
			state.lastCaptureAt = ServerTime(); // quiesce window: batch deploys build once, not per file
		}
		// Once the immutable map base exists, no replacement generation is queued.
		// Selection compares this current identity with the base identity: changed or
		// late paths go native, and an exact restoration becomes canonical again.
	}

	std::string PrepareVanillaFile(const std::string& virtualPath, const std::string& contents)
	{
		if (!IsEnabled() || !SupportsCanonicalRegistration())
			return contents;

		if (IsInitFile(virtualPath))
			return std::string(clientBootstrap) + "\n" + contents;

		return canonicalStubSource;
	}

	bool ConsumeBootstrapRefresh()
	{
		const bool refresh = state.bootstrapRefresh;
		state.bootstrapRefresh = false;
		return refresh;
	}

	BaselineDecision DecideBaselineForClient(int slot)
	{
		if (!IsEnabled() || !IsValidSlot(slot))
			return BaselineDecision();
		if (const char* recoveryFailure = BeginPendingRecoveryBaseline(slot))
			return {BaselineAction::Reject, recoveryFailure};

		if (Policy::NeedsConnectionEpochAtBaseline(
			state.clients[slot].connectionSerial))
			StartClientEpoch(slot);
		ClientPin& client = state.clients[slot];
		ResolveDeliveryLane(slot, client);
		if (Policy::ShouldPinCurrentBaseForBaseline(ClientLane(client),
			!client.generation.empty(), !state.currentGeneration.empty()))
			PinCurrentGeneration(client);
		const Policy::BaseAvailability base = BaseAvailabilityForClient(client, true);
		switch (Policy::SelectBaseline(ClientLane(client), SupportsCanonicalRegistration(), base))
		{
			case Policy::Action::Native:
				return {BaselineAction::NativeSource, nullptr};
			case Policy::Action::CanonicalStub:
				return {BaselineAction::BasePlusDelta, nullptr};
			case Policy::Action::Reject:
				if (!SupportsCanonicalRegistration())
					return {BaselineAction::Reject, "canonical Lua placeholder registration is unavailable on this platform"};
				if (base == Policy::BaseAvailability::Missing)
					return {BaselineAction::Reject, "no immutable map base was pinned before the client string-table baseline"};
				return {BaselineAction::Reject, "the pinned map base is absent, invalid, or not in the engine download queue"};
		}

		return {BaselineAction::Reject, "invalid map-base baseline policy result"};
	}

	BaselineDecision DecideFileBaselineForClient(int slot, const std::string& virtualPath)
	{
		if (!IsEnabled() || !IsValidSlot(slot))
			return BaselineDecision();

		ClientPin& client = state.clients[slot];
		ResolveDeliveryLane(slot, client);
		const Policy::BaseAvailability base = BaseAvailabilityForClient(client, false);
		bool nativeDelta = true;
		if (base == Policy::BaseAvailability::Ready)
		{
			auto generation = state.generations.find(client.generation);
			if (generation != state.generations.end())
				nativeDelta = IsNativeDelta(generation->second, virtualPath);
		}

		switch (Policy::SelectFile(ClientLane(client), SupportsCanonicalRegistration(),
			base, IsInitFile(virtualPath), nativeDelta))
		{
			case Policy::Action::Native:
				return {BaselineAction::NativeSource, nullptr};
			case Policy::Action::CanonicalStub:
				return {BaselineAction::CanonicalStub, nullptr};
			case Policy::Action::Reject:
				return {BaselineAction::Reject, base == Policy::BaseAvailability::Missing
					? "the immutable map base is not pinned" : "the immutable map base is unusable"};
		}

		return {BaselineAction::Reject, "invalid per-file map-base policy result"};
	}

	bool NeedsNativeHashUpdate(int slot)
	{
		if (!IsEnabled() || !IsValidSlot(slot))
			return false;
		// If the hook set disappeared since the previous Think, registrations are still
		// canonical until the queued all-file refresh runs. Keep repairing native bodies
		// during that bounded transition; a fresh unsupported start has neither flag.
		if (!SupportsCanonicalRegistration() &&
			!state.canonicalRegistrationAvailableLastFrame)
			return false;

		ClientPin& client = state.clients[slot];
		ResolveDeliveryLane(slot, client);
		// Global registration is canonical when the per-client baseline hook is active.
		// Every native body therefore receives its current per-client hash, both for JIP
		// deltas and for active-client hot refreshes on wholly native lanes.
		return Policy::NeedsPerClientNativeHashes(ClientLane(client));
	}

	DeliveryDecision DecideDeliveryForClient(int slot, const std::string& virtualPath, size_t nativeSourceBytes)
	{
		DeliveryDecision decision;
		if (!IsEnabled() || !IsValidSlot(slot))
			return decision;

		ClientPin& client = state.clients[slot];
		ResolveDeliveryLane(slot, client);
		const BaselineDecision fileBaseline = DecideFileBaselineForClient(slot, virtualPath);
		if (fileBaseline.action == BaselineAction::Reject)
			return {DeliveryAction::Reject, nullptr, fileBaseline.failure};
		if (fileBaseline.action == BaselineAction::NativeSource || fileBaseline.action == BaselineAction::Unchanged)
		{
			RecordNativeJoin(client, nativeSourceBytes);
			return decision;
		}

		auto generation = state.generations.find(client.generation);
		if (generation == state.generations.end() || !generation->second.compressedRequiredStub)
			return {DeliveryAction::Reject, nullptr, "the immutable map base became unavailable during Lua transfer"};
		if (!client.active)
		{
			++client.joinRequiredStubs;
			if (client.joinRequiredStubs == 1)
				Msg(PROJECT_NAME " - luapack: client slot %i is using required map base %s with per-path native deltas; stubbing unchanged files without waiting for READY\n",
					slot, client.generation.c_str());
		}
		return {DeliveryAction::Stub, generation->second.compressedRequiredStub, nullptr};
	}

	void DisconnectRequiredClient(int slot, const char* failure)
	{
		if (!IsValidSlot(slot))
			return;

		ClientPin& client = state.clients[slot];
		if (client.disconnectIssued)
			return;
		client.disconnectIssued = true;

		Warning(PROJECT_NAME " - luapack: disconnecting required-pack client slot %i: %s\n",
			slot, failure ? failure : "unspecified required-pack failure");
		CBaseClient* baseClient = Util::server ? Util::GetClientByIndex(slot) : nullptr;
		if (baseClient)
		{
			baseClient->Disconnect("%s",
				"Required LuaPack failed. Automatic native recovery was unavailable or exhausted. Manual opt out: set launch option +tv_nochat no_gluapack, restart Garry's Mod, then reconnect.");
		}
	}

	bool ClientConnect(int slot)
	{
		if (!IsValidSlot(slot))
			return false;

		if (PreserveClaimedRecoveryLifecycle(slot, true))
		{
			const ClientPin& client = state.clients[slot];
			Msg(PROJECT_NAME " - luapack: preserving claimed native recovery epoch %llu for slot %i (%llu) across its late game-layer ClientConnect callback\n",
				static_cast<unsigned long long>(client.connectionSerial), slot,
				static_cast<unsigned long long>(client.steamID64));
			return true;
		}

		// A real network connection supersedes a merely queued request. Once the engine
		// reconnect was invoked, preserve its gate across Source's disconnect/connect
		// callback ordering; authenticated ServerInfo ownership decides whether to consume it.
		if (!state.recoveryHandoffs[slot].Invoked())
			state.recoveryHandoffs[slot].Reset();
		StartClientEpoch(slot);
		return false;
	}

	void ClientActive(int slot)
	{
		if (!IsValidSlot(slot))
			return;

		ClientPin& client = state.clients[slot];
		client.active = true;
		ReleaseGenerationReference(client);

		// The acknowledgement window starts now, not at connect: queued client commands only
		// flush post-signon, so a pin must survive the whole download phase to be ackable at all.
		if (!client.ready && !client.fallback && !client.generation.empty())
			client.deadline = ServerTime() + GetConfig().readyDeadlineSeconds;

		ClearProvenRecoveryState(slot, client);

		if (IsEnabled() && !client.joinSummaryLogged &&
			(client.joinNativeFiles > 0 || client.joinOptimisticStubs > 0 ||
				client.joinRequiredStubs > 0 || client.joinReadyStubs > 0))
		{
			client.joinSummaryLogged = true;
			// The acknowledgement usually arrives after the client spawned (queued client
			// commands only flush post-signon), so ready=no here is normal for stub joins.
			Msg(PROJECT_NAME " - luapack: join summary slot %i (%s): %u native files (%llu source bytes), %u required stubs, %u speculative stubs, %u acknowledged stubs, lane=%s generation=%s ready=%s latched=%s fallback=%s\n",
				slot,
				client.networkID.empty() ? "?" : client.networkID.c_str(),
				client.joinNativeFiles, client.joinNativeBytes,
				client.joinRequiredStubs, client.joinOptimisticStubs, client.joinReadyStubs,
				client.requiredRecovery ? "recovery-native" :
					(client.optOut ? "optout-native" : (client.requiredLane ? "required" : "fail-open")),
				client.generation.empty() ? "-" : client.generation.c_str(),
				client.ready ? "yes" : "no",
				client.nativeLatched ? "yes" : "no",
				client.fallback ? "yes" : "no");
		}

		// The required map base is admitted only through Source's pre-spawn resource
		// phase. A missing/corrupt base must fail closed in the bootstrap; it is never
		// repaired after spawn by switching the whole join to native or HTTP delivery.
	}

	bool ClientDisconnect(int slot, bool gameLayerCallback)
	{
		if (!IsValidSlot(slot))
			return false;

		if (PreserveClaimedRecoveryLifecycle(slot, gameLayerCallback))
		{
			const ClientPin& recovery = state.clients[slot];
			Msg(PROJECT_NAME " - luapack: preserving claimed native recovery epoch %llu for slot %i (%llu) across its late game-layer ClientDisconnect callback\n",
				static_cast<unsigned long long>(recovery.connectionSerial), slot,
				static_cast<unsigned long long>(recovery.steamID64));
			return true;
		}

		ClientPin& client = state.clients[slot];
		// A late game-layer disconnect can precede the replacement ServerInfo. Keep an
		// invoked handoff for that exact authenticated boundary, but discard a queued
		// request or any handoff at the lower-level physical disconnect boundary.
		if (!Policy::ShouldKeepInvokedRecoveryHandoff(gameLayerCallback,
			state.recoveryHandoffs[slot].Invoked()))
			state.recoveryHandoffs[slot].Reset();
		// A speculated join that never acknowledged its generation is treated as failed even
		// without the client's explicit recovery command: its channel may have died before
		// that command flushed. Latching here is what makes every failure path converge —
		// the next attempt from this account is native no matter how this connection ended.
		if (client.joinOptimisticStubs > 0 && !client.ready && !client.networkID.empty())
		{
			state.nativeLatches[client.networkID] = ServerTime() + GetConfig().unreadyTtlSeconds;
			Msg(PROJECT_NAME " - luapack: client slot %i (%s) disconnected with %u unacknowledged speculative stubs; its next join is latched native\n",
				slot, client.networkID.c_str(), client.joinOptimisticStubs);
		}
		ReleasePin(client);
		return false;
	}

	void PhysicalClientDisconnect(int slot, std::uint64_t steamID64)
	{
		if (!IsValidSlot(slot))
			return;

		unsigned int clearedHandoffs = 0;
		if (steamID64 != 0)
		{
			for (Policy::RequiredRecoveryHandoff& handoff : state.recoveryHandoffs)
			{
				if (handoff.ResetIfOwnedBy(steamID64))
					++clearedHandoffs;
			}
		}
		if (clearedHandoffs > 0)
		{
			Msg(PROJECT_NAME " - luapack: physical Steam disconnect for slot %i (%llu) cleared %u required recovery handoff(s)\n",
				slot, static_cast<unsigned long long>(steamID64), clearedHandoffs);
		}

		// Unlike late game-layer callbacks around CGameClient::Reconnect, the Steam
		// disconnect must discard any unidentified per-slot handoff so a reused slot
		// cannot inherit the reconnect gate. The account tracker deliberately survives:
		// an Armed latch remains available to the next physical connection, while a
		// Consumed tombstone must remain until proven success or server reset.
		ClientDisconnect(slot, false);
	}

	MODULE_RESULT ClientCommand(int slot, const CCommand* args)
	{
		if (!args || args->ArgC() < 1)
			return MODULE_RESULT::CONTINUE;

		if (V_stricmp(args->Arg(0), "holylib_luapack_failed") == 0)
		{
			// Always consume the private command. Only an exact failure for this connection's
			// pinned required generation may arm recovery; malformed/stale commands still fail
			// closed and can only disconnect the sender.
			if (!IsEnabled() || !IsValidSlot(slot))
				return MODULE_RESULT::STOP;

			ClientPin& client = state.clients[slot];
			const bool ownsConsumedRecovery = state.requiredRecovery.OwnsConsumed(
				client.steamID64, client.connectionSerial);
			if (Policy::ShouldIgnoreLateRecoveryFailure(
				client.requiredRecovery && client.nativeLane, ownsConsumedRecovery,
				client.recoveryStateCleared))
			{
				Warning(PROJECT_NAME " - luapack: ignored a late required-generation failure from the failed baseline during native recovery epoch %llu for slot %i (%llu)\n",
					static_cast<unsigned long long>(client.connectionSerial), slot,
					static_cast<unsigned long long>(client.steamID64));
				return MODULE_RESULT::STOP;
			}
			const bool exactFailure = client.requiredLane && !client.requiredRecovery &&
				args->ArgC() == 2 && !client.generation.empty() &&
				client.generation == args->Arg(1);
			bool handoffQueued = false;
			if (exactFailure && GetConfig().requiredRecovery)
			{
				// Some queue clients had no ticket identity at their initial ServerInfo. First
				// authenticated binding is safe for this connection epoch; an identity that was
				// already bound must still match exactly to prevent slot-reuse ownership leaks.
				const bool previouslyResolved = client.resolvedIdentity;
				const std::uint64_t expectedSteamID64 = client.steamID64;
				const bool authenticatedNow = BindAuthenticatedIdentity(slot, client);
				if (Policy::RequiredFailureIdentityReady(true, client.resolvedIdentity,
					client.authenticatedIdentity) && authenticatedNow &&
					Policy::RequiredFailureIdentityMatches(previouslyResolved,
						expectedSteamID64, client.steamID64))
				{
					const double ttl = GetConfig().requiredRecoveryTtlSeconds;
					const Policy::RecoveryArmResult armed = state.requiredRecovery.Arm(
						client.steamID64, client.connectionSerial, ServerTime(), ttl);
					if (armed == Policy::RecoveryArmResult::Armed)
					{
						const double handoffWindow = Policy::RecoveryHandoffWindow(ttl);
						handoffQueued = state.recoveryHandoffs[slot].Queue(
							client.steamID64, client.connectionSerial, ServerTime(), handoffWindow);
						if (handoffQueued)
						{
							Msg(PROJECT_NAME " - luapack: authenticated required failure for slot %i (%llu); one wholly native next connection armed for %.0f seconds and one engine reconnect queued with a %.0f-second ServerInfo window\n",
								slot, static_cast<unsigned long long>(client.steamID64), ttl,
								handoffWindow);
						}
						else
						{
							state.requiredRecovery.Clear(client.steamID64);
							Warning(PROJECT_NAME " - luapack: required recovery handoff could not be queued for slot %i (%llu); latch removed and join remains fail-closed\n",
								slot, static_cast<unsigned long long>(client.steamID64));
						}
					}
					else
					{
						Warning(PROJECT_NAME " - luapack: required recovery was not re-armed for slot %i (%llu); its one-shot state is already pending or consumed\n",
							slot, static_cast<unsigned long long>(client.steamID64));
					}
				}
				else
				{
					Warning(PROJECT_NAME " - luapack: required recovery was not armed for slot %i because authenticated SteamID64 ownership could not be revalidated\n",
						slot);
				}
			}
			if (client.requiredLane && !handoffQueued)
				DisconnectRequiredClient(slot, exactFailure
					? "the authenticated client reported that its required generation could not resolve a stub"
					: "the client reported a stale or malformed required-generation failure");
			return MODULE_RESULT::STOP;
		}

		if (V_stricmp(args->Arg(0), "holylib_luapack_unready") == 0)
		{
			// Always consume our private recovery command. A client sends it right before it
			// reconnects because a speculative stub could not be resolved into pack content;
			// forging it only buys the forger vanilla file delivery.
			if (!IsEnabled() || !IsValidSlot(slot))
				return MODULE_RESULT::STOP;

			ClientPin& client = state.clients[slot];
			const bool ownsConsumedRecovery = state.requiredRecovery.OwnsConsumed(
				client.steamID64, client.connectionSerial);
			if (Policy::ShouldIgnoreLateRecoveryFailure(
				client.requiredRecovery && client.nativeLane, ownsConsumedRecovery,
				client.recoveryStateCleared))
			{
				Warning(PROJECT_NAME " - luapack: ignored a late legacy unready command from the failed baseline during native recovery epoch %llu for slot %i (%llu)\n",
					static_cast<unsigned long long>(client.connectionSerial), slot,
					static_cast<unsigned long long>(client.steamID64));
				return MODULE_RESULT::STOP;
			}
			if (client.requiredLane)
			{
				DisconnectRequiredClient(slot, "the client reported an unresolvable required stub through the legacy recovery command");
				return MODULE_RESULT::STOP;
			}
			MarkFallback(client); // whatever remains of this connection goes native immediately
			client.nativeLatched = true;
			if (!client.networkID.empty())
			{
				const double ttl = GetConfig().unreadyTtlSeconds;
				state.nativeLatches[client.networkID] = ServerTime() + ttl;
				Msg(PROJECT_NAME " - luapack: client slot %i (%s) reported an unresolvable stub; native delivery latched for %.0f seconds\n",
					slot, client.networkID.c_str(), ttl);
			}
			return MODULE_RESULT::STOP;
		}

		if (V_stricmp(args->Arg(0), "holylib_luapack_ready") != 0)
			return MODULE_RESULT::CONTINUE;

		// Always consume our private acknowledgement command, including forged or stale generations.
		if (!IsEnabled() || !IsValidSlot(slot) || args->ArgC() != 3)
			return MODULE_RESULT::STOP;

		ClientPin& client = state.clients[slot];
		if (client.nativeLane)
			return MODULE_RESULT::STOP;
		const std::string generationId = args->Arg(1);
		const std::string md5 = args->Arg(2);
		auto generation = state.generations.find(client.generation);
		// A matching acknowledgement is definitive evidence that the client mounted this exact
		// immutable object, so it is accepted even after the deadline released the pin (heavy
		// first joins routinely outlive any fixed window). The deadline only bounds how long a
		// silent slot keeps a superseded generation pinned in memory.
		if (!client.generation.empty() && generation != state.generations.end() &&
			generationId == client.generation && md5 == generation->second.md5)
		{
			client.ready = true;
			client.everReady = true;
			client.fallback = false;
			// Proof of a mounted pack also heals the account's native latch.
			client.nativeLatched = false;
			if (!client.networkID.empty())
				state.nativeLatches.erase(client.networkID);
			if (!client.active && !client.holdsPin)
			{
				client.holdsPin = true;
				++generation->second.pins;
			}
			if (client.active)
			{
				ReleaseGenerationReference(client);
				ClearProvenRecoveryState(slot, client);
			}
			Msg(PROJECT_NAME " - luapack: client slot %i acknowledged required map base %s\n", slot, generationId.c_str());
		}

		return MODULE_RESULT::STOP;
	}
}
