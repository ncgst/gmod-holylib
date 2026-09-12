local nativeVector = GMOD_Vector or Vector
local nativeAngle = GMOD_Angle or Angle

print(string.format("[HolyLib update identity] VERSION=%s VERSIONSTR=%s BRANCH=%s jit=%s",
    tostring(VERSION), tostring(VERSIONSTR), tostring(BRANCH), tostring(jit.version)))
RunConsoleCommand("version")

local function checkTypes()
    local samples = {
        { value = nil, id = TYPE_NIL, name = "nil" },
        { value = false, id = TYPE_BOOL, name = "boolean" },
        { value = true, id = TYPE_BOOL, name = "boolean" },
        { value = 123, id = TYPE_NUMBER, name = "number" },
        { value = "update", id = TYPE_STRING, name = "string" },
        { value = {}, id = TYPE_TABLE, name = "table" },
        { value = function() end, id = TYPE_FUNCTION, name = "function" },
        { value = coroutine.create(function() end), id = TYPE_THREAD, name = "thread" },
        { value = Vector(1, 2, 3), id = TYPE_VECTOR, name = "Vector" },
        { value = nativeVector(4, 5, 6), id = TYPE_VECTOR, name = "Vector" },
        { value = Angle(10, 20, 30), id = TYPE_ANGLE, name = "Angle" },
        { value = nativeAngle(40, 50, 60), id = TYPE_ANGLE, name = "Angle" },
    }

    for _, sample in ipairs(samples) do
        assert(TypeID(sample.value) == sample.id, "TypeID mismatch for " .. sample.name)
        assert(type(sample.value) == sample.name, "type mismatch for " .. sample.name)
        assert(isvector(sample.value) == (sample.id == TYPE_VECTOR), "isvector mismatch")
        assert(isangle(sample.value) == (sample.id == TYPE_ANGLE), "isangle mismatch")
    end
    return true
end

-- Use a changing argument so the recorder must produce the type ID, not leave
-- the original argument in its return slot. Keep optimizer settings untouched.
local function hotPrimitiveTypes()
    local sum = 0
    for index = 1, 4096 do
        sum = sum + TypeID(index) + TypeID(index % 2 == 0)
    end
    assert(sum == 4096 * (TYPE_NUMBER + TYPE_BOOL), "hot TypeID returned the argument instead of its type")
    return true
end

return {
    groupName = "GMod native and FFI type compatibility",
    cases = {
        {
            name = "Loads HolyLib and the requested LuaJIT runtime",
            func = function()
                expect(_HOLYLIB).to.beTrue()
                local enabled = GetConVar("holylib_enable_luajit")
                assert(enabled ~= nil, "HolyLib LuaJIT module configuration is unavailable")
                if enabled:GetBool() then
                    assert(string.find(jit.version, "HolyLib", 1, true), "Requested LuaJIT replacement did not activate")
                end
            end
        },
        {
            name = "Preserves stock type contracts for native and replacement values",
            func = function()
                expect(checkTypes()).to.beTrue()
            end
        },
        {
            name = "Preserves primitive type IDs on hot JIT paths and after GC",
            func = function()
                local wasEnabled = jit.status()
                local ok, err = pcall(function()
                    jit.on()
                    jit.flush(hotPrimitiveTypes)
                    hotPrimitiveTypes()
                    collectgarbage("collect")
                    hotPrimitiveTypes()
                    checkTypes()
                end)
                jit.flush(hotPrimitiveTypes)
                if not wasEnabled then jit.off() end
                assert(ok, err)
                expect(ok).to.beTrue()
            end
        },
        {
            name = "Serializes mixed native and replacement vectors and angles",
            func = function()
                util.AddNetworkString("HolyLib_TypeCompatibility")
                local ok, err = pcall(function()
                    net.Start("HolyLib_TypeCompatibility")
                    net.WriteTable({
                        ffiVector = Vector(1, 2, 3),
                        nativeVector = nativeVector(4, 5, 6),
                        ffiAngle = Angle(10, 20, 30),
                        nativeAngle = nativeAngle(40, 50, 60),
                    })
                    assert(net.BytesWritten() > 0, "net.WriteTable did not write a payload")
                end)
                net.Abort()
                assert(ok, err)
                expect(ok).to.beTrue()
            end
        },
    }
}
