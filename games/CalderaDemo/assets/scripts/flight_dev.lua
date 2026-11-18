-- Developer air start for the placeholder aircraft: F5 puts it 300 m above the runway start, flying.
local FlightDev = {}

local kStart = { x = -377.815, y = 324.15, z = 370.209 }
local kHeading = { x = 0.0, y = 0.92387953, z = 0.0, w = 0.38268343 }
local kAirspeed = 55.0
local kThrottle = 0.6

function FlightDev.OnUpdate(self, world, entity, dt)
    if world:just_pressed("FlightDevAirStart") then
        local ok = world:reset_aircraft(entity, kStart, kHeading, kAirspeed, kThrottle)
        print(string.format("flight_dev: air start %s", ok and "done" or "refused"))
    end
end

return FlightDev
