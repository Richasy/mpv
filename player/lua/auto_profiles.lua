-- Note: anything global is accessible by profile condition expressions.

local msg = require 'mp.msg'

local profiles = {}
local watched_properties = {}       -- indexed by property name (used as a set)
local cached_properties = {}        -- property name -> last known raw value
local properties_to_profiles = {}   -- property name -> set of profiles using it
local have_dirty_profiles = false   -- at least one profile is marked dirty
local pending_hooks = {}            -- as set (keys only, meaningless values)

-- Used during evaluation of the profile condition, and should contain the
-- profile the condition is evaluated for.
local current_profile = nil

-- Cached set of all top-level mpv properties. Only used for extra validation.
local property_set = {}
for _, property in pairs(mp.get_property_native("property-list")) do
    property_set[property] = true
end

-- === Rodel.Player fork: targeted Restore debounce =========================
-- Background:
--   `video-target-params` is fired on MPV_EVENT_TICK (player/command.c). The
--   value is recomputed every draw_frame from the swapchain hint negotiation
--   in vo_gpu_next.c. While a vf chain reconfig is in flight (especially with
--   hwdec=*-copy on Windows d3d11), `video-target-params/max-luma` briefly
--   becomes nil / SDR_WHITE / HDR(1000) in rapid succession. Profiles whose
--   condition reads this property therefore see TRUE↔FALSE flips per frame.
--   When `apply-profile` itself triggers vf reconfig (e.g. profile sets vf-pre
--   to enable nvidia-true-hdr), this becomes a positive feedback loop that
--   never converges — observed on customer logs as 167+ Apply/Restore cycles.
--
-- Fix: asymmetric hysteresis on Restore for profiles that depend on
-- `video-target-params`:
--   * FALSE -> TRUE: Apply immediately (no regression for first activation).
--   * TRUE  -> FALSE on a video-target-params-dependent profile: schedule a
--     RESTORE_STABILITY_DELAY-second timer. On expiry, re-evaluate cond.
--     If still FALSE -> Restore. If TRUE again -> cancel, no-op.
--   * Profiles that don't read video-target-params keep upstream behavior
--     (immediate Apply/Restore) so path/resolution-driven profiles stay
--     instant.
--
-- Tuning: 1.0s covers the longest observed first-cycle reconfig window
-- (~1.45s on hwdec=d3d11va-copy, but that's first-frame init; subsequent
-- per-cycle reconfig is <300ms in customer logs).
local RESTORE_STABILITY_DELAY = 1.0

local function profile_uses_target_params(profile)
    if not profile.properties then
        return false
    end
    for name, _ in pairs(profile.properties) do
        if name == "video-target-params" or
           name:find("^video%-target%-params/") then
            return true
        end
    end
    return false
end

local function cancel_restore_timer(profile)
    if profile.restore_timer then
        profile.restore_timer:kill()
        profile.restore_timer = nil
    end
end

local function do_apply(profile)
    msg.info("Applying auto profile: " .. profile.name)
    mp.commandv("apply-profile", profile.name)
    profile.status = true
end

local function do_restore(profile)
    msg.info("Restoring profile: " .. profile.name)
    mp.commandv("apply-profile", profile.name, "restore")
    profile.status = false
end

-- Re-run the profile condition. Returns true/false (errors counted as false,
-- matching upstream evaluate() semantics).
local function eval_cond(profile)
    current_profile = profile
    local ok, res = pcall(profile.cond)
    current_profile = nil
    if not ok then
        msg.verbose("Profile condition error on evaluating: " .. res)
        return false
    end
    return not not res
end

local function evaluate(profile)
    msg.verbose("Re-evaluating auto profile " .. profile.name)

    local res = eval_cond(profile)

    if res then
        -- TRUE: cancel any pending restore (cond came back before timer fired).
        if profile.restore_timer then
            msg.verbose("Cancelling pending restore for profile " .. profile.name
                        .. " (condition recovered)")
            cancel_restore_timer(profile)
        end
        if profile.status ~= true then
            do_apply(profile)
        end
    elseif profile.status == true and profile.has_restore_opt then
        if profile_uses_target_params(profile) then
            -- Debounce: only commit Restore if cond stays FALSE for the full
            -- delay window. This breaks the apply/restore feedback loop for
            -- profiles that read transient vo target params.
            if not profile.restore_timer then
                msg.verbose("Scheduling restore check for profile "
                            .. profile.name .. " in "
                            .. RESTORE_STABILITY_DELAY .. "s")
                profile.restore_timer = mp.add_timeout(RESTORE_STABILITY_DELAY,
                    function()
                        profile.restore_timer = nil
                        if profile.status ~= true then
                            return
                        end
                        if not eval_cond(profile) then
                            do_restore(profile)
                        else
                            msg.verbose("Restore deferred check for "
                                        .. profile.name
                                        .. ": condition true again, no-op")
                        end
                    end)
            end
        else
            -- Profile doesn't depend on volatile target params; preserve
            -- upstream immediate-restore semantics.
            do_restore(profile)
        end
    else
        -- profile.status was nil (initial) or already false; just settle to
        -- false without firing Restore (matches upstream behavior).
        profile.status = false
    end

    profile.dirty = false
end
-- === end Rodel.Player fork ================================================

local function on_property_change(name, val)
    cached_properties[name] = val
    -- Mark all profiles reading this property as dirty, so they get re-evaluated
    -- the next time the script goes back to sleep.
    local dependent_profiles = properties_to_profiles[name]
    if dependent_profiles then
        for profile, _ in pairs(dependent_profiles) do
            assert(profile.cond) -- must be a profile table
            profile.dirty = true
            have_dirty_profiles = true
        end
    end
end

local function on_idle()
    -- When events and property notifications stop, re-evaluate all dirty profiles.
    if have_dirty_profiles then
        for _, profile in ipairs(profiles) do
            if profile.dirty then
                evaluate(profile)
            end
        end
    end
    have_dirty_profiles = false
    -- Release all hooks (the point was to wait until an idle event)
    while true do
        local h = next(pending_hooks)
        if not h then
            break
        end
        pending_hooks[h] = nil
        h:cont()
    end
end

local function on_hook(h)
    h:defer()
    pending_hooks[h] = true
end

function get(name, default)
    -- Normally, we use the cached value only
    if not watched_properties[name] then
        watched_properties[name] = true
        local res, err = mp.get_property_native(name)
        -- Property has to not exist and the toplevel of property in the name must also
        -- not have an existing match in the property set for this to be considered an error.
        -- This allows things like user-data/test to still work.
        if err == "property not found" and property_set[name:match("^([^/]+)")] == nil then
            msg.error("Property '" .. name .. "' was not found.")
            return default
        end
        cached_properties[name] = res
        mp.observe_property(name, "native", on_property_change)
    end
    -- The first time the property is read we need add it to the
    -- properties_to_profiles table, which will be used to mark the profile
    -- dirty if a property referenced by it changes.
    if current_profile then
        local map = properties_to_profiles[name]
        if not map then
            map = {}
            properties_to_profiles[name] = map
        end
        map[current_profile] = true
        -- Rodel.Player fork: also record the dependency on the profile itself
        -- so profile_uses_target_params() can quickly check without iterating
        -- the global properties_to_profiles map.
        current_profile.properties[name] = true
    end
    local val = cached_properties[name]
    if val == nil then
        val = default
    end
    return val
end

local function magic_get(name)
    -- Lua identifiers can't contain "-", so in order to match with mpv
    -- property conventions, replace "_" to "-"
    name = string.gsub(name, "_", "-")
    return get(name, nil)
end

local evil_magic = {}
setmetatable(evil_magic, {
    __index = function(_, key)
        -- interpret everything as property, unless it already exists as
        -- a non-nil global value
        local v = _G[key]
        if type(v) ~= "nil" then
            return v
        end
        return magic_get(key)
    end,
})

p = {}
setmetatable(p, {
    __index = function(_, key)
        return magic_get(key)
    end,
})

local function compile_cond(name, s)
    local code, chunkname = "return " .. s, "profile " .. name .. " condition"
    local chunk, err
    -- luacheck: push
    -- luacheck: ignore setfenv loadstring
    if setfenv then -- lua 5.1
        chunk, err = loadstring(code, chunkname)
        if chunk then
            setfenv(chunk, evil_magic)
        end
    else -- lua 5.2
        chunk, err = load(code, chunkname, "t", evil_magic)
    end
    -- luacheck: pop
    if not chunk then
        msg.error("Profile '" .. name .. "' condition: " .. err)
        chunk = function() return false end
    end
    return chunk
end

local function load_profiles(profiles_property)
    for _, v in ipairs(profiles_property) do
        local cond = v["profile-cond"]
        if cond and #cond > 0 then
            local profile = {
                name = v.name,
                cond = compile_cond(v.name, cond),
                properties = {},
                status = nil,
                dirty = true, -- need re-evaluate
                has_restore_opt = v["profile-restore"] and v["profile-restore"] ~= "default"
            }
            profiles[#profiles + 1] = profile
            have_dirty_profiles = true
        end
    end
end

mp.observe_property("profile-list", "native", function (_, profiles_property)
    -- Rodel.Player fork: kill any pending restore timers from the previous
    -- profile set; the profile tables themselves are about to be replaced.
    for _, profile in ipairs(profiles) do
        cancel_restore_timer(profile)
    end

    profiles = {}
    watched_properties = {}
    cached_properties = {}
    properties_to_profiles = {}
    mp.unobserve_property(on_property_change)

    load_profiles(profiles_property)

    if #profiles < 1 and mp.get_property("load-auto-profiles") == "auto" then
        exit()
        return
    end

    on_idle() -- re-evaluate all profiles immediately
end)

mp.register_idle(on_idle)
for _, name in ipairs({"on_load", "on_preloaded", "on_loaded", "on_before_start_file"}) do
    mp.add_hook(name, 5, on_hook)
end
