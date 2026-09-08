-- Thin wrappers over the vanilla sound API that attach Steam Audio per-sound
-- parameters to the sound they start. Playback itself is untouched: the
-- engine decodes and mixes exactly as without the module; the parameters
-- only steer how the module spatializes the captured PCM.
--
-- The `params` table takes the same keys as steamaudio.SetSoundParams:
--   gain/volume, soundlevel|distmult, radius, dipole_weight, dipole_power,
--   air_absorption, reverb_gain, pos, forward|angles, spatialize, occlusion,
--   transmission, reflections, pathing.
-- Keys that are absent keep the engine-derived value.
if SERVER then return end

local steamaudio = steamaudio
if not steamaudio then return end

local CHAN_AUTO = CHAN_AUTO or 0
local ANY_CHANNEL = -1
local EXPECT_TTL = 1.0

local function native(name)
    local fn = steamaudio[name]
    return isfunction(fn) and fn or nil
end

-- Attaches `params` to the sound that `play()` starts.
-- Preferred key: the GUID the engine reports for the last emitted sound, if it
-- changed across the call. Fallback: a one-shot expectation for the next new
-- sound from `entIndex` (optionally restricted to `channel`).
-- Returns the GUID (0 when unknown) and the key used: "guid", "expect", "none".
local function playWithParams(entIndex, channel, params, play)
    local getGuid = native("GetLastSoundGuid")
    local setParams = native("SetSoundParams")
    local expect = native("ExpectSound")
    if not istable(params) or not (setParams or expect) then
        play()
        return 0, "none"
    end
    if channel == nil or channel == CHAN_AUTO then
        channel = ANY_CHANNEL
    end
    local before = getGuid and getGuid() or 0
    -- Registered before playback so the very first poll already sees it; if
    -- the GUID path below succeeds this expectation is consumed by the same
    -- sound and the GUID override just layers on top of it.
    if expect and entIndex ~= nil then
        expect(entIndex, channel, params, EXPECT_TTL)
    end
    play()
    local after = getGuid and getGuid() or 0
    if setParams and after ~= 0 and after ~= before then
        setParams(after, params)
        return after, "guid"
    end
    if expect and entIndex ~= nil then
        return 0, "expect"
    end
    return 0, "none"
end

-- Entity:EmitSound with Steam Audio parameters.
-- steamaudio.EmitSound(ent, soundName, soundLevel, pitch, volume, channel, flags, dsp, params)
function steamaudio.EmitSound(ent, soundName, soundLevel, pitch, volume, channel, flags, dsp, params)
    if not IsValid(ent) then return 0, "none" end
    return playWithParams(ent:EntIndex(), channel, params, function()
        ent:EmitSound(soundName, soundLevel, pitch, volume, channel, flags, dsp)
    end)
end

-- sound.Play with Steam Audio parameters (world sound at `pos`).
-- steamaudio.PlayWorldSound(soundName, pos, soundLevel, pitch, volume, params)
function steamaudio.PlayWorldSound(soundName, pos, soundLevel, pitch, volume, params)
    if istable(params) and params.pos == nil then
        params = table.Copy(params)
        params.pos = pos
    end
    return playWithParams(0, ANY_CHANNEL, params, function()
        sound.Play(soundName, pos, soundLevel, pitch, volume)
    end)
end

-- Entity:StopSound + drop any GUID/entity overrides that belonged to it.
function steamaudio.StopSound(ent, soundName)
    if not IsValid(ent) then return end
    ent:StopSound(soundName)
    local clearEntity = native("ClearEntitySoundParams")
    if clearEntity then clearEntity(ent:EntIndex()) end
end

-- Persistent per-entity parameters: every engine sound from `ent` that has no
-- more specific (GUID) override gets these.
function steamaudio.SetEntityParams(ent, params)
    local set = native("SetEntitySoundParams")
    if not set or not IsValid(ent) then return false end
    set(ent:EntIndex(), params)
    return true
end

function steamaudio.ClearEntityParams(ent)
    local clear = native("ClearEntitySoundParams")
    if not clear or not IsValid(ent) then return false end
    return clear(ent:EntIndex())
end

-- Broad rules by sample path glob ("weapons/*", "*.mp3", "npc/zombie/*").
-- Optional `channel` (CHAN_*) restricts the rule; higher `priority` wins.
-- Returns the rule id for steamaudio.RemoveSoundRule.
function steamaudio.Rule(pattern, params, priority, channel)
    local add = native("AddSoundRule")
    if not add then return nil end
    return add(pattern, params, { priority = priority or 0, channel = channel or ANY_CHANNEL })
end
