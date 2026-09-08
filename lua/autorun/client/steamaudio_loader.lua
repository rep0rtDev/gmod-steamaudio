-- Loads the Steam Audio client module on startup. The module registers the
-- `steamaudio` table, the snd_sa_* convars and the snd_sa_* console commands.
-- steamaudio/emitsound.lua adds the Lua-side EmitSound/sound.Play wrappers.
if SERVER then return end

local ok, err = pcall(require, "steamaudio")
if not ok then
    MsgN("[steamaudio] module not loaded: " .. tostring(err))
    return
end

if file.Exists("steamaudio/emitsound.lua", "LUA") then
    include("steamaudio/emitsound.lua")
end

local sa = steamaudio
if not sa or not sa.GetBakeStatus then return end

local hookName = "steamaudio_bake_ui"
local state = { nextPoll = 0, lastId = 0, waiting = false, stopPending = false }
local phases = {
    idle = "Подготовка не требуется",
    preparing = "Подготовка отдельной акустической сцены",
    probes = "Размещение акустических проб",
    reflections = "Запекание отражений",
    awaiting_pathing = "Отражения готовы. Рассчитать маршруты звука?",
    pathing = "Построение маршрутов звука",
    saving = "Сохранение акустического кэша",
    ready = "Подготовка завершена",
    cancelled = "Запекание отменено",
    failed = "Не удалось завершить запекание"
}
local background = Color(20, 24, 31, 240)
local foreground = Color(230, 235, 244)
local secondary = Color(165, 180, 200)
local accent = Color(82, 163, 227)

local function duration(seconds)
    if not seconds or seconds < 0 then return "оценивается" end
    seconds = math.ceil(seconds)
    if seconds >= 3600 then
        return string.format("%d ч %02d мин", math.floor(seconds / 3600), math.floor(seconds / 60) % 60)
    end
    return string.format("%d:%02d", math.floor(seconds / 60), seconds % 60)
end

local function phaseText(status)
    local text = phases[status.phase] or status.phase
    if status.phase == "pathing" then text = text .. " — проход " .. tostring(status.pass) end
    return text
end

local function closeWindow()
    state.waiting = false
    if IsValid(state.window) then state.window:Remove() end
    state.window = nil
end

local function skipBake()
    if state.status and state.status.phase == "awaiting_pathing" then
        sa.ContinueBakePathing(false)
    else
        sa.CancelBake()
    end
    state.stopPending = true
    closeWindow()
end

local function openWindow()
    if IsValid(state.window) then
        state.window:MakePopup()
        return
    end
    if not state.status or not state.status.active then
        notification.AddLegacy("Steam Audio: сейчас запекание не идёт", NOTIFY_GENERIC, 4)
        return
    end
    local width = math.min(680, ScrW() - 32)
    local frame = vgui.Create("DFrame")
    state.window = frame
    state.waiting = true
    frame:SetSize(width, 390)
    frame:Center()
    frame:SetTitle("Steam Audio — подготовка акустики")
    frame:SetDraggable(false)
    frame:ShowCloseButton(false)
    frame:SetDeleteOnClose(true)
    frame:MakePopup()
    frame.OnRemove = function(panel)
        if state.window == panel then
            state.window = nil
            state.waiting = false
        end
    end
    frame.OnKeyCodePressed = function(_, key)
        if key == KEY_ESCAPE then skipBake() end
    end

    local function label(y, height, font)
        local panel = vgui.Create("DLabel", frame)
        panel:SetPos(20, y)
        panel:SetSize(width - 40, height)
        panel:SetFont(font or "DermaDefault")
        panel:SetTextColor(foreground)
        panel:SetWrap(true)
        return panel
    end
    local stage = label(45, 48, "DermaDefaultBold")
    local progress = vgui.Create("DPanel", frame)
    progress:SetPos(20, 100)
    progress:SetSize(width - 40, 28)
    progress.Paint = function(_, w, h)
        local status = state.status
        draw.RoundedBox(4, 0, 0, w, h, background)
        if not status then return end
        local value = math.Clamp(status.progress or 0, 0, 1)
        local text
        if status.phase == "awaiting_pathing" then
            draw.RoundedBox(4, 0, 0, w, h, accent)
            text = "Требуется выбор"
        elseif status.phase == "reflections" or status.phase == "pathing" then
            draw.RoundedBox(4, 0, 0, w * value, h, accent)
            text = string.format("%.1f%% текущего этапа", value * 100)
        else
            draw.RoundedBox(4, (math.sin(RealTime() * 2) + 1) * 0.4 * w, 0, w * 0.2, h, accent)
            text = phases[status.phase] or "Подготовка..."
        end
        draw.SimpleText(text, "DermaDefaultBold", w / 2, h / 2, color_white, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
    end
    local count = label(140, 34)
    local timing = label(178, 52)
    local note = label(234, 80)
    local skip = vgui.Create("DButton", frame)
    skip:SetPos(20, 334)
    skip:SetSize(width - 40, 36)
    skip.DoClick = skipBake
    local paths = vgui.Create("DButton", frame)
    paths:SetPos(width / 2 + 4, 334)
    paths:SetSize(width / 2 - 24, 36)
    paths:SetText("Рассчитать маршруты")
    paths:SetVisible(false)
    paths.DoClick = function()
        if sa.ContinueBakePathing(true) then paths:SetEnabled(false) end
    end
    frame.Think = function()
        local status = state.status
        if not status or not status.active then closeWindow() return end
        stage:SetText(phaseText(status) .. "\nТрассировщик: " .. tostring(status.backend))
        if status.phase == "reflections" then
            count:SetText(string.format("Проб обработано: %d / %d", status.completed, status.probes))
        elseif status.probes > 0 then
            count:SetText(string.format("Акустических проб: %d", status.probes))
        else
            count:SetText("Определяется объём акустических данных...")
        end
        local eta = duration(status.remaining)
        if status.progress >= 1 and status.active then eta = "финализация этапа" end
        if status.phase == "awaiting_pathing" then eta = "ожидается ваш выбор" end
        timing:SetText("Прошло: " .. duration(status.elapsed) .. "\nОсталось до конца этапа: " .. eta)
        local awaiting = status.phase == "awaiting_pathing"
        paths:SetVisible(awaiting)
        paths:SetEnabled(awaiting and status.pathing_allowed == true)
        skip:SetSize(awaiting and width / 2 - 24 or width - 40, 36)
        skip:SetText(awaiting and "Играть с готовыми отражениями" or "Пропустить и продолжить игру")
        if awaiting then
            local estimate = tonumber(status.pathing_estimated_mb) or 0
            local budget = tonumber(status.pathing_budget_mb) or 0
            local policy = status.pathing_allowed == true and "Маршруты необязательны." or "Запуск маршрутов заблокирован защитой памяти."
            note:SetText(string.format("Отражения готовы%s. Оценка памяти маршрутов: %.0f МиБ; бюджет с запасом для системы: %.0f МиБ. %s Можно играть с готовыми отражениями. Бюджет: snd_sa_pathing_memory_mb.",
                status.cache_saved and " и сохранены" or "", estimate, budget, policy))
        elseif status.phase == "pathing" then
            note:SetText(string.format("Память: прирост процесса %.0f МиБ / бюджет %.0f МиБ. При нехватке памяти запрашивается отмена; кэш отражений сохранится. %s",
                tonumber(status.pathing_growth_mb) or 0, tonumber(status.pathing_budget_mb) or 0, status.detail or ""))
        else
            note:SetText("Это первичная подготовка карты. Кэш сохранится для следующих входов. Оценка времени приблизительная и относится только к текущему этапу. Пропуск не отключает HRTF и окклюзию.")
        end
    end
end

hook.Add("Think", hookName, function()
    local now = RealTime()
    if now < state.nextPoll then return end
    state.nextPoll = now + 0.2
    local status = sa.GetBakeStatus()
    state.status = status
    if status.id ~= state.lastId then
        state.lastId = status.id
        state.stopPending = false
        if status.active and game.SinglePlayer() then
            openWindow()
        elseif status.active then
            notification.AddLegacy("Steam Audio: идёт подготовка карты. Окно: snd_sa_bake_status; отмена: snd_sa_bake_cancel", NOTIFY_GENERIC, 8)
        end
    end
    if not status.active then
        closeWindow()
        if state.wasActive then
            local text = phaseText(status)
            if status.phase == "failed" and status.detail ~= "" then text = text .. ": " .. status.detail end
            if status.memory_limited then text = "Отражения готовы. Расчёт маршрутов остановлен защитой памяти." end
            notification.AddLegacy(text, status.phase == "failed" and NOTIFY_ERROR or NOTIFY_GENERIC, 6)
        end
    end
    state.wasActive = status.active
end)

hook.Add("CreateMove", hookName, function(command)
    if state.waiting and IsValid(state.window) and state.window:IsVisible() then
        command:ClearMovement()
        command:ClearButtons()
    end
end)

hook.Add("HUDPaint", hookName, function()
    local status = state.status
    if not status or not status.active or state.waiting then return end
    local width, height = math.min(440, ScrW() - 32), 106
    local x, y = ScrW() - width - 16, 24
    draw.RoundedBox(8, x, y, width, height, background)
    local title = status.phase == "awaiting_pathing" and "Ожидается выбор: маршруты" or phaseText(status)
    draw.SimpleText("Steam Audio: " .. title, "DermaDefaultBold", x + 12, y + 12, foreground)
    local progress = math.Clamp(status.progress or 0, 0, 1)
    draw.RoundedBox(3, x + 12, y + 38, width - 24, 7, Color(50, 58, 70))
    draw.RoundedBox(3, x + 12, y + 38, (width - 24) * progress, 7, accent)
    local text = state.stopPending and "Отмена запрошена..." or
        string.format("%.1f%%  |  прошло %s  |  этап: ещё %s", progress * 100, duration(status.elapsed), duration(status.remaining))
    if status.phase == "awaiting_pathing" then
        text = status.pathing_allowed == true and "Отражения готовы; маршруты ожидают вашего выбора" or
            "Отражения готовы; маршруты заблокированы защитой памяти"
    end
    draw.SimpleText(text, "DermaDefault", x + 12, y + 54, secondary)
    draw.SimpleText("Окно: snd_sa_bake_status  |  Отмена: snd_sa_bake_cancel", "DermaDefault", x + 12, y + 78, secondary)
end)

hook.Add("ShutDown", hookName, closeWindow)
concommand.Add("snd_sa_bake_status", openWindow, nil, "Open the acoustic preparation progress window")
