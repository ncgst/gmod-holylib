return {
    groupName = "vprof node ownership",
    cases = {
        {
            name = "Borrowed wrappers release independently of engine nodes",
            when = HolyLib_IsModuleEnabled("vprof"),
            func = function()
                expect(vprof.NODE_GC_SAFE).to.beTrue()
                local root = vprof.GetRoot()
                local name = root:GetName()
                local alias = vprof.GetRoot()
                -- Run the actual finalizer deterministically, then collect the
                -- released wrapper again to exercise idempotent invalidation.
                getmetatable(root).__gc(root)
                expect(alias:GetName()).to.equal(name)
                expect(vprof.GetRoot():GetName()).to.equal(name)
                root = nil
                collectgarbage("collect")
                expect(alias:GetName()).to.equal(name)
                local child = alias:GetChild()
                if child then
                    local childName = child:GetName()
                    local childAlias = alias:GetChild()
                    getmetatable(child).__gc(child)
                    expect(childAlias:GetName()).to.equal(childName)
                    child = nil
                    collectgarbage("collect")
                    expect(childAlias:GetName()).to.equal(childName)
                end
            end
        },
    }
}
