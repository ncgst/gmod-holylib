return {
    groupName = "stringtable.AllowCreation",
    cases = {
        {
            name = "Function exists on table",
            when = HolyLib_IsModuleEnabled("stringtable"),
            func = function()
                expect( stringtable.AllowCreation ).to.beA( "function" )
            end
        },
        {
            name = "Table doesn't exist",
            when = not HolyLib_IsModuleEnabled("stringtable"),
            func = function()
                expect( stringtable ).to.beA( "nil" )
            end
        },
        {
            name = "Allows creation properly",
            when = HolyLib_IsModuleEnabled("stringtable"),
            func = function()
                stringtable.AllowCreation( true )
                local table = stringtable.CreateStringTable( GetTestStringTableName(), 4096, 0, 0 )
                stringtable.AllowCreation( false )

                expect( table:IsValid() ).to.beTrue()

                stringtable.RemoveTable( table ) -- Cleanup
            end
        },
        {
            name = "If not used creation fails",
            when = HolyLib_IsModuleEnabled("stringtable"),
            func = function()
                stringtable.AllowCreation( false )
                local name = GetTestStringTableName()
                expect(function()
                    stringtable.CreateStringTable( name, 4096, 0, 0 )
                end).to.errWith("Tried to create string table '" .. name .. "' at the wrong time!")
            end
        },
    }
}
