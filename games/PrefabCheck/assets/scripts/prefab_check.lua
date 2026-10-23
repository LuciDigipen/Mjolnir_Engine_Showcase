local PrefabCheck = {}

local function fail(check, message)
    error("prefab_check " .. check .. ": " .. message, 2)
end

local function named(world, wanted)
    local found = {}
    world:each("Name", function(entity, name)
        if name.name == wanted then
            found[#found + 1] = entity
        end
    end)
    return found
end

local function single(world, wanted, check)
    local found = named(world, wanted)
    if #found ~= 1 then
        fail(check, "expected one entity named " .. wanted .. ", found " .. #found)
    end
    return found[1]
end

local function controller(world, entity, check)
    local value = world:get(entity, "CameraController")
    if value == nil then
        fail(check, "an expected entity has no CameraController")
    end
    return value
end

local function position_x(world, entity, check)
    local transform = world:get(entity, "Transform")
    if transform == nil then
        fail(check, "an expected entity has no Transform")
    end
    return transform.position.x
end

local function close(left, right)
    return math.abs(left - right) < 1e-3
end

function PrefabCheck.OnStart(self, world, entity)
    local crates = named(world, "Crate")
    if #crates ~= 2 then
        fail("crates", "expected two Crate instances, found " .. #crates)
    end

    local child_a = single(world, "CrateChildA", "scene override")
    local child_b = single(world, "CrateChild", "crate references")
    local root_a = nil
    local root_b = nil
    for _, crate in ipairs(crates) do
        local target = controller(world, crate, "crate references").target
        if target == child_a then
            root_a = crate
        elseif target == child_b then
            root_b = crate
        end
    end
    if root_a == nil then
        fail("crate references", "no crate references CrateChildA")
    end
    if root_b == nil then
        fail("crate references", "no crate references CrateChild")
    end
    if root_a == root_b then
        fail("crates", "both crates resolved to one entity")
    end

    local cross = controller(world, child_b, "cross-instance reference").target
    if cross ~= root_a then
        fail("cross-instance reference", "CrateChild does not target the other crate root")
    end

    local door_a = single(world, "DoorScened", "nested scene override")
    local door_b = single(world, "Door", "nested instances")
    if door_a == door_b then
        fail("nested instances", "both doors resolved to one entity")
    end
    if not close(position_x(world, door_a, "nested occurrence override"), -2.0) then
        fail("nested occurrence override", "DoorScened is not at its nested position")
    end
    if not close(position_x(world, door_b, "nested occurrence override"), 2.0) then
        fail("nested occurrence override", "Door is not at its nested position")
    end

    if not controller(world, door_a, "null reference").target:is_null() then
        fail("null reference", "DoorScened target is not null")
    end
    local handle = controller(world, door_b, "nested reference").target
    local handle_name = world:get(handle, "Name")
    if handle:is_null() or handle_name == nil or handle_name.name ~= "DoorHandle" then
        fail("nested reference", "Door does not target its own handle")
    end

    local copy = single(world, "CrateCopy", "detached copy")
    local copy_child = single(world, "CrateCopyChild", "detached copy")
    local copy_target = controller(world, copy, "detached copy").target
    if copy_target ~= copy_child then
        fail("detached copy", "CrateCopy does not target its copied child")
    end
    if copy_target == child_a or copy_target == child_b then
        fail("detached copy", "CrateCopy targets an original crate child")
    end

    local camera = single(world, "Camera", "camera")
    if world:get(camera, "Camera") == nil then
        fail("camera", "the named camera has no Camera component")
    end
    if controller(world, camera, "camera").target ~= root_a then
        fail("camera", "the camera does not follow a crate instance")
    end

    print("prefab_check: all prefab acceptance checks passed")
end

return PrefabCheck
