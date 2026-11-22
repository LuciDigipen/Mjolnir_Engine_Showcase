-- Game UI drawn from Lua: a panel in the bottom-left corner that fades in, counts clicks, and pauses and resumes
-- the game. OnUI draws the whole UI again every frame, and what it remembers lives in self.
local UIDemo = {}

-- a HUD in play: the mouse clicks it, but the arrow keys, the stick and Enter stay the game's
local hud = { pointer_only = true }

-- text options style one widget's text; OnUI sets font here and in hud, the game's own Turret Road
local title = {
    size = 22,
    align = "center",
    color = { 1, 0.85, 0.4, 1 },
    outline = { width = 1 },
    shadow = { x = 1, y = 2, color = { 0, 0, 0, 0.6 } },
}

function UIDemo.NewInstanceData()
    return { clicks = 0, showBar = true, level = 0.5 }
end

function UIDemo.OnStart(self, world, entity)
    print("UI demo: a Lua panel in the bottom-left corner of the Game view")
    world:set_value("UIDemo.alpha", 0)
    world:tween_value("UIDemo.alpha", 1, { duration = 0.5, ease = "Out", clock = "ui" })
end

function UIDemo.OnUI(self, world, entity, ui, dt)
    -- loaded once and cached; nil, the default font, if it does not load
    local font = ui:font("fonts/TurretRoad-Bold.ttf")
    title.font = font
    hud.font = font

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
        if ui:button("pause", rows:rest(), paused and "Resume" or "Pause", hud).clicked then
            if paused then world:resume() else world:pause() end
        end
    end)
end

return UIDemo
