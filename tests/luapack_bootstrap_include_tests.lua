local function fail(message)
	error(message, 2)
end

local function equal(actual, expected, message)
	if actual ~= expected then
		fail((message or "values differ") .. ": expected " .. tostring(expected) ..
			", got " .. tostring(actual))
	end
end

local sourceFile = assert(io.open("source/modules/gmoddatapack_luapack.cpp", "rb"))
local source = sourceFile:read("*a")
sourceFile:close()
local fragments = {}
for fragment in source:gmatch('R"HOLYLUAPACK%((.-)%)HOLYLUAPACK"') do
	fragments[#fragments + 1] = fragment
end
equal(#fragments, 2, "embedded bootstrap fragment count")
local bootstrap = table.concat(fragments)

local generation = string.rep("a", 32)
local salt = "s"
local snapshot = "1|" .. generation .. "|74657374|73|" .. generation
local allCommands = {}

local function normalize(path)
	path = (path or ""):gsub("^@", ""):gsub("\\", "/")
	return path
end

local function newRuntime(nativeFiles)
	nativeFiles = nativeFiles or {}
	local runtime = {
		commands = {},
		compileCalls = {},
		nativeCompileCalls = {},
		nativeIncludeCalls = {},
		pathStack = {},
	}
	local environment = {}
	for key, value in pairs(_G) do environment[key] = value end
	environment._G = environment
	runtime.environment = environment

	local convars = {}
	local function convar(value)
		return {
			GetString = function() return value end,
			GetBool = function() return value ~= "" and value ~= "0" end,
			SetString = function(_, nextValue) value = nextValue end,
		}
	end
	convars.holylib_gmoddatapack_luapack_manifest = convar(snapshot)
	convars.holylib_gmoddatapack_luapack_required = convar("1")
	convars.holylib_gmoddatapack_luapack_allow_optout = convar("1")
	convars.tv_nochat = convar("")
	convars.cl_downloadfilter = convar("none")
	runtime.convars = convars

	environment.FCVAR_REPLICATED = 0
	environment.FCVAR_PROTECTED = 0
	environment.FCVAR_DONTRECORD = 0
	environment.FCVAR_UNLOGGED = 0
	environment.FCVAR_UNREGISTERED = 0
	environment.Msg = function() end
	environment.CreateConVar = function(name, default)
		if not convars[name] then convars[name] = convar(default) end
		return convars[name]
	end
	environment.GetConVar_Internal = function(name) return convars[name] end
	environment.GetConVar = environment.GetConVar_Internal
	environment.RunConsoleCommand = function(...)
		local command = {...}
		runtime.commands[#runtime.commands + 1] = command
		allCommands[#allCommands + 1] = command
	end
	environment.file = {Open = function() return nil end}

	local hashes = {}
	local hashIndex = 0
	local function fakeMD5(value)
		local hash = hashes[value]
		if hash then return hash end
		hashIndex = hashIndex + 1
		hash = string.format("%032x", hashIndex)
		hashes[value] = hash
		return hash
	end
	environment.util = {
		MD5 = fakeMD5,
		Decompress = function(value) return value end,
	}
	environment.CompileString = function(code, identifier, handleError)
		runtime.compileCalls[#runtime.compileCalls + 1] = {
			identifier = identifier,
			handleError = handleError,
		}
		local chunk, compileError = loadstring(code,
			string.sub(identifier, 1, 1) == "@" and identifier or ("@" .. normalize(identifier)))
		if not chunk then
			return handleError == false and compileError or nil
		end
		setfenv(chunk, environment)
		return chunk
	end

	local function originalCompileFile(path, showError)
		runtime.nativeCompileCalls[#runtime.nativeCompileCalls + 1] = {
			path = path,
			showError = showError,
		}
		local code = nativeFiles[normalize(path)]
		if not code then return nil end
		return environment.CompileString(code, normalize(path), showError)
	end

	local function originalInclude(path)
		local info = debug.getinfo(2, "S")
		local caller = runtime.pathStack[#runtime.pathStack] or
			normalize(info and info.source or "")
		local normalized = normalize(path)
		local directory = caller:match("^(.*)/[^/]*$")
		local relative = directory and (directory .. "/" .. normalized) or nil
		local selected = relative and nativeFiles[relative] and relative or
			(nativeFiles[normalized] and normalized or nil)
		runtime.nativeIncludeCalls[#runtime.nativeIncludeCalls + 1] = {
			path = path,
			caller = caller,
			selected = selected,
		}
		if not selected then error("native include missing: " .. tostring(path) ..
			" (caller " .. tostring(caller) .. ", relative " .. tostring(relative) .. ")", 2) end
		local chunk = assert(environment.CompileString(nativeFiles[selected], selected, true))
		runtime.pathStack[#runtime.pathStack + 1] = selected
		local results = {pcall(chunk)}
		runtime.pathStack[#runtime.pathStack] = nil
		if not results[1] then error(results[2], 0) end
		return unpack(results, 2)
	end
	environment.CompileFile = originalCompileFile
	environment.include = originalInclude
	runtime.originalCompileFile = originalCompileFile
	runtime.originalInclude = originalInclude

	local chunk, compileError = loadstring(bootstrap, "@HolyLib luapack bootstrap")
	if not chunk then fail("embedded bootstrap did not compile: " .. tostring(compileError)) end
	setfenv(chunk, environment)
	chunk()
	if type(environment.__holypack) ~= "function" or
		type(environment.__holypack_packs) ~= "table" then
		fail("embedded bootstrap did not expose its resolver")
	end

	function runtime:installPack(exactFiles, localFiles)
		local pack = {vfs = {}, vfsLCL = {}, salt = salt,
			manifest = {generation = generation, md5 = generation, salt = salt}}
		for path, code in pairs(exactFiles or {}) do pack.vfs[fakeMD5(salt .. path)] = code end
		for path, code in pairs(localFiles or {}) do pack.vfsLCL[fakeMD5(salt .. path)] = code end
		environment.__holypack_packs[generation] = pack
		return pack
	end

	function runtime:registerCanonical(paths)
		for _, path in ipairs(paths) do
			nativeFiles[path] = [[return __holypack()()]]
		end
	end

	function runtime:registerNative(path, code)
		nativeFiles[path] = code
	end

	function runtime:runStub(path)
		local stub = assert(environment.CompileString(
			"return __holypack()()", path, true))
		runtime.pathStack[#runtime.pathStack + 1] = normalize(path)
		local results = {pcall(stub)}
		runtime.pathStack[#runtime.pathStack] = nil
		if not results[1] then error(results[2], 0) end
		return unpack(results, 2)
	end

	function runtime:run(path, code)
		local chunk = assert(environment.CompileString(code, path, true))
		runtime.pathStack[#runtime.pathStack + 1] = normalize(path)
		local results = {pcall(chunk)}
		runtime.pathStack[#runtime.pathStack] = nil
		if not results[1] then error(results[2], 0) end
		return unpack(results, 2)
	end

	return runtime
end

-- The production incident: the loader's config, language, and nested helper are all
-- in the immutable object, but every include is caller-relative.
do
	local runtime = newRuntime()
	runtime:installPack({
		["avdrones/loader.lua"] = [[
			local config = include("config/avdrones.lua")
			local language = include("lang/en.lua")
			return config .. ":" .. language
		]],
		["avdrones/config/avdrones.lua"] = [[return include("helpers.lua")]],
		["avdrones/config/helpers.lua"] = [[return "config-ok"]],
		["avdrones/lang/en.lua"] = [[return "language-ok"]],
	})
	runtime:registerCanonical({
		"avdrones/config/avdrones.lua",
		"avdrones/config/helpers.lua",
		"avdrones/lang/en.lua",
	})
	equal(runtime:runStub("avdrones/loader.lua"), "config-ok:language-ok",
		"caller-relative packed include chain")
	equal(#runtime.nativeIncludeCalls, 3,
		"packed children re-enter through current engine registrations")
	equal(runtime.nativeIncludeCalls[1].selected, "avdrones/config/avdrones.lua",
		"AVDrones config caller-relative registration")
	equal(runtime.environment.include, runtime.originalInclude,
		"packed execution restores native include")
	equal(runtime.environment.CompileFile, runtime.originalCompileFile,
		"packed execution restores native CompileFile")
end

-- The production AVDrones loader also includes every language file from its own
-- directory. Exercise the real six-name loop instead of a single representative.
do
	local runtime = newRuntime()
	local packFiles = {
		["avdrones/loader.lua"] = [[
			local values = {}
			for _, language in ipairs({"de", "en", "es", "fr", "ru", "tr"}) do
				values[#values + 1] = include("lang/" .. language .. ".lua")
			end
			return table.concat(values, ":")
		]],
	}
	local canonical = {}
	for _, language in ipairs({"de", "en", "es", "fr", "ru", "tr"}) do
		local path = "avdrones/lang/" .. language .. ".lua"
		packFiles[path] = "return \"" .. language .. "\""
		canonical[#canonical + 1] = path
	end
	runtime:installPack(packFiles)
	runtime:registerCanonical(canonical)
	equal(runtime:runStub("avdrones/loader.lua"), "de:en:es:fr:ru:tr",
		"AVDrones language include loop")
end

-- The engine's current registration is the per-path authority. A native delta wins
-- even when the immutable base also has that path (and a colliding Lua-root path);
-- replacing it with the canonical placeholder restores the exact base body.
do
	local runtime = newRuntime()
	runtime:installPack({
		["folder/caller.lua"] = [[return include("hot.lua")]],
		["folder/hot.lua"] = [[return "base-relative"]],
		["hot.lua"] = [[return "wrong-root-base"]],
	})
	runtime:registerNative("folder/hot.lua", [[return "native-delta"]])
	equal(runtime:runStub("folder/caller.lua"), "native-delta",
		"relative native delta wins over immutable and root entries")
	runtime:registerCanonical({"folder/hot.lua"})
	equal(runtime:runStub("folder/caller.lua"), "base-relative",
		"canonical restoration re-enters immutable relative path")
	runtime:registerNative("folder/hot.lua", [[return "native-delta-two"]])
	equal(runtime:runStub("folder/caller.lua"), "native-delta-two",
		"subsequent cached native delta remains authoritative")
end

-- Native include precedence is caller-relative first, then Lua-root absolute.
do
	local runtime = newRuntime()
	runtime:installPack({
		["folder/main.lua"] = [[return include("same.lua")]],
		["folder/same.lua"] = [[return "relative"]],
		["same.lua"] = [[return "root"]],
		["folder/root.lua"] = [[return include("shared/value.lua")]],
		["shared/value.lua"] = [[return "absolute"]],
	})
	runtime:registerCanonical({"folder/same.lua", "same.lua", "shared/value.lua"})
	equal(runtime:runStub("folder/main.lua"), "relative", "relative include precedence")
	equal(runtime:runStub("folder/root.lua"), "absolute", "Lua-root packed include")
end

-- A closure defined by a packed body keeps that body's source identity after the
-- initial canonical stub has returned, so a later relative include still selects the
-- current registered sibling before re-entering the immutable base.
do
	local runtime = newRuntime()
	runtime:installPack({
		["deferred/main.lua"] = [[
			return function()
				local value = include("child.lua")
				return value
			end
		]],
		["deferred/child.lua"] = [[return "deferred-relative"]],
	})
	runtime:registerCanonical({"deferred/child.lua"})
	local deferred = runtime:runStub("deferred/main.lua")
	equal(deferred(), "deferred-relative", "deferred packed relative include")
end

-- Packed execution never replaces the global data-plane functions, including on error.
do
	local runtime = newRuntime({["native/value.lua"] = [[return "native-after-error"]]})
	runtime:installPack({["error/main.lua"] = [[error("packed-boom")]]})
	local ok = pcall(function() runtime:runStub("error/main.lua") end)
	equal(ok, false, "packed runtime error propagation")
	equal(runtime.environment.include, runtime.originalInclude,
		"include restored after packed error")
	equal(runtime.environment.CompileFile, runtime.originalCompileFile,
		"CompileFile restored after packed error")
	equal(runtime:run("native/main.lua", [[local value = include("value.lua"); return value]]),
		"native-after-error", "native include after packed error")
end

-- Addon-prefixed debug sources must use the pack's internal compatibility aliases
-- without making addon-prefixed public include arguments valid.
do
	local runtime = newRuntime()
	runtime:installPack({}, {
		["avdrones/loader.lua"] = [[return include("config/avdrones.lua")]],
		["avdrones/config/avdrones.lua"] = [[return "addon-relative"]],
	})
	runtime:registerCanonical({"addons/avdrones/lua/avdrones/config/avdrones.lua"})
	equal(runtime:runStub("addons/avdrones/lua/avdrones/loader.lua"), "addon-relative",
		"addon-prefixed caller resolution")
end

-- A pack miss must preserve the packed caller for native relative lookup, preserve
-- Lua-root fallback, propagate all return values, and execute exactly once.
do
	local runtime = newRuntime({
		["fallback/native.lua"] = [[return "native-relative", 2]],
		["native-root.lua"] = [[return "native-root"]],
	})
	runtime:installPack({
		["fallback/main.lua"] = [[return include("native.lua")]],
		["fallback/root.lua"] = [[return include("native-root.lua")]],
	})
	local first, second = runtime:runStub("fallback/main.lua")
	equal(first, "native-relative", "native relative fallback value")
	equal(second, 2, "native relative fallback vararg")
	equal(runtime.nativeIncludeCalls[1].caller, "fallback/main.lua",
		"native fallback caller identity")
	equal(runtime:runStub("fallback/root.lua"), "native-root", "native Lua-root fallback")
	equal(#runtime.nativeIncludeCalls, 2, "native fallbacks execute once")
end

-- No global override exists before a generation is selected. Wholly native opt-out
-- and recovery lanes retain the original caller-relative behavior.
do
	local runtime = newRuntime({["native/config/value.lua"] = [[return "native-lane"]]})
	runtime:installPack({})
	equal(runtime:run("native/loader.lua", [[local value = include("config/value.lua"); return value]]),
		"native-lane", "no-generation native include")
	equal(runtime.nativeIncludeCalls[1].caller, "native/loader.lua",
		"no-generation caller identity")
end

-- CompileFile stays Lua-root-relative and reaches canonical registrations through the
-- engine. A broken packed entry is reported by __holypack rather than falling through
-- to the unrelated native source that existed before canonical registration.
do
	local runtime = newRuntime()
	runtime:installPack({
		["seed.lua"] = [[return "seed"]],
		["compile-caller.lua"] = [[
			local compiled = CompileFile("compile.lua", false)
			return type(compiled), compiled()
		]],
		["compile.lua"] = [[return "compiled"]],
	})
	runtime:registerCanonical({"compile.lua"})
	runtime:runStub("seed.lua")
	local kind, value = runtime:runStub("compile-caller.lua")
	equal(kind, "function", "packed CompileFile result")
	equal(value, "compiled", "packed CompileFile execution")
	local forwarded = false
	for _, call in ipairs(runtime.compileCalls) do
		if call.identifier == "compile.lua" and call.handleError == false then forwarded = true end
	end
	equal(forwarded, true, "CompileFile showError forwarding")
	equal(#runtime.nativeCompileCalls, 1, "canonical CompileFile registration count")
end

-- Unsupported public path forms remain native engine lookups instead of becoming
-- valid through the pack's internal addon/lua aliases. Ambiguous local aliases likewise
-- cannot select an immutable body; only an exact canonical registration can do that.
do
	local runtime = newRuntime({
		["lua/forbidden.lua"] = [[return "native-invalid-form"]],
		["collision.lua"] = [[return "native-collision"]],
	})
	runtime:installPack({
		["seed.lua"] = [[return "seed"]],
		["forbidden.lua"] = [[return "wrong-packed-form"]],
		["invalid.lua"] = [[return include("lua/forbidden.lua")]],
		["collision-main.lua"] = [[return include("collision.lua")]],
	}, { ["collision.lua"] = false })
	runtime:runStub("seed.lua")
	equal(runtime:runStub("invalid.lua"), "native-invalid-form", "invalid public form delegation")
	equal(runtime:runStub("collision-main.lua"), "native-collision", "collision delegation")
end

for _, command in ipairs(allCommands) do
	if command[1] == "holylib_luapack_failed" or command[1] == "retry" then
		fail("bootstrap include tests unexpectedly requested recovery")
	end
end

print("luapack bootstrap include tests passed")
