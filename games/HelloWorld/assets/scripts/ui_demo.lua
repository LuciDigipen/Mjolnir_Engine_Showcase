-- Game UI drawn from Lua: a panel in the bottom-left corner that fades in, counts clicks, and pauses and resumes
-- the game, a dial beside it that follows the slider, and a marker on the duck. OnUI draws the whole UI again every
-- frame, and what it remembers lives in self. The look, Turret Road and the colours, is ui/style.json's, which game.json names, so the
-- engine's pause menu wears it too.
local UIDemo = {}

-- a HUD in play: the mouse clicks it, but the arrow keys, the stick and Enter stay the game's
local hud = { pointer_only = true }

-- text options style one widget's text over the style
local title = {
    size = 22,
    align = "center",
    color = { 1, 0.85, 0.4, 1 },
    outline = { width = 1 },
    shadow = { x = 1, y = 2, color = { 0, 0, 0, 0.6 } },
}

-- with_style keys are the style file's
local warning = { widget_color = { 0.55, 0.32, 0.08, 0.95 }, widget_hovered_color = { 0.7, 0.42, 0.12, 0.95 } }

-- shapes and rotation: angles are degrees, 0 to the right and clockwise
local function DrawDial(ui, cx, cy, r, value)
    local from, sweep = 135, 270
    local angle = from + sweep * value
    ui:circle(cx, cy, r + 8, { color = { 0, 0, 0, 0.45 } })
    ui:circle(cx, cy, r + 4, { width = 1, dash = { 4, 4 }, color = { 1, 1, 1, 0.35 } })
    ui:arc(cx, cy, r - 6, from, from + sweep, { width = 6, color = { 1, 1, 1, 0.15 } })
    ui:arc(cx, cy, r - 6, from, angle, {
        width = 6,
        gradient = { from = { cx - r, cy }, to = { cx + r, cy },
                     from_color = { 0.3, 0.9, 0.5, 1 }, to_color = { 1, 0.35, 0.2, 1 } },
    })
    for i = 0, 10 do
        ui:with_rotation(from + sweep * i / 10, cx, cy, function()
            ui:line(cx + r - 20, cy, cx + r - 13, cy, { width = i % 5 == 0 and 2 or 1 })
        end)
    end
    ui:with_rotation(angle, cx, cy, function()
        ui:polygon({ cx - 8, cy - 3, cx + r - 22, cy, cx - 8, cy + 3 }, { color = { 1, 0.3, 0.2, 1 } })
    end)
    ui:circle(cx, cy, 5, { color = { 0.9, 0.9, 0.9, 1 } })
    ui:label("dial_value", { x = cx - 40, y = cy + 16, w = 80, h = 24 },
        string.format("%d%%", math.floor(value * 100 + 0.5)),
        { size = 20, align = "center", outline = true })
end

-- the duck is entity 7 in the scene's files; its origin is at its feet
local kDuck = 7
local kDuckHeight = 0.6

-- a box and its distance over the duck, or an arrow on the screen's edge toward it when the camera does not see it
local function DrawDuckMarker(ui, world)
    local duck = world:find_scene_entity(world:primary_scene(), kDuck)
    local at = duck and world:world_position(duck)
    if at == nil then return end
    -- an entity or a point; this one is a little above the duck's
    at.y = at.y + kDuckHeight
    local seen = ui:world_to_screen(at)
    local color = { 1, 0.85, 0.4, 1 }
    ui:with_world_anchor(at, { offscreen = "edge", inset = 28 }, function(on_screen, angle)
        if on_screen then
            ui:rect({ x = -28, y = -28, w = 56, h = 56 }, { width = 2, rounding = 4, color = color })
            ui:label("duck_distance", { x = -40, y = -52, w = 80, h = 20 }, string.format("%.0f m", seen.depth),
                { size = 16, align = "center", outline = true, color = color })
        else
            ui:with_rotation(angle, function()
                ui:polygon({ 14, 0, -7, -10, -7, 10 }, { color = color })
            end)
        end
    end)
end

function UIDemo.NewInstanceData()
    return { clicks = 0, showBar = true, level = 0.5, needle = 0.5 }
end

function UIDemo.OnStart(self, world, entity)
    print("UI demo: a Lua panel in the bottom-left corner of the Game view")
    world:set_value("UIDemo.alpha", 0)
    world:tween_value("UIDemo.alpha", 1, { duration = 0.5, ease = "Out", clock = "ui" })
end

function UIDemo.OnUI(self, world, entity, ui, dt)
    DrawDuckMarker(ui, world)

    local panel = ui:anchor(ui:inset(ui:safe_area(), 16), "bottom_left", 240, 200)
    ui:with_opacity(world:value("UIDemo.alpha", 1), function()
        ui:panel(panel)
        local rows = ui:column(ui:inset(panel, 8), 6)
        ui:label("title", rows:next(24), "Lua UI", title)
        if ui:button("count", rows:next(32), "Clicked " .. self.clicks, hud).clicked then
            self.clicks = self.clicks + 1
        end
        self.showBar = ui:toggle("show_bar", rows:next(28), "Show bar", self.showBar, hud)
        self.level = ui:slider("level", rows:next(28), self.level, 0, 1, { step = 0.1, pointer_only = true })
        if self.showBar then
            ui:bar("level_bar", rows:next(12), self.level)
        end
        local paused = world:is_paused()
        ui:with_style(warning, function()
            if ui:button("pause", rows:rest(), paused and "Resume" or "Pause", hud).clicked then
                if paused then world:resume() else world:pause() end
            end
        end)

        -- the needle eases after the slider rather than jumping to it
        self.needle = self.needle + (self.level - self.needle) * math.min(1, dt * 8)
        local r = 56
        DrawDial(ui, panel.x + panel.w + 24 + r, panel.y + panel.h - r - 8, r, self.needle)
    end)
end

return UIDemo
