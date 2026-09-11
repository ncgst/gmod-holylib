-- Run the production async fixture with real LuaJIT and a small userdata proxy.
-- Cover both available trace events and stock GMod's disabled vmevent API.
local path = "gluatests/lua/tests/core/GetReferencedUserData.lua"
local group = assert(loadfile(path))()
local case = group.cases[#group.cases]
for _, traceEvents in ipairs({ true, false }) do
for _, enabled in ipairs({ true, false }) do
    if enabled then jit.on() else jit.off() end
    local state, callbacks, done, expectations = {}, {}, false, 0
    local userdata = newproxy(true)
    local values = {}
    getmetatable(userdata).__index = values
    getmetatable(userdata).__newindex = values
    local env = setmetatable({
        _HOLYLIB_CORE = { PushReferencedTestUserData = function() return userdata end },
        expect = function(value)
            return { to = { beTrue = function()
                assert(value == true, "Fixture did not compile a trace")
            end, equal = function(expected)
                assert(value == expected, "Userdata read result changed")
                expectations = expectations + 1
            end } }
        end,
        timer = { Simple = function(_, fn) callbacks[#callbacks+1] = fn end },
        done = function() done = true end,
    }, { __index = _G })
    if not traceEvents then
        env.jit = setmetatable({attach = function() error("vmevent API disabled") end}, {__index = jit})
    end
    setfenv(case.func, env)
    case.func(state)
    while #callbacks > 0 do table.remove(callbacks, 1)() end
    case.cleanup(state)
    assert(done and expectations == 2, "Both trace generations must finish")
    assert(jit.status() == enabled, "Trace fixture leaked the JIT state")
end
end
print("Trace fixture handles enabled/disabled events and preserves JIT state")
