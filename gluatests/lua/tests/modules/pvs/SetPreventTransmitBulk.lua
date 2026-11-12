return {
    groupName = "pvs.SetPreventTransmitBulk",
    cases = {
        {
            name = "Function exists on table",
            when = HolyLib_IsModuleEnabled("pvs"),
            func = function()
                expect( pvs.SetPreventTransmitBulk ).to.beA( "function" )
            end
        },
        {
            name = "Table doesn't exist",
            when = not HolyLib_IsModuleEnabled("pvs"),
            func = function()
                expect( pvs ).to.beA( "nil" )
            end
        },
        {
            name = "Stops and resumes transmitting the entity to the player",
            when = HolyLib_IsModuleEnabled("pvs"),
            async = true,
            timeout = 10,
            cleanup = function(state)
                hook.Remove("HolyLib:PostCheckTransmit", "pvs.SetPreventTransmitBulk")
                pvs.EnablePostTransmitHook(false)
                if IsValid(state.ent) then state.ent:Remove() end
            end,
            func = function(state)
                -- Bots only get CheckTransmit calls with sv_stressbots, which autorun/gmod_tests_init.lua enables.
                local bot = player.GetBots()[1]
                expect( bot ).to.beValid()

                local ent = ents.Create("prop_physics")
                ent:SetModel("models/hunter/blocks/cube025x025x025.mdl")
                ent:SetPos(bot:EyePos() + Vector(0, 0, 32))
                ent:Spawn()
                state.ent = ent

                -- Each step waits until the bot's transmit matches, then runs its action outside of CheckTransmit.
                local steps = {
                    { transmitted = true, action = function() pvs.SetPreventTransmitBulk(ent, bot, true) end },
                    { transmitted = false, action = function() pvs.SetPreventTransmitBulk({ent}, {bot}, false) end },
                    { transmitted = true, action = function() done() end },
                }
                local step = 1
                local pending = false
                pvs.EnablePostTransmitHook(true)
                hook.Add("HolyLib:PostCheckTransmit", "pvs.SetPreventTransmitBulk", function(ply)
                    if ply ~= bot or pending or not steps[step] then return end
                    if table.HasValue(pvs.GetEntitiesFromTransmit(), ent) ~= steps[step].transmitted then return end

                    local action = steps[step].action
                    step = step + 1
                    pending = true
                    timer.Simple(0, function()
                        pending = false
                        action()
                    end)
                end)
            end
        },
    }
}
