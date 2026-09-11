-- Exercise the production benchmark wrapper with a real LuaJIT engine.
local sourceFile = assert(io.open("gluatests/lua/gmod_tests/sh_init.lua", "rb"))
local source = sourceFile:read("*a")
sourceFile:close()
local first = assert(source:find("local rec = false", 1, true))
local last = assert(source:find("\nif SERVER then", first, true))

local clock = 0
local env = setmetatable({
    SysTime = function()
        clock = clock + 0.005
        return clock
    end,
    print = function() end,
    HTTP = function() end,
    _HOLYLIB_RUN_NUMBER = 1,
    BRANCH = "test",
    file = { Read = function(path)
        if path == "_workflow/github_repo.txt" then return "test/repo" end
        if path == "_workflow/loki_public_host.txt" then return "https://unused.invalid" end
    end },
    util = { TableToJSON = function() return "{}" end },
    string = setmetatable({ Trim = function(value)
        return value:match("^%s*(.-)%s*$")
    end }, { __index = string }),
}, { __index = _G })
local chunk = assert(loadstring(source:sub(first, last - 1), "@benchmark-helper"))
setfenv(chunk, env)
chunk()

local initiallyEnabled = jit.status()
for _, enabled in ipairs({true, false}) do
    if enabled then jit.on() else jit.off() end
    local calls = 0
    env.HolyLib_RunPerformanceTest("success", function() calls = calls + 1 end)
    assert(calls > 0, "benchmark callback was skipped")
    assert(jit.status() == enabled, "successful benchmark leaked JIT state")

    local success, err = pcall(env.HolyLib_RunPerformanceTest, "failure", function()
        error("benchmark callback failed", 0)
    end)
    assert(not success and err == "benchmark callback failed", "benchmark swallowed the callback error")
    assert(jit.status() == enabled, "failed benchmark leaked JIT state")
end

-- Simulate clocks that cannot resolve the warmup, or report an extremely
-- small positive interval. The callback budget makes the old infinite batch
-- fail promptly instead of hanging this regression test too.
for _, warmupInterval in ipairs({0, 1e-12}) do
    local reads, calls = 0, 0
    env.SysTime = function()
        reads = reads + 1
        if reads == 1 then return 0 end
        if reads == 2 then return warmupInterval end
        return (reads - 2) * 0.1
    end
    env.HolyLib_RunPerformanceTest("clock resolution", function()
        calls = calls + 1
        assert(calls < 500000, "calibration produced an unbounded batch")
    end)
    assert(calls > 0, "clock resolution test skipped the callback")
end

env.SysTime = os.clock
env.HolyLib_RunPerformanceTest("real clock", function() end)
if initiallyEnabled then jit.on() else jit.off() end
print("Benchmark JIT state, error propagation and timing bounds passed")
