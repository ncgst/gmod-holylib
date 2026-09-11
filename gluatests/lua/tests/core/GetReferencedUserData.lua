return {
    groupName = "HolyLib manages to properly Get HolyLib Referenced UserData to Lua",
    cases = {
        {
            name = "Is _HOLYLIB_CORE.GetReferencedTestUserData function existent",
            func = function()
                expect( _HOLYLIB_CORE.GetReferencedTestUserData ).to.beA( "function" )
            end
        },
        {
            name = "Properly got the valid pointer to our userdata",
            func = function()
                local userdata = _HOLYLIB_CORE.PushReferencedTestUserData()
                expect( userdata ).to.beA( "_HOLYLIB_CORE_TEST_REFERENCED" )

                -- If we got a garbage pointer, this will return false or crash.
                expect( _HOLYLIB_CORE.GetReferencedTestUserData(userdata) ).to.beTrue()
            end
        },
        {
            name = "__newindex and __index functions properly work on userdata",
            func = function()
                local userdata = _HOLYLIB_CORE.PushReferencedTestUserData()
                expect( userdata ).to.beA( "_HOLYLIB_CORE_TEST_REFERENCED" )

                userdata.example = "Hello World"

                expect( userdata.example ).to.equal( "Hello World" )
            end
        },
        {
            name = "Performance",
            func = function()
                local userdata = _HOLYLIB_CORE.PushReferencedTestUserData()
                HolyLib_RunPerformanceTest("_HOLYLIB_CORE.GetReferencedTestUserData", function() _HOLYLIB_CORE.RawReferencedGetTestUserData(userdata) end)

                HolyLib_RunPerformanceTest("_HOLYLIB_CORE(ref).__newindex", function() userdata.example = "Hello World" end)
                HolyLib_RunPerformanceTest("_HOLYLIB_CORE(ref).__index", function() return userdata.example end)
            end
        },
        {
            name = "LuaJIT traces do not create a crash",
            async = true,
            timeout = 5,
            cleanup = function(state)
                if state.traceCallback then jit.attach(state.traceCallback) end
                if state.wasJITEnabled == nil then return end
                jit.flush()
                if state.wasJITEnabled then jit.on() else jit.off() end
            end,
            func = function(state)
                --[[
                    Very specific crash caused by LuaJIT generating a GCtrace which then for very magical reasons can lead to memory corruption of userdata
                ]]

                state.wasJITEnabled = jit.status()
                jit.on()
                state.traceCallback = function(what)
                    if what == "stop" then state.traceCompleted = true end
                end
                jit.attach(state.traceCallback, "trace")
                local function generate_trace()
                    jit.flush()
                    state.traceCompleted = false

                    --[[jit.attach(function(what, traceno, func, pc, exitno)
                        if exitno ~= nil then what = "abort" end

                        local src = "<unknown>"
                        if type(func) == "function" or type(func) == "proto" then
                            local info = jit.util.funcinfo(func)
                            if info then
                                src = string.format("%s:%d", info.source or "C", info.linedefined or 0)
                            end
                        elseif func ~= nil then
                            src = tostring(func)
                        end

                        print(string.format("[TRACE] %-6s %s %-35s pc=%s exit=%s",
                            tostring(what),
                            tostring(traceno or "?"),
                            tostring(src),
                            tostring(pc or "-"),
                            tostring(exitno or "-")
                        ))
                    end, "trace")]]

                    local function trace_userdata(u)
                        return u.test
                    end

                    local userData = _HOLYLIB_CORE.PushReferencedTestUserData()
                    for n = 1, 1e4 do -- Generate those sweet GCtrace
                        trace_userdata(userData)
                    end
                    return state.traceCompleted
                end

                expect( generate_trace() ).to.beTrue()

                timer.Simple(1, function()
                    collectgarbage("collect")

                    timer.Simple(1, function()
                        -- Keep verifying that the userdata path actually traces,
                        -- including after collection, without leaking hotloop=1.
                        expect( generate_trace() ).to.beTrue()

                        done()
                    end)
                end)
            end
        },
    }
}
