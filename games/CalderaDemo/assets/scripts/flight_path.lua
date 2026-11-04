-- Measurement flight: follows a fixed route at the speed in this entity's Name ("speed=60").
-- Prints frame time percentiles for each leg and "flight_done" at the end.
local Flight = {}

local kWarmupSeconds = 8.0
local kSpinRate = math.pi -- 180 degrees a second, one full turn over the "turn" leg's start
local kTurnRate = math.rad(45.0) -- nose follows the route at this rate

local legs = {
    { name = "high", points = { { -1700, 1400, -1700 }, { 1500, 1400, 1500 } } },
    { name = "descent", points = { { 1500, 1400, 1500 }, { 1250, 900, 900 }, { 800, 450, 100 } } },
    { name = "low_peak", points = { { 800, 450, 100 }, { 600, 330, -150 }, { 257, 310, -415 }, { -50, 220, -650 }, { -350, 190, -850 } } },
    { name = "turn", spin = true, points = { { -350, 190, -850 }, { -800, 190, -900 }, { -1250, 190, -700 } } },
    { name = "approach", points = { { -1250, 190, -700 }, { -1600, 150, 100 }, { -1267, 90, 1193 }, { -744, 40, 727 }, { -296, 32, 327 } } },
}

local function Sub(a, b) return { a[1] - b[1], a[2] - b[2], a[3] - b[3] } end
local function Length(v) return math.sqrt(v[1] * v[1] + v[2] * v[2] + v[3] * v[3]) end

local function SetLook(world, entity, yaw, pitch)
    local cy, sy, cp, sp = math.cos(yaw * 0.5), math.sin(yaw * 0.5), math.cos(pitch * 0.5), math.sin(pitch * 0.5)
    world:set(entity, "Transform", "rotation", { x = cy * sp, y = sy * cp, z = -sy * sp, w = cy * cp })
end

local function Percentile(sorted, p)
    return sorted[math.max(1, math.ceil(#sorted * p))] * 1000.0
end

local function ReportLeg(name, samples)
    table.sort(samples)
    print(string.format("flight_leg %s frames=%d p50=%.2f p95=%.2f p99=%.2f max=%.2f", name, #samples,
        Percentile(samples, 0.50), Percentile(samples, 0.95), Percentile(samples, 0.99), samples[#samples] * 1000.0))
end

function Flight.NewInstanceData()
    return { t = 0.0, leg = 1, segment = 1, along = 0.0, spin = 0.0, samples = {}, started = false, done = false, speed = 60.0 }
end

function Flight.OnStart(self, world, entity)
    self.speed = tonumber(string.match(world:get(entity, "Name").name, "speed=(%d+)")) or 60.0
end

function Flight.OnUpdate(self, world, entity, dt)
    if self.done then
        return
    end
    self.t = self.t + dt
    local leg = legs[self.leg]
    if not self.started then
        local p = leg.points[1]
        local d = Sub(leg.points[2], p)
        self.yaw = math.atan2(-d[1], -d[3])
        self.pitch = math.asin(d[2] / Length(d))
        world:set(entity, "Transform", "position", { x = p[1], y = p[2], z = p[3] })
        SetLook(world, entity, self.yaw, self.pitch)
        if self.t >= kWarmupSeconds then
            self.started = true
            print("flight_start speed=" .. self.speed)
            print("flight_mark " .. leg.name)
        end
        return
    end

    self.samples[#self.samples + 1] = dt
    self.along = self.along + self.speed * dt
    local from, to = leg.points[self.segment], leg.points[self.segment + 1]
    local span = Length(Sub(to, from))
    while self.along >= span do
        self.along = self.along - span
        self.segment = self.segment + 1
        if self.segment >= #leg.points then
            ReportLeg(leg.name, self.samples)
            self.samples = {}
            self.segment = 1
            self.spin = 0.0
            self.leg = self.leg + 1
            if self.leg > #legs then
                self.done = true
                print("flight_done")
                return
            end
            leg = legs[self.leg]
            print("flight_mark " .. leg.name)
        end
        from, to = leg.points[self.segment], leg.points[self.segment + 1]
        span = Length(Sub(to, from))
    end

    local f = self.along / span
    local d = Sub(to, from)
    world:set(entity, "Transform", "position", { x = from[1] + d[1] * f, y = from[2] + d[2] * f, z = from[3] + d[3] * f })

    -- The nose turns toward the route at a limited rate, so waypoints are not view snaps.
    local maxStep = kTurnRate * dt
    local yawError = math.atan2(-d[1], -d[3]) - self.yaw
    yawError = math.atan2(math.sin(yawError), math.cos(yawError))
    self.yaw = self.yaw + math.max(-maxStep, math.min(maxStep, yawError))
    local pitchError = math.asin(d[2] / span) - self.pitch
    self.pitch = self.pitch + math.max(-maxStep, math.min(maxStep, pitchError))

    local yaw = self.yaw
    if leg.spin and self.spin < 2.0 * math.pi then
        self.spin = self.spin + kSpinRate * dt
        yaw = yaw + self.spin
    end
    SetLook(world, entity, yaw, self.pitch)
end

return Flight
