--[[
Compatibility layer: lets addons written for the Ashita v4 addon API run on VanaCore, by putting
that API's names (ashita.*, AshitaCore, ...) over vana.*. Runs after vana.lua in every addon.

It covers what the addons tried so far use; the rest of the API is added as addons need it. Calls
to parts that aren't here yet fail with a clear error instead of misbehaving.
--]]

local vana = vana
local ffi = require('ffi')
local getinfo = debug.getinfo -- kept: an addon may replace `debug` (SimpleLog sets it to false)

-- Ashita's Lua (moonjit 2.2) carries Lua 5.2 library functions that LuaJIT 2.1 as built here doesn't (apidump, 2026-10-08:
-- table.pack, table.unpack, package.searchers). Addons written there use them: HXUI's progress bar calls table.pack.
if table.unpack == nil then
    table.unpack = unpack
end
if table.pack == nil then
    table.pack = function (...) return { n = select('#', ...), ... } end
end
if package.searchers == nil then
    package.searchers = package.loaders
end

-- Their gdifonts library (FancyChat's text, xivbar, AMANTracker) tells its DLL where its fonts are with
-- renderer.SetFontFolder(string.gsub(path, 'include.lua', '')): gsub's second result is an argument too many, which
-- LuaJIT refuses ("wrong number of arguments") and their pcall hides. In Ashita the DLL guesses the folder from the
-- game's own (exe\..\addons\fancychat\gdifonts), which is where it is in Ashita's layout and isn't in VanaCore's (addons
-- are in addons\ashita\): Consolas, which the folder carries, then comes out as no texture at all and the text isn't
-- drawn. So the call is made with its one argument.
-- Some addons (AMANTracker) ship their gdifonts without the DLL, expecting Ashita's own copy to be found; when
-- the one asked for isn't there, the same library from another addon that carries it is loaded instead.
do
    local load = ffi.load
    local function exists(path)
        local f = io.open(path, 'rb')
        if f then f:close() return true end
        return false
    end
    local function fallback(name)
        local root = name:match('^(.*[\\/]addons[\\/][^\\/]+[\\/])[^\\/]+[\\/]')
        if root == nil then return nil end
        for _, relative in ipairs({ 'fancychat/gdifonts/', 'xivbar/libs/gdifonts/', 'FancyChat/gdifonts/', 'xivbar/gdifonts/' }) do
            local path = root .. relative .. 'gdifonttexture.dll'
            if exists(path) then return path end
        end
        return nil
    end
    ffi.load = function (name, ...)
        local is_gdi = type(name) == 'string' and name:lower():find('gdifonttexture', 1, true) ~= nil
        local ok, library = pcall(load, name, ...)
        if not ok then
            local alternative = is_gdi and not exists(name) and fallback(name)
            if not alternative then error(library, 0) end
            library = load(alternative, ...)
        end
        if not is_gdi then
            return library
        end
        return setmetatable({}, {
            __index = function (self, key)
                local value = library[key]
                if key == 'SetFontFolder' then
                    local call = value
                    value = function (folder) return call(folder) end
                end
                rawset(self, key, value)
                return value
            end,
        })
    end
end

pcall(ffi.cdef, [[
    typedef struct _VC_XINPUT_GAMEPAD {
        uint16_t wButtons;
        uint8_t  bLeftTrigger;
        uint8_t  bRightTrigger;
        int16_t  sThumbLX;
        int16_t  sThumbLY;
        int16_t  sThumbRX;
        int16_t  sThumbRY;
    } VC_XINPUT_GAMEPAD;
    typedef struct _VC_XINPUT_STATE {
        uint32_t          dwPacketNumber;
        VC_XINPUT_GAMEPAD Gamepad;
    } VC_XINPUT_STATE;
    typedef struct _VC_DIJOYSTATE {
        int32_t  lX;
        int32_t  lY;
        int32_t  lZ;
        int32_t  lRx;
        int32_t  lRy;
        int32_t  lRz;
        int32_t  rglSlider[2];
        uint32_t rgdwPOV[4];
        uint8_t  rgbButtons[32];
    } VC_DIJOYSTATE;
    typedef struct _VC_DIJOYSTATE2 {
        int32_t  lX;
        int32_t  lY;
        int32_t  lZ;
        int32_t  lRx;
        int32_t  lRy;
        int32_t  lRz;
        int32_t  rglSlider[2];
        uint32_t rgdwPOV[4];
        uint8_t  rgbButtons[128];
        int32_t  lVX;
        int32_t  lVY;
        int32_t  lVZ;
        int32_t  lVRx;
        int32_t  lVRy;
        int32_t  lVRz;
        int32_t  rglVSlider[2];
        int32_t  lAX;
        int32_t  lAY;
        int32_t  lAZ;
        int32_t  lARx;
        int32_t  lARy;
        int32_t  lARz;
        int32_t  rglASlider[2];
        int32_t  lFX;
        int32_t  lFY;
        int32_t  lFZ;
        int32_t  lFRx;
        int32_t  lFRy;
        int32_t  lFRz;
        int32_t  rglFSlider[2];
    } VC_DIJOYSTATE2;
]])

-- Their events as VanaCore's: the event VanaCore raises, and the handler for it that calls theirs with
-- what they expect (a table they can set `blocked` in; a scene's begin and end with whether it's on the back buffer, as
-- the game's scenes happen). The rest (command, packet_in, packet_out, load, unload) are as they are.
local event_adapters = {
    d3d_present = function (fn) return 'present', fn end,
    d3d_beginscene = function (fn) return 'beginscene', function (back_buffer) return fn(back_buffer) end end,
    d3d_endscene = function (fn) return 'endscene', function (back_buffer) return fn(back_buffer) end end,
    xinput_state = function (fn)
        return 'xinput_state', function (e)
            if type(e) == 'table' then
                if type(e.state) == 'userdata' then
                    local ok, ptr = pcall(ffi.cast, 'VC_XINPUT_STATE*', e.state)
                    if ok and ptr ~= nil then e.state = ptr end
                end
                if type(e.state_modified) == 'userdata' then
                    local ok, ptr = pcall(ffi.cast, 'VC_XINPUT_STATE*', e.state_modified)
                    if ok and ptr ~= nil then e.state_modified = ptr end
                end
            end
            return fn(e)
        end
    end,
    dinput_state = function (fn)
        return 'dinput_state', function (e)
            if type(e) == 'table' then
                local ctype = (tonumber(e.size) or 0) >= 272 and 'VC_DIJOYSTATE2*' or 'VC_DIJOYSTATE*'
                if type(e.state) == 'userdata' then
                    local ok, ptr = pcall(ffi.cast, ctype, e.state)
                    if ok and ptr ~= nil then e.state = ptr end
                end
                if type(e.state_modified) == 'userdata' then
                    local ok, ptr = pcall(ffi.cast, ctype, e.state_modified)
                    if ok and ptr ~= nil then e.state_modified = ptr end
                end
            end
            return fn(e)
        end
    end,
    mouse = function (fn)
        return 'mouse', function (message, x, y, delta, blocked)
            local e = { message = message + 0x200, x = x, y = y, delta = delta, blocked = blocked }
            fn(e)
            return e.blocked == true
        end
    end,
    key = function (fn)
        return 'key', function (wparam, lparam, blocked)
            local e = { wparam = wparam, lparam = lparam, blocked = blocked }
            fn(e)
            return e.blocked == true
        end
    end,
}

local function event_name(name)
    name = tostring(name):lower()
    local adapter = event_adapters[name]
    return adapter and (adapter(function () end)) or name
end

local function event_handler(name, fn)
    local adapter = event_adapters[tostring(name):lower()]
    if adapter == nil then
        return fn
    end
    local _, handler = adapter(fn)
    return handler
end

-- One of their objects or namespaces: what the Lua here has, then the SDK's methods on VanaCore's C++ object behind it
-- (sdk_fallback). A name neither has reads nil, as an unknown name on Ashita's own objects does: addons feature-test
-- (`player.HasPetCommand and player:HasPetCommand(id)`) and some read fields that were never there (XIUI's
-- `player.isZoning`, nil in Ashita too, so its check passes). Raising here broke what runs in Ashita; a call to such a
-- name still fails as it would there ("attempt to call method ... (a nil value)"). Each one is logged once, with the
-- line that read it, for whoever ports the next addon.
local sdk_of = setmetatable({}, { __mode = 'k' }) -- a partial table: the SDK methods behind it (sdk_fallback)
local unknown_seen = {}

local function partial(name, t)
    return setmetatable(t or {}, {
        __index = function (self, key)
            local sdk = sdk_of[self]
            local method = sdk ~= nil and rawget(sdk, key) or nil
            if method ~= nil then
                return method
            end
            local what = name .. '.' .. tostring(key)
            if not unknown_seen[what] then
                unknown_seen[what] = true
                local caller = getinfo(2, 'Sl')
                vana.log(('%s isn\'t in Ashita\'s API or VanaCore\'s: it reads nil (%s:%s)'):format(what,
                    caller and caller.short_src or '?', caller and caller.currentline or '?'))
            end
            return nil
        end,
    })
end

ashita = {}

-- Their handlers can wait (LuAshitacast's packet handler calls coroutine.sleep), so each runs as a
-- task: straight away, and if it waits, the rest of it runs later. Returns what it returned if it
-- finished without waiting.
local function tasked(fn)
    return function (...)
        local finished, result = vana.tasks.spawn(fn, ...)
        return finished and result or nil
    end
end

-- The Addons plugin's fields, as the interface [addons] ashita.interface picks has them (vana.ashita.interface): 4.16's
-- Addons (CatsEyeXI's Addons.dll) sets interface_version 4.16 and addons_version 2.1, 4.30's 4.3 and 4.1 (nil was
-- Ashita 4.0's). addons_version: Ashita's Text and the other printf-style ImGui calls took a format string before
-- Addons 2.2 (XIUI's mobdb writes '%%%%' for a '%' then, '%%' after), and VanaCore's take theirs to match.
-- interface_version: tCrossBar takes XInput's triggers and sticks as 16-21 with it set (32-37 without, which nothing
-- raises) and passes D3DX_DEFAULT for its icons' size; XIUI's ImGui 1.92 paths want 4.3 or later.
local INTERFACE = (vana.ashita.interface or 4.30) > 4.2 and 4.30 or 4.16
ashita.addons_version = INTERFACE == 4.30 and 4.1 or 2.1
ashita.interface_version = INTERFACE == 4.30 and 4.3 or 4.16

-- The libraries Ashita addons load are Ashita 4.30's whatever the interface ([addons] ashita.libs, vana.ashita.libs:
-- 4.30 unless 4.16 says CatsEyeXI's Ashita's own). 4.30's have everything 4.16's had, and more (chat.color, reflex, ...),
-- except ImGui 1.81's names that 1.92 renamed or dropped, which 4.30's imgui library doesn't define. They're defined
-- here, before the addon runs, as ImGui 1.92 itself still has them (its obsolete names: a renamed one as its new name's
-- value, the two window flags BeginChild turns into child flags as 1.92's own obsolete bits, a dropped one as 0), so an
-- addon written for 4.16 finds ImGuiCol_TabActive and ImDrawCornerFlags_All on 4.30's library, which leaves these as
-- they are. An addon loading 4.16's library (ashita.libs=4.16) gets every one of these from it instead, with 1.81's
-- numbers, which lua_imgui.cpp turns into 1.92's.
if (vana.ashita.libs or 4.30) > 4.2 then
    ImGuiWindowFlags_AlwaysUseWindowPadding    = bit.lshift(1, 30) -- 1.92's obsolete bit: BeginChild makes it ImGuiChildFlags_AlwaysUseWindowPadding
    ImGuiWindowFlags_NavFlattened              = bit.lshift(1, 29) -- as above, ImGuiChildFlags_NavFlattened
    ImGuiCol_TabActive                         = 36 -- ImGuiCol_TabSelected
    ImGuiCol_TabUnfocused                      = 38 -- ImGuiCol_TabDimmed
    ImGuiCol_TabUnfocusedActive                = 39 -- ImGuiCol_TabDimmedSelected
    ImGuiCol_NavHighlight                      = 56 -- ImGuiCol_NavCursor
    ImGuiInputTextFlags_AlwaysInsertMode       = bit.lshift(1, 11) -- ImGuiInputTextFlags_AlwaysOverwrite
    ImGuiInputTextFlags_Multiline              = bit.lshift(1, 26) -- InputTextMultiline's own, as in 1.81
    ImGuiInputTextFlags_NoMarkEdited           = 0 -- gone
    ImGuiTreeNodeFlags_AllowItemOverlap        = bit.lshift(1, 2) -- ImGuiTreeNodeFlags_AllowOverlap
    ImGuiTreeNodeFlags_NavLeftJumpsBackHere    = bit.lshift(1, 17) -- ImGuiTreeNodeFlags_NavLeftJumpsToParent
    ImGuiSelectableFlags_DontClosePopups       = bit.lshift(1, 0) -- ImGuiSelectableFlags_NoAutoClosePopups
    ImGuiSelectableFlags_AllowItemOverlap      = bit.lshift(1, 4) -- ImGuiSelectableFlags_AllowOverlap
    ImGuiTabBarFlags_FittingPolicyResizeDown   = bit.lshift(1, 8) -- ImGuiTabBarFlags_FittingPolicyShrink
    ImGuiDragDropFlags_SourceAutoExpirePayload = bit.lshift(1, 5) -- ImGuiDragDropFlags_PayloadAutoExpire
    ImGuiButtonFlags_MouseButtonDefault_       = bit.lshift(1, 0) -- ImGuiButtonFlags_MouseButtonLeft
    ImGuiColorEditFlags_AlphaPreview           = 0 -- the default since 1.91.8
    -- ImGuiColorEditFlags_DefaultOptions_ (Uint8, DisplayRGB, InputRGB, PickerHueBar), DisplayMask_, DataTypeMask_,
    -- PickerMask_ and InputMask_
    ImGuiColorEditFlags__OptionsDefault        = bit.bor(bit.lshift(1, 23), bit.lshift(1, 20), bit.lshift(1, 27), bit.lshift(1, 25))
    ImGuiColorEditFlags__DisplayMask           = bit.bor(bit.lshift(1, 20), bit.lshift(1, 21), bit.lshift(1, 22))
    ImGuiColorEditFlags__DataTypeMask          = bit.bor(bit.lshift(1, 23), bit.lshift(1, 24))
    ImGuiColorEditFlags__PickerMask            = bit.bor(bit.lshift(1, 26), bit.lshift(1, 25))
    ImGuiColorEditFlags__InputMask             = bit.bor(bit.lshift(1, 27), bit.lshift(1, 28))
    ImGuiConfigFlags_NavEnableSetMousePos      = bit.lshift(1, 2) -- 1.92's obsolete bit (io.ConfigNavMoveSetMousePos)
    ImGuiConfigFlags_NavNoCaptureKeyboard      = bit.lshift(1, 3) -- 1.92's obsolete bit (io.ConfigNavCaptureKeyboard)
    ImGuiKey_KeyPadEnter                       = 627 -- ImGuiKey_KeypadEnter
    ImGuiKey_COUNT                             = 667 -- ImGuiKey_NamedKey_END, as 1.92 has it
    ImGuiKeyModFlags_None                      = 0 -- ImGuiMod_None, and the rest ImGuiMod_
    ImGuiKeyModFlags_Ctrl                      = bit.lshift(1, 12)
    ImGuiKeyModFlags_Shift                     = bit.lshift(1, 13)
    ImGuiKeyModFlags_Alt                       = bit.lshift(1, 14)
    ImGuiKeyModFlags_Super                     = bit.lshift(1, 15)
    -- 1.81's ImDrawCornerFlags, as 1.92's ImDrawFlags_RoundCorners
    ImDrawCornerFlags_None                     = bit.lshift(1, 8) -- ImDrawFlags_RoundCornersNone (0 is all corners in 1.92)
    ImDrawCornerFlags_TopLeft                  = bit.lshift(1, 4)
    ImDrawCornerFlags_TopRight                 = bit.lshift(1, 5)
    ImDrawCornerFlags_BotLeft                  = bit.lshift(1, 6)
    ImDrawCornerFlags_BotRight                 = bit.lshift(1, 7)
    ImDrawCornerFlags_Top                      = 0x30
    ImDrawCornerFlags_Bot                      = 0xC0
    ImDrawCornerFlags_Left                     = 0x50
    ImDrawCornerFlags_Right                    = 0xA0
    ImDrawCornerFlags_All                      = 0xF0
    -- the nav inputs 1.92 has no table of (io.NavInputs is gone): nothing reads them
    for _, name in ipairs({ 'Activate', 'Cancel', 'Input', 'Menu', 'DpadLeft', 'DpadRight', 'DpadUp', 'DpadDown',
                            'LStickLeft', 'LStickRight', 'LStickUp', 'LStickDown', 'FocusPrev', 'FocusNext', 'TweakSlow',
                            'TweakFast', 'KeyMenu_', 'KeyLeft_', 'KeyRight_', 'KeyUp_', 'KeyDown_', 'COUNT',
                            'InternalStart_' }) do
        _G['ImGuiNavInput_' .. name] = 0
    end
end

-- SimpleLog (its folder's name): its check for a buffer the server sent again (lib\packethandlers.lua) blocks real
-- actions. record_packets keeps each buffer's packets and check_duplicates blocks a received 0x028 found among the last
-- three buffers'. A buffer starts, for it, at a packet whose bytes are the buffer's first packet's, so a copy of that
-- packet further on in the same buffer (the server sends the same 0x037, 0x076 or 0x0DF more than once in a buffer, as a
-- party's buffs land) starts it over part way: the buffer's own packets join the last three, and every action after the
-- copy is blocked as a copy of itself. The game never gets it: a spell cast with no casting and no line, a weapon skill
-- that takes the TP and shows nothing (XIUI's cast bar, which doesn't look at `blocked`, still fills). Ashita's chunk is
-- the same, so it does this there too. With [client] simplelog.fix (on unless the settings file turns it off; read as
-- each buffer comes) the two are swapped for these, which start a buffer when VanaCore's number for it (e.vana_chunk)
-- moves on, so a buffer sent again is still blocked and nothing else is. Off, SimpleLog's own are put back.
local simplelog_fix = nil
if tostring(addon and addon.name or ''):lower() == 'simplelog' then
    simplelog_fix = function (handler)
        local chunk, current, previous = nil, nil, {} -- the buffer's packets (a set), and the last three's
        local function record(e)
            if e.vana_chunk == chunk then
                return
            end
            chunk = e.vana_chunk
            if current ~= nil then
                table.insert(previous, 1, current)
                previous[4] = nil
            end
            current = {}
            local data = e.chunk_data or ''
            local size_of, at = math.min(#data, tonumber(e.chunk_size) or 0), 0
            while at + 4 <= size_of do
                local size = ashita.bits.unpack_be(data, at, 9, 7) * 4
                if size < 4 or at + size > size_of then
                    break
                end
                current[data:sub(at + 1, at + size)] = true
                at = at + size
            end
        end
        local function check(e)
            for _, packets in ipairs(previous) do
                if packets[e.data] then
                    e.blocked = true
                    return true
                end
            end
            return false
        end
        local theirs, read_at, on = nil, nil, true
        return function (e)
            if e.vana_chunk ~= nil and e.vana_chunk ~= read_at then
                read_at = e.vana_chunk
                on = (tonumber(vana.config.get('client', 'simplelog.fix') or '') or 1) ~= 0
            end
            if on and e.vana_chunk ~= nil then
                if theirs == nil and type(rawget(_G, 'record_packets')) == 'function' and
                   type(rawget(_G, 'check_duplicates')) == 'function' then
                    theirs = { record = record_packets, check = check_duplicates }
                    record_packets, check_duplicates = record, check
                end
            elseif theirs ~= nil then
                record_packets, check_duplicates = theirs.record, theirs.check
                theirs = nil
            end
            return handler(e)
        end
    end
end

ashita.events = {
    register = function (name, key, fn)
        if simplelog_fix ~= nil and tostring(name):lower() == 'packet_in' then
            fn = simplelog_fix(fn)
        end
        vana.events.on(event_name(name), event_handler(name, tasked(fn)), key)
    end,
    unregister = function (name, key)
        vana.events.off(event_name(name), key)
    end,
}

local memory = {}
for name, fn in pairs(vana.memory) do
    memory[name] = fn
end
memory.dealloc = vana.memory.free
-- Ashita 4.3's find takes 0 (or nil) for the module: the game's own, FFXiMain.dll (Boussole's zone/sub-zone sig,
-- ScentHound's, XIUI's macros and mobdb all call find(0, 0, ...)). vana.memory.find reads a number as an address.
-- Ashita 4.30 also takes two tokens for a module: ':ffximain' (FFXiMain.dll) and ':boot', the exe the game runs in.
local find = vana.memory.find
local function is_boot(module)
    return INTERFACE == 4.30 and type(module) == 'string' and module:lower() == ':boot'
end
memory.find = function (module, size, ...)
    if module == nil or module == 0 or (INTERFACE == 4.30 and type(module) == 'string' and module:lower() == ':ffximain') then
        module = 'FFXiMain.dll'
    elseif is_boot(module) then
        local base, whole = vana.memory.module()
        if base == nil then
            return 0
        end
        return find(base, (tonumber(size) or 0) > 0 and size or whole, ...)
    end
    return find(module, size, ...)
end
-- get_base(module) and get_size(module): where a module is loaded and how much of memory it takes
-- (vana.memory.module gives both). 0 or nil is the game's own, as for find; 0 for a module that isn't loaded.
local function module_range(module)
    if module == nil or module == 0 or (INTERFACE == 4.30 and type(module) == 'string' and module:lower() == ':ffximain') then
        module = 'FFXiMain.dll'
    end
    local base, size
    if is_boot(module) then
        base, size = vana.memory.module()
    else
        base, size = vana.memory.module(module)
    end
    return base or 0, size or 0
end
memory.get_base = function (module) return (module_range(module)) end
memory.get_size = function (module) return select(2, module_range(module)) end
-- read_literal(address, size): `size` bytes as a string, NULs and all (read_string stops at the first); nil for no bytes
-- or memory that can't be read, as the Addons plugin has it
memory.read_literal = function (address, size)
    size = tonumber(size) or 0
    if size <= 0 then
        return nil
    end
    return vana.memory.read_bytes(address, size)
end
-- ashita.memory.assembler, Ashita 4.30's (4.16's Addons has none): x86 assembly text, read as AsmTK reads it.
-- assemble(text) gives its bytes as a table from 1 (nil if it doesn't assemble); cave(text) puts it in executable memory
-- of its own and gives the address; release(address) frees that; call/jmp(address, target, nops) and nop(address, count)
-- write those instructions over the game's code.
memory.assembler = nil
if INTERFACE == 4.30 then
    local assembler = vana.memory.assembler
    memory.assembler = partial('ashita.memory.assembler', {
        assemble = function (text, base)
            local code = assembler.assemble(tostring(text), base)
            if code == nil then
                return nil
            end
            local bytes = {}
            for i = 1, #code do
                bytes[i] = code:byte(i)
            end
            return bytes
        end,
        cave = function (text) return (assembler.cave(tostring(text))) end,
        release = function (address) assembler.release(address) end,
        call = assembler.call,
        jmp = assembler.jmp,
        nop = assembler.nop,
    })
end
ashita.memory = partial('ashita.memory', memory)

-- Paths their code builds for Ashita's own layout, pointed at VanaCore's: <install>\addons\x (Ashita's
-- addons folder; tCrossBar looks for '%saddons/%s/resources/layouts/', ssl.lua for '%s/addons/libs/socket/')
-- is <install>\addons\ashita\x here. Paths already under addons\ashita, \windower or \vanacore stay. Runs of slashes
-- count as one: GetInstallPath ends in a backslash, so Boussole's '%s\\addons\\boussole\\data\\homepoint.xml' has two.
-- VanaCore.Core.dll's own file imports apply the same rule to everything else an addon reaches Windows with
-- (compat/ashita/paths.cpp: io.popen and os.execute's command lines, os.remove, ffi.load, ...).
local remap
do
    local function squash(path) -- slashes as backslashes, a run of them as one (past the first: a UNC path's \\ stays)
        local p = path:gsub('/', '\\')
        local rest = p:sub(2):gsub('\\\\+', '\\')
        return p:sub(1, 1) .. rest
    end
    local from = squash(vana.paths.addons .. '\\'):lower()
    local to = vana.paths.addons .. '\\ashita\\'
    local kinds = { ashita = true, windower = true, vanacore = true }
    remap = function (path)
        if type(path) ~= 'string' then
            return path
        end
        local s = squash(path)
        local n = s:lower()
        if n:sub(1, #from) ~= from then
            return path
        end
        local first = n:sub(#from + 1):match('^[^\\]*')
        if first == '' or kinds[first] then
            return path
        end
        return to .. s:sub(#from + 1)
    end
    local open, lines, run, load = io.open, io.lines, dofile, loadfile
    io.open = function (path, ...) return open(remap(path), ...) end
    io.lines = function (path, ...) return lines(remap(path), ...) end
    dofile = function (path) return run(remap(path)) end
    loadfile = function (path, ...) return load(remap(path), ...) end
end

-- Ashita's ashita.fs (its annotations/ashita/fs.lua lists it): paths through remap. normalize, equivalent, size, space
-- and status are std::filesystem's; Lua has what they need.
local function normalize(path)
    path = tostring(remap(path)):gsub('/', '\\')
    local parts = {}
    for part in path:gmatch('[^\\]+') do
        if part == '..' and #parts > 0 and parts[#parts] ~= '..' then
            table.remove(parts)
        elseif part ~= '.' and part ~= '' then
            parts[#parts + 1] = part
        end
    end
    local out = table.concat(parts, '\\')
    if path:match('^\\\\') then
        out = '\\\\' .. out
    elseif path:match('^\\') then
        out = '\\' .. out
    end
    return out
end
local function file_size(path)
    local f = io.open(remap(path), 'rb')
    if f == nil then
        return 0
    end
    local size = f:seek('end') or 0
    f:close()
    return size
end
local function install_folder(game)
    local folder = vana.paths.game
    if folder == nil or folder == '' then
        return vana.paths.install
    end
    if tonumber(game) == 0 then
        local pol = folder:gsub('[^\\]+$', '') .. 'PlayOnlineViewer'
        if vana.fs.exists(pol) then
            return pol
        end
    end
    return folder
end
local function status(path)
    path = remap(path)
    local exists = vana.fs.exists(path)
    local dir = exists and vana.fs.is_dir(path) or false
    return {
        permissions = exists and 0x1FF or 0, exists = exists, is_regular_file = exists and not dir, is_directory = dir,
        is_block_file = false, is_character_file = false, is_fifo = false, is_socket = false, is_symlink = false,
    }
end
-- (path, mask, subfolders), as Addons.dll has it (4.16 and 4.30 alike): nil unless path is a folder; else the names
-- matching mask (a regular expression for the whole name, case counting; all when nil). Folders always; files unless
-- mask and subfolders are both nil, so XIUI's get_directory(dir) gets its job theme folders and its
-- get_directory(dir, '.*\\.png$') its icons. With subfolders true it goes into every subfolder, and the names below
-- the top are paths from path ('sub\\name'): XIIM's get_dir(dir, '.*', true) finds its character folders.
local function get_directory(path, mask, subfolders)
    path = remap(path)
    if type(path) ~= 'string' or path == '' or not vana.fs.is_dir(path) then
        return nil
    end
    local which = mask == nil and subfolders == nil and 'folders' or nil
    return vana.fs.list(path, mask, { which = which, deep = subfolders == true, case = true })
end
ashita.fs = partial('ashita.fs', {
    preferred_separator = '\\',
    exists = function (path) return vana.fs.exists(remap(path)) end,
    create_dir = function (path) return vana.fs.create_dir(remap(path)) end,
    create_directory = function (path) return vana.fs.create_dir(remap(path)) end,
    remove = function (path) return vana.fs.remove(remap(path)) end,
    rename = function (from, to) return os.rename(remap(from), remap(to)) == true end,
    get_directory = get_directory,
    get_dir = get_directory,
    -- (language, game): the game's folder from the registry in Ashita (0 PlayOnline, 1 FFXI, 2 Tetra Master); here
    -- the folder FFXiMain.dll runs from (their dats library reads FTABLE.DAT and the ROMs under it), PlayOnline's beside
    -- it when it's there, VanaCore's own without a game
    get_install_dir = function (_, game) return install_folder(game) end,
    get_install_directory = function (_, game) return install_folder(game) end,
    current_dir = function () return vana.paths.install end,
    current_directory = function () return vana.paths.install end,
    normalize = normalize,
    equals = function (a, b) return normalize(a):lower() == normalize(b):lower() end,
    equivalent = function (a, b) return normalize(a):lower() == normalize(b):lower() end,
    size = file_size,
    -- (path): the disk's space in bytes, as std::filesystem::space has it (0s for a path that isn't there)
    space = function (path)
        local available, capacity, free = ffi.new('uint64_t[1]'), ffi.new('uint64_t[1]'), ffi.new('uint64_t[1]')
        if ffi.C.GetDiskFreeSpaceExA(remap(tostring(path or '.')), available, capacity, free) == 0 then
            return { available = 0, capacity = 0, free = 0 }
        end
        return { available = tonumber(available[0]), capacity = tonumber(capacity[0]), free = tonumber(free[0]) }
    end,
    status = status,
})

ashita.tasks = vana.tasks

-- The Windows calls behind ashita.time, the clipboard and fs.space. Each is declared on its own so one the FFI already
-- has under another prototype doesn't stop the rest; the system times take a SYSTEMTIME as eight words.
for _, declaration in ipairs({
    'unsigned long GetTickCount(void);',
    'unsigned long long GetTickCount64(void);',
    'int QueryPerformanceCounter(int64_t* count);',
    'int QueryPerformanceFrequency(int64_t* frequency);',
    'void GetLocalTime(uint16_t* time);',
    'void GetSystemTime(uint16_t* time);',
    'void* GetActiveWindow(void);',
    'int OpenClipboard(void* window);',
    'int CloseClipboard(void);',
    'int EmptyClipboard(void);',
    'void* GetClipboardData(unsigned int format);',
    'void* SetClipboardData(unsigned int format, void* memory);',
    'void* GlobalAlloc(unsigned int flags, size_t bytes);',
    'void* GlobalLock(void* memory);',
    'int GlobalUnlock(void* memory);',
    'int GetDiskFreeSpaceExA(const char* path, uint64_t* available, uint64_t* capacity, uint64_t* free);',
}) do
    pcall(ffi.cdef, declaration)
end

-- A LARGE_INTEGER as Ashita's LargeInteger: quad_part/high_part/low_part and q/h/l (xitools reads qpc().q)
local function counter(fn)
    local value = ffi.new('int64_t[1]')
    fn(value)
    local q = tonumber(value[0])
    local h = tonumber(bit.arshift(value[0], 32))
    local l = tonumber(bit.band(value[0], 0xFFFFFFFFLL))
    return { quad_part = q, high_part = h, low_part = l, q = q, h = h, l = l }
end

-- SYSTEMTIME as Ashita's SystemTime (year, month, dayofweek, day, hour, minute, second, ms and y/m/wd/d/hh/mm/ss),
-- with the structure's own names and spelled-out ones kept
local function system_time(fn)
    local t = ffi.new('uint16_t[8]')
    fn(t)
    return {
        year = t[0], month = t[1], dayofweek = t[2], day = t[3], hour = t[4], minute = t[5], second = t[6], ms = t[7],
        y = t[0], m = t[1], wd = t[2], d = t[3], hh = t[4], mm = t[5], ss = t[6],
        day_of_week = t[2], milliseconds = t[7],
        wYear = t[0], wMonth = t[1], wDayOfWeek = t[2], wDay = t[3], wHour = t[4], wMinute = t[5], wSecond = t[6], wMilliseconds = t[7],
    }
end

local function get_tick() return tonumber(ffi.C.GetTickCount()) end
local function get_tick64() return tonumber(ffi.C.GetTickCount64()) end
local function query_performance_counter() return counter(ffi.C.QueryPerformanceCounter) end
local function query_performance_frequency() return counter(ffi.C.QueryPerformanceFrequency) end
local function get_localtime() return system_time(ffi.C.GetLocalTime) end
local function get_systemtime() return system_time(ffi.C.GetSystemTime) end

ashita.time = partial('ashita.time', {
    clock = function ()
        local seconds = vana.time.clock()
        return {
            s = math.floor(seconds), ms = math.floor(seconds * 1000),
            micro = math.floor(seconds * 1e6), nano = math.floor(seconds * 1e9),
        }
    end,
    get_tick = get_tick,
    tick = get_tick,
    get_tick64 = get_tick64,
    tick64 = get_tick64,
    query_performance_counter = query_performance_counter,
    qpc = query_performance_counter,
    query_performance_frequency = query_performance_frequency,
    qpf = query_performance_frequency,
    get_localtime = get_localtime,
    glt = get_localtime,
    get_systemtime = get_systemtime,
    gst = get_systemtime,
})

-- The clipboard's text (CF_TEXT, 1), as ANSI like the game's own strings. The window that owns it is the game's: a
-- clipboard opened without one can't be set.
local function get_clipboard()
    local C = ffi.C
    if C.OpenClipboard(C.GetActiveWindow()) == 0 then
        return ''
    end
    local text = ''
    local handle = C.GetClipboardData(1)
    if handle ~= nil then
        local memory = C.GlobalLock(handle)
        if memory ~= nil then
            text = ffi.string(memory)
            C.GlobalUnlock(handle)
        end
    end
    C.CloseClipboard()
    return text
end

local function set_clipboard(text)
    local C = ffi.C
    text = tostring(text)
    local handle = C.GlobalAlloc(0x2, #text + 1) -- GMEM_MOVEABLE
    if handle == nil then
        return false
    end
    local memory = C.GlobalLock(handle)
    if memory == nil then
        return false
    end
    ffi.copy(memory, text) -- and its terminating zero
    C.GlobalUnlock(handle)
    if C.OpenClipboard(C.GetActiveWindow()) == 0 then
        return false
    end
    C.EmptyClipboard()
    local set = C.SetClipboardData(1, handle) ~= nil -- the clipboard owns the memory once it takes it
    C.CloseClipboard()
    return set
end

ashita.misc = partial('ashita.misc', {
    get_clipboard = get_clipboard,
    set_clipboard = set_clipboard,
    open_url = vana.system.open_url,
    -- PlaySound and ShellExecute open the file in Windows' own DLLs, past VanaCore.Core.dll's imports (paths.cpp), so
    -- their paths are remapped here: ventures plays '<install>/addons/ventures/sounds/<x>.wav'.
    play_sound = function (path) return vana.system.play_sound(remap(path)) end,
    execute = function (file, parameters)
        return vana.ashita.execute(tostring(remap(file)), parameters and tostring(parameters) or '')
    end,
    hide_console = function () vana.ashita.console(false) end,
    show_console = function () vana.ashita.console(true) end,
})

ashita.regex = partial('ashita.regex', vana.ashita.regex)

-- ashita.bits: Ashita's BinaryData::UnpackBitsBE and PackBitsBE. Despite the name, bits run from each byte's lowest
-- (a little-endian word read at the byte, shifted down by the bit offset), as FFXI packs its packets. The data a
-- pointer (e.data_raw, e.data_modified_raw: light userdata or cdata), a string, or a table of bytes from 1
-- (packet:totable()). unpack_be(data, byte_offset, bit_offset, bits), or (data, bit_offset, bits);
-- pack_be(data, value, byte_offset, bit_offset, bits), or (data, value, bit_offset, bits), into a pointer or table.
local function byte_access(data)
    local kind = type(data)
    if kind == 'string' then
        return function (i) return data:byte(i + 1) or 0 end, nil
    elseif kind == 'table' then
        return function (i) return tonumber(data[i + 1]) or 0 end, function (i, v) data[i + 1] = v end
    elseif kind == 'userdata' or kind == 'cdata' then
        local p = ffi.cast('uint8_t*', data)
        return function (i) return p[i] end, function (i, v) p[i] = v end
    end
    error('ashita.bits: the data must be a pointer, a string or a table of bytes', 3)
end

local function unpack_be(data, byte_offset, bit_offset, bits)
    if bits == nil then
        byte_offset, bit_offset, bits = 0, byte_offset, bit_offset
    end
    local read = byte_access(data)
    local position = (tonumber(byte_offset) or 0) * 8 + (tonumber(bit_offset) or 0)
    bits = tonumber(bits) or 0
    local value, scale = 0, 1
    while bits > 0 do
        local shift = position % 8
        local take = math.min(8 - shift, bits)
        local part = math.floor(read(math.floor(position / 8)) / 2 ^ shift) % 2 ^ take
        value = value + part * scale
        scale = scale * 2 ^ take
        position = position + take
        bits = bits - take
    end
    return value
end

local function pack_be(data, value, byte_offset, bit_offset, bits)
    if bits == nil then
        byte_offset, bit_offset, bits = 0, byte_offset, bit_offset
    end
    local read, write = byte_access(data)
    if write == nil then
        error('ashita.bits.pack_be: the data must be a pointer or a table of bytes (a string can\'t be written)', 2)
    end
    local position = (tonumber(byte_offset) or 0) * 8 + (tonumber(bit_offset) or 0)
    bits = tonumber(bits) or 0
    value = math.floor(tonumber(value) or 0) % 2 ^ bits
    while bits > 0 do
        local index, shift = math.floor(position / 8), position % 8
        local take = math.min(8 - shift, bits)
        local part = value % 2 ^ take
        value = math.floor(value / 2 ^ take)
        local byte = read(index)
        local old = math.floor(byte / 2 ^ shift) % 2 ^ take
        write(index, byte + (part - old) * 2 ^ shift)
        position = position + take
        bits = bits - take
    end
    return position
end

-- unpack_le and pack_le: BinaryData's UnpackBitsLE and PackBitsLE, with the same arguments. They read the bytes as a
-- big-endian word, so bits run from each byte's highest: bit offset 0 is the first byte's top bit.
local function unpack_le(data, byte_offset, bit_offset, bits)
    if bits == nil then
        byte_offset, bit_offset, bits = 0, byte_offset, bit_offset
    end
    local read = byte_access(data)
    local position = (tonumber(byte_offset) or 0) * 8 + (tonumber(bit_offset) or 0)
    bits = tonumber(bits) or 0
    local value = 0
    while bits > 0 do
        local shift = position % 8
        local take = math.min(8 - shift, bits)
        local part = math.floor(read(math.floor(position / 8)) / 2 ^ (8 - shift - take)) % 2 ^ take
        value = value * 2 ^ take + part
        position = position + take
        bits = bits - take
    end
    return value
end

local function pack_le(data, value, byte_offset, bit_offset, bits)
    if bits == nil then
        byte_offset, bit_offset, bits = 0, byte_offset, bit_offset
    end
    local read, write = byte_access(data)
    if write == nil then
        error('ashita.bits.pack_le: the data must be a pointer or a table of bytes (a string can\'t be written)', 2)
    end
    local position = (tonumber(byte_offset) or 0) * 8 + (tonumber(bit_offset) or 0)
    bits = tonumber(bits) or 0
    value = math.floor(tonumber(value) or 0) % 2 ^ bits
    while bits > 0 do
        local index, shift = math.floor(position / 8), position % 8
        local take = math.min(8 - shift, bits)
        bits = bits - take
        local part = math.floor(value / 2 ^ bits) % 2 ^ take -- the highest of what's left
        local low = 8 - shift - take
        local byte = read(index)
        local old = math.floor(byte / 2 ^ low) % 2 ^ take
        write(index, byte + (part - old) * 2 ^ low)
        position = position + take
    end
    return position
end

ashita.bits = partial('ashita.bits', { unpack_be = unpack_be, pack_be = pack_be, unpack_le = unpack_le, pack_le = pack_le })

-- addon.instance, as both interfaces' Addons plugin has it: the addon's state (Ashita's numbers: 3 loading, 1 running,
-- 4 unloading, 5 reloading, 0 error), the frames it has seen (current_frame, what oncef counts) and get_memory_usage()
-- (the bytes its Lua holds). And Ashita 4.30's AddonManager global: the Ashita addons loaded (Get takes 0 to Count() - 1)
-- and each one's details by name; an addon that isn't loaded has state 0 and no details.
do
    local own = tostring(addon and addon.name or ''):lower()
    local function entries(ashita_only)
        local out = {}
        for _, loaded in ipairs(vana.addons.loaded()) do
            if not ashita_only or loaded.source == 'ashita' then
                out[#out + 1] = loaded
            end
        end
        return out
    end
    local function entry(name, ashita_only)
        name = tostring(name or ''):lower()
        for _, loaded in ipairs(entries(ashita_only)) do
            if loaded.name:lower() == name then
                return loaded
            end
        end
        return nil
    end
    if addon ~= nil then
        addon.instance = setmetatable({}, {
            __index = function (_, key)
                if key == 'state' then
                    local loaded = entry(own)
                    return loaded and loaded.state or 3
                elseif key == 'current_frame' then
                    return vana.tasks.frame()
                elseif key == 'get_memory_usage' then
                    return function () return collectgarbage('count') * 1024 end
                end
                return nil
            end,
        })
    end
    if INTERFACE == 4.30 then
        local function detail(key)
            return function (_, name)
                local loaded = entry(name, true)
                local value = loaded and loaded[key]
                return value ~= nil and value ~= '' and value or nil
            end
        end
        AddonManager = partial('AddonManager', {
            Count = function () return #entries(true) end,
            Get = function (_, index)
                local loaded = entries(true)[(tonumber(index) or -1) + 1]
                return loaded and loaded.name or nil
            end,
            IsLoaded = function (_, name) return entry(name, true) ~= nil end,
            GetState = function (_, name)
                local loaded = entry(name, true)
                return loaded and loaded.state or 0
            end,
            GetAuthor = detail('author'),
            GetDescription = detail('desc'),
            GetFileName = detail('file'),
            GetLink = detail('link'),
            GetVersion = detail('version'),
            GetMemoryUsage = detail('memory'),
        })
    end
end

-- utf8: Lua 5.3's library, which Ashita's Lua has as a global (fancychat builds its icons with utf8.char)
if utf8 == nil then
    utf8 = {}
    utf8.charpattern = '[%z\1-\x7F\xC2-\xFD][\x80-\xBF]*' -- (Lua 5.1's patterns take no NUL: %z)

    function utf8.char(...)
        local out = {}
        for k = 1, select('#', ...) do
            local c = math.floor(tonumber((select(k, ...))) or 0)
            if c < 0 or c > 0x7FFFFFFF then
                error(('bad argument #%d to \'char\' (value out of range)'):format(k), 2)
            elseif c < 0x80 then
                out[k] = string.char(c)
            elseif c < 0x800 then
                out[k] = string.char(0xC0 + math.floor(c / 0x40), 0x80 + c % 0x40)
            elseif c < 0x10000 then
                out[k] = string.char(0xE0 + math.floor(c / 0x1000), 0x80 + math.floor(c / 0x40) % 0x40, 0x80 + c % 0x40)
            else
                out[k] = string.char(0xF0 + math.floor(c / 0x40000), 0x80 + math.floor(c / 0x1000) % 0x40,
                    0x80 + math.floor(c / 0x40) % 0x40, 0x80 + c % 0x40)
            end
        end
        return table.concat(out)
    end

    -- the character starting at byte i: its code and the byte after it, or nil if it isn't valid UTF-8 there
    local function decode(s, i)
        local c = s:byte(i)
        if c == nil then
            return nil
        elseif c < 0x80 then
            return c, i + 1
        end
        local n, code
        if c >= 0xF0 and c < 0xF8 then
            n, code = 3, c - 0xF0
        elseif c >= 0xE0 then
            n, code = 2, c - 0xE0
        elseif c >= 0xC0 then
            n, code = 1, c - 0xC0
        else
            return nil
        end
        for k = 1, n do
            local b = s:byte(i + k)
            if b == nil or b < 0x80 or b >= 0xC0 then
                return nil
            end
            code = code * 0x40 + (b - 0x80)
        end
        return code, i + n + 1
    end

    local function position(i, len)
        if i >= 0 then
            return i
        elseif -i > len then
            return 0
        end
        return len + i + 1
    end

    function utf8.codepoint(s, i, j)
        s = tostring(s)
        i = position(tonumber(i) or 1, #s)
        j = position(tonumber(j) or i, #s)
        local out = {}
        while i <= j do
            local code, after = decode(s, i)
            if code == nil then
                error('invalid UTF-8 code', 2)
            end
            out[#out + 1] = code
            i = after
        end
        return unpack(out)
    end

    function utf8.len(s, i, j)
        s = tostring(s)
        i = position(tonumber(i) or 1, #s)
        j = position(tonumber(j) or -1, #s)
        local n = 0
        while i <= j do
            local code, after = decode(s, i)
            if code == nil then
                return nil, i
            end
            n = n + 1
            i = after
        end
        return n
    end

    local function continuation(s, i)
        local b = s:byte(i)
        return b ~= nil and b >= 0x80 and b < 0xC0
    end

    function utf8.offset(s, n, i)
        s = tostring(s)
        n = tonumber(n)
        i = position(tonumber(i) or (n >= 0 and 1 or #s + 1), #s)
        if n == 0 then
            while i > 1 and continuation(s, i) do
                i = i - 1
            end
            return i
        end
        if continuation(s, i) then
            error('initial position is a continuation byte', 2)
        end
        if n > 0 then
            n = n - 1
            while n > 0 and i <= #s do
                i = i + 1
                while continuation(s, i) do
                    i = i + 1
                end
                n = n - 1
            end
            return n == 0 and i or nil
        end
        while n < 0 and i > 1 do
            i = i - 1
            while i > 1 and continuation(s, i) do
                i = i - 1
            end
            n = n + 1
        end
        return n == 0 and i or nil
    end

    function utf8.codes(s)
        s = tostring(s)
        local i = 1
        return function ()
            if i > #s then
                return nil
            end
            local code, after = decode(s, i)
            if code == nil then
                error('invalid UTF-8 code', 2)
            end
            local at = i
            i = after
            return at, code
        end
    end
end

-------------------------------------------------------------------------------
-- Their enumerations and the structures their font and primitive libraries make
-------------------------------------------------------------------------------

FrameAnchor = { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3, Right = 1, Bottom = 2 }
FontBorderFlags = { None = 0, Top = 1, Bottom = 2, Left = 4, Right = 8, All = 15 }
FontCreateFlags = { None = 0, Bold = 1, Italic = 2, StrikeThrough = 4, Underlined = 8, CustomFile = 16, ClearType = 32 }
FontDrawFlags = { None = 0, Filtered = 1, CenterX = 2, CenterY = 4, RightJustified = 8, Outlined = 16, ManualRender = 32 }
PrimitiveDrawFlags = { None = 0, ManualRender = 1 }

ffi.cdef[[
    typedef struct { int32_t left, top, right, bottom; } vana_RECT;
    typedef struct { uint16_t Raw; } vana_skill;
    typedef struct { int32_t cx, cy; } vana_SIZE;
    typedef struct { uint16_t Id; uint16_t Index; uint32_t Count; uint32_t Flags; uint32_t Price; uint8_t Extra[28]; } vana_item_t;
    typedef struct { uint32_t Slot; uint32_t Index; } vana_equipmententry_t;
    typedef struct {
        uint32_t Use; uint32_t ItemId; uint32_t Count; uint32_t Flags; uint32_t Price; uint8_t Extra[28]; uint32_t Status;
        uint16_t Lot; uint16_t WinningLot; uint32_t WinningEntityServerId; uint32_t WinningEntityTargetIndex;
        int8_t WinningEntityName[16]; uint32_t TimeToLive; uint32_t DropTime;
    } vana_treasureitem_t;
    typedef struct { uint32_t Slot; uint16_t ItemId; uint16_t padding0006; uint8_t Extra[28]; } vana_itemcheck_t;
    typedef struct { uint16_t AbilityId; int8_t Level; uint8_t Unknown0000; } vana_IMonAbility;
    typedef struct {
        uint32_t Id; uint16_t Flags; uint16_t StackSize; uint16_t Type; uint16_t ResourceId; uint16_t Targets;
        uint16_t Level; uint16_t Slots; uint16_t Races; uint32_t Jobs; uint16_t SuperiorLevel; uint16_t ShieldSize;
        uint8_t MaxCharges; uint16_t CastTime; uint16_t CastDelay; uint32_t RecastDelay; uint16_t BaseItemId;
        uint16_t ItemLevel; uint16_t Damage; uint16_t Delay; uint16_t DPS; uint8_t Skill; uint8_t JugSize;
        uint32_t WeaponUnknown0000; uint8_t Range; uint8_t AreaRange; uint8_t AreaShapeType; uint8_t AreaCursorTargetType;
        uint16_t Element; uint32_t Storage; uint32_t AttachmentFlags;
        uint16_t InstinctCost;
        uint16_t MonstrosityId; char MonstrosityName[0x20]; uint8_t MonstrosityData[0x0A];
        vana_IMonAbility MonstrosityAbilities[0x10];
        uint16_t PuppetSlotId; uint32_t PuppetElements;
        uint8_t SlipData[0x46];
        uint16_t UsableData0000; uint32_t UsableData0001; uint32_t UsableData0002;
        uint32_t Article;
        const char* Name[3]; const char* Description[3]; const char* LogNameSingular[3]; const char* LogNamePlural[3];
        uint32_t ImageSize; uint8_t ImageType; uint8_t ImageName[0x10]; uint8_t Bitmap[0x980];
    } vana_IItem;
    typedef struct {
        uint16_t Id; uint8_t Type; uint8_t Element; uint16_t ListIconId; uint16_t ManaCost; uint16_t RecastTimerId;
        uint16_t Targets; int16_t TPCost; uint8_t MenuCategoryId; uint8_t MonsterLevel; int8_t Range; uint8_t AreaRange;
        uint8_t AreaShapeType; uint8_t CursorTargetType; uint8_t Unknown0000; uint8_t Unknown0001; uint16_t Unknown0002;
        uint16_t Unknown0003; uint16_t Unknown0004; uint16_t Unknown0005; uint16_t Unknown0006; uint16_t Unknown0007;
        uint16_t Unknown0008; uint8_t Unknown0009[11]; uint8_t EOE;
        const char* Name[3]; const char* Description[3];
    } vana_IAbility;
    typedef struct {
        uint16_t Index; uint16_t Type; uint16_t Element; uint16_t Targets; uint16_t Skill; uint16_t ManaCost;
        uint8_t CastTime; uint8_t RecastDelay; int16_t LevelRequired[24]; uint16_t Id; uint16_t ListIconNQ;
        uint16_t ListIconHQ; uint8_t Requirements; int8_t Range; uint8_t AreaRange; uint8_t AreaShapeType;
        uint8_t CursorTargetType; uint8_t Unknown0000[3]; uint32_t AreaFlags; uint16_t Unknown0001; uint8_t Unknown0002;
        uint8_t Unknown0003; uint8_t Unknown0004[8]; uint32_t JobPointMask; uint8_t Unknown0005[3]; uint8_t EOE;
        const char* Name[3]; const char* Description[3];
    } vana_ISpell;
    typedef struct {
        uint16_t Index; uint16_t Id; uint8_t CanCancel; uint8_t HideTimer; const char* Description[3];
        uint32_t ImageSize; uint8_t ImageType; uint8_t ImageName[0x10]; uint8_t Bitmap[0x156A];
    } vana_IStatusIcon;
]]

-- RECT and SIZE, as their libraries make them (RECT.new(), SIZE.new(), RECT.__type.is(v)). Theirs are userdata; these
-- are cdata, which their fonts library doesn't recognise as a RECT (type(v) == 'userdata') and so formats with
-- tostring and splits at the commas (points' border sizes): they print as 'left,top,right,bottom' for it.
ffi.metatype('vana_RECT', {
    __tostring = function (r) return ('%d,%d,%d,%d'):format(r.left, r.top, r.right, r.bottom) end,
})
ffi.metatype('vana_SIZE', {
    __tostring = function (s) return ('%d,%d'):format(s.cx, s.cy) end,
})
RECT = {
    new = function (...) return ffi.new('vana_RECT', ...) end,
    __type = { name = 'RECT', is = function (v) return ffi.istype('vana_RECT', v) end },
}
SIZE = {
    new = function (...) return ffi.new('vana_SIZE', ...) end,
    __type = { name = 'SIZE', is = function (v) return ffi.istype('vana_SIZE', v) end },
}

-------------------------------------------------------------------------------
-- VanaCore's own Ashita objects (the ones plugins of interface 4.16 get: compat/ashita), called through
-- their vtables with the FFI. Each table lists an interface's methods as abi.hpp declares them: the
-- vtable slot, name, return type, parameters, and what a returned pointer is (a font, a primitive, an
-- item, a string). Methods passing callbacks, and overloads (MSVC orders them its own way), aren't bound.
-------------------------------------------------------------------------------

local FONT_OBJECT = {
    { 0, 'Render', 'void', '' },
    { 1, 'GetTextSize', 'void', 'vana_SIZE*' },
    { 2, 'HitTest', 'bool', 'int32_t, int32_t' },
    { 3, 'GetParent', 'void*', '', 'font' },
    { 4, 'GetBackground', 'void*', '', 'primitive' },
    { 5, 'GetRealPositionX', 'float', '' },
    { 6, 'GetRealPositionY', 'float', '' },
    { 7, 'GetAlias', 'const char*', '', 'string' },
    { 8, 'GetVisible', 'bool', '' },
    { 9, 'GetCanFocus', 'bool', '' },
    { 10, 'GetLocked', 'bool', '' },
    { 11, 'GetLockedZ', 'bool', '' },
    { 12, 'GetIsDirty', 'bool', '' },
    { 13, 'GetWindowWidth', 'float', '' },
    { 14, 'GetWindowHeight', 'float', '' },
    { 15, 'GetFontFile', 'const char*', '', 'string' },
    { 16, 'GetFontFamily', 'const char*', '', 'string' },
    { 17, 'GetFontHeight', 'uint32_t', '' },
    { 18, 'GetCreateFlags', 'uint32_t', '' },
    { 19, 'GetDrawFlags', 'uint32_t', '' },
    { 20, 'GetBold', 'bool', '' },
    { 21, 'GetItalic', 'bool', '' },
    { 22, 'GetRightJustified', 'bool', '' },
    { 23, 'GetStrikeThrough', 'bool', '' },
    { 24, 'GetUnderlined', 'bool', '' },
    { 25, 'GetColor', 'uint32_t', '' },
    { 26, 'GetColorOutline', 'uint32_t', '' },
    { 27, 'GetPadding', 'float', '' },
    { 28, 'GetPositionX', 'float', '' },
    { 29, 'GetPositionY', 'float', '' },
    { 30, 'GetAutoResize', 'bool', '' },
    { 31, 'GetAnchor', 'uint32_t', '' },
    { 32, 'GetAnchorParent', 'uint32_t', '' },
    { 33, 'GetText', 'const char*', '', 'string' },
    { 34, 'SetAlias', 'void', 'const char*' },
    { 35, 'SetVisible', 'void', 'bool' },
    { 36, 'SetCanFocus', 'void', 'bool' },
    { 37, 'SetLocked', 'void', 'bool' },
    { 38, 'SetLockedZ', 'void', 'bool' },
    { 39, 'SetIsDirty', 'void', 'bool' },
    { 40, 'SetWindowWidth', 'void', 'float' },
    { 41, 'SetWindowHeight', 'void', 'float' },
    { 42, 'SetFontFile', 'void', 'const char*' },
    { 43, 'SetFontFamily', 'void', 'const char*' },
    { 44, 'SetFontHeight', 'void', 'uint32_t' },
    { 45, 'SetCreateFlags', 'void', 'uint32_t' },
    { 46, 'SetDrawFlags', 'void', 'uint32_t' },
    { 47, 'SetBold', 'void', 'bool' },
    { 48, 'SetItalic', 'void', 'bool' },
    { 49, 'SetRightJustified', 'void', 'bool' },
    { 50, 'SetStrikeThrough', 'void', 'bool' },
    { 51, 'SetUnderlined', 'void', 'bool' },
    { 52, 'SetColor', 'void', 'uint32_t' },
    { 53, 'SetColorOutline', 'void', 'uint32_t' },
    { 54, 'SetPadding', 'void', 'float' },
    { 55, 'SetPositionX', 'void', 'float' },
    { 56, 'SetPositionY', 'void', 'float' },
    { 57, 'SetAutoResize', 'void', 'bool' },
    { 58, 'SetAnchor', 'void', 'uint32_t' },
    { 59, 'SetAnchorParent', 'void', 'uint32_t' },
    { 60, 'SetText', 'void', 'const char*' },
    { 61, 'SetParent', 'void', 'void*' },
}

local PRIMITIVE_OBJECT = {
    { 0, 'Render', 'void', '' },
    { 1, 'SetTextureFromFile', 'bool', 'const char*' },
    { 2, 'SetTextureFromMemory', 'bool', 'const void*, uint32_t, uint32_t' },
    { 3, 'SetTextureFromResource', 'bool', 'const char*, const char*' },
    { 4, 'SetTextureFromResourceCache', 'bool', 'const char*' },
    { 5, 'SetTextureFromTexture', 'bool', 'void*, uint32_t, uint32_t' },
    { 6, 'HitTest', 'bool', 'int32_t, int32_t' },
    { 7, 'GetAlias', 'const char*', '', 'string' },
    { 8, 'GetTextureOffsetX', 'float', '' },
    { 9, 'GetTextureOffsetY', 'float', '' },
    { 10, 'GetBorderVisible', 'bool', '' },
    { 11, 'GetBorderColor', 'uint32_t', '' },
    { 12, 'GetBorderFlags', 'uint32_t', '' },
    { 13, 'GetBorderSizes', 'vana_RECT*', 'vana_RECT*', 'rect' }, -- a RECT returned: through a pointer (MSVC's member functions)
    { 14, 'GetVisible', 'bool', '' },
    { 15, 'GetPositionX', 'float', '' },
    { 16, 'GetPositionY', 'float', '' },
    { 17, 'GetCanFocus', 'bool', '' },
    { 18, 'GetLocked', 'bool', '' },
    { 19, 'GetLockedZ', 'bool', '' },
    { 20, 'GetScaleX', 'float', '' },
    { 21, 'GetScaleY', 'float', '' },
    { 22, 'GetWidth', 'float', '' },
    { 23, 'GetHeight', 'float', '' },
    { 24, 'GetDrawFlags', 'uint32_t', '' },
    { 25, 'GetColor', 'uint32_t', '' },
    { 26, 'SetAlias', 'void', 'const char*' },
    { 27, 'SetTextureOffsetX', 'void', 'float' },
    { 28, 'SetTextureOffsetY', 'void', 'float' },
    { 29, 'SetBorderVisible', 'void', 'bool' },
    { 30, 'SetBorderColor', 'void', 'uint32_t' },
    { 31, 'SetBorderFlags', 'void', 'uint32_t' },
    { 32, 'SetBorderSizes', 'void', 'vana_RECT' },
    { 33, 'SetVisible', 'void', 'bool' },
    { 34, 'SetPositionX', 'void', 'float' },
    { 35, 'SetPositionY', 'void', 'float' },
    { 36, 'SetCanFocus', 'void', 'bool' },
    { 37, 'SetLocked', 'void', 'bool' },
    { 38, 'SetLockedZ', 'void', 'bool' },
    { 39, 'SetScaleX', 'void', 'float' },
    { 40, 'SetScaleY', 'void', 'float' },
    { 41, 'SetWidth', 'void', 'float' },
    { 42, 'SetHeight', 'void', 'float' },
    { 43, 'SetDrawFlags', 'void', 'uint32_t' },
    { 44, 'SetColor', 'void', 'uint32_t' },
}

local OBJECT_MANAGER = { -- IFontManager and IPrimitiveManager alike; `kind` is what they make
    { 0, 'Create', 'void*', 'const char*', 'kind' },
    { 1, 'Get', 'void*', 'const char*', 'kind' },
    { 2, 'Delete', 'void', 'const char*' },
    { 3, 'GetFocusedObject', 'void*', '', 'kind' },
    { 4, 'SetFocusedObject', 'bool', 'const char*' },
    { 5, 'GetVisible', 'bool', '' },
    { 6, 'SetVisible', 'void', 'bool' },
}

local RESOURCE_MANAGER = {
    { 0, 'GetAbilityById', 'void*', 'uint32_t', 'ability' },
    { 1, 'GetAbilityByName', 'void*', 'const char*, uint32_t', 'ability' },
    { 2, 'GetAbilityByTimerId', 'void*', 'uint32_t', 'ability' },
    { 3, 'GetSpellById', 'void*', 'uint32_t', 'spell' },
    { 4, 'GetSpellByName', 'void*', 'const char*, uint32_t', 'spell' },
    { 5, 'GetItemById', 'void*', 'uint32_t', 'item' },
    { 6, 'GetItemByName', 'void*', 'const char*, uint32_t', 'item' },
    { 7, 'GetEntityCount', 'uint32_t', '' },
    { 8, 'GetEntityIndexById', 'uint32_t', 'uint32_t' },
    { 9, 'GetEntityIdByIndex', 'uint32_t', 'uint32_t' },
    { 10, 'GetEntityNameByIndex', 'const char*', 'uint32_t', 'string' },
    { 11, 'GetEntityNameById', 'const char*', 'uint32_t', 'string' },
    { 12, 'GetStatusIconByIndex', 'void*', 'uint16_t', 'statusicon' },
    { 13, 'GetStatusIconById', 'void*', 'uint16_t', 'statusicon' },
    { 20, 'GetTexture', 'void*', 'const char*' },
    { 22, 'GetFilePath', 'uint32_t', 'uint32_t, char*, uint32_t' },
    { 23, 'GetAbilityRange', 'uint32_t', 'uint32_t, bool' },
    { 24, 'GetAbilityType', 'uint32_t', 'uint32_t' },
    { 25, 'GetSpellRange', 'uint32_t', 'uint32_t, bool' },
}

-- IProperties: the game's window
local PROPERTIES = {
    { 16, 'GetFinalFantasyHwnd', 'void*', '' },
    { 17, 'GetFinalFantasyStyle', 'uint32_t', '' },
    { 18, 'GetFinalFantasyStyleEx', 'uint32_t', '' },
    { 19, 'GetFinalFantasyRect', 'vana_RECT*', 'vana_RECT*', 'rect' }, -- a RECT returned: through a pointer
}

local RECAST = {
    { 0, 'GetAbilityRecast', 'uint32_t', 'uint32_t' },
    { 1, 'GetAbilityCalc1', 'uint8_t', 'uint32_t' },
    { 2, 'GetAbilityTimerId', 'uint32_t', 'uint32_t' },
    { 3, 'GetAbilityCalc2', 'int16_t', 'uint32_t' },
    { 4, 'GetAbilityTimer', 'uint32_t', 'uint32_t' },
    { 5, 'GetSpellTimer', 'uint32_t', 'uint32_t' },
}

local POINTER_MANAGER = {
    { 2, 'Get', 'uintptr_t', 'const char*' },
}

local INVENTORY = {
    { 0, 'GetRawStructure', 'void*', '' },
    { 1, 'GetContainerItem', 'vana_item_t*', 'uint32_t, uint32_t', 'invitem' },
    { 2, 'GetContainerCount', 'uint32_t', 'uint32_t' },
    { 3, 'GetContainerCountMax', 'uint32_t', 'uint32_t' },
    { 4, 'GetTreasurePoolItem', 'vana_treasureitem_t*', 'uint32_t', 'treasureitem' },
    { 5, 'GetTreasurePoolStatus', 'uint32_t', '' },
    { 6, 'GetTreasurePoolItemCount', 'uint32_t', '' },
    { 7, 'GetContainerUpdateCounter', 'uint32_t', '' },
    { 8, 'GetContainerUpdateFlags', 'uint32_t', '' },
    { 9, 'GetDisplayItemSlot', 'uint32_t', '' },
    { 10, 'GetDisplayItemPointer', 'uint32_t', '' },
    { 11, 'GetEquippedItem', 'vana_equipmententry_t*', 'uint32_t', 'equipentry' },
    { 12, 'GetCheckEquippedItem', 'vana_itemcheck_t*', 'uint32_t', 'itemcheck' },
    { 13, 'GetCheckTargetIndex', 'uint32_t', '' },
    { 14, 'GetCheckServerId', 'uint32_t', '' },
    { 15, 'GetCheckFlags', 'uint32_t', '' },
    { 16, 'GetCheckMainJob', 'uint8_t', '' },
    { 17, 'GetCheckSubJob', 'uint8_t', '' },
    { 18, 'GetCheckMainJobLevel', 'uint8_t', '' },
    { 19, 'GetCheckSubJobLevel', 'uint8_t', '' },
    { 20, 'GetCheckMainJob2', 'uint8_t', '' },
    { 21, 'GetCheckMasteryLevel', 'uint8_t', '' },
    { 22, 'GetCheckMasteryFlags', 'uint8_t', '' },
    { 23, 'GetCheckLinkshellName', 'const char*', '', 'string' },
    { 24, 'GetCheckLinkshellColor', 'uint16_t', '' },
    { 25, 'GetCheckLinkshellIconSetId', 'uint8_t', '' },
    { 26, 'GetCheckLinkshellIconSetIndex', 'uint8_t', '' },
    { 27, 'GetCheckBallistaChevronCount', 'uint32_t', '' },
    { 28, 'GetCheckBallistaChevronFlags', 'uint16_t', '' },
    { 29, 'GetCheckBallistaFlags', 'uint16_t', '' },
    { 30, 'GetSearchComment', 'const char*', '', 'string' },
    { 31, 'GetCraftStatus', 'uint32_t', '' },
    { 33, 'GetCraftTimestampAttempt', 'uint32_t', '' },
    { 34, 'GetCraftTimestampResponse', 'uint32_t', '' },
    { 39, 'GetSelectedItemName', 'const char*', '', 'string' },
    { 40, 'GetSelectedItemId', 'uint16_t', '' },
    { 41, 'GetSelectedItemIndex', 'uint8_t', '' },
}

local wrap -- (kind, pointer): the Lua object for an Ashita object, below

-- An item (or a status icon) as their addons read one: its numbers, strings as Lua strings (Name, Description and
-- the log names as { default, Japanese, English }), Bitmap as the bytes.
local item_mt = {
    __index = function (self, key)
        local item = rawget(self, '__item')
        if key == 'Name' or key == 'Description' or key == 'LogNameSingular' or key == 'LogNamePlural' then
            local names = item[key]
            local function text(i) return names[i] ~= nil and ffi.string(names[i]) or nil end
            local value = { text(0) or text(2), text(1), text(2) }
            rawset(self, key, value)
            return value
        end
        if key == 'MonstrosityName' then
            return ffi.string(item.MonstrosityName)
        end
        if key == 'ImageName' then
            return ffi.string(item.ImageName, 16)
        end
        -- the image's bytes as a Lua string: `#` fails on an FFI array (timers: SetTextureFromMemory(icon.Bitmap,
        -- #icon.Bitmap, ...)), and a string passes wherever an API takes the bytes as a pointer. An item's is read
        -- from its DAT now: an addon's item is made without it (ashita.item). One a plugin's hook gave (cexidats' cache
        -- of CatsEyeXI's own items) is whole: its Bitmap is a BMP file of its ImageSize, not the DAT's icon (which is 3
        -- bytes longer, so D3DX, given that ImageSize of it, couldn't read it: Trove's Odious Codex had no icon).
        if key == 'Bitmap' and rawget(self, '__kind') == 'item' then
            local value
            if vana.ashita.item_stub(tonumber(ffi.cast('uintptr_t', item))) then
                value = vana.ashita.item_icon(tonumber(item.Id))
            else
                value = ffi.string(item.Bitmap, math.min(tonumber(item.ImageSize) or 0, ffi.sizeof(item.Bitmap)))
            end
            rawset(self, key, value)
            return value
        end
        if key == 'Bitmap' and rawget(self, '__kind') == 'statusicon' then
            local value = ffi.string(item.Bitmap, math.min(tonumber(item.ImageSize) or 0, ffi.sizeof(item.Bitmap)))
            rawset(self, key, value)
            return value
        end
        if key == 'LevelRequired' then
            local levels = {}
            for i = 0, 23 do
                levels[i + 1] = item.LevelRequired[i]
            end
            rawset(self, key, levels)
            return levels
        end
        -- a field the structure doesn't have reads nil, as on Ashita's objects (the FFI would raise); which names a
        -- kind has is learnt once each
        local fields = rawget(self, '__fields')
        local has = fields[key]
        if has == nil then
            has = type(key) == 'string' and pcall(function () return item[key] end) or false
            fields[key] = has
            if not has then
                local caller = getinfo(2, 'Sl')
                vana.log(('%s.%s isn\'t a field of it: it reads nil (%s:%s)'):format(rawget(self, '__kind'), tostring(key),
                    caller and caller.short_src or '?', caller and caller.currentline or '?'))
            end
        end
        if not has then
            return nil
        end
        return item[key]
    end,
}
local item_fields = { item = {}, statusicon = {}, ability = {}, spell = {} } -- per kind: name -> whether it's a field

-- The game's own structures the inventory hands out (an item in a bag, in the treasure pool, on a checked player, an
-- equipment slot), as Ashita's Lua has them: fields read and write the game's memory live, a byte array is a Lua
-- string (an item's Extra: its 28 bytes, which their struct.unpack and :totable() take and trove, XIUI and LuAshitacast
-- test with type() and #; a name: up to its NUL), and a name the structure lacks reads nil.
local struct_kinds = {
    invitem = { 'vana_item_t*', { Extra = 28 }, {} },
    treasureitem = { 'vana_treasureitem_t*', { Extra = 28 }, { WinningEntityName = 16 } },
    itemcheck = { 'vana_itemcheck_t*', { Extra = 28 }, {} },
    equipentry = { 'vana_equipmententry_t*', {}, {} },
}
local struct_mts = {}
for kind, spec in pairs(struct_kinds) do
    local bytes, texts, fields = spec[2], spec[3], {}
    local function has(s, key)
        local known = fields[key]
        if known == nil then
            known = type(key) == 'string' and pcall(function () return s[key] end) or false
            fields[key] = known
        end
        return known
    end
    struct_mts[kind] = {
        __index = function (self, key)
            local s = rawget(self, '__s')
            local length = bytes[key]
            if length then
                return ffi.string(s[key], length)
            end
            length = texts[key]
            if length then
                local p, n = s[key], 0
                while n < length and p[n] ~= 0 do
                    n = n + 1
                end
                return ffi.string(p, n)
            end
            if has(s, key) then
                return s[key]
            end
            return nil
        end,
        __newindex = function (self, key, value)
            local s = rawget(self, '__s')
            local length = bytes[key] or texts[key]
            if length then
                local text = tostring(value)
                ffi.fill(s[key], length)
                ffi.copy(s[key], text, math.min(#text, length))
            elseif has(s, key) then
                s[key] = value
            else
                rawset(self, key, value)
            end
        end,
        __tostring = function (self) return kind .. ': ' .. tostring(rawget(self, '__s')) end,
    }
end

-- >>> SDK (tools/gen_ashita_lua.py: the Ashita SDK's interfaces, version 4.16's, from abi.hpp; don't edit)
local SDK = {
    IAshitaCore = {
        { 0, 'GetHandle', 'void*', '' },
        { 1, 'GetInstallPath', 'const char*', '', 'string' },
        { 2, 'GetDirect3DDevice', 'void*', '' },
        { 3, 'GetProperties', 'void*', '', 'iface:IProperties' },
        { 4, 'GetChatManager', 'void*', '', 'iface:IChatManager' },
        { 5, 'GetConfigurationManager', 'void*', '', 'iface:IConfigurationManager' },
        { 6, 'GetFontManager', 'void*', '', 'iface:IFontManager' },
        { 7, 'GetGuiManager', 'void*', '', 'iface:IGuiManager' },
        { 8, 'GetInputManager', 'void*', '', 'iface:IInputManager' },
        { 9, 'GetMemoryManager', 'void*', '', 'iface:IMemoryManager' },
        { 10, 'GetOffsetManager', 'void*', '', 'iface:IOffsetManager' },
        { 11, 'GetPacketManager', 'void*', '', 'iface:IPacketManager' },
        { 12, 'GetPluginManager', 'void*', '', 'iface:IPluginManager' },
        { 13, 'GetPolPluginManager', 'void*', '', 'iface:IPolPluginManager' },
        { 14, 'GetPointerManager', 'void*', '', 'iface:IPointerManager' },
        { 15, 'GetPrimitiveManager', 'void*', '', 'iface:IPrimitiveManager' },
        { 16, 'GetResourceManager', 'void*', '', 'iface:IResourceManager' },
        { 17, 'Direct3DCreate8', 'void*', 'uint32_t' },
        { 21, 'OpenMutexA', 'void*', 'uint32_t, int32_t, const char*' },
        { 24, 'CreateWindowExA', 'void*', 'uint32_t, const char*, const char*, uint32_t, int32_t, int32_t, int32_t, int32_t, void*, void*, void*, void*' },
        { 26, 'ExitProcess', 'void', 'uint32_t' },
        { 27, 'GetSystemMetrics', 'int32_t', 'int32_t' },
        { 35, 'RemoveMenu', 'int32_t', 'void*, uint32_t, uint32_t' },
        { 36, 'SendMessageA', 'intptr_t', 'void*, uint32_t, uintptr_t, intptr_t' },
        { 37, 'SetCursorPos', 'int32_t', 'int32_t, int32_t' },
        { 38, 'SetFocus', 'void*', 'void*' },
        { 39, 'SetForegroundWindow', 'int32_t', 'void*' },
        { 40, 'SetPriorityClass', 'int32_t', 'void*, uint32_t' },
        { 42, 'SetWindowTextA', 'int32_t', 'void*, const char*' },
    },
    IAutoFollow = {
        { 0, 'GetRawStructure', 'void*', '' },
        { 1, 'GetTargetIndex', 'uint32_t', '' },
        { 2, 'GetTargetServerId', 'uint32_t', '' },
        { 3, 'GetFollowDeltaX', 'float', '' },
        { 4, 'GetFollowDeltaZ', 'float', '' },
        { 5, 'GetFollowDeltaY', 'float', '' },
        { 6, 'GetFollowDeltaW', 'float', '' },
        { 7, 'GetFollowTargetIndex', 'uint32_t', '' },
        { 8, 'GetFollowTargetServerId', 'uint32_t', '' },
        { 9, 'GetIsFirstPersonCamera', 'uint8_t', '' },
        { 10, 'GetIsAutoRunning', 'uint8_t', '' },
        { 11, 'GetIsCameraLocked', 'uint8_t', '' },
        { 12, 'GetIsCameraLockedOn', 'uint8_t', '' },
        { 13, 'SetTargetIndex', 'void', 'uint32_t' },
        { 14, 'SetTargetServerId', 'void', 'uint32_t' },
        { 15, 'SetFollowDeltaX', 'void', 'float' },
        { 16, 'SetFollowDeltaZ', 'void', 'float' },
        { 17, 'SetFollowDeltaY', 'void', 'float' },
        { 18, 'SetFollowDeltaW', 'void', 'float' },
        { 19, 'SetFollowTargetIndex', 'void', 'uint32_t' },
        { 20, 'SetFollowTargetServerId', 'void', 'uint32_t' },
        { 21, 'SetIsFirstPersonCamera', 'void', 'uint8_t' },
        { 22, 'SetIsAutoRunning', 'void', 'uint8_t' },
        { 23, 'SetIsCameraLocked', 'void', 'uint8_t' },
        { 24, 'SetIsCameraLockedOn', 'void', 'uint8_t' },
    },
    ICastBar = {
        { 0, 'GetRawStructure', 'void*', '' },
        { 1, 'GetMax', 'float', '' },
        { 2, 'GetCount', 'float', '' },
        { 3, 'GetPercent', 'float', '' },
        { 4, 'GetCastType', 'uint32_t', '' },
        { 5, 'SetMax', 'void', 'float' },
        { 6, 'SetCount', 'void', 'float' },
        { 7, 'SetPercent', 'void', 'float' },
        { 8, 'SetCastType', 'void', 'uint32_t' },
    },
    IChatManager = {
        { 0, 'ParseCommand', 'void', 'int32_t, const char*' },
        { 1, 'QueueCommand', 'void', 'int32_t, const char*' },
        { 2, 'Write', 'void', 'int32_t, bool, const char*' },
        { 4, 'AddChatMessage', 'void', 'int32_t, bool, const char*' },
        { 6, 'ExecuteScript', 'void', 'const char*, const char*, bool' },
        { 7, 'ExecuteScriptString', 'void', 'const char*, const char*, bool' },
        { 8, 'GetInputTextRaw', 'const char*', '', 'string' },
        { 9, 'SetInputTextRaw', 'void', 'const char*, uint32_t' },
        { 10, 'GetInputTextRawLength', 'uint32_t', '' },
        { 11, 'GetInputTextRawCaretPosition', 'uint32_t', '' },
        { 12, 'GetInputTextParsed', 'const char*', '', 'string' },
        { 13, 'SetInputTextParsed', 'void', 'const char*' },
        { 14, 'GetInputTextParsedLength', 'uint32_t', '' },
        { 15, 'GetInputTextParsedLengthMax', 'uint32_t', '' },
        { 16, 'GetInputTextDisplay', 'const char*', '', 'string' },
        { 17, 'SetInputTextDisplay', 'void', 'const char*' },
        { 18, 'SetInputText', 'void', 'const char*' },
        { 19, 'IsInputOpen', 'uint8_t', '' },
        { 20, 'GetSilentAliases', 'bool', '' },
        { 21, 'SetSilentAliases', 'void', 'bool' },
    },
    IConfigurationManager = {
        { 0, 'Load', 'bool', 'const char*, const char*' },
        { 1, 'Save', 'bool', 'const char*, const char*' },
        { 2, 'Delete', 'void', 'const char*' },
        { 5, 'GetString', 'const char*', 'const char*, const char*, const char*', 'string' },
        { 6, 'SetValue', 'void', 'const char*, const char*, const char*, const char*' },
        { 7, 'GetBool', 'bool', 'const char*, const char*, const char*, bool' },
        { 8, 'GetUInt8', 'uint8_t', 'const char*, const char*, const char*, uint8_t' },
        { 9, 'GetUInt16', 'uint16_t', 'const char*, const char*, const char*, uint16_t' },
        { 10, 'GetUInt32', 'uint32_t', 'const char*, const char*, const char*, uint32_t' },
        { 11, 'GetUInt64', 'uint64_t', 'const char*, const char*, const char*, uint64_t' },
        { 12, 'GetInt8', 'int8_t', 'const char*, const char*, const char*, int8_t' },
        { 13, 'GetInt16', 'int16_t', 'const char*, const char*, const char*, int16_t' },
        { 14, 'GetInt32', 'int32_t', 'const char*, const char*, const char*, int32_t' },
        { 15, 'GetInt64', 'int64_t', 'const char*, const char*, const char*, int64_t' },
        { 16, 'GetFloat', 'float', 'const char*, const char*, const char*, float' },
        { 17, 'GetDouble', 'double', 'const char*, const char*, const char*, double' },
    },
    IController = {
        { 1, 'RemoveCallback', 'void', 'const char*' },
        { 2, 'QueueButtonData', 'void', 'uint32_t, uint32_t' },
        { 3, 'GetTrackDeadZone', 'bool', '' },
        { 4, 'SetTrackDeadZone', 'void', 'bool' },
    },
    IEntity = {
        { 0, 'GetRawEntity', 'void*', 'uint32_t' },
        { 1, 'GetEntityMapSize', 'uint32_t', '' },
        { 2, 'GetLocalPositionX', 'float', 'uint32_t' },
        { 3, 'GetLocalPositionZ', 'float', 'uint32_t' },
        { 4, 'GetLocalPositionY', 'float', 'uint32_t' },
        { 5, 'GetLocalPositionW', 'float', 'uint32_t' },
        { 6, 'GetLocalPositionRoll', 'float', 'uint32_t' },
        { 7, 'GetLocalPositionYaw', 'float', 'uint32_t' },
        { 8, 'GetLocalPositionPitch', 'float', 'uint32_t' },
        { 9, 'GetLastPositionX', 'float', 'uint32_t' },
        { 10, 'GetLastPositionZ', 'float', 'uint32_t' },
        { 11, 'GetLastPositionY', 'float', 'uint32_t' },
        { 12, 'GetLastPositionW', 'float', 'uint32_t' },
        { 13, 'GetLastPositionRoll', 'float', 'uint32_t' },
        { 14, 'GetLastPositionYaw', 'float', 'uint32_t' },
        { 15, 'GetLastPositionPitch', 'float', 'uint32_t' },
        { 16, 'GetMoveX', 'float', 'uint32_t' },
        { 17, 'GetMoveZ', 'float', 'uint32_t' },
        { 18, 'GetMoveY', 'float', 'uint32_t' },
        { 19, 'GetMoveW', 'float', 'uint32_t' },
        { 20, 'GetMoveDeltaX', 'float', 'uint32_t' },
        { 21, 'GetMoveDeltaZ', 'float', 'uint32_t' },
        { 22, 'GetMoveDeltaY', 'float', 'uint32_t' },
        { 23, 'GetMoveDeltaW', 'float', 'uint32_t' },
        { 24, 'GetMoveDeltaRoll', 'float', 'uint32_t' },
        { 25, 'GetMoveDeltaYaw', 'float', 'uint32_t' },
        { 26, 'GetMoveDeltaPitch', 'float', 'uint32_t' },
        { 27, 'GetTargetIndex', 'uint32_t', 'uint32_t' },
        { 28, 'GetServerId', 'uint32_t', 'uint32_t' },
        { 29, 'GetName', 'const char*', 'uint32_t', 'string' },
        { 30, 'GetMovementSpeed', 'float', 'uint32_t' },
        { 31, 'GetAnimationSpeed', 'float', 'uint32_t' },
        { 32, 'GetActorPointer', 'uintptr_t', 'uint32_t' },
        { 33, 'GetAttachment', 'uintptr_t', 'uint32_t, uint32_t' },
        { 34, 'GetEventPointer', 'uintptr_t', 'uint32_t' },
        { 35, 'GetDistance', 'float', 'uint32_t' },
        { 36, 'GetTurnSpeed', 'uint32_t', 'uint32_t' },
        { 37, 'GetTurnSpeedHead', 'uint32_t', 'uint32_t' },
        { 38, 'GetHeading', 'float', 'uint32_t' },
        { 39, 'GetNext', 'uintptr_t', 'uint32_t' },
        { 40, 'GetHPPercent', 'uint8_t', 'uint32_t' },
        { 41, 'GetType', 'uint8_t', 'uint32_t' },
        { 42, 'GetRace', 'uint8_t', 'uint32_t' },
        { 43, 'GetLocalMoveCount', 'uint16_t', 'uint32_t' },
        { 44, 'GetActorLockFlag', 'uint16_t', 'uint32_t' },
        { 45, 'GetModelUpdateFlags', 'uint16_t', 'uint32_t' },
        { 46, 'GetDoorId', 'uint32_t', 'uint32_t' },
        { 47, 'GetLookHair', 'uint16_t', 'uint32_t' },
        { 48, 'GetLookHead', 'uint16_t', 'uint32_t' },
        { 49, 'GetLookBody', 'uint16_t', 'uint32_t' },
        { 50, 'GetLookHands', 'uint16_t', 'uint32_t' },
        { 51, 'GetLookLegs', 'uint16_t', 'uint32_t' },
        { 52, 'GetLookFeet', 'uint16_t', 'uint32_t' },
        { 53, 'GetLookMain', 'uint16_t', 'uint32_t' },
        { 54, 'GetLookSub', 'uint16_t', 'uint32_t' },
        { 55, 'GetLookRanged', 'uint16_t', 'uint32_t' },
        { 56, 'GetActionTimer1', 'uint16_t', 'uint32_t' },
        { 57, 'GetActionTimer2', 'uint16_t', 'uint32_t' },
        { 58, 'GetRenderFlags0', 'uint32_t', 'uint32_t' },
        { 59, 'GetRenderFlags1', 'uint32_t', 'uint32_t' },
        { 60, 'GetRenderFlags2', 'uint32_t', 'uint32_t' },
        { 61, 'GetRenderFlags3', 'uint32_t', 'uint32_t' },
        { 62, 'GetRenderFlags4', 'uint32_t', 'uint32_t' },
        { 63, 'GetRenderFlags5', 'uint32_t', 'uint32_t' },
        { 64, 'GetRenderFlags6', 'uint32_t', 'uint32_t' },
        { 65, 'GetRenderFlags7', 'uint32_t', 'uint32_t' },
        { 66, 'GetRenderFlags8', 'uint32_t', 'uint32_t' },
        { 67, 'GetPopEffect', 'uint8_t', 'uint32_t' },
        { 68, 'GetUpdateMask', 'uint8_t', 'uint32_t' },
        { 69, 'GetInteractionTargetIndex', 'uint16_t', 'uint32_t' },
        { 70, 'GetNpcSpeechFrame', 'uint16_t', 'uint32_t' },
        { 71, 'GetLookAxisX', 'uint16_t', 'uint32_t' },
        { 72, 'GetLookAxisY', 'uint16_t', 'uint32_t' },
        { 73, 'GetMouthCounter', 'uint16_t', 'uint32_t' },
        { 74, 'GetMouthWaitCounter', 'uint16_t', 'uint32_t' },
        { 75, 'GetCraftTimerUnknown', 'uint16_t', 'uint32_t' },
        { 76, 'GetCraftServerId', 'uint32_t', 'uint32_t' },
        { 77, 'GetCraftAnimationEffect', 'uint16_t', 'uint32_t' },
        { 78, 'GetCraftAnimationStep', 'uint8_t', 'uint32_t' },
        { 79, 'GetCraftParam', 'uint8_t', 'uint32_t' },
        { 80, 'GetMovementSpeed2', 'float', 'uint32_t' },
        { 81, 'GetNpcWalkPosition1', 'uint16_t', 'uint32_t' },
        { 82, 'GetNpcWalkPosition2', 'uint16_t', 'uint32_t' },
        { 83, 'GetNpcWalkMode', 'uint16_t', 'uint32_t' },
        { 84, 'GetCostumeId', 'uint16_t', 'uint32_t' },
        { 85, 'GetMou4', 'uint32_t', 'uint32_t' },
        { 86, 'GetStatusServer', 'uint32_t', 'uint32_t' },
        { 87, 'GetStatus', 'uint32_t', 'uint32_t' },
        { 88, 'GetStatusEvent', 'uint32_t', 'uint32_t' },
        { 89, 'GetModelTime', 'uint32_t', 'uint32_t' },
        { 90, 'GetModelStartTime', 'uint32_t', 'uint32_t' },
        { 91, 'GetClaimStatus', 'uint32_t', 'uint32_t' },
        { 92, 'GetZoneId', 'uint32_t', 'uint32_t' },
        { 93, 'GetAnimation', 'uint32_t', 'uint32_t, uint32_t' },
        { 94, 'GetAnimationTime', 'uint16_t', 'uint32_t' },
        { 95, 'GetAnimationStep', 'uint16_t', 'uint32_t' },
        { 96, 'GetAnimationPlay', 'uint8_t', 'uint32_t' },
        { 97, 'GetEmoteTargetIndex', 'uint16_t', 'uint32_t' },
        { 98, 'GetEmoteId', 'uint16_t', 'uint32_t' },
        { 99, 'GetEmoteIdString', 'uint32_t', 'uint32_t' },
        { 100, 'GetEmoteTargetActorPointer', 'uintptr_t', 'uint32_t' },
        { 101, 'GetSpawnFlags', 'uint32_t', 'uint32_t' },
        { 102, 'GetLinkshellColor', 'uint32_t', 'uint32_t' },
        { 103, 'GetNameColor', 'uint16_t', 'uint32_t' },
        { 104, 'GetCampaignNameFlag', 'uint8_t', 'uint32_t' },
        { 105, 'GetMountId', 'uint8_t', 'uint32_t' },
        { 106, 'GetFishingUnknown0000', 'int32_t', 'uint32_t' },
        { 107, 'GetFishingUnknown0001', 'int32_t', 'uint32_t' },
        { 108, 'GetFishingActionCountdown', 'int32_t', 'uint32_t' },
        { 109, 'GetFishingRodCastTime', 'int16_t', 'uint32_t' },
        { 110, 'GetFishingUnknown0002', 'int16_t', 'uint32_t' },
        { 111, 'GetLastActionId', 'uint32_t', 'uint32_t' },
        { 112, 'GetLastActionActorPointer', 'uintptr_t', 'uint32_t' },
        { 113, 'GetTargetedIndex', 'uint16_t', 'uint32_t' },
        { 114, 'GetPetTargetIndex', 'uint16_t', 'uint32_t' },
        { 115, 'GetUpdateRequestDelay', 'uint16_t', 'uint32_t' },
        { 116, 'GetIsDirty', 'uint8_t', 'uint32_t' },
        { 117, 'GetBallistaFlags', 'uint8_t', 'uint32_t' },
        { 118, 'GetPankrationEnabled', 'uint8_t', 'uint32_t' },
        { 119, 'GetPankrationFlagFlip', 'uint8_t', 'uint32_t' },
        { 120, 'GetModelSize', 'float', 'uint32_t' },
        { 121, 'GetModelHitboxSize', 'float', 'uint32_t' },
        { 122, 'GetEnvironmentAreaId', 'uint32_t', 'uint32_t' },
        { 123, 'GetMonstrosityFlag', 'uint16_t', 'uint32_t' },
        { 124, 'GetMonstrosityNameId', 'uint16_t', 'uint32_t' },
        { 125, 'GetMonstrosityName', 'const char*', 'uint32_t', 'string' },
        { 126, 'GetMonstrosityNameEnd', 'uint8_t', 'uint32_t' },
        { 127, 'GetMonstrosityNameAbbr', 'const char*', 'uint32_t', 'string' },
        { 128, 'GetMonstrosityNameAbbrEnd', 'uint8_t', 'uint32_t' },
        { 129, 'GetCustomProperties', 'uint8_t*', 'uint32_t' },
        { 130, 'GetBallistaInfo', 'uint8_t*', 'uint32_t' },
        { 131, 'GetFellowTargetIndex', 'uint16_t', 'uint32_t' },
        { 132, 'GetWarpTargetIndex', 'uint16_t', 'uint32_t' },
        { 133, 'GetTrustOwnerTargetIndex', 'uint16_t', 'uint32_t' },
        { 134, 'GetAreaDisplayTargetIndex', 'uint16_t', 'uint32_t' },
        { 135, 'SetLocalPositionX', 'void', 'uint32_t, float' },
        { 136, 'SetLocalPositionZ', 'void', 'uint32_t, float' },
        { 137, 'SetLocalPositionY', 'void', 'uint32_t, float' },
        { 138, 'SetLocalPositionW', 'void', 'uint32_t, float' },
        { 139, 'SetLocalPositionRoll', 'void', 'uint32_t, float' },
        { 140, 'SetLocalPositionYaw', 'void', 'uint32_t, float' },
        { 141, 'SetLocalPositionPitch', 'void', 'uint32_t, float' },
        { 142, 'SetLastPositionX', 'void', 'uint32_t, float' },
        { 143, 'SetLastPositionZ', 'void', 'uint32_t, float' },
        { 144, 'SetLastPositionY', 'void', 'uint32_t, float' },
        { 145, 'SetLastPositionW', 'void', 'uint32_t, float' },
        { 146, 'SetLastPositionRoll', 'void', 'uint32_t, float' },
        { 147, 'SetLastPositionYaw', 'void', 'uint32_t, float' },
        { 148, 'SetLastPositionPitch', 'void', 'uint32_t, float' },
        { 149, 'SetMoveX', 'void', 'uint32_t, float' },
        { 150, 'SetMoveZ', 'void', 'uint32_t, float' },
        { 151, 'SetMoveY', 'void', 'uint32_t, float' },
        { 152, 'SetMoveW', 'void', 'uint32_t, float' },
        { 153, 'SetMoveDeltaX', 'void', 'uint32_t, float' },
        { 154, 'SetMoveDeltaZ', 'void', 'uint32_t, float' },
        { 155, 'SetMoveDeltaY', 'void', 'uint32_t, float' },
        { 156, 'SetMoveDeltaW', 'void', 'uint32_t, float' },
        { 157, 'SetMoveDeltaRoll', 'void', 'uint32_t, float' },
        { 158, 'SetMoveDeltaYaw', 'void', 'uint32_t, float' },
        { 159, 'SetMoveDeltaPitch', 'void', 'uint32_t, float' },
        { 160, 'SetTargetIndex', 'void', 'uint32_t, uint32_t' },
        { 161, 'SetServerId', 'void', 'uint32_t, uint32_t' },
        { 162, 'SetName', 'void', 'uint32_t, const char*' },
        { 163, 'SetMovementSpeed', 'void', 'uint32_t, float' },
        { 164, 'SetAnimationSpeed', 'void', 'uint32_t, float' },
        { 165, 'SetActorPointer', 'void', 'uint32_t, uintptr_t' },
        { 166, 'SetAttachment', 'void', 'uint32_t, uint32_t, uintptr_t' },
        { 167, 'SetEventPointer', 'void', 'uint32_t, uintptr_t' },
        { 168, 'SetDistance', 'void', 'uint32_t, float' },
        { 169, 'SetTurnSpeed', 'void', 'uint32_t, uint32_t' },
        { 170, 'SetTurnSpeedHead', 'void', 'uint32_t, uint32_t' },
        { 171, 'SetHeading', 'void', 'uint32_t, float' },
        { 172, 'SetNext', 'void', 'uint32_t, uintptr_t' },
        { 173, 'SetHPPercent', 'void', 'uint32_t, uint8_t' },
        { 174, 'SetType', 'void', 'uint32_t, uint8_t' },
        { 175, 'SetRace', 'void', 'uint32_t, uint8_t' },
        { 176, 'SetLocalMoveCount', 'void', 'uint32_t, uint16_t' },
        { 177, 'SetActorLockFlag', 'void', 'uint32_t, uint16_t' },
        { 178, 'SetModelUpdateFlags', 'void', 'uint32_t, uint16_t' },
        { 179, 'SetDoorId', 'void', 'uint32_t, uint32_t' },
        { 180, 'SetLookHair', 'void', 'uint32_t, uint16_t' },
        { 181, 'SetLookHead', 'void', 'uint32_t, uint16_t' },
        { 182, 'SetLookBody', 'void', 'uint32_t, uint16_t' },
        { 183, 'SetLookHands', 'void', 'uint32_t, uint16_t' },
        { 184, 'SetLookLegs', 'void', 'uint32_t, uint16_t' },
        { 185, 'SetLookFeet', 'void', 'uint32_t, uint16_t' },
        { 186, 'SetLookMain', 'void', 'uint32_t, uint16_t' },
        { 187, 'SetLookSub', 'void', 'uint32_t, uint16_t' },
        { 188, 'SetLookRanged', 'void', 'uint32_t, uint16_t' },
        { 189, 'SetActionTimer1', 'void', 'uint32_t, uint16_t' },
        { 190, 'SetActionTimer2', 'void', 'uint32_t, uint16_t' },
        { 191, 'SetRenderFlags0', 'void', 'uint32_t, uint32_t' },
        { 192, 'SetRenderFlags1', 'void', 'uint32_t, uint32_t' },
        { 193, 'SetRenderFlags2', 'void', 'uint32_t, uint32_t' },
        { 194, 'SetRenderFlags3', 'void', 'uint32_t, uint32_t' },
        { 195, 'SetRenderFlags4', 'void', 'uint32_t, uint32_t' },
        { 196, 'SetRenderFlags5', 'void', 'uint32_t, uint32_t' },
        { 197, 'SetRenderFlags6', 'void', 'uint32_t, uint32_t' },
        { 198, 'SetRenderFlags7', 'void', 'uint32_t, uint32_t' },
        { 199, 'SetRenderFlags8', 'void', 'uint32_t, uint32_t' },
        { 200, 'SetPopEffect', 'void', 'uint32_t, uint8_t' },
        { 201, 'SetUpdateMask', 'void', 'uint32_t, uint8_t' },
        { 202, 'SetInteractionTargetIndex', 'void', 'uint32_t, uint16_t' },
        { 203, 'SetNpcSpeechFrame', 'void', 'uint32_t, uint16_t' },
        { 204, 'SetLookAxisX', 'void', 'uint32_t, uint16_t' },
        { 205, 'SetLookAxisY', 'void', 'uint32_t, uint16_t' },
        { 206, 'SetMouthCounter', 'void', 'uint32_t, uint16_t' },
        { 207, 'SetMouthWaitCounter', 'void', 'uint32_t, uint16_t' },
        { 208, 'SetCraftTimerUnknown', 'void', 'uint32_t, uint16_t' },
        { 209, 'SetCraftServerId', 'void', 'uint32_t, uint32_t' },
        { 210, 'SetCraftAnimationEffect', 'void', 'uint32_t, uint16_t' },
        { 211, 'SetCraftAnimationStep', 'void', 'uint32_t, uint8_t' },
        { 212, 'SetCraftParam', 'void', 'uint32_t, uint8_t' },
        { 213, 'SetMovementSpeed2', 'void', 'uint32_t, float' },
        { 214, 'SetNpcWalkPosition1', 'void', 'uint32_t, uint16_t' },
        { 215, 'SetNpcWalkPosition2', 'void', 'uint32_t, uint16_t' },
        { 216, 'SetNpcWalkMode', 'void', 'uint32_t, uint16_t' },
        { 217, 'SetCostumeId', 'void', 'uint32_t, uint16_t' },
        { 218, 'SetMou4', 'void', 'uint32_t, uint32_t' },
        { 219, 'SetStatusServer', 'void', 'uint32_t, uint32_t' },
        { 220, 'SetStatus', 'void', 'uint32_t, uint32_t' },
        { 221, 'SetStatusEvent', 'void', 'uint32_t, uint32_t' },
        { 222, 'SetModelTime', 'void', 'uint32_t, uint32_t' },
        { 223, 'SetModelStartTime', 'void', 'uint32_t, uint32_t' },
        { 224, 'SetClaimStatus', 'void', 'uint32_t, uint32_t' },
        { 225, 'SetZoneId', 'void', 'uint32_t, uint32_t' },
        { 226, 'SetAnimation', 'void', 'uint32_t, uint32_t, uint32_t' },
        { 227, 'SetAnimationTime', 'void', 'uint32_t, uint16_t' },
        { 228, 'SetAnimationStep', 'void', 'uint32_t, uint16_t' },
        { 229, 'SetAnimationPlay', 'void', 'uint32_t, uint8_t' },
        { 230, 'SetEmoteTargetIndex', 'void', 'uint32_t, uint16_t' },
        { 231, 'SetEmoteId', 'void', 'uint32_t, uint16_t' },
        { 232, 'SetEmoteIdString', 'void', 'uint32_t, uint32_t' },
        { 233, 'SetEmoteTargetActorPointer', 'void', 'uint32_t, uintptr_t' },
        { 234, 'SetSpawnFlags', 'void', 'uint32_t, uint32_t' },
        { 235, 'SetLinkshellColor', 'void', 'uint32_t, uint32_t' },
        { 236, 'SetNameColor', 'void', 'uint32_t, uint16_t' },
        { 237, 'SetCampaignNameFlag', 'void', 'uint32_t, uint8_t' },
        { 238, 'SetMountId', 'void', 'uint32_t, uint8_t' },
        { 239, 'SetFishingUnknown0000', 'void', 'uint32_t, int32_t' },
        { 240, 'SetFishingUnknown0001', 'void', 'uint32_t, int32_t' },
        { 241, 'SetFishingActionCountdown', 'void', 'uint32_t, int16_t' },
        { 242, 'SetFishingRodCastTime', 'void', 'uint32_t, int16_t' },
        { 243, 'SetFishingUnknown0002', 'void', 'uint32_t, int16_t' },
        { 244, 'SetLastActionId', 'void', 'uint32_t, uint32_t' },
        { 245, 'SetLastActionActorPointer', 'void', 'uint32_t, uintptr_t' },
        { 246, 'SetTargetedIndex', 'void', 'uint32_t, uint16_t' },
        { 247, 'SetPetTargetIndex', 'void', 'uint32_t, uint16_t' },
        { 248, 'SetUpdateRequestDelay', 'void', 'uint32_t, uint16_t' },
        { 249, 'SetIsDirty', 'void', 'uint32_t, uint8_t' },
        { 250, 'SetBallistaFlags', 'void', 'uint32_t, uint8_t' },
        { 251, 'SetPankrationEnabled', 'void', 'uint32_t, uint8_t' },
        { 252, 'SetPankrationFlagFlip', 'void', 'uint32_t, uint8_t' },
        { 253, 'SetModelSize', 'void', 'uint32_t, float' },
        { 254, 'SetModelHitboxSize', 'void', 'uint32_t, float' },
        { 255, 'SetEnvironmentAreaId', 'void', 'uint32_t, uint32_t' },
        { 256, 'SetMonstrosityFlag', 'void', 'uint32_t, uint16_t' },
        { 257, 'SetMonstrosityNameId', 'void', 'uint32_t, uint16_t' },
        { 258, 'SetMonstrosityName', 'void', 'uint32_t, const char*' },
        { 259, 'SetMonstrosityNameEnd', 'void', 'uint32_t, uint8_t' },
        { 260, 'SetMonstrosityNameAbbr', 'void', 'uint32_t, const char*' },
        { 261, 'SetMonstrosityNameAbbrEnd', 'void', 'uint32_t, uint8_t' },
        { 264, 'SetFellowTargetIndex', 'void', 'uint32_t, uint16_t' },
        { 265, 'SetWarpTargetIndex', 'void', 'uint32_t, uint16_t' },
        { 266, 'SetTrustOwnerTargetIndex', 'void', 'uint32_t, uint16_t' },
        { 267, 'SetAreaDisplayTargetIndex', 'void', 'uint32_t, uint16_t' },
    },
    IFontManager = {
        { 0, 'Create', 'void*', 'const char*', 'font' },
        { 1, 'Get', 'void*', 'const char*', 'font' },
        { 2, 'Delete', 'void', 'const char*' },
        { 3, 'GetFocusedObject', 'void*', '', 'font' },
        { 4, 'SetFocusedObject', 'bool', 'const char*' },
        { 5, 'GetVisible', 'bool', '' },
        { 6, 'SetVisible', 'void', 'bool' },
    },
    IFontObject = {
        { 0, 'Render', 'void', '' },
        { 2, 'HitTest', 'bool', 'int32_t, int32_t' },
        { 3, 'GetParent', 'void*', '', 'font' },
        { 4, 'GetBackground', 'void*', '', 'primitive' },
        { 5, 'GetRealPositionX', 'float', '' },
        { 6, 'GetRealPositionY', 'float', '' },
        { 7, 'GetAlias', 'const char*', '', 'string' },
        { 8, 'GetVisible', 'bool', '' },
        { 9, 'GetCanFocus', 'bool', '' },
        { 10, 'GetLocked', 'bool', '' },
        { 11, 'GetLockedZ', 'bool', '' },
        { 12, 'GetIsDirty', 'bool', '' },
        { 13, 'GetWindowWidth', 'float', '' },
        { 14, 'GetWindowHeight', 'float', '' },
        { 15, 'GetFontFile', 'const char*', '', 'string' },
        { 16, 'GetFontFamily', 'const char*', '', 'string' },
        { 17, 'GetFontHeight', 'uint32_t', '' },
        { 18, 'GetCreateFlags', 'uint32_t', '' },
        { 19, 'GetDrawFlags', 'uint32_t', '' },
        { 20, 'GetBold', 'bool', '' },
        { 21, 'GetItalic', 'bool', '' },
        { 22, 'GetRightJustified', 'bool', '' },
        { 23, 'GetStrikeThrough', 'bool', '' },
        { 24, 'GetUnderlined', 'bool', '' },
        { 25, 'GetColor', 'uint32_t', '' },
        { 26, 'GetColorOutline', 'uint32_t', '' },
        { 27, 'GetPadding', 'float', '' },
        { 28, 'GetPositionX', 'float', '' },
        { 29, 'GetPositionY', 'float', '' },
        { 30, 'GetAutoResize', 'bool', '' },
        { 31, 'GetAnchor', 'uint32_t', '' },
        { 32, 'GetAnchorParent', 'uint32_t', '' },
        { 33, 'GetText', 'const char*', '', 'string' },
        { 34, 'SetAlias', 'void', 'const char*' },
        { 35, 'SetVisible', 'void', 'bool' },
        { 36, 'SetCanFocus', 'void', 'bool' },
        { 37, 'SetLocked', 'void', 'bool' },
        { 38, 'SetLockedZ', 'void', 'bool' },
        { 39, 'SetIsDirty', 'void', 'bool' },
        { 40, 'SetWindowWidth', 'void', 'float' },
        { 41, 'SetWindowHeight', 'void', 'float' },
        { 42, 'SetFontFile', 'void', 'const char*' },
        { 43, 'SetFontFamily', 'void', 'const char*' },
        { 44, 'SetFontHeight', 'void', 'uint32_t' },
        { 45, 'SetCreateFlags', 'void', 'uint32_t' },
        { 46, 'SetDrawFlags', 'void', 'uint32_t' },
        { 47, 'SetBold', 'void', 'bool' },
        { 48, 'SetItalic', 'void', 'bool' },
        { 49, 'SetRightJustified', 'void', 'bool' },
        { 50, 'SetStrikeThrough', 'void', 'bool' },
        { 51, 'SetUnderlined', 'void', 'bool' },
        { 52, 'SetColor', 'void', 'uint32_t' },
        { 53, 'SetColorOutline', 'void', 'uint32_t' },
        { 54, 'SetPadding', 'void', 'float' },
        { 55, 'SetPositionX', 'void', 'float' },
        { 56, 'SetPositionY', 'void', 'float' },
        { 57, 'SetAutoResize', 'void', 'bool' },
        { 58, 'SetAnchor', 'void', 'uint32_t' },
        { 59, 'SetAnchorParent', 'void', 'uint32_t' },
        { 60, 'SetText', 'void', 'const char*' },
        { 61, 'SetParent', 'void', 'void*' },
    },
    IInputManager = {
        { 0, 'GetController', 'void*', '', 'iface:IController' },
        { 1, 'GetKeyboard', 'void*', '', 'iface:IKeyboard' },
        { 2, 'GetMouse', 'void*', '', 'iface:IMouse' },
        { 3, 'GetXInput', 'void*', '', 'iface:IXInput' },
        { 4, 'GetAllowGamepadInBackground', 'bool', '' },
        { 5, 'SetAllowGamepadInBackground', 'void', 'bool' },
        { 6, 'GetDisableGamepad', 'bool', '' },
        { 7, 'SetDisableGamepad', 'void', 'bool' },
    },
    IInventory = {
        { 0, 'GetRawStructure', 'void*', '' },
        { 1, 'GetContainerItem', 'void*', 'uint32_t, uint32_t' },
        { 2, 'GetContainerCount', 'uint32_t', 'uint32_t' },
        { 3, 'GetContainerCountMax', 'uint32_t', 'uint32_t' },
        { 4, 'GetTreasurePoolItem', 'vana_treasureitem_t*', 'uint32_t' },
        { 5, 'GetTreasurePoolStatus', 'uint32_t', '' },
        { 6, 'GetTreasurePoolItemCount', 'uint32_t', '' },
        { 7, 'GetContainerUpdateCounter', 'uint32_t', '' },
        { 8, 'GetContainerUpdateFlags', 'uint32_t', '' },
        { 9, 'GetDisplayItemSlot', 'uint32_t', '' },
        { 10, 'GetDisplayItemPointer', 'uint32_t', '' },
        { 11, 'GetEquippedItem', 'void*', 'uint32_t' },
        { 12, 'GetCheckEquippedItem', 'vana_itemcheck_t*', 'uint32_t' },
        { 13, 'GetCheckTargetIndex', 'uint32_t', '' },
        { 14, 'GetCheckServerId', 'uint32_t', '' },
        { 15, 'GetCheckFlags', 'uint32_t', '' },
        { 16, 'GetCheckMainJob', 'uint8_t', '' },
        { 17, 'GetCheckSubJob', 'uint8_t', '' },
        { 18, 'GetCheckMainJobLevel', 'uint8_t', '' },
        { 19, 'GetCheckSubJobLevel', 'uint8_t', '' },
        { 20, 'GetCheckMainJob2', 'uint8_t', '' },
        { 21, 'GetCheckMasteryLevel', 'uint8_t', '' },
        { 22, 'GetCheckMasteryFlags', 'uint8_t', '' },
        { 23, 'GetCheckLinkshellName', 'uint8_t*', '' },
        { 24, 'GetCheckLinkshellColor', 'uint16_t', '' },
        { 25, 'GetCheckLinkshellIconSetId', 'uint8_t', '' },
        { 26, 'GetCheckLinkshellIconSetIndex', 'uint8_t', '' },
        { 27, 'GetCheckBallistaChevronCount', 'uint32_t', '' },
        { 28, 'GetCheckBallistaChevronFlags', 'uint16_t', '' },
        { 29, 'GetCheckBallistaFlags', 'uint16_t', '' },
        { 30, 'GetSearchComment', 'const char*', '', 'string' },
        { 31, 'GetCraftStatus', 'uint32_t', '' },
        { 32, 'GetCraftCallback', 'uintptr_t', '' },
        { 33, 'GetCraftTimestampAttempt', 'uint32_t', '' },
        { 34, 'GetCraftTimestampResponse', 'uint32_t', '' },
        { 35, 'SetCraftStatus', 'void', 'uint32_t' },
        { 36, 'SetCraftCallback', 'void', 'uintptr_t' },
        { 37, 'SetCraftTimestampAttempt', 'void', 'uint32_t' },
        { 38, 'SetCraftTimestampResponse', 'void', 'uint32_t' },
        { 39, 'GetSelectedItemName', 'const char*', '', 'string' },
        { 40, 'GetSelectedItemId', 'uint16_t', '' },
        { 41, 'GetSelectedItemIndex', 'uint8_t', '' },
    },
    IKeyboard = {
        { 0, 'Bind', 'void', 'uint32_t, bool, bool, bool, bool, bool, bool, const char*' },
        { 1, 'Unbind', 'void', 'uint32_t, bool, bool, bool, bool, bool, bool' },
        { 2, 'UnbindAll', 'void', '' },
        { 3, 'IsBound', 'bool', 'uint32_t, bool, bool, bool, bool, bool, bool' },
        { 5, 'RemoveCallback', 'void', 'const char*' },
        { 6, 'V2D', 'uint32_t', 'uint32_t' },
        { 7, 'D2V', 'uint32_t', 'uint32_t' },
        { 8, 'S2D', 'uint32_t', 'const char*' },
        { 9, 'D2S', 'const char*', 'uint32_t', 'string' },
        { 10, 'GetWindowsKeyEnabled', 'bool', '' },
        { 11, 'SetWindowsKeyEnabled', 'void', 'bool' },
        { 12, 'GetBlockInput', 'bool', '' },
        { 13, 'SetBlockInput', 'void', 'bool' },
        { 14, 'GetBlockBindsDuringInput', 'bool', '' },
        { 15, 'SetBlockBindsDuringInput', 'void', 'bool' },
        { 16, 'GetSilentBinds', 'bool', '' },
        { 17, 'SetSilentBinds', 'void', 'bool' },
    },
    ILogManager = {
        { 0, 'Log', 'bool', 'uint32_t, const char*, const char*' },
        { 2, 'GetLogLevel', 'uint32_t', '' },
        { 3, 'SetLogLevel', 'void', 'uint32_t' },
    },
    IMemoryManager = {
        { 0, 'GetAutoFollow', 'void*', '', 'iface:IAutoFollow' },
        { 1, 'GetCastBar', 'void*', '', 'iface:ICastBar' },
        { 2, 'GetEntity', 'void*', '', 'iface:IEntity' },
        { 3, 'GetInventory', 'void*', '', 'iface:IInventory' },
        { 4, 'GetParty', 'void*', '', 'iface:IParty' },
        { 5, 'GetPlayer', 'void*', '', 'iface:IPlayer' },
        { 6, 'GetRecast', 'void*', '', 'iface:IRecast' },
        { 7, 'GetTarget', 'void*', '', 'iface:ITarget' },
    },
    IMouse = {
        { 1, 'RemoveCallback', 'void', 'const char*' },
        { 2, 'GetBlockInput', 'bool', '' },
        { 3, 'SetBlockInput', 'void', 'bool' },
    },
    IOffsetManager = {
        { 0, 'Add', 'void', 'const char*, const char*, int32_t' },
        { 1, 'Get', 'int32_t', 'const char*, const char*' },
        { 3, 'Delete', 'void', 'const char*' },
        { 2, 'Delete', 'void', 'const char*, const char*' },
    },
    IPacketManager = {
    },
    IParty = {
        { 0, 'GetRawStructure', 'void*', '' },
        { 1, 'GetAllianceLeaderServerId', 'uint32_t', '' },
        { 2, 'GetAlliancePartyLeaderServerId1', 'uint32_t', '' },
        { 3, 'GetAlliancePartyLeaderServerId2', 'uint32_t', '' },
        { 4, 'GetAlliancePartyLeaderServerId3', 'uint32_t', '' },
        { 5, 'GetAlliancePartyVisible1', 'int8_t', '' },
        { 6, 'GetAlliancePartyVisible2', 'int8_t', '' },
        { 7, 'GetAlliancePartyVisible3', 'int8_t', '' },
        { 8, 'GetAlliancePartyMemberCount1', 'int8_t', '' },
        { 9, 'GetAlliancePartyMemberCount2', 'int8_t', '' },
        { 10, 'GetAlliancePartyMemberCount3', 'int8_t', '' },
        { 11, 'GetAllianceInvited', 'int8_t', '' },
        { 12, 'GetMemberIndex', 'uint8_t', 'uint32_t' },
        { 13, 'GetMemberNumber', 'uint8_t', 'uint32_t' },
        { 14, 'GetMemberName', 'const char*', 'uint32_t', 'string' },
        { 15, 'GetMemberServerId', 'uint32_t', 'uint32_t' },
        { 16, 'GetMemberTargetIndex', 'uint32_t', 'uint32_t' },
        { 17, 'GetMemberLastUpdatedTimestamp', 'uint32_t', 'uint32_t' },
        { 18, 'GetMemberHP', 'uint32_t', 'uint32_t' },
        { 19, 'GetMemberMP', 'uint32_t', 'uint32_t' },
        { 20, 'GetMemberTP', 'uint32_t', 'uint32_t' },
        { 21, 'GetMemberHPPercent', 'uint8_t', 'uint32_t' },
        { 22, 'GetMemberMPPercent', 'uint8_t', 'uint32_t' },
        { 23, 'GetMemberZone', 'uint16_t', 'uint32_t' },
        { 24, 'GetMemberZone2', 'uint16_t', 'uint32_t' },
        { 25, 'GetMemberFlagMask', 'uint32_t', 'uint32_t' },
        { 26, 'GetMemberTreasureLot', 'uint16_t', 'uint32_t, uint32_t' },
        { 27, 'GetMemberMonstrosityItemId', 'uint16_t', 'uint32_t' },
        { 28, 'GetMemberMonstrosityPrefixFlag1', 'uint8_t', 'uint32_t' },
        { 29, 'GetMemberMonstrosityPrefixFlag2', 'uint8_t', 'uint32_t' },
        { 30, 'GetMemberMonstrosityName', 'const char*', 'uint32_t', 'string' },
        { 31, 'GetMemberMainJob', 'uint8_t', 'uint32_t' },
        { 32, 'GetMemberMainJobLevel', 'uint8_t', 'uint32_t' },
        { 33, 'GetMemberSubJob', 'uint8_t', 'uint32_t' },
        { 34, 'GetMemberSubJobLevel', 'uint8_t', 'uint32_t' },
        { 35, 'GetMemberServerId2', 'uint32_t', 'uint32_t' },
        { 36, 'GetMemberHPPercent2', 'uint8_t', 'uint32_t' },
        { 37, 'GetMemberMPPercent2', 'uint8_t', 'uint32_t' },
        { 38, 'GetMemberIsActive', 'uint8_t', 'uint32_t' },
        { 39, 'GetRawStructureStatusIcons', 'void*', '' },
        { 40, 'GetStatusIconsServerId', 'uint32_t', 'uint32_t' },
        { 41, 'GetStatusIconsTargetIndex', 'uint32_t', 'uint32_t' },
        { 42, 'GetStatusIconsBitMask', 'uint64_t', 'uint32_t' },
        { 43, 'GetStatusIcons', 'uint8_t*', 'uint32_t' },
    },
    IPlayer = {
        { 0, 'GetRawStructure', 'void*', '' },
        { 1, 'GetHPMax', 'uint32_t', '' },
        { 2, 'GetMPMax', 'uint32_t', '' },
        { 3, 'GetMainJob', 'uint8_t', '' },
        { 4, 'GetMainJobLevel', 'uint8_t', '' },
        { 5, 'GetSubJob', 'uint8_t', '' },
        { 6, 'GetSubJobLevel', 'uint8_t', '' },
        { 7, 'GetExpCurrent', 'uint16_t', '' },
        { 8, 'GetExpNeeded', 'uint16_t', '' },
        { 9, 'GetStat', 'int16_t', 'uint32_t' },
        { 10, 'GetStatModifier', 'int16_t', 'uint32_t' },
        { 11, 'GetAttack', 'int16_t', '' },
        { 12, 'GetDefense', 'int16_t', '' },
        { 13, 'GetResist', 'int16_t', 'uint32_t' },
        { 14, 'GetTitle', 'uint16_t', '' },
        { 15, 'GetRank', 'uint16_t', '' },
        { 16, 'GetRankPoints', 'uint16_t', '' },
        { 17, 'GetHomepoint', 'uint16_t', '' },
        { 18, 'GetNation', 'uint8_t', '' },
        { 19, 'GetResidence', 'uint8_t', '' },
        { 20, 'GetSuLevel', 'uint16_t', '' },
        { 21, 'GetHighestItemLevel', 'uint8_t', '' },
        { 22, 'GetItemLevel', 'uint8_t', '' },
        { 23, 'GetMainHandItemLevel', 'uint8_t', '' },
        { 24, 'GetRangedItemLevel', 'uint8_t', '' },
        { 25, 'GetUnityFaction', 'uint32_t', '' },
        { 26, 'GetUnityPoints', 'uint32_t', '' },
        { 27, 'GetUnityPartialPersonalEvalutionPoints', 'uint16_t', '' },
        { 28, 'GetUnityPersonalEvaluationPoints', 'uint16_t', '' },
        { 29, 'GetUnityChatColorFlag', 'uint32_t', '' },
        { 30, 'GetMasteryJob', 'uint8_t', '' },
        { 31, 'GetMasteryJobLevel', 'uint8_t', '' },
        { 32, 'GetMasteryFlags', 'uint8_t', '' },
        { 33, 'GetMasteryUnknown0000', 'uint8_t', '' },
        { 34, 'GetMasteryExp', 'uint32_t', '' },
        { 35, 'GetMasteryExpNeeded', 'uint32_t', '' },
        { 36, 'GetCombatSkill', 'vana_skill*', 'vana_skill*, uint32_t', 'combatskill' },
        { 37, 'GetCraftSkill', 'vana_skill*', 'vana_skill*, uint32_t', 'craftskill' },
        { 38, 'GetAbilityRecast', 'uint16_t', 'uint32_t' },
        { 39, 'GetAbilityRecastCalc1', 'uint8_t', 'uint32_t' },
        { 40, 'GetAbilityRecastTimerId', 'uint16_t', 'uint32_t' },
        { 41, 'GetAbilityRecastCalc2', 'int16_t', 'uint32_t' },
        { 42, 'GetMountRecast', 'uint32_t', '' },
        { 43, 'GetMountRecastTimerId', 'uint32_t', '' },
        { 44, 'GetDataLoadedFlags', 'uint8_t', '' },
        { 45, 'GetLimitPoints', 'uint16_t', '' },
        { 46, 'GetMeritPoints', 'uint8_t', '' },
        { 47, 'GetAssimilationPoints', 'uint8_t', '' },
        { 48, 'GetIsLimitBreaker', 'bool', '' },
        { 49, 'GetIsExperiencePointsLocked', 'bool', '' },
        { 50, 'GetIsLimitModeEnabled', 'bool', '' },
        { 51, 'GetMeritPointsMax', 'uint8_t', '' },
        { 52, 'GetHomepointMasks', 'uint8_t*', '' },
        { 53, 'GetIsZoning', 'uint32_t', '' },
        { 54, 'GetStatusOffset1X', 'float', '' },
        { 55, 'GetStatusOffset1Z', 'float', '' },
        { 56, 'GetStatusOffset1Y', 'float', '' },
        { 57, 'GetStatusOffset1W', 'float', '' },
        { 58, 'GetStatusOffset2X', 'float', '' },
        { 59, 'GetStatusOffset2Z', 'float', '' },
        { 60, 'GetStatusOffset2Y', 'float', '' },
        { 61, 'GetStatusOffset2W', 'float', '' },
        { 62, 'GetStatusOffset3X', 'float', '' },
        { 63, 'GetStatusOffset3Z', 'float', '' },
        { 64, 'GetStatusOffset3Y', 'float', '' },
        { 65, 'GetStatusOffset3W', 'float', '' },
        { 66, 'GetStatusOffset4X', 'float', '' },
        { 67, 'GetStatusOffset4Z', 'float', '' },
        { 68, 'GetStatusOffset4Y', 'float', '' },
        { 69, 'GetStatusOffset4W', 'float', '' },
        { 70, 'GetCapacityPoints', 'uint16_t', 'uint32_t' },
        { 71, 'GetJobPoints', 'uint16_t', 'uint32_t' },
        { 72, 'GetJobPointsSpent', 'uint16_t', 'uint32_t' },
        { 73, 'GetStatusIcons', 'int16_t*', '' },
        { 74, 'GetStatusTimers', 'uint32_t*', '' },
        { 75, 'GetBuffs', 'int16_t*', '' },
        { 76, 'GetPetMPPercent', 'uint8_t', '' },
        { 77, 'GetPetTP', 'uint32_t', '' },
        { 78, 'GetJobLevel', 'uint8_t', 'uint32_t' },
        { 79, 'GetJobMasterFlags', 'uint32_t', '' },
        { 80, 'GetJobMasterLevel', 'uint8_t', 'uint32_t' },
        { 81, 'GetLoginStatus', 'uint8_t', '' },
        { 82, 'HasAbilityData', 'bool', '' },
        { 83, 'HasSpellData', 'bool', '' },
        { 84, 'HasAbility', 'bool', 'uint32_t' },
        { 85, 'HasPetCommand', 'bool', 'uint32_t' },
        { 86, 'HasSpell', 'bool', 'uint32_t' },
        { 87, 'HasTrait', 'bool', 'uint32_t' },
        { 88, 'HasWeaponSkill', 'bool', 'uint32_t' },
        { 89, 'HasKeyItem', 'bool', 'uint32_t' },
    },
    IPluginManager = {
        { 0, 'Load', 'bool', 'const char*, const char*' },
        { 1, 'Unload', 'bool', 'const char*' },
        { 2, 'UnloadAll', 'void', '' },
        { 3, 'IsLoaded', 'bool', 'const char*' },
        { 5, 'Get', 'void*', 'const char*' },
        { 4, 'Get', 'void*', 'uint32_t' },
        { 6, 'Count', 'uint32_t', '' },
        { 8, 'GetSilentPlugins', 'bool', '' },
        { 9, 'SetSilentPlugins', 'void', 'bool' },
    },
    IPointerManager = {
        { 1, 'Add', 'void', 'const char*, uintptr_t' },
        { 0, 'Add', 'uintptr_t', 'const char*, const char*, const char*, int32_t, uint32_t' },
        { 2, 'Get', 'uintptr_t', 'const char*' },
        { 3, 'Delete', 'void', 'const char*' },
    },
    IPolPluginManager = {
        { 0, 'Load', 'bool', 'const char*, const char*' },
        { 1, 'Unload', 'bool', 'const char*' },
        { 2, 'UnloadAll', 'void', '' },
        { 3, 'IsLoaded', 'bool', 'const char*' },
        { 5, 'Get', 'void*', 'const char*' },
        { 4, 'Get', 'void*', 'uint32_t' },
        { 6, 'Count', 'uint32_t', '' },
    },
    IPrimitiveManager = {
        { 0, 'Create', 'void*', 'const char*', 'primitive' },
        { 1, 'Get', 'void*', 'const char*', 'primitive' },
        { 2, 'Delete', 'void', 'const char*' },
        { 3, 'GetFocusedObject', 'void*', '', 'primitive' },
        { 4, 'SetFocusedObject', 'bool', 'const char*' },
        { 5, 'GetVisible', 'bool', '' },
        { 6, 'SetVisible', 'void', 'bool' },
    },
    IPrimitiveObject = {
        { 0, 'Render', 'void', '' },
        { 1, 'SetTextureFromFile', 'bool', 'const char*' },
        { 3, 'SetTextureFromResource', 'bool', 'const char*, const char*' },
        { 4, 'SetTextureFromResourceCache', 'bool', 'const char*' },
        { 5, 'SetTextureFromTexture', 'bool', 'void*, uint32_t, uint32_t' },
        { 6, 'HitTest', 'bool', 'int32_t, int32_t' },
        { 7, 'GetAlias', 'const char*', '', 'string' },
        { 8, 'GetTextureOffsetX', 'float', '' },
        { 9, 'GetTextureOffsetY', 'float', '' },
        { 10, 'GetBorderVisible', 'bool', '' },
        { 11, 'GetBorderColor', 'uint32_t', '' },
        { 12, 'GetBorderFlags', 'uint32_t', '' },
        { 13, 'GetBorderSizes', 'vana_RECT*', 'vana_RECT*', 'rect' },
        { 14, 'GetVisible', 'bool', '' },
        { 15, 'GetPositionX', 'float', '' },
        { 16, 'GetPositionY', 'float', '' },
        { 17, 'GetCanFocus', 'bool', '' },
        { 18, 'GetLocked', 'bool', '' },
        { 19, 'GetLockedZ', 'bool', '' },
        { 20, 'GetScaleX', 'float', '' },
        { 21, 'GetScaleY', 'float', '' },
        { 22, 'GetWidth', 'float', '' },
        { 23, 'GetHeight', 'float', '' },
        { 24, 'GetDrawFlags', 'uint32_t', '' },
        { 25, 'GetColor', 'uint32_t', '' },
        { 26, 'SetAlias', 'void', 'const char*' },
        { 27, 'SetTextureOffsetX', 'void', 'float' },
        { 28, 'SetTextureOffsetY', 'void', 'float' },
        { 29, 'SetBorderVisible', 'void', 'bool' },
        { 30, 'SetBorderColor', 'void', 'uint32_t' },
        { 31, 'SetBorderFlags', 'void', 'uint32_t' },
        { 32, 'SetBorderSizes', 'void', 'vana_RECT' },
        { 33, 'SetVisible', 'void', 'bool' },
        { 34, 'SetPositionX', 'void', 'float' },
        { 35, 'SetPositionY', 'void', 'float' },
        { 36, 'SetCanFocus', 'void', 'bool' },
        { 37, 'SetLocked', 'void', 'bool' },
        { 38, 'SetLockedZ', 'void', 'bool' },
        { 39, 'SetScaleX', 'void', 'float' },
        { 40, 'SetScaleY', 'void', 'float' },
        { 41, 'SetWidth', 'void', 'float' },
        { 42, 'SetHeight', 'void', 'float' },
        { 43, 'SetDrawFlags', 'void', 'uint32_t' },
        { 44, 'SetColor', 'void', 'uint32_t' },
    },
    IProperties = {
        { 0, 'GetPlayOnlineHwnd', 'void*', '' },
        { 1, 'GetPlayOnlineStyle', 'uint32_t', '' },
        { 2, 'GetPlayOnlineStyleEx', 'uint32_t', '' },
        { 3, 'GetPlayOnlineRect', 'vana_RECT*', 'vana_RECT*', 'rect' },
        { 4, 'SetPlayOnlineHwnd', 'void', 'void*' },
        { 5, 'SetPlayOnlineStyle', 'void', 'uint32_t' },
        { 6, 'SetPlayOnlineStyleEx', 'void', 'uint32_t' },
        { 7, 'SetPlayOnlineRect', 'void', 'vana_RECT' },
        { 8, 'GetPlayOnlineMaskHwnd', 'void*', '' },
        { 9, 'GetPlayOnlineMaskStyle', 'uint32_t', '' },
        { 10, 'GetPlayOnlineMaskStyleEx', 'uint32_t', '' },
        { 11, 'GetPlayOnlineMaskRect', 'vana_RECT*', 'vana_RECT*', 'rect' },
        { 12, 'SetPlayOnlineMaskHwnd', 'void', 'void*' },
        { 13, 'SetPlayOnlineMaskStyle', 'void', 'uint32_t' },
        { 14, 'SetPlayOnlineMaskStyleEx', 'void', 'uint32_t' },
        { 15, 'SetPlayOnlineMaskRect', 'void', 'vana_RECT' },
        { 16, 'GetFinalFantasyHwnd', 'void*', '' },
        { 17, 'GetFinalFantasyStyle', 'uint32_t', '' },
        { 18, 'GetFinalFantasyStyleEx', 'uint32_t', '' },
        { 19, 'GetFinalFantasyRect', 'vana_RECT*', 'vana_RECT*', 'rect' },
        { 20, 'SetFinalFantasyHwnd', 'void', 'void*' },
        { 21, 'SetFinalFantasyStyle', 'void', 'uint32_t' },
        { 22, 'SetFinalFantasyStyleEx', 'void', 'uint32_t' },
        { 23, 'SetFinalFantasyRect', 'void', 'vana_RECT' },
        { 24, 'GetD3DAmbientEnabled', 'bool', '' },
        { 25, 'GetD3DAmbientColor', 'uint32_t', '' },
        { 26, 'GetD3DFillMode', 'uint32_t', '' },
        { 27, 'SetD3DAmbientEnabled', 'void', 'bool' },
        { 28, 'SetD3DAmbientColor', 'void', 'uint32_t' },
        { 29, 'SetD3DFillMode', 'void', 'uint32_t' },
    },
    IRecast = {
        { 0, 'GetAbilityRecast', 'uint32_t', 'uint32_t' },
        { 1, 'GetAbilityCalc1', 'uint8_t', 'uint32_t' },
        { 2, 'GetAbilityTimerId', 'uint32_t', 'uint32_t' },
        { 3, 'GetAbilityCalc2', 'int16_t', 'uint32_t' },
        { 4, 'GetAbilityTimer', 'uint32_t', 'uint32_t' },
        { 5, 'GetSpellTimer', 'uint32_t', 'uint32_t' },
    },
    IResourceManager = {
        { 0, 'GetAbilityById', 'void*', 'uint32_t', 'ability' },
        { 1, 'GetAbilityByName', 'void*', 'const char*, uint32_t', 'ability' },
        { 2, 'GetAbilityByTimerId', 'void*', 'uint32_t', 'ability' },
        { 3, 'GetSpellById', 'void*', 'uint32_t', 'spell' },
        { 4, 'GetSpellByName', 'void*', 'const char*, uint32_t', 'spell' },
        { 5, 'GetItemById', 'void*', 'uint32_t', 'item' },
        { 6, 'GetItemByName', 'void*', 'const char*, uint32_t', 'item' },
        { 7, 'GetEntityCount', 'uint32_t', '' },
        { 8, 'GetEntityIndexById', 'uint32_t', 'uint32_t' },
        { 9, 'GetEntityIdByIndex', 'uint32_t', 'uint32_t' },
        { 10, 'GetEntityNameByIndex', 'const char*', 'uint32_t', 'string' },
        { 11, 'GetEntityNameById', 'const char*', 'uint32_t', 'string' },
        { 12, 'GetStatusIconByIndex', 'void*', 'uint16_t', 'statusicon' },
        { 13, 'GetStatusIconById', 'void*', 'uint16_t', 'statusicon' },
        { 17, 'GetString', 'const char*', 'const char*, uint32_t', 'string' },
        { 16, 'GetString', 'const char*', 'const char*, uint32_t, uint32_t', 'string' },
        { 15, 'GetString', 'int32_t', 'const char*, const char*' },
        { 14, 'GetString', 'int32_t', 'const char*, const char*, uint32_t' },
        { 19, 'GetStringLength', 'int32_t', 'const char*, uint32_t' },
        { 18, 'GetStringLength', 'int32_t', 'const char*, uint32_t, uint32_t' },
        { 20, 'GetTexture', 'void*', 'const char*' },
        { 23, 'GetAbilityRange', 'uint32_t', 'uint32_t, bool' },
        { 24, 'GetAbilityType', 'uint32_t', 'uint32_t' },
        { 25, 'GetSpellRange', 'uint32_t', 'uint32_t, bool' },
    },
    ITarget = {
        { 0, 'GetRawStructure', 'void*', '' },
        { 1, 'GetRawStructureWindow', 'void*', '' },
        { 2, 'GetTargetIndex', 'uint32_t', 'uint32_t' },
        { 3, 'GetServerId', 'uint32_t', 'uint32_t' },
        { 4, 'GetEntityPointer', 'uintptr_t', 'uint32_t' },
        { 5, 'GetActorPointer', 'uintptr_t', 'uint32_t' },
        { 6, 'GetArrowPositionX', 'float', 'uint32_t' },
        { 7, 'GetArrowPositionZ', 'float', 'uint32_t' },
        { 8, 'GetArrowPositionY', 'float', 'uint32_t' },
        { 9, 'GetArrowPositionW', 'float', 'uint32_t' },
        { 10, 'GetIsActive', 'uint8_t', 'uint32_t' },
        { 11, 'GetIsModelActor', 'uint8_t', 'uint32_t' },
        { 12, 'GetIsArrowActive', 'uint8_t', 'uint32_t' },
        { 13, 'GetChecksum', 'uint16_t', 'uint32_t' },
        { 14, 'GetIsWalk', 'uint8_t', '' },
        { 15, 'GetIsAutoNotice', 'uint8_t', '' },
        { 16, 'GetIsSubTargetActive', 'uint8_t', '' },
        { 17, 'GetDeactivateTarget', 'uint8_t', '' },
        { 18, 'GetModeChangeLock', 'uint8_t', '' },
        { 19, 'GetIsMouseRequestStack', 'uint8_t', '' },
        { 20, 'GetIsMouseRequestCancel', 'uint8_t', '' },
        { 21, 'GetIsPlayerMoving', 'uint8_t', '' },
        { 22, 'GetLockedOnFlags', 'uint32_t', '' },
        { 23, 'GetSubTargetFlags', 'uint32_t', '' },
        { 24, 'GetDefaultMode', 'uint16_t', '' },
        { 25, 'GetMenuTargetLock', 'uint16_t', '' },
        { 26, 'GetActionTargetActive', 'uint8_t', '' },
        { 27, 'GetActionTargetMaxYalms', 'uint8_t', '' },
        { 28, 'GetIsMenuOpen', 'uint8_t', '' },
        { 29, 'GetIsActionAoe', 'uint8_t', '' },
        { 30, 'GetActionType', 'uint8_t', '' },
        { 31, 'GetActionAoeRange', 'uint8_t', '' },
        { 32, 'GetActionId', 'uint32_t', '' },
        { 33, 'GetActionTargetServerId', 'uint32_t', '' },
        { 34, 'GetFocusTargetIndex', 'uint32_t', '' },
        { 35, 'GetFocusTargetServerId', 'uint32_t', '' },
        { 36, 'GetTargetPosF', 'float', 'uint32_t' },
        { 37, 'GetLastTargetName', 'const char*', '', 'string' },
        { 38, 'GetLastTargetIndex', 'uint32_t', '' },
        { 39, 'GetLastTargetServerId', 'uint32_t', '' },
        { 40, 'GetLastTargetChecksum', 'uint32_t', '' },
        { 41, 'GetActionCallback', 'uintptr_t', '' },
        { 42, 'GetCancelCallback', 'uintptr_t', '' },
        { 43, 'GetMyroomCallback', 'uintptr_t', '' },
        { 44, 'GetActionAoeCallback', 'uintptr_t', '' },
        { 45, 'GetWindowName', 'const char*', '', 'string' },
        { 46, 'GetWindowEntityPointer', 'uintptr_t', '' },
        { 47, 'GetWindowFrameChildrenOffsetX', 'uint16_t', '' },
        { 48, 'GetWindowFrameChildrenOffsetY', 'uint16_t', '' },
        { 49, 'GetWindowIconPositionX', 'uint16_t', '' },
        { 50, 'GetWindowIconPositionY', 'uint16_t', '' },
        { 51, 'GetWindowFrameOffsetX', 'uint16_t', '' },
        { 52, 'GetWindowFrameOffsetY', 'uint16_t', '' },
        { 53, 'GetWindowLockShape', 'uint32_t', '' },
        { 54, 'GetWindowServerId', 'uint32_t', '' },
        { 55, 'GetWindowHPPercent', 'uint8_t', '' },
        { 56, 'GetWindowDeathFlag', 'uint8_t', '' },
        { 57, 'GetWindowReraiseFlag', 'uint8_t', '' },
        { 58, 'GetWindowDeathNameColor', 'uint32_t', '' },
        { 59, 'GetWindowIsWindowLoaded', 'uint8_t', '' },
        { 60, 'GetWindowHelpString', 'uint32_t', '' },
        { 61, 'GetWindowHelpTitle', 'uint32_t', '' },
        { 62, 'GetWindowAnkShape', 'uint32_t', 'uint32_t' },
        { 63, 'GetWindowSub', 'uint8_t', '' },
        { 64, 'GetWindowAnkNum', 'uint8_t', '' },
        { 65, 'GetWindowAnkX', 'int16_t', '' },
        { 66, 'GetWindowAnkY', 'int16_t', '' },
        { 67, 'GetWindowSubAnkX', 'int16_t', '' },
        { 68, 'GetWindowSubAnkY', 'int16_t', '' },
        { 69, 'SetTarget', 'void', 'uint32_t, bool' },
    },
    IXInput = {
        { 1, 'RemoveCallback', 'void', 'const char*' },
        { 2, 'QueueButtonData', 'void', 'uint8_t, int16_t' },
        { 3, 'GetTrackDeadZone', 'bool', '' },
        { 4, 'SetTrackDeadZone', 'void', 'bool' },
    },
}
-- Version 4.30's methods that 4.16's interfaces don't have, by 4.30's vtable slots
local SDK430 = {
    IEntity = {
        { 112, 'GetMogMotionId', 'uint32_t', 'uint32_t' },
        { 246, 'SetMogMotionId', 'void', 'uint32_t, uint32_t' },
    },
    IParty = {
        { 12, 'GetAllianceInviteParty', 'int8_t', '' },
        { 36, 'GetMemberMasterLevel', 'uint8_t', 'uint32_t' },
        { 37, 'GetMemberMasterBreaker', 'uint8_t', 'uint32_t' },
    },
    IPlayer = {
        { 69, 'GetSubMapNum', 'uint32_t', '' },
    },
    ITarget = {
        { 24, 'GetOldNotice', 'uint32_t', '' },
    },
}
-- <<< SDK

-- A method of one of VanaCore's objects through its vtable: m = { slot, name, return type, parameter types, what the
-- result becomes ('string', 'rect': a RECT through a pointer given first, 'kind': the list's kind, another kind
-- wrap makes, 'iface:<interface>': an SDK object) }. The function, how many arguments it takes and of what sort.
local function vtable_method(m, kind)
    local slot, ret, params, returns = m[1], m[3], m[4], m[5]
    local fntype = ffi.typeof(('%s (__thiscall *)(void*%s)'):format(ret, params ~= '' and (', ' .. params) or ''))
    local bools, rects, objects, numbers, sorts = {}, {}, {}, {}, {}
    local i = 0
    -- (an empty list is none: counted as one, a method taking nothing was called with a nil)
    for param in (params ~= '' and params .. ',' or ''):gmatch('%s*([^,]-)%s*,') do
        i = i + 1
        if param == 'bool' then bools[i] = true end
        if param == 'vana_RECT' then rects[i] = true end
        if param == 'void*' then objects[i] = true end
        if param:match('int%d*_t$') or param == 'float' or param == 'double' or param:match('intptr_t$') then numbers[i] = true end
        sorts[i] = param == 'const char*' and 'string' or (numbers[i] or bools[i]) and 'number' or 'any'
    end
    local cached_vtable, cached_fn
    local function fn(self)
        local vtable = ffi.cast('void***', self.__ptr)[0]
        if vtable ~= cached_vtable then
            cached_vtable, cached_fn = vtable, ffi.cast(fntype, vtable[slot])
        end
        return cached_fn
    end
    -- arguments from `first` on, as the parameters from `first` on take them
    local function convert(first, n, ...)
        local args = { ... }
        for k = 1, n do
            local v, p = args[k], first + k - 1
            if bools[p] then
                args[k] = v and v ~= 0 and true or false
            elseif rects[p] and type(v) == 'table' then
                args[k] = ffi.new('vana_RECT', v.left or v[1] or 0, v.top or v[2] or 0, v.right or v[3] or 0, v.bottom or v[4] or 0)
            elseif objects[p] and type(v) == 'table' then
                args[k] = rawget(v, '__ptr') or (rawget(v, 'obj') and rawget(v.obj, '__ptr')) or nil
            elseif numbers[p] and v == nil then
                args[k] = 0
            end
        end
        return unpack(args, 1, n)
    end
    local count = i
    if returns == 'combatskill' or returns == 'craftskill' then
        table.remove(sorts, 1)
        local craft = returns == 'craftskill'
        return function (self, ...)
            local out = ffi.new('vana_skill')
            fn(self)(self.__ptr, out, convert(2, count - 1, ...))
            local raw = out.Raw
            return {
                Raw = raw,
                GetSkill = function () return craft and bit.rshift(bit.band(raw, 0x1FE0), 5) or bit.band(raw, 0x7FFF) end,
                GetRank = function () return bit.band(raw, 0x1F) end,
                IsCapped = function () return bit.band(raw, 0x8000) ~= 0 end,
            }
        end, count - 1, sorts
    end
    if returns == 'rect' then
        -- the hidden pointer is the first parameter; theirs follow it
        table.remove(sorts, 1)
        return function (self, ...)
            local r = ffi.new('vana_RECT')
            fn(self)(self.__ptr, r, convert(2, count - 1, ...))
            return r
        end, count - 1, sorts
    end
    local result_kind = returns == 'kind' and kind or returns
    return function (self, ...)
        local result = fn(self)(self.__ptr, convert(1, count, ...))
        if result_kind == 'string' then
            return result ~= nil and ffi.string(result) or nil
        elseif result_kind ~= nil then
            return wrap(result_kind, result)
        elseif type(result) == 'cdata' and ret:find('%*$') and result == nil then
            return nil
        end
        return result
    end, count, sorts
end

-- The methods of a list, by name. A name given more than once (the SDK's overloads, GetString(table, index) and
-- GetString(table, text)) picks among them by the arguments: their number, then strings against numbers.
local function method_list(list, kind)
    local methods, variants = {}, {}
    for _, m in ipairs(list) do
        local f, count, sorts = vtable_method(m, kind)
        variants[m[2]] = variants[m[2]] or {}
        table.insert(variants[m[2]], { fn = f, count = count, sorts = sorts })
    end
    for name, v in pairs(variants) do
        if #v == 1 then
            methods[name] = v[1].fn
        else
            methods[name] = function (self, ...)
                local n = select('#', ...)
                local fallback
                for _, c in ipairs(v) do
                    if c.count == n then
                        local fits = true
                        for k = 1, n do
                            local sort, value = c.sorts[k], (select(k, ...))
                            if (sort == 'string' and type(value) ~= 'string') or (sort == 'number' and type(value) == 'string') then
                                fits = false
                            end
                        end
                        if fits then
                            return c.fn(self, ...)
                        end
                        fallback = fallback or c
                    end
                end
                return (fallback or v[1]).fn(self, ...)
            end
        end
    end
    methods.__index = methods
    return methods
end

-- A hand-written list's methods with the SDK's interface `iface` behind them: what the list has, else the SDK's
local function with_sdk(methods, iface)
    if SDK[iface] then
        setmetatable(methods, { __index = method_list(SDK[iface]) })
    end
    return methods
end

local kinds = {
    font = with_sdk(method_list(FONT_OBJECT), 'IFontObject'),
    primitive = with_sdk(method_list(PRIMITIVE_OBJECT), 'IPrimitiveObject'),
}
local objects = setmetatable({}, { __mode = 'v' }) -- address -> the Lua object: the same one each time

wrap = function (kind, pointer)
    if pointer == nil then
        return nil
    end
    local at = tonumber(ffi.cast('uintptr_t', pointer))
    local key = kind .. at
    local object = objects[key]
    if object == nil then
        local struct = ({ item = 'vana_IItem*', statusicon = 'vana_IStatusIcon*', ability = 'vana_IAbility*', spell = 'vana_ISpell*' })[kind]
        if struct ~= nil then
            object = setmetatable({ __item = ffi.cast(struct, pointer), __fields = item_fields[kind], __kind = kind }, item_mt)
        elseif struct_kinds[kind] ~= nil then
            object = setmetatable({ __s = ffi.cast(struct_kinds[kind][1], pointer) }, struct_mts[kind])
        else
            if kinds[kind] == nil and kind:sub(1, 6) == 'iface:' then
                kinds[kind] = method_list(SDK[kind:sub(7)] or {})
            end
            object = setmetatable({ __ptr = ffi.cast('void*', pointer) }, kinds[kind])
        end
        objects[key] = object
    end
    return object
end

-- One of VanaCore's Ashita objects by name (vana.ashita.object), with these methods; nil without it.
local function native(name, list, kind, iface)
    local pointer = vana.ashita.object(name)
    if pointer == nil then
        return nil
    end
    return setmetatable({ __ptr = ffi.cast('void*', pointer) }, with_sdk(method_list(list, kind), iface))
end

local font_manager = native('fonts', OBJECT_MANAGER, 'font', 'IFontManager')
local primitive_manager = native('primitives', OBJECT_MANAGER, 'primitive', 'IPrimitiveManager')
local resource_manager = native('resources', RESOURCE_MANAGER, nil, 'IResourceManager')
if resource_manager ~= nil then
    -- GetString(table, index | text, [language]): the string (nil if none), or the index of the text (-1); and its
    -- length (-1 if none). Through vana.ashita.string: the SDK's GetString overloads aren't safe to reach by slot.
    resource_manager.GetString = function (_, table, at, language)
        return vana.ashita.string(tostring(table), at, tonumber(language) or 0)
    end
    resource_manager.GetStringLength = function (_, table, index, language)
        local text = vana.ashita.string(tostring(table), tonumber(index) or 0, tonumber(language) or 0)
        return text ~= nil and #text or -1
    end
    -- GetItemById(id), GetItemByName(name, [language]): items without their icons, read when Bitmap is (item_mt). Whole,
    -- an item is 2,776 bytes, and SimpleLog asks for every item there is (~57,000: 151 MB of a 32-bit game's memory).
    resource_manager.GetItemById = function (_, id)
        return wrap('item', vana.ashita.item(tonumber(id) or 0))
    end
    resource_manager.GetItemByName = function (_, name, language)
        if name == nil then
            return nil
        end
        return wrap('item', vana.ashita.item(tostring(name), tonumber(language) or 0))
    end
    -- GetFilePath(index): the file's path as a string ('' if none), as Ashita's Lua gives it (Boussole opens the map DATs
    -- with it); with a buffer and a size, (index, buffer, size), the SDK's own: the buffer filled, the length returned.
    local get_file_path = resource_manager.GetFilePath
    resource_manager.GetFilePath = function (self, index, buffer, size)
        if buffer ~= nil then
            return get_file_path(self, index, buffer, size)
        end
        local length = get_file_path(self, index, nil, 0)
        if length == nil or length <= 0 then
            return ''
        end
        local out = ffi.new('char[?]', length + 1)
        get_file_path(self, index, out, length + 1)
        return ffi.string(out, length)
    end
end
local inventory = native('inventory', INVENTORY, nil, 'IInventory')
local recast = native('recast', RECAST, nil, 'IRecast')
local pointer_manager = native('pointers', POINTER_MANAGER, nil, 'IPointerManager')
local properties = native('properties', PROPERTIES, nil, 'IProperties')

-------------------------------------------------------------------------------
-- struct: pack, unpack and size, as the struct library their addons have (Roberto Ierusalimschy's):
-- < > = byte order, !n alignment, x a padding byte, b B h H l L T i[n] I[n] j J integers, f d floats,
-- s a zero-terminated string, c[n] n characters (c0: the count from the value before), ' ' ignored.
-------------------------------------------------------------------------------

do
    local float_buf, double_buf = ffi.new('float[1]'), ffi.new('double[1]')

    -- the format's items, one at a time: option, size, what's left
    local function next_option(fmt, at)
        local c = fmt:sub(at, at)
        local digits = fmt:match('^%d+', at + 1)
        return c, digits and tonumber(digits) or nil, at + 1 + (digits and #digits or 0)
    end

    local INT_SIZES = { b = 1, B = 1, h = 2, H = 2, l = 4, L = 4, T = 4, j = 8, J = 8 }

    local function aligned(pos, size, align)
        local a = math.min(size, align)
        if a > 1 then
            local r = (pos - 1) % a
            if r ~= 0 then
                pos = pos + a - r
            end
        end
        return pos
    end

    local function read_int(data, pos, size, little, signed)
        local v = 0
        for k = 0, size - 1 do
            local b = data:byte(pos + (little and (size - 1 - k) or k)) or 0
            v = v * 256 + b
        end
        if signed and size < 8 then
            local limit = 2 ^ (size * 8 - 1)
            if v >= limit then
                v = v - 2 * limit
            end
        end
        return v
    end

    local function write_int(v, size, little)
        v = math.floor(tonumber(v) or 0)
        if v < 0 then
            v = v + 2 ^ (size * 8)
        end
        local bytes = {}
        for k = 1, size do
            bytes[k] = v % 256
            v = math.floor(v / 256)
        end
        local s = string.char(unpack(bytes))
        return little and s or s:reverse()
    end

    local function float_bytes(v, size, little)
        local buf = size == 4 and float_buf or double_buf
        buf[0] = tonumber(v) or 0
        local s = ffi.string(buf, size)
        return little and s or s:reverse()
    end

    local function bytes_float(data, pos, size, little)
        local s = data:sub(pos, pos + size - 1)
        if not little then
            s = s:reverse()
        end
        local buf = size == 4 and float_buf or double_buf
        ffi.copy(buf, s, size)
        return buf[0]
    end

    struct = {}

    function struct.pack(fmt, ...)
        local args, n = { ... }, 1
        local out, little, align = {}, true, 1
        local len, at = 0, 1
        while at <= #fmt do
            local c, count
            c, count, at = next_option(fmt, at)
            if c == '<' or c == '=' then little = true
            elseif c == '>' then little = false
            elseif c == '!' then align = count or 8
            elseif c == 'x' then out[#out + 1] = '\0'; len = len + 1
            elseif INT_SIZES[c] or c == 'i' or c == 'I' then
                local size = INT_SIZES[c] or count or 4
                local p = aligned(len + 1, size, align)
                if p > len + 1 then out[#out + 1] = ('\0'):rep(p - len - 1); len = p - 1 end
                out[#out + 1] = write_int(args[n], size, little); n = n + 1; len = len + size
            elseif c == 'f' or c == 'd' then
                local size = c == 'f' and 4 or 8
                local p = aligned(len + 1, size, align)
                if p > len + 1 then out[#out + 1] = ('\0'):rep(p - len - 1); len = p - 1 end
                out[#out + 1] = float_bytes(args[n], size, little); n = n + 1; len = len + size
            elseif c == 's' then
                local s = tostring(args[n] or ''); n = n + 1
                out[#out + 1] = s .. '\0'; len = len + #s + 1
            elseif c == 'c' then
                local s = tostring(args[n] or ''); n = n + 1
                local size = count == nil and 1 or count == 0 and #s or count
                s = s:sub(1, size) .. ('\0'):rep(size - math.min(#s, size))
                out[#out + 1] = s; len = len + size
            end
        end
        return table.concat(out)
    end

    function struct.unpack(fmt, data, init)
        local results, little, align = {}, true, 1
        local pos, at, last = tonumber(init) or 1, 1, nil
        while at <= #fmt do
            local c, count
            c, count, at = next_option(fmt, at)
            if c == '<' or c == '=' then little = true
            elseif c == '>' then little = false
            elseif c == '!' then align = count or 8
            elseif c == 'x' then pos = pos + 1
            elseif INT_SIZES[c] or c == 'i' or c == 'I' then
                local size = INT_SIZES[c] or count or 4
                pos = aligned(pos, size, align)
                last = read_int(data, pos, size, little, c == 'b' or c == 'h' or c == 'l' or c == 'j' or c == 'i')
                results[#results + 1] = last; pos = pos + size
            elseif c == 'f' or c == 'd' then
                local size = c == 'f' and 4 or 8
                pos = aligned(pos, size, align)
                last = bytes_float(data, pos, size, little)
                results[#results + 1] = last; pos = pos + size
            elseif c == 's' then
                local stop = data:find('\0', pos, true) or (#data + 1)
                results[#results + 1] = data:sub(pos, stop - 1); pos = stop + 1
            elseif c == 'c' then
                local size = count == nil and 1 or count == 0 and (tonumber(last) or 0) or count
                if count == 0 then
                    results[#results] = nil
                end
                results[#results + 1] = data:sub(pos, pos + size - 1); pos = pos + size
            end
        end
        results[#results + 1] = pos
        return unpack(results)
    end

    function struct.size(fmt)
        local len, align, at = 0, 1, 1
        while at <= #fmt do
            local c, count
            c, count, at = next_option(fmt, at)
            if c == '!' then align = count or 8
            elseif c == 'x' then len = len + 1
            elseif INT_SIZES[c] or c == 'i' or c == 'I' or c == 'f' or c == 'd' then
                local size = INT_SIZES[c] or (c == 'f' and 4) or (c == 'd' and 8) or count or 4
                len = aligned(len + 1, size, align) - 1 + size
            elseif c == 'c' then len = len + (count or 1)
            elseif c == 's' then error('struct.size: a variable-length string (s) has no size', 2)
            end
        end
        return len
    end

    package.loaded.struct = struct -- so require('struct') finds it, as with their struct library
end

-------------------------------------------------------------------------------
-- string.pack, string.unpack and string.packsize: Lua 5.3's (lstrlib's), which Ashita's Lua has in its string library
-- (Addons.dll's string table lists pack, packsize and unpack, with 5.3's messages) and LuaJIT 2.1 as built here doesn't.
-- Addons read packets with them: string.unpack('<HH', e.data, 1) gave "attempt to call field 'unpack' (a nil value)".
-- Sizes as on 32-bit Windows: h 2, i and l 4, j 8, T 4, f 4, d and n 8; '!' alone aligns to 8. Integers come back as
-- Lua numbers (doubles): unsigned ones as unsigned (I4 0xFFFFFFFF is 4294967295), and 8-byte ones past 2^53 lose
-- their lowest bits. The Windower layer's string.unpack is Windower's own (other arguments), in its addons' states.
-------------------------------------------------------------------------------

do
    local bit = require('bit')
    local byte, sub, find, rep, char = string.byte, string.sub, string.find, string.rep, string.char
    local floor, huge = math.floor, math.huge
    local float_buf, double_buf = ffi.new('float[1]'), ffi.new('double[1]')
    local MAXINTSIZE, MAXALIGN, SZINT = 16, 8, 8
    local SIZES = { b = 1, B = 1, h = 2, H = 2, l = 4, L = 4, j = 8, J = 8, T = 4, f = 4, d = 8, n = 8 }
    local KINDS = { b = 'int', h = 'int', l = 'int', j = 'int', B = 'uint', H = 'uint', L = 'uint', J = 'uint',
        T = 'uint', f = 'float', d = 'float', n = 'float' }
    local entries = {}

    -- raises the error at the addon's call, as the C functions' errors are
    local function fail(fname, arg, msg)
        if arg then
            msg = ("bad argument #%d to '%s' (%s)"):format(arg, fname, msg)
        end
        local level = 2
        while true do
            local info = getinfo(level, 'f')
            if info == nil then
                error(msg, 2)
            end
            if entries[info.func] then
                error(msg, level + 1)
            end
            level = level + 1
        end
    end

    local function typename(v)
        return v == nil and 'no value' or type(v)
    end

    local function checkstr(fname, arg, v)
        if type(v) == 'string' then return v end
        if type(v) == 'number' then return tostring(v) end
        fail(fname, arg, 'string expected, got ' .. typename(v))
    end

    local function checknum(fname, arg, v)
        local n = tonumber(v)
        if n == nil then
            fail(fname, arg, 'number expected, got ' .. typename(v))
        end
        return n
    end

    local function checkint(fname, arg, v)
        local n = checknum(fname, arg, v)
        if n ~= floor(n) or n == huge or n == -huge then
            fail(fname, arg, 'number has no integer representation')
        end
        return n
    end

    local function digits(fmt, at)
        local d = fmt:match('^%d+', at)
        if d then
            return tonumber(d), at + #d
        end
        return nil, at
    end

    local function intsize(fname, fmt, at, default)
        local n
        n, at = digits(fmt, at)
        n = n or default
        if n > MAXINTSIZE or n <= 0 then
            fail(fname, nil, ('integral size (%d) out of limits [1,%d]'):format(n, MAXINTSIZE))
        end
        return n, at
    end

    -- one option of the format: its kind, its size, and where the next one starts. h holds the byte order and the
    -- maximum alignment, which the options < > = ! change.
    local function option(fname, h, fmt, at)
        local c = sub(fmt, at, at)
        at = at + 1
        local kind = KINDS[c]
        if kind then
            return kind, SIZES[c], at
        elseif c == 'i' or c == 'I' then
            local n
            n, at = intsize(fname, fmt, at, 4)
            return c == 'i' and 'int' or 'uint', n, at
        elseif c == 's' then
            local n
            n, at = intsize(fname, fmt, at, SIZES.T)
            return 'string', n, at
        elseif c == 'c' then
            local n
            n, at = digits(fmt, at)
            if n == nil then
                fail(fname, nil, "missing size for format option 'c'")
            end
            return 'char', n, at
        elseif c == 'z' then return 'zstr', 0, at
        elseif c == 'x' then return 'padding', 1, at
        elseif c == 'X' then return 'paddalign', 0, at
        elseif c == ' ' then return 'nop', 0, at
        elseif c == '<' or c == '=' then h.little = true; return 'nop', 0, at
        elseif c == '>' then h.little = false; return 'nop', 0, at
        elseif c == '!' then
            h.maxalign, at = intsize(fname, fmt, at, MAXALIGN)
            return 'nop', 0, at
        end
        fail(fname, nil, ("invalid format option '%s'"):format(c))
    end

    -- an option with the padding that aligns it at total (the bytes before it)
    local function details(fname, h, fmt, at, total)
        local kind, size
        kind, size, at = option(fname, h, fmt, at)
        local align = size
        if kind == 'paddalign' then
            local next_kind
            if at > #fmt then
                fail(fname, 1, "invalid next option for option 'X'")
            end
            next_kind, align, at = option(fname, h, fmt, at)
            if next_kind == 'char' or align == 0 then
                fail(fname, 1, "invalid next option for option 'X'")
            end
        end
        local ntoalign = 0
        if align > 1 and kind ~= 'char' then
            if align > h.maxalign then
                align = h.maxalign
            end
            if bit.band(align, align - 1) ~= 0 then
                fail(fname, 1, 'format asks for alignment not power of 2')
            end
            ntoalign = bit.band(align - bit.band(total, align - 1), align - 1)
        end
        return kind, size, ntoalign, at
    end

    -- an integer's size bytes, two's complement (bytes past 8 extend the sign)
    local function int_bytes(v, size, little)
        local bytes, neg = {}, v < 0
        if neg then
            v = -(v + 1)
        end
        for k = 1, size do
            local b = v % 256
            v = (v - b) / 256
            bytes[k] = neg and 255 - b or b
        end
        local s = char(unpack(bytes))
        return little and s or s:reverse()
    end

    -- the integer at at (1-based)
    local function read_int(data, at, size, little, signed)
        local function b(k) -- the k-th least significant byte, from 0
            return byte(data, little and at + k or at + size - 1 - k)
        end
        local limit = size > SZINT and SZINT or size
        local neg = signed and b(limit - 1) >= 0x80
        local v = 0
        for k = limit - 1, 0, -1 do
            local x = b(k)
            v = v * 256 + (neg and 255 - x or x)
        end
        if neg then
            v = -v - 1
        end
        for k = limit, size - 1 do
            if b(k) ~= (neg and 0xFF or 0) then
                fail('unpack', nil, ('%d-byte integer does not fit into Lua Integer'):format(size))
            end
        end
        return v
    end

    local function float_bytes(v, size, little)
        local buf = size == 4 and float_buf or double_buf
        buf[0] = v
        local s = ffi.string(buf, size)
        return little and s or s:reverse()
    end

    local function read_float(data, at, size, little)
        local s = sub(data, at, at + size - 1)
        if not little then
            s = s:reverse()
        end
        local buf = size == 4 and float_buf or double_buf
        ffi.copy(buf, s, size)
        return tonumber(buf[0])
    end

    local function pack(fmt, ...)
        fmt = checkstr('pack', 1, fmt)
        local args = { ... }
        local h = { little = true, maxalign = 1 }
        local out, total, arg, at = {}, 0, 1, 1
        while at <= #fmt do
            local kind, size, ntoalign
            kind, size, ntoalign, at = details('pack', h, fmt, at, total)
            total = total + ntoalign + size
            if ntoalign > 0 then
                out[#out + 1] = rep('\0', ntoalign)
            end
            if kind == 'int' or kind == 'uint' then
                arg = arg + 1
                local v = checkint('pack', arg, args[arg - 1])
                if size < SZINT then
                    if kind == 'int' then
                        local lim = 2 ^ (size * 8 - 1)
                        if v < -lim or v >= lim then
                            fail('pack', arg, 'integer overflow')
                        end
                    elseif v < 0 or v >= 2 ^ (size * 8) then
                        fail('pack', arg, 'unsigned overflow')
                    end
                end
                out[#out + 1] = int_bytes(v, size, h.little)
            elseif kind == 'float' then
                arg = arg + 1
                out[#out + 1] = float_bytes(checknum('pack', arg, args[arg - 1]), size, h.little)
            elseif kind == 'char' then
                arg = arg + 1
                local s = checkstr('pack', arg, args[arg - 1])
                if #s > size then
                    fail('pack', arg, 'string longer than given size')
                end
                out[#out + 1] = s .. rep('\0', size - #s)
            elseif kind == 'string' then
                arg = arg + 1
                local s = checkstr('pack', arg, args[arg - 1])
                if size < SZINT and #s >= 2 ^ (size * 8) then
                    fail('pack', arg, 'string length does not fit in given size')
                end
                out[#out + 1] = int_bytes(#s, size, h.little) .. s
                total = total + #s
            elseif kind == 'zstr' then
                arg = arg + 1
                local s = checkstr('pack', arg, args[arg - 1])
                if find(s, '\0', 1, true) then
                    fail('pack', arg, 'string contains zeros')
                end
                out[#out + 1] = s .. '\0'
                total = total + #s + 1
            elseif kind == 'padding' then
                out[#out + 1] = '\0'
            end
        end
        return table.concat(out)
    end

    local function unpack_(fmt, data, init)
        fmt = checkstr('unpack', 1, fmt)
        data = checkstr('unpack', 2, data)
        local ld = #data
        local pos = init == nil and 1 or checkint('unpack', 3, init)
        if pos < 0 then
            pos = -pos > ld and 0 or ld + pos + 1
        end
        pos = pos - 1 -- from here, the bytes before the next value
        if pos < 0 or pos > ld then
            fail('unpack', 3, 'initial position out of string')
        end
        local h = { little = true, maxalign = 1 }
        local out, n, at = {}, 0, 1
        while at <= #fmt do
            local kind, size, ntoalign
            kind, size, ntoalign, at = details('unpack', h, fmt, at, pos)
            if ntoalign + size > ld - pos then
                fail('unpack', 2, 'data string too short')
            end
            pos = pos + ntoalign
            if kind == 'int' or kind == 'uint' then
                n = n + 1
                out[n] = read_int(data, pos + 1, size, h.little, kind == 'int')
            elseif kind == 'float' then
                n = n + 1
                out[n] = read_float(data, pos + 1, size, h.little)
            elseif kind == 'char' then
                n = n + 1
                out[n] = sub(data, pos + 1, pos + size)
            elseif kind == 'string' then
                local len = read_int(data, pos + 1, size, h.little, false)
                if len > ld - pos - size then
                    fail('unpack', 2, 'data string too short')
                end
                n = n + 1
                out[n] = sub(data, pos + size + 1, pos + size + len)
                pos = pos + len
            elseif kind == 'zstr' then
                local zero = find(data, '\0', pos + 1, true)
                if zero == nil then
                    fail('unpack', 2, "unfinished string for format 'z'")
                end
                n = n + 1
                out[n] = sub(data, pos + 1, zero - 1)
                pos = zero
            end
            pos = pos + size
        end
        n = n + 1
        out[n] = pos + 1
        return unpack(out, 1, n)
    end

    local function packsize(fmt)
        fmt = checkstr('packsize', 1, fmt)
        local h = { little = true, maxalign = 1 }
        local total, at = 0, 1
        while at <= #fmt do
            local kind, size, ntoalign
            kind, size, ntoalign, at = details('packsize', h, fmt, at, total)
            if kind == 'string' or kind == 'zstr' then
                fail('packsize', 1, 'variable-length format')
            end
            total = total + ntoalign + size
        end
        return total
    end

    entries[pack], entries[unpack_], entries[packsize] = true, true, true
    if string.pack == nil then string.pack = pack end
    if string.unpack == nil then string.unpack = unpack_ end
    if string.packsize == nil then string.packsize = packsize end
end

-------------------------------------------------------------------------------
-- AshitaCore: the managers addons reach through it.
-------------------------------------------------------------------------------

-- Their configuration aliases and sections for vanacore.ini's: 'boot' is the settings the game
-- starts with, which for VanaCore is vanacore.ini. Their ffxi.registry values are the game's settings
-- as VanaCore hands them to it ([ffxi] by name, or [ffxi.registry]).
local function config_value(alias, section, key)
    if tostring(alias):lower() ~= 'boot' then
        return nil
    end
    section = tostring(section):lower()
    if section == 'ffxi.registry' then
        return vana.config.game(tostring(key))
    end
    if section == 'ashita.language' then
        section = 'language'
    end
    return vana.config.get(section, tostring(key))
end

local configuration = partial('ConfigurationManager', {
    GetString = function (_, alias, section, key, default)
        return config_value(alias, section, key) or default
    end,
    GetBool = function (_, alias, section, key, default)
        local value = config_value(alias, section, key)
        if value == nil then
            return default
        end
        value = value:lower()
        return value == '1' or value == 'true' or value == 'yes' or value == 'on'
    end,
})
local function config_number(_, alias, section, key, default)
    return tonumber(config_value(alias, section, key)) or default
end
for _, name in ipairs({ 'GetInt8', 'GetUInt8', 'GetInt16', 'GetUInt16', 'GetInt32', 'GetUInt32', 'GetInt64',
                        'GetUInt64', 'GetFloat', 'GetDouble' }) do
    rawset(configuration, name, config_number)
end

local chat_manager = partial('ChatManager', {
    -- The text with its auto-translate phrases spelled out (the game's own parser); brackets: inside the game's green
    -- ones, as the chat log shows them
    ParseAutoTranslate = function (_, text, brackets)
        return vana.ashita.parse_auto_translate(tostring(text or ''), brackets and true or false)
    end,
    -- Ashita's own modes (-1 parse, -2 script, -3 force-handle) run as typed, as the C++ QueueCommand runs them
    QueueCommand = function (_, mode, command)
        mode = tonumber(mode) or 1
        vana.chat.run(tostring(command), mode < 0 and 1 or mode)
    end,
    AddChatMessage = function (_, mode, indent, message)
        vana.chat.write(tostring(message), tonumber(mode) or 1)
    end,
    Write = function (_, mode, indent, message)
        vana.chat.write(tostring(message), tonumber(mode) or 1)
    end,
})

-------------------------------------------------------------------------------
-- Memory manager: the game's data (vana.game) through the getters their addons call. Reads are
-- cached for the rest of the frame, as addons call a getter per field per member.
-------------------------------------------------------------------------------

local game = vana.game
local cache = {}
vana.events.on('present', function () cache = {} end, '__ashita_cache')

local function cached_party()
    if cache.party == nil then
        cache.party = game.party() or false
    end
    return cache.party or nil
end

local function cached_entity(index)
    index = tonumber(index)
    if index == nil then
        return nil
    end
    cache.entities = cache.entities or {}
    local entity = cache.entities[index]
    if entity == nil then
        entity = game.entity(index) or false
        cache.entities[index] = entity
    end
    return entity or nil
end

local function cached_target()
    if cache.target == nil then
        cache.target = game.target() or false
    end
    return cache.target or nil
end

-- The local player's entity index, or nil when not logged in.
local function player_index()
    local party = cached_party()
    local me = party and party.members[1]
    if me == nil or not me.active or me.id == 0 or me.target_index == 0 or cached_entity(me.target_index) == nil then
        return nil
    end
    return me.target_index
end

-- Getters over one record kind: method name -> field (or function of the record). A missing
-- record reads as 0 (nil for names).
local function getters(name, read, fields)
    local t = {}
    for method, field in pairs(fields) do
        t[method] = function (_, index)
            local record = read(index)
            if record == nil then
                if method:find('Name$') then
                    return nil
                end
                return 0
            end
            if type(field) == 'function' then
                return field(record)
            end
            local value = record[field]
            if type(value) == 'boolean' then
                return value and 1 or 0
            end
            return value
        end
    end
    return t
end

local party = getters('Party', function (slot)
    local p = cached_party()
    return p and p.members[(tonumber(slot) or 0) + 1]
end, {
    GetMemberName = 'name', GetMemberServerId = 'id', GetMemberTargetIndex = 'target_index', GetMemberHP = 'hp',
    GetMemberMP = 'mp', GetMemberTP = 'tp', GetMemberHPPercent = 'hpp', GetMemberMPPercent = 'mpp',
    -- the player's (slot 0) is the zone the server's zone-in (0x00A) named until the game's list has it, once the zone has
    -- loaded: zonename reads it a second after 0x00A, when Ashita's read still has the last zone
    GetMemberZone = function (m) return m.slot == 0 and game.player_zone(m.zone) or m.zone end,
    GetMemberMainJob = 'main_job', GetMemberMainJobLevel = 'main_job_level',
    GetMemberSubJob = 'sub_job', GetMemberSubJobLevel = 'sub_job_level', GetMemberIsActive = 'active',
    -- the member's index in the party, 0 for an empty slot (Ashita's own dump, 2026-10-08: GetMemberIndex(1) is 0 alone)
    GetMemberFlagMask = 'flags', GetMemberIndex = function (m) return m.active and m.slot or 0 end,
})
local function alliance(field, i)
    return function ()
        local p = cached_party()
        if p == nil then
            return 0
        end
        return i and p[field][i] or p[field]
    end
end
party.GetAllianceLeaderServerId = alliance('leader')
for i = 1, 3 do
    party['GetAlliancePartyLeaderServerId' .. i] = alliance('party_leaders', i)
    party['GetAlliancePartyMemberCount' .. i] = alliance('party_counts', i)
end
party = partial('Party', party)

local entity = partial('Entity', getters('Entity', cached_entity, {
    GetName = 'name', GetServerId = 'id', GetTargetIndex = 'index', GetDistance = 'distance', GetHPPercent = 'hpp',
    GetStatus = 'status', GetType = 'type', GetRace = 'race', GetLocalPositionX = 'x', GetLocalPositionY = 'y',
    GetLocalPositionZ = 'z', GetHeading = 'heading', GetMovementSpeed = 'speed', GetRenderFlags0 = 'render_flags',
    GetRenderFlags1 = 'name_flags', GetSpawnFlags = 'spawn_flags', GetModelSize = 'model_size',
    GetModelHitboxSize = 'hitbox_size', GetClaimStatus = 'claim', GetPetTargetIndex = 'pet_index',
    GetTargetedIndex = 'target_index',
}))

-- Their target slots are the game's two entries as they are.
local target = getters('Target', function (slot)
    local t = cached_target()
    return t and t.raw[(tonumber(slot) or 0) + 1]
end, { GetTargetIndex = 'index', GetServerId = 'id', GetIsActive = 'active' })
target.GetIsSubTargetActive = function ()
    local t = cached_target()
    return t and t.is_sub_target and 1 or 0
end
target.GetIsMenuOpen = function ()
    local t = cached_target()
    return t and t.menu_open and 1 or 0
end
target.GetLockedOnFlags = function ()
    local t = cached_target()
    return t and t.locked and 1 or 0
end
-- the game's own SetTarget, with the entity's actor (an entity out of render range has none: nothing happens). Ashita's
-- force flag has no counterpart in the game's call.
target.SetTarget = function (_, index, force)
    game.set_target(tonumber(index) or 0)
end
target = partial('Target', target)

local player = {}
local function stats()
    if cache.player_stats == nil then
        cache.player_stats = game.player() or false
    end
    return cache.player_stats or nil
end
local function stat(field)
    return function ()
        local s = stats()
        return s and s[field] or 0
    end
end
-- Ashita's values: 2 once logged in (the player's entity exists), 1 between zones (from just after the server's zone-out
-- until the player is back: FancyChat and others read 0 as a logout and reload), 0 at the title screen.
player.GetLoginStatus = function ()
    local zoning = vana.game.zoning() -- also what clears it once the player is back
    if player_index() ~= nil then
        return 2
    end
    return zoning and 1 or 0
end
player.GetMainJob = stat('main_job')
player.GetMainJobLevel = stat('main_job_level')
player.GetSubJob = stat('sub_job')
player.GetSubJobLevel = stat('sub_job_level')
player.GetHPMax = stat('hp_max')
player.GetMPMax = stat('mp_max')
player.GetTitle = stat('title')
player.GetNation = stat('nation')
-- What the player has, from the game's own bits (vana.game): spells by id, abilities by Ashita's ids (weapon skills from
-- 0, job abilities from 0x200, pet commands from 0x400, traits from 0x600), key items by id
local function has_bit(bits, n)
    if bits == nil or n == nil or n < 0 then
        return false
    end
    local b = bits:byte(math.floor(n / 8) + 1)
    return b ~= nil and bit.band(b, bit.lshift(1, n % 8)) ~= 0
end
player.HasSpell = function (_, id) return has_bit(vana.game.spells(), tonumber(id)) end
player.HasAbility = function (_, id) return has_bit(vana.game.abilities(), tonumber(id)) end
player.HasKeyItem = function (_, id) return has_bit(vana.game.key_items(), tonumber(id)) end
player.HasWeaponSkill = function (_, id)
    id = tonumber(id)
    return id ~= nil and id < 0x200 and has_bit(vana.game.abilities(), id)
end
player.HasPetCommand = function (_, id)
    id = tonumber(id)
    return id ~= nil and id >= 0x400 and id < 0x600 and has_bit(vana.game.abilities(), id)
end
player.HasSpellData = function () return vana.game.spells() ~= nil end
player.HasAbilityData = function () return vana.game.abilities() ~= nil end
player.GetBuffs = function ()
    local s = stats()
    return s and s.buffs or {}
end
player = partial('Player', player)

local memory_manager = partial('MemoryManager', {
    GetParty = function () return party end,
    GetEntity = function () return entity end,
    GetTarget = function () return target end,
    GetPlayer = function () return player end,
    GetInventory = function () return inventory end,
    GetRecast = function () return recast end,
})

-- The packet manager: a packet as a table of bytes (or a string), the whole packet, header first (VanaCore sets the
-- header's id, size and sequence)
local function packet_bytes(data)
    if type(data) == 'table' then
        local bytes = {}
        for i = 1, #data do
            bytes[i] = string.char(bit.band(tonumber(data[i]) or 0, 0xFF))
        end
        return table.concat(bytes)
    end
    return tostring(data)
end
local function queue_packet(_, id, length, align, p1, p2, fill)
    return vana.packets.queue(id, length, align or 0, p1 or 0, p2 or 0, fill)
end
local packet_manager = partial('PacketManager', {
    AddIncomingPacket = function (_, id, data) vana.packets.inject(true, id, packet_bytes(data)) end,
    AddOutgoingPacket = function (_, id, data) vana.packets.inject(false, id, packet_bytes(data)) end,
    -- In the game's own outgoing queue: fill(pointer) fills it.
    QueuePacket = queue_packet,
    QueueOutgoingPacket = queue_packet,
})

-- The GUI manager: ImGui (lua_imgui.cpp), which their imgui library puts its names over. A plain
-- table: their addons test for a function by its name being nil.
local gui_manager = vana.imgui
gui_manager.GetVisible = function () return vana.ashita.gui_visible() end
gui_manager.SetVisible = function (_, visible) vana.ashita.gui_visible(visible and true or false) end
-- ImGui's structures an addon makes, as Ashita 4.3's globals: ImGuiListClipper.new(), ImGuiTextFilter.new('x'), ...
for name, type_table in pairs(gui_manager.types or {}) do
    if rawget(_G, name) == nil then
        _G[name] = type_table
    end
end

-- The plugin manager, as far as their addons ask it: the Addons plugin's interface version, which
-- decides between ImGui 1.81's calls (4.16) and 1.92's (4.3); VanaCore has both.
local addons_plugin = partial('IPlugin', {
    GetName = function () return 'Addons' end,
    GetInterfaceVersion = function () return INTERFACE end,
})
-- Sequencer only patches Ashita's own packet code, which VanaCore doesn't have, so the DLL is refused; but its one job
-- for addons is to be present so LuAshitacast re-applies gear on the game's action/item resends (which VanaCore does
-- pass to outgoing-packet handlers). Reported present, like Addons. No addon calls its methods, only checks it exists.
local sequencer_plugin = partial('IPlugin', {
    GetName = function () return 'Sequencer' end,
    GetInterfaceVersion = function () return INTERFACE end,
})
local plugin_manager = partial('PluginManager', {
    Get = function (_, name)
        local lower = tostring(name):lower()
        if lower == 'addons' then
            return addons_plugin
        end
        if lower == 'sequencer' then
            return sequencer_plugin
        end
        return nil
    end,
    IsLoaded = function (_, name)
        local lower = tostring(name):lower()
        return lower == 'addons' or lower == 'sequencer'
    end,
    -- A plugin event: plugins' HandleEvent, and every addon's 'plugin_event' at the next frame. The data a string, or a
    -- table of bytes.
    RaiseEvent = function (_, name, data)
        if type(data) == 'table' then
            local bytes = {}
            for i = 1, #data do
                bytes[i] = string.char(bit.band(tonumber(data[i]) or 0, 0xFF))
            end
            data = table.concat(bytes)
        end
        vana.plugins.raise(tostring(name), data ~= nil and tostring(data) or '')
    end,
})

AshitaCore = partial('AshitaCore', {
    -- With the trailing backslash Ashita's has: LuAshitacast and others write '%sconfig\\addons\\...'.
    GetInstallPath = function ()
        return vana.paths.install .. '\\'
    end,
    GetConfigurationManager = function ()
        return configuration
    end,
    GetChatManager = function ()
        return chat_manager
    end,
    GetMemoryManager = function ()
        return memory_manager
    end,
    GetGuiManager = function ()
        return gui_manager
    end,
    GetPluginManager = function ()
        return plugin_manager
    end,
    GetFontManager = function ()
        return font_manager
    end,
    GetPrimitiveManager = function ()
        return primitive_manager
    end,
    GetResourceManager = function ()
        return resource_manager
    end,
    GetPacketManager = function ()
        return packet_manager
    end,
    GetPointerManager = function ()
        return pointer_manager
    end,
    GetProperties = function ()
        return properties
    end,
    -- The Win32 calls IAshitaCore passes on (move's /bringtofront): the window a handle or a number. Through
    -- vana.ashita, not ffi.cdef: an addon's own declaration of them would clash with ours in its state.
    SetFocus = function (_, window)
        return vana.ashita.win32('SetFocus', tonumber(ffi.cast('uintptr_t', window)) or 0)
    end,
    SetForegroundWindow = function (_, window)
        return vana.ashita.win32('SetForegroundWindow', tonumber(ffi.cast('uintptr_t', window)) or 0) ~= 0
    end,
    GetSystemMetrics = function (_, index)
        return vana.ashita.win32('GetSystemMetrics', tonumber(index) or 0)
    end,
    -- the game's Direct3D 8 device (their d3d8 library casts it to IDirect3DDevice8*)
    GetDirect3DDevice = function ()
        return vana.ashita.object('device')
    end,
})

-- An entity with their field names (a table here; a structure in their runtime).
local function entity_table(e)
    if e == nil then
        return nil
    end
    return {
        Name = e.name, ServerId = e.id, TargetIndex = e.index, HPPercent = e.hpp, Distance = e.distance,
        Status = e.status, Race = e.race, Type = e.type, SpawnFlags = e.spawn_flags, ModelSize = e.model_size,
        ModelHitboxSize = e.hitbox_size, ClaimStatus = e.claim, PetTargetIndex = e.pet_index,
        TargetedIndex = e.target_index, Heading = e.heading, MovementSpeed = e.speed,
        Render = { Flags0 = e.render_flags, Flags1 = e.name_flags },
        Movement = { LocalPosition = { X = e.x, Y = e.y, Z = e.z, Yaw = e.heading } },
        Look = { Hair = e.models[1], Head = e.models[2], Body = e.models[3], Hands = e.models[4], Legs = e.models[5],
                 Feet = e.models[6], Main = e.models[7], Sub = e.models[8], Ranged = e.models[9] },
    }
end

-------------------------------------------------------------------------------
-- Entities as their runtime gives them (GetEntity, GetPlayerEntity): a view over the game's own entity structure
-- (Ashita's entity_t, the one plugins get from IEntity::GetRawEntity, 684 bytes), so every field reads live and a write
-- reaches the game (ceximodels and sexchange set ModelUpdateFlags and Look.Hair to change a model; noname and pcfilter
-- read Render.Flags2 and ActorPointer; Sidekick and fancychat FellowTargetIndex). Name and MonstrosityName are Lua
-- strings; Movement, Look and Render are the structures themselves (e.Movement.LocalPosition.X, e.Look.Hair = 5); a
-- field entity_t doesn't have reads nil (sparks' WarpPointer, Ashita v3's). Without the game's objects (a test process),
-- the copy entity_table makes.
-------------------------------------------------------------------------------
ffi.cdef[[
    typedef struct { float X, Z, Y, W, Roll, Yaw, Pitch, unknown001C; } vana_position_t;
    typedef struct { float X, Z, Y, W, DeltaX, DeltaZ, DeltaY, DeltaW, DeltaRoll, DeltaYaw, DeltaPitch, unknown002C; } vana_move_t;
    typedef struct { vana_position_t LocalPosition; vana_position_t LastPosition; vana_move_t Move; } vana_movement_t;
    typedef struct { uint16_t Hair, Head, Body, Hands, Legs, Feet, Main, Sub, Ranged; uint8_t unknown0012[14]; } vana_look_t;
    typedef struct { uint32_t Flags0, Flags1, Flags2, Flags3, Flags4, Flags5, Flags6, Flags7, Flags8; } vana_render_t;
    typedef struct {
        uintptr_t VTablePointer; vana_movement_t Movement; uint32_t TargetIndex; uint32_t ServerId; int8_t Name[28];
        float MovementSpeed; float AnimationSpeed; uintptr_t ActorPointer; uintptr_t Attachments[12]; uintptr_t EventPointer;
        float Distance; uint32_t TurnSpeed; uint32_t TurnSpeedHead; float Heading; uintptr_t Next; uint8_t HPPercent;
        uint8_t unknown00ED; uint8_t Type; uint8_t Race; uint16_t LocalMoveCount; uint16_t ActorLockFlag; uint16_t ModelUpdateFlags;
        uint8_t unknown00F6; uint8_t padding00F7; uint32_t DoorId; vana_look_t Look; uint16_t ActionTimer1; uint16_t ActionTimer2;
        vana_render_t Render; uint8_t PopEffect; uint8_t UpdateMask; uint16_t InteractionTargetIndex; uint16_t NpcSpeechFrame;
        uint16_t LookAxisX; uint16_t LookAxisY; uint16_t MouthCounter; uint16_t MouthWaitCounter; uint16_t CraftTimerUnknown;
        uint32_t CraftServerId; uint16_t CraftAnimationEffect; uint8_t CraftAnimationStep; uint8_t CraftParam; float MovementSpeed2;
        uint16_t NpcWalkPosition1; uint16_t NpcWalkPosition2; uint16_t NpcWalkMode; uint16_t CostumeId; uint32_t Mou4;
        uint32_t StatusServer; uint32_t Status; uint32_t StatusEvent; uint32_t unknown0178; uint32_t unknown017C; uint32_t ModelTime;
        uint32_t ModelStartTime; uint32_t ClaimStatus; uint32_t ZoneId; uint32_t Animations[10]; uint16_t AnimationTime;
        uint16_t AnimationStep; uint8_t AnimationPlay; uint8_t unknown01BD; uint16_t EmoteTargetIndex; uint16_t EmoteId;
        uint16_t padding01C2; uint32_t EmoteIdString; uintptr_t EmoteTargetActorPointer; uint32_t unknown01CC; uint32_t SpawnFlags;
        uint32_t LinkshellColor; uint16_t NameColor; uint8_t CampaignNameFlag; uint8_t MountId; int32_t FishingUnknown0000;
        int32_t FishingUnknown0001; int32_t FishingActionCountdown; int16_t FishingRodCastTime; int16_t FishingUnknown0002;
        uint32_t LastActionId; uint32_t MogMotionId; uintptr_t LastActionActorPointer; uint16_t TargetedIndex; uint16_t PetTargetIndex;
        uint16_t UpdateRequestDelay; uint8_t IsDirty; uint8_t BallistaFlags; uint8_t PankrationEnabled; uint8_t PankrationFlagFlip;
        uint16_t unknown0202; float ModelSize; float ModelHitboxSize; uint32_t EnvironmentAreaId; uint16_t MonstrosityFlag;
        uint16_t MonstrosityNameId; int8_t MonstrosityName[32]; uint8_t MonstrosityNameEnd; uint8_t MonstrosityNameAbbr[24];
        uint8_t MonstrosityNameAbbrEnd; uint16_t padding024E; uint8_t CustomProperties[44]; uint8_t BallistaInfo[36];
        uint16_t FellowTargetIndex; uint16_t WarpTargetIndex; uint16_t TrustOwnerTargetIndex; uint16_t AreaDisplayTargetIndex;
        uint32_t unknown02A8;
    } vana_entity_t;
]]

local entity_fields = {}
for name in ([[VTablePointer Movement TargetIndex ServerId Name MovementSpeed AnimationSpeed ActorPointer Attachments
    EventPointer Distance TurnSpeed TurnSpeedHead Heading Next HPPercent Type Race LocalMoveCount ActorLockFlag
    ModelUpdateFlags DoorId Look ActionTimer1 ActionTimer2 Render PopEffect UpdateMask InteractionTargetIndex NpcSpeechFrame
    LookAxisX LookAxisY MouthCounter MouthWaitCounter CraftTimerUnknown CraftServerId CraftAnimationEffect CraftAnimationStep
    CraftParam MovementSpeed2 NpcWalkPosition1 NpcWalkPosition2 NpcWalkMode CostumeId Mou4 StatusServer Status StatusEvent
    ModelTime ModelStartTime ClaimStatus ZoneId Animations AnimationTime AnimationStep AnimationPlay EmoteTargetIndex EmoteId
    EmoteIdString EmoteTargetActorPointer SpawnFlags LinkshellColor NameColor CampaignNameFlag MountId FishingUnknown0000
    FishingUnknown0001 FishingActionCountdown FishingRodCastTime FishingUnknown0002 LastActionId MogMotionId
    LastActionActorPointer TargetedIndex PetTargetIndex UpdateRequestDelay IsDirty BallistaFlags PankrationEnabled
    PankrationFlagFlip ModelSize ModelHitboxSize EnvironmentAreaId MonstrosityFlag MonstrosityNameId MonstrosityName
    MonstrosityNameEnd MonstrosityNameAbbr MonstrosityNameAbbrEnd CustomProperties BallistaInfo FellowTargetIndex
    WarpTargetIndex TrustOwnerTargetIndex AreaDisplayTargetIndex]]):gmatch('%S+') do
    entity_fields[name] = true
end
local entity_strings = { Name = 28, MonstrosityName = 32 }

local entity_view_mt = {
    __index = function (self, key)
        if not entity_fields[key] then
            return nil
        end
        local e = rawget(self, '__entity')
        local length = entity_strings[key]
        if length then
            local bytes = e[key]
            local n = 0
            while n < length and bytes[n] ~= 0 do
                n = n + 1
            end
            return ffi.string(bytes, n)
        end
        return e[key]
    end,
    __newindex = function (self, key, value)
        if not entity_fields[key] then
            return
        end
        local e = rawget(self, '__entity')
        local length = entity_strings[key]
        if length then
            local text = tostring(value):sub(1, length - 1)
            ffi.fill(e[key], length)
            ffi.copy(e[key], text)
            return
        end
        e[key] = value
    end,
    __tostring = function (self)
        return ('entity_t: %s'):format(tostring(rawget(self, '__entity')))
    end,
}
local entity_views = setmetatable({}, { __mode = 'v' })

local function entity_view(raw)
    if raw == nil then
        return nil
    end
    local at = tonumber(ffi.cast('uintptr_t', raw))
    local view = entity_views[at]
    if view == nil then
        view = setmetatable({ __entity = ffi.cast('vana_entity_t*', raw) }, entity_view_mt)
        entity_views[at] = view
    end
    return view
end

-- Globals their addons expect from the runtime.
function GetEntity(index)
    index = tonumber(index)
    if index == nil then
        return nil
    end
    if rawget(entity, '__ptr') ~= nil then
        local raw = entity:GetRawEntity(index)
        return type(raw) == 'table' and raw or entity_view(raw)
    end
    return entity_table(cached_entity(index))
end

function GetPlayerEntity()
    local index = player_index()
    return index and GetEntity(index) or nil
end

-------------------------------------------------------------------------------
-- What the Lua above doesn't have of an object, VanaCore's own C++ one has (the object Ashita plugins get, with the
-- SDK's vtable): an Ashita addon calling anything of the SDK gets what a plugin would.
-------------------------------------------------------------------------------

local function sdk_fallback(t, object, iface)
    local pointer = vana.ashita.object(object)
    if t == nil or pointer == nil or SDK[iface] == nil then
        return
    end
    rawset(t, '__ptr', ffi.cast('void*', pointer))
    sdk_of[t] = method_list(SDK[iface])
    -- interface 4.30: its methods 4.16 doesn't have, from the object 4.30's plugins get
    local pointer430 = INTERFACE == 4.30 and SDK430[iface] ~= nil and vana.ashita.object(object, 4.30) or nil
    if pointer430 ~= nil then
        local object430 = { __ptr = ffi.cast('void*', pointer430) }
        for name, fn in pairs(method_list(SDK430[iface])) do
            if name ~= '__index' then
                rawset(sdk_of[t], name, function (_, ...) return fn(object430, ...) end)
            end
        end
    end
end

sdk_fallback(AshitaCore, 'core', 'IAshitaCore')
sdk_fallback(chat_manager, 'chat', 'IChatManager')
sdk_fallback(configuration, 'config', 'IConfigurationManager')
sdk_fallback(memory_manager, 'memory', 'IMemoryManager')
sdk_fallback(party, 'party', 'IParty')
sdk_fallback(entity, 'entity', 'IEntity')
sdk_fallback(target, 'target', 'ITarget')
sdk_fallback(player, 'player', 'IPlayer')
sdk_fallback(packet_manager, 'packets', 'IPacketManager')
sdk_fallback(plugin_manager, 'plugins', 'IPluginManager')

-- LogManager: Ashita's global for the log, the object plugins get as ILogManager (gdifonts' Error path, in every addon
-- that bundles it, logs a missing font with LogManager:Log(1, 'GdiFonts', text) and read nil here: PetsReborn didn't
-- load without its fonts). Log(level, source, text) is a line in VanaCore's log, "[source] level: text" (1 critical to
-- 5 debug; one above the log's level is dropped, false); Logf formats first, as the SDK's does (its C variadic can't be
-- reached by slot); GetLogLevel and SetLogLevel are the object's own.
LogManager = partial('LogManager', {
    Log = function (self, level, source, text)
        local sdk = sdk_of[self]
        local log = sdk ~= nil and rawget(sdk, 'Log') or nil
        if log ~= nil then
            return log(self, tonumber(level) or 0, tostring(source or '?'), tostring(text or ''))
        end
        vana.log(('[%s] %s'):format(tostring(source or '?'), tostring(text or '')))
        return true
    end,
    Logf = function (self, level, source, text, ...)
        local ok, formatted = pcall(string.format, tostring(text or ''), ...)
        return self:Log(level, source, ok and formatted or tostring(text or ''))
    end,
})
sdk_fallback(LogManager, 'log', 'ILogManager')

-- IPlayer:GetStatusIcons() and :GetStatusTimers(): in Ashita a table of the 32 slots from 1 (statustimers reads
-- icons[j + 1] for j = 0..31, partybuffs icons[1..32]); the SDK's bindings are bare pointers from 0, which put every
-- slot one off and read the last one past the end (statustimers: status effect "27272", every frame).
-- The timers are signed 32-bit values in Ashita (statustimers/timers subtract a wrapped clock from them and expect the
-- result to wrap through negative); unsigned, every duration came out 2^32 frames too long (19884:35:27).
do
    local sdk = sdk_of[player]
    for _, name in ipairs({ 'GetStatusIcons', 'GetStatusTimers' }) do
        local raw = sdk ~= nil and rawget(sdk, name) or nil
        local signed = name == 'GetStatusTimers'
        if raw ~= nil then
            rawset(player, name, function (self)
                local slots = raw(self)
                if slots == nil then
                    return nil
                end
                local list = {}
                for i = 0, 31 do
                    local value = tonumber(slots[i])
                    if signed and value >= 2147483648 then
                        value = value - 4294967296
                    end
                    list[i + 1] = value
                end
                return list
            end)
        end
    end
end

-- IEntity:GetRawEntity(index): in Ashita the entity structure itself, with its fields (AMANTracker's
-- entity.PetTargetIndex, mobdb's and ceximodels' reads); the SDK binding alone gives a bare pointer. The same view as
-- GetEntity's, nil for an empty slot.
do
    local sdk = sdk_of[entity]
    local raw_entity = sdk ~= nil and rawget(sdk, 'GetRawEntity') or nil
    if raw_entity ~= nil then
        rawset(entity, 'GetRawEntity', function (self, index)
            return entity_view(raw_entity(self, index))
        end)
    end
end

-- >>> https (the tests load this part alone)
-- https for LuaSocket. socket.http asks for 'socket.ssl.https' (LuaSec) when a URL is https, and LuaSec's ssl.dll has
-- its own copy of Lua built in, which a LuaJIT state can't run (ventures failed to load on 'ssl.core' not found). So the
-- connection object socket.http drives is made here, and the request goes through Windows' WinHTTP, which does the TLS
-- and checks the certificates. socket.http writes its request to the connection and reads the answer back, so the
-- connection keeps what's sent until the first read, makes the request then, and reads the answer from what WinHTTP
-- gave back (as an HTTP/1.1 response with a Content-Length). Redirects are left to socket.http. A request blocks the
-- game's thread, as LuaSocket's own do.
do
    local ffi = require('ffi')
    local defined = pcall(ffi.cdef, [[
        typedef void* vc_hinternet;
        vc_hinternet WinHttpOpen(const wchar_t* agent, uint32_t access, const wchar_t* proxy, const wchar_t* bypass, uint32_t flags);
        vc_hinternet WinHttpConnect(vc_hinternet session, const wchar_t* server, uint16_t port, uint32_t reserved);
        vc_hinternet WinHttpOpenRequest(vc_hinternet connection, const wchar_t* verb, const wchar_t* object, const wchar_t* version,
            const wchar_t* referrer, const wchar_t** accept, uint32_t flags);
        int WinHttpSetTimeouts(vc_hinternet handle, int resolve, int connect, int send, int receive);
        int WinHttpSetOption(vc_hinternet handle, uint32_t option, void* buffer, uint32_t length);
        int WinHttpSendRequest(vc_hinternet request, const wchar_t* headers, uint32_t headers_length, const void* optional,
            uint32_t optional_length, uint32_t total_length, uintptr_t context);
        int WinHttpReceiveResponse(vc_hinternet request, void* reserved);
        int WinHttpQueryHeaders(vc_hinternet request, uint32_t info, const wchar_t* name, void* buffer, uint32_t* length, uint32_t* index);
        int WinHttpReadData(vc_hinternet request, void* buffer, uint32_t to_read, uint32_t* read);
        int WinHttpCloseHandle(vc_hinternet handle);
    ]])
    local loaded, winhttp = pcall(ffi.load, 'winhttp')
    if loaded and (defined or pcall(function () return winhttp.WinHttpOpen end)) then
        -- The text as UTF-16 (header and URL text is ASCII; any other byte goes through as the character of that number)
        local function wide(text)
            local out = ffi.new('wchar_t[?]', #text + 1)
            for i = 1, #text do
                out[i - 1] = text:byte(i)
            end
            return out
        end

        -- A response's header lines (the status line first, no blank line after), as text
        local function raw_headers(request)
            local size = ffi.new('uint32_t[1]', 0)
            winhttp.WinHttpQueryHeaders(request, 22, nil, nil, size, nil) -- WINHTTP_QUERY_RAW_HEADERS_CRLF: asks how long
            if size[0] == 0 then
                return nil
            end
            local buffer = ffi.new('wchar_t[?]', size[0] / 2 + 1)
            if winhttp.WinHttpQueryHeaders(request, 22, nil, buffer, size, nil) == 0 then
                return nil
            end
            local chars = {}
            for i = 0, size[0] / 2 - 1 do
                chars[#chars + 1] = string.char(buffer[i] % 256)
            end
            return (table.concat(chars):gsub('[\r\n]+$', ''))
        end

        -- The raw headers and the body of the answer to a request, or nil and why
        local function perform(method, host, port, uri, headers, body, seconds)
            local ms = math.floor(math.min(seconds or 15, 20) * 1000)
            local session = winhttp.WinHttpOpen(wide('VanaCore'), 0, nil, nil, 0)
            if session == nil then
                return nil, 'WinHTTP failed to open'
            end
            local connection, request
            local function finish(...)
                if request ~= nil then winhttp.WinHttpCloseHandle(request) end
                if connection ~= nil then winhttp.WinHttpCloseHandle(connection) end
                winhttp.WinHttpCloseHandle(session)
                return ...
            end
            connection = winhttp.WinHttpConnect(session, wide(host), port, 0)
            if connection == nil then
                return finish(nil, 'connection failed')
            end
            request = winhttp.WinHttpOpenRequest(connection, wide(method), wide(uri), nil, nil, nil, 0x00800000) -- _FLAG_SECURE
            if request == nil then
                return finish(nil, 'request failed')
            end
            winhttp.WinHttpSetTimeouts(request, ms, ms, ms, ms)
            winhttp.WinHttpSetOption(request, 88, ffi.new('uint32_t[1]', 0), 4) -- REDIRECT_POLICY: NEVER (socket.http follows them)
            local sent = winhttp.WinHttpSendRequest(request, #headers > 0 and wide(headers) or nil, #headers > 0 and 0xFFFFFFFF or 0,
                #body > 0 and body or nil, #body, #body, 0)
            if sent == 0 then
                return finish(nil, ('send failed (%d)'):format(ffi.errno()))
            end
            if winhttp.WinHttpReceiveResponse(request, nil) == 0 then
                return finish(nil, ('no response (%d)'):format(ffi.errno()))
            end
            local raw = raw_headers(request)
            if raw == nil then
                return finish(nil, 'no response headers')
            end
            local chunks, buffer, read = {}, ffi.new('uint8_t[?]', 16384), ffi.new('uint32_t[1]')
            while true do
                if winhttp.WinHttpReadData(request, buffer, 16384, read) == 0 then
                    return finish(nil, ('read failed (%d)'):format(ffi.errno()))
                end
                if read[0] == 0 then
                    break
                end
                chunks[#chunks + 1] = ffi.string(buffer, read[0])
            end
            return finish(raw, table.concat(chunks))
        end

        -- A chunked body (what socket.http sends when it has no length for the body) as it was before the chunk sizes
        local function unchunk(data)
            local out, at = {}, 1
            while true do
                local line_end = data:find('\r\n', at, true)
                local size = line_end and tonumber(data:sub(at, line_end - 1):match('^%x+') or '', 16)
                if not size or size == 0 then
                    break
                end
                out[#out + 1] = data:sub(line_end + 2, line_end + 1 + size)
                at = line_end + 2 + size + 2
            end
            return table.concat(out)
        end

        -- The request socket.http wrote: its method, uri, the headers WinHTTP doesn't make itself, and the body
        local function parse_request(text)
            local head, body = text:match('^(.-)\r\n\r\n(.*)$')
            local method, uri = (head or ''):match('^(%S+) (%S+) HTTP/')
            if not method then
                return nil
            end
            local kept, chunked = {}, false
            for line in head:gmatch('[^\r\n]+') do
                local name, value = line:match('^([^:]+):%s*(.*)$')
                if name then
                    local lower = name:lower()
                    if lower == 'transfer-encoding' and value:lower():find('chunked', 1, true) then
                        chunked = true
                    end
                    if lower ~= 'host' and lower ~= 'content-length' and lower ~= 'transfer-encoding' and lower ~= 'connection' then
                        kept[#kept + 1] = name .. ': ' .. value
                    end
                end
            end
            return method, uri, #kept > 0 and table.concat(kept, '\r\n') .. '\r\n' or '', chunked and unchunk(body) or body
        end

        -- What socket.http reads: WinHTTP's answer as an HTTP/1.1 response that ends with its body
        local function response(raw, body)
            local status, rest = raw:match('^([^\r\n]*)\r?\n?(.*)$')
            local kept = {}
            for line in rest:gmatch('[^\r\n]+') do
                local name = line:match('^([^:]+):')
                if name then
                    local lower = name:lower()
                    if lower ~= 'transfer-encoding' and lower ~= 'content-length' and lower ~= 'connection' then
                        kept[#kept + 1] = line
                    end
                end
            end
            kept[#kept + 1] = 'Content-Length: ' .. #body
            kept[#kept + 1] = 'Connection: close'
            return status .. '\r\n' .. table.concat(kept, '\r\n') .. '\r\n\r\n' .. body
        end

        local https = { _VERSION = '1.0.2', _COPYRIGHT = 'LuaSec 1.0.2 - Copyright (C) 2009-2021 PUC-Rio (WinHTTP in VanaCore)', PORT = 443, TIMEOUT = 60 }

        -- The connection object socket.http uses (LuaSocket's tcp object's methods that it calls)
        local Connection = {}
        Connection.__index = Connection

        function Connection:settimeout(seconds)
            self.seconds = tonumber(seconds) or self.seconds
            return 1
        end

        function Connection:connect(host, port)
            self.host, self.port = host, tonumber(port) or https.PORT
            return 1
        end

        function Connection:send(data, first, last)
            self.sent[#self.sent + 1] = data:sub(first or 1, last or -1)
            return last or #data
        end

        function Connection:getfd() return -1 end
        function Connection:dirty() return false end
        function Connection:setoption() return 1 end
        function Connection:shutdown() return 1 end
        function Connection:close() return 1 end

        -- Makes the request, once; nil, or why it failed
        function Connection:ask()
            local method, uri, headers, body = parse_request(table.concat(self.sent))
            if not method then
                return 'malformed request'
            end
            local raw, answer = perform(method, self.host, self.port, uri, headers, body, self.seconds)
            if raw == nil then
                return answer
            end
            self.answer, self.at = response(raw, answer), 1
        end

        -- LuaSocket's receive: a line ('*l'), the rest ('*a') or a number of bytes, with `prefix` put in front of what's returned
        function Connection:receive(pattern, prefix)
            if self.answer == nil then
                local failed = self:ask()
                if failed then
                    return nil, failed
                end
            end
            local answer, at = self.answer, self.at
            prefix = prefix or ''
            pattern = pattern or '*l'
            if type(pattern) == 'number' then
                local data = answer:sub(at, at + pattern - 1)
                self.at = at + #data
                if #data < pattern then
                    return nil, 'closed', prefix .. data
                end
                return prefix .. data
            end
            pattern = pattern:gsub('^%*', '')
            if pattern == 'a' then
                local data = answer:sub(at)
                self.at = #answer + 1
                if #data == 0 then
                    return nil, 'closed', prefix
                end
                return prefix .. data
            end
            local newline = answer:find('\n', at, true)
            if newline then
                local line = answer:sub(at, newline - 1):gsub('\r$', '')
                self.at = newline + 1
                return prefix .. line
            end
            local data = answer:sub(at)
            self.at = #answer + 1
            return nil, 'closed', prefix .. data
        end

        -- LuaSec's: the 'create' function socket.http calls to make its connection
        function https.tcp(params)
            return function ()
                return setmetatable({ sent = {}, seconds = https.TIMEOUT }, Connection)
            end
        end

        -- LuaSec's request(url [, body]): socket.http's, over https
        function https.request(url, body)
            local http, parser = require('socket.http'), require('socket.url')
            local result, text = {}, type(url) == 'string'
            if text then
                local ltn12 = require('socket.ltn12')
                local target = {
                    url = parser.build(parser.parse(url, { port = https.PORT })),
                    method = body and 'POST' or 'GET',
                    sink = ltn12.sink.table(result),
                }
                if body then
                    target.source = ltn12.source.string(body)
                    target.headers = { ['content-length'] = #body, ['content-type'] = 'application/x-www-form-urlencoded' }
                end
                url = target
            else
                url.url = parser.build(parser.parse(url.url, { port = https.PORT }))
            end
            if http.PROXY or url.proxy then
                return nil, 'proxy not supported'
            elseif url.redirect then
                return nil, 'redirect not supported'
            elseif url.create then
                return nil, 'create function not permitted'
            end
            url.create = https.tcp(url)
            local ok, code, headers, status = http.request(url)
            if ok and text then
                return table.concat(result), code, headers, status
            end
            return ok, code, headers, status
        end

        package.preload['socket.ssl.https'] = function () return https end
        package.preload['ssl.https'] = package.preload['socket.ssl.https']
    end
end

-- LuaSec itself ('socket.ssl', 'ssl', and the C modules ssl.lua asks for): LuaSec's ssl.dll has its own Lua built in,
-- and loading it into a LuaJIT state crashes the game (trustme, which requires 'socket.ssl' as it loads). VanaCore has
-- no TLS over an addon's own socket, so these are a LuaSec whose calls say so: the addon loads, an https request through
-- socket.http works as above, and a socket it wraps itself gets an error it can handle.
do
    local reason = 'TLS over an addon\'s own socket isn\'t available in VanaCore (https through socket.http is)'
    local function unavailable() return nil, reason end
    local ssl = { _VERSION = '1.0.2', wrap = unavailable, newcontext = unavailable, loadcertificate = unavailable }
    package.preload['socket.ssl'] = function () return ssl end
    package.preload['ssl'] = package.preload['socket.ssl']
    for _, name in ipairs({ 'ssl.core', 'ssl.context', 'ssl.x509', 'ssl.config' }) do
        package.preload[name] = function () error(reason, 2) end
    end
end
-- <<< https

-- >>> font awesome
-- Ashita's ImGui carries Font Awesome 6, and its addons name the icons as IconsFontAwesome6.h does, as globals
-- (boussole's window title is ICON_FA_COMPASS .. ' boussole'). VanaCore loads the same fonts (gui.cpp) with the
-- same glyph ranges, so the globals are those icons' UTF-8: name=codepoint, from the fonts' glyph names
-- (fonts/imgui/fa-*.ttf; solid, then brands, then regular for the names only it has).
do
    local icons = [[
0=30;1=31;2=32;3=33;4=34;5=35;6=36;7=37;8=38;9=39;a=41;b=42;c=43;d=44;e=45;f=46;g=47;h=48;i=49;j=4A;k=4B;l=4C;
m=4D;n=4E;o=4F;p=50;q=51;r=52;s=53;t=54;u=55;v=56;w=57;x=58;y=59;z=5A;faucet=E005;faucet-drip=E006;
house-chimney-window=E00D;house-signal=E012;temperature-arrow-down=E03F;temperature-arrow-up=E040;
trailer=E041;bacteria=E059;bacterium=E05A;box-tissue=E05B;hand-holding-medical=E05C;hand-sparkles=E05D;
hands-bubbles=E05E;handshake-simple-slash=E05F;handshake-slash=E060;head-side-cough=E061;
head-side-cough-slash=E062;head-side-mask=E063;head-side-virus=E064;house-chimney-user=E065;house-laptop=E066;
lungs-virus=E067;people-arrows=E068;plane-slash=E069;pump-medical=E06A;pump-soap=E06B;shield-virus=E06C;
sink=E06D;soap=E06E;stopwatch-20=E06F;shop-slash=E070;store-slash=E071;toilet-paper-slash=E072;
users-slash=E073;virus=E074;virus-slash=E075;viruses=E076;vest=E085;vest-patches=E086;arrow-trend-down=E097;
arrow-trend-up=E098;arrow-up-from-bracket=E09A;austral-sign=E0A9;baht-sign=E0AC;bitcoin-sign=E0B4;
bolt-lightning=E0B7;book-bookmark=E0BB;cable-car=E0CF;camera-rotate=E0D8;cedi-sign=E0DF;chart-column=E0E3;
chart-gantt=E0E4;clapperboard=E131;clover=E139;code-compare=E13A;code-fork=E13B;code-pull-request=E13C;
colon-sign=E140;cruzeiro-sign=E152;display=E163;dong-sign=E169;elevator=E16D;filter-circle-xmark=E17B;
florin-sign=E184;folder-closed=E185;franc-sign=E18F;guarani-sign=E19A;gun=E19B;hands-clapping=E1A8;
house-user=E1B0;indian-rupee-sign=E1BC;kip-sign=E1C4;lari-sign=E1C8;litecoin-sign=E1D3;manat-sign=E1D5;
mask-face=E1D7;mill-sign=E1ED;money-bills=E1F3;naira-sign=E1F6;notdef=E1FE;panorama=E209;peseta-sign=E221;
peso-sign=E222;plane-up=E22D;rupiah-sign=E23D;stairs=E289;timeline=E29C;truck-front=E2B7;
turkish-lira-sign=E2BB;vault=E2C5;wand-magic-sparkles=E2CA;wheat-awn=E2CD;wheelchair-move=E2CE;
bangladeshi-taka-sign=E2E6;bowl-rice=E2EB;person-pregnant=E31E;house-chimney=E3AF;house-crack=E3B1;
house-medical=E3B2;cent-sign=E3F5;plus-minus=E43C;sailboat=E445;section=E447;shrimp=E448;
brazilian-real-sign=E46C;chart-simple=E473;diagram-next=E476;diagram-predecessor=E477;diagram-successor=E47A;
earth-oceania=E47B;bug-slash=E490;file-circle-plus=E494;shop-lock=E4A5;virus-covid=E4A8;
virus-covid-slash=E4A9;anchor-circle-check=E4AA;anchor-circle-exclamation=E4AB;anchor-circle-xmark=E4AC;
anchor-lock=E4AD;arrow-down-up-across-line=E4AF;arrow-down-up-lock=E4B0;arrow-right-to-city=E4B3;
arrow-up-from-ground-water=E4B5;arrow-up-from-water-pump=E4B6;arrow-up-right-dots=E4B7;
arrows-down-to-line=E4B8;arrows-down-to-people=E4B9;arrows-left-right-to-line=E4BA;arrows-spin=E4BB;
arrows-split-up-and-left=E4BC;arrows-to-circle=E4BD;arrows-to-dot=E4BE;arrows-to-eye=E4BF;
arrows-turn-right=E4C0;arrows-turn-to-dots=E4C1;arrows-up-to-line=E4C2;bore-hole=E4C3;bottle-droplet=E4C4;
bottle-water=E4C5;bowl-food=E4C6;boxes-packing=E4C7;bridge=E4C8;bridge-circle-check=E4C9;
bridge-circle-exclamation=E4CA;bridge-circle-xmark=E4CB;bridge-lock=E4CC;bridge-water=E4CE;bucket=E4CF;
bugs=E4D0;building-circle-arrow-right=E4D1;building-circle-check=E4D2;building-circle-exclamation=E4D3;
building-circle-xmark=E4D4;building-flag=E4D5;building-lock=E4D6;building-ngo=E4D7;building-shield=E4D8;
building-un=E4D9;building-user=E4DA;building-wheat=E4DB;burst=E4DC;car-on=E4DD;car-tunnel=E4DE;
child-combatant=E4E0;children=E4E1;circle-nodes=E4E2;clipboard-question=E4E3;cloud-showers-water=E4E4;
computer=E4E5;cubes-stacked=E4E6;envelope-circle-check=E4E8;explosion=E4E9;ferry=E4EA;
file-circle-exclamation=E4EB;file-circle-minus=E4ED;file-circle-question=E4EF;file-shield=E4F0;
fire-burner=E4F1;fish-fins=E4F2;flask-vial=E4F3;glass-water=E4F4;glass-water-droplet=E4F5;
group-arrows-rotate=E4F6;hand-holding-hand=E4F7;handcuffs=E4F8;hands-bound=E4F9;hands-holding-child=E4FA;
hands-holding-circle=E4FB;heart-circle-bolt=E4FC;heart-circle-check=E4FD;heart-circle-exclamation=E4FE;
heart-circle-minus=E4FF;heart-circle-plus=E500;heart-circle-xmark=E501;helicopter-symbol=E502;helmet-un=E503;
hill-avalanche=E507;hill-rockslide=E508;house-circle-check=E509;house-circle-exclamation=E50A;
house-circle-xmark=E50B;house-fire=E50C;house-flag=E50D;house-flood-water=E50E;
house-flood-water-circle-arrow-right=E50F;house-lock=E510;house-medical-circle-check=E511;
house-medical-circle-exclamation=E512;house-medical-circle-xmark=E513;house-medical-flag=E514;
house-tsunami=E515;jar=E516;jar-wheat=E517;jet-fighter-up=E518;jug-detergent=E519;kitchen-set=E51A;
land-mine-on=E51B;landmark-flag=E51C;laptop-file=E51D;lines-leaning=E51E;location-pin-lock=E51F;locust=E520;
magnifying-glass-arrow-right=E521;magnifying-glass-chart=E522;mars-and-venus-burst=E523;mask-ventilator=E524;
mattress-pillow=E525;mobile-retro=E527;money-bill-transfer=E528;money-bill-trend-up=E529;
money-bill-wheat=E52A;mosquito=E52B;mosquito-net=E52C;mound=E52D;mountain-city=E52E;mountain-sun=E52F;
oil-well=E532;people-group=E533;people-line=E534;people-pulling=E535;people-robbery=E536;people-roof=E537;
person-arrow-down-to-line=E538;person-arrow-up-from-line=E539;person-breastfeeding=E53A;person-burst=E53B;
person-cane=E53C;person-chalkboard=E53D;person-circle-check=E53E;person-circle-exclamation=E53F;
person-circle-minus=E540;person-circle-plus=E541;person-circle-question=E542;person-circle-xmark=E543;
person-dress-burst=E544;person-drowning=E545;person-falling=E546;person-falling-burst=E547;
person-half-dress=E548;person-harassing=E549;person-military-pointing=E54A;person-military-rifle=E54B;
person-military-to-person=E54C;person-rays=E54D;person-rifle=E54E;person-shelter=E54F;
person-walking-arrow-loop-left=E551;person-walking-arrow-right=E552;
person-walking-dashed-line-arrow-right=E553;person-walking-luggage=E554;plane-circle-check=E555;
plane-circle-exclamation=E556;plane-circle-xmark=E557;plane-lock=E558;plate-wheat=E55A;plug-circle-bolt=E55B;
plug-circle-check=E55C;plug-circle-exclamation=E55D;plug-circle-minus=E55E;plug-circle-plus=E55F;
plug-circle-xmark=E560;ranking-star=E561;road-barrier=E562;road-bridge=E563;road-circle-check=E564;
road-circle-exclamation=E565;road-circle-xmark=E566;road-lock=E567;road-spikes=E568;rug=E569;sack-xmark=E56A;
school-circle-check=E56B;school-circle-exclamation=E56C;school-circle-xmark=E56D;school-flag=E56E;
school-lock=E56F;sheet-plastic=E571;shield-cat=E572;shield-dog=E573;shield-heart=E574;square-nfi=E576;
square-person-confined=E577;square-virus=E578;staff-snake=E579;sun-plant-wilt=E57A;tarp=E57B;
tarp-droplet=E57C;tent=E57D;tent-arrow-down-to-line=E57E;tent-arrow-left-right=E57F;tent-arrow-turn-left=E580;
tent-arrows-down=E581;tents=E582;toilet-portable=E583;toilets-portable=E584;tower-cell=E585;
tower-observation=E586;tree-city=E587;trowel=E589;trowel-bricks=E58A;truck-arrow-right=E58B;
truck-droplet=E58C;truck-field=E58D;truck-field-un=E58E;truck-plane=E58F;users-between-lines=E591;
users-line=E592;users-rays=E593;users-rectangle=E594;users-viewfinder=E595;vial-circle-check=E596;
vial-virus=E597;wheat-awn-circle-exclamation=E598;worm=E599;xmarks-lines=E59A;child-dress=E59C;
child-reaching=E59D;file-circle-check=E5A0;file-circle-xmark=E5A1;person-through-window=E5A9;plant-wilt=E5AA;
stapler=E5AF;train-tram=E5B4;table-cells-column-lock=E678;table-cells-row-lock=E67A;web-awesome=E682;
thumbtack-slash=E68F;table-cells-row-unlock=E691;chart-diagram=E695;comment-nodes=E696;file-fragment=E697;
file-half-dashed=E698;hexagon-nodes=E699;hexagon-nodes-bolt=E69A;square-binary=E69B;martini-glass-empty=F000;
music=F001;magnifying-glass=F002;envelope=F003;heart=F004;star=F005;user=F007;film=F008;
table-cells-large=F009;table-cells=F00A;table-list=F00B;check=F00C;xmark=F00D;magnifying-glass-plus=F00E;
magnifying-glass-minus=F010;power-off=F011;signal=F012;gear=F013;trash-can=F014;house=F015;file=F016;
clock=F017;road=F018;download=F019;circle-down=F01A;circle-up=F01B;inbox=F01C;circle-play=F01D;
arrow-rotate-right=F01E;arrows-rotate=F021;rectangle-list=F022;lock=F023;flag=F024;headphones=F025;
volume-off=F026;volume-low=F027;volume-high=F028;qrcode=F029;barcode=F02A;tag=F02B;tags=F02C;book=F02D;
bookmark=F02E;print=F02F;camera=F030;font=F031;bold=F032;italic=F033;text-height=F034;text-width=F035;
align-left=F036;align-center=F037;align-right=F038;align-justify=F039;list=F03A;outdent=F03B;indent=F03C;
video=F03D;image=F03E;pencil=F040;location-pin=F041;circle-half-stroke=F042;droplet=F043;pen-to-square=F044;
share-from-square=F045;square-check=F046;arrows-up-down-left-right=F047;backward-step=F048;backward-fast=F049;
backward=F04A;play=F04B;pause=F04C;stop=F04D;forward=F04E;forward-fast=F050;forward-step=F051;eject=F052;
chevron-left=F053;chevron-right=F054;circle-plus=F055;circle-minus=F056;circle-xmark=F057;circle-check=F058;
circle-question=F059;circle-info=F05A;crosshairs=F05B;ban=F05E;arrow-left=F060;arrow-right=F061;arrow-up=F062;
arrow-down=F063;share=F064;expand=F065;compress=F066;minus=F068;circle-exclamation=F06A;gift=F06B;leaf=F06C;
fire=F06D;eye=F06E;eye-slash=F070;triangle-exclamation=F071;plane=F072;calendar-days=F073;shuffle=F074;
comment=F075;magnet=F076;chevron-up=F077;chevron-down=F078;retweet=F079;cart-shopping=F07A;folder=F07B;
folder-open=F07C;arrows-up-down=F07D;arrows-left-right=F07E;chart-bar=F080;camera-retro=F083;key=F084;
gears=F085;comments=F086;thumbs-up=F087;thumbs-down=F088;star-half=F089;arrow-right-from-bracket=F08B;
thumbtack=F08D;arrow-up-right-from-square=F08E;arrow-right-to-bracket=F090;trophy=F091;upload=F093;lemon=F094;
phone=F095;square=F096;square-phone=F098;unlock=F09C;credit-card=F09D;rss=F09E;hard-drive=F0A0;bullhorn=F0A1;
bell=F0A2;certificate=F0A3;hand-point-right=F0A4;hand-point-left=F0A5;hand-point-up=F0A6;hand-point-down=F0A7;
circle-arrow-left=F0A8;circle-arrow-right=F0A9;circle-arrow-up=F0AA;circle-arrow-down=F0AB;globe=F0AC;
wrench=F0AD;list-check=F0AE;filter=F0B0;briefcase=F0B1;up-down-left-right=F0B2;users=F0C0;link=F0C1;
cloud=F0C2;flask=F0C3;scissors=F0C4;copy=F0C5;paperclip=F0C6;floppy-disk=F0C7;bars=F0C9;list-ul=F0CA;
list-ol=F0CB;strikethrough=F0CC;underline=F0CD;table=F0CE;wand-magic=F0D0;truck=F0D1;money-bill=F0D6;
caret-down=F0D7;caret-up=F0D8;caret-left=F0D9;caret-right=F0DA;table-columns=F0DB;sort=F0DC;sort-down=F0DD;
sort-up=F0DE;arrow-rotate-left=F0E2;gavel=F0E3;gauge-simple-high=F0E4;bolt=F0E7;sitemap=F0E8;umbrella=F0E9;
paste=F0EA;lightbulb=F0EB;arrow-right-arrow-left=F0EC;cloud-arrow-down=F0ED;cloud-arrow-up=F0EE;
user-doctor=F0F0;stethoscope=F0F1;suitcase=F0F2;mug-saucer=F0F4;utensils=F0F5;file-lines=F0F6;building=F0F7;
hospital=F0F8;truck-medical=F0F9;suitcase-medical=F0FA;jet-fighter=F0FB;beer-mug-empty=F0FC;square-h=F0FD;
square-plus=F0FE;angles-left=F100;angles-right=F101;angles-up=F102;angles-down=F103;angle-left=F104;
angle-right=F105;angle-up=F106;angle-down=F107;desktop=F108;laptop=F109;tablet-button=F10A;mobile-button=F10B;
circle=F10C;quote-left=F10D;quote-right=F10E;spinner=F110;reply=F112;face-smile=F118;face-frown=F119;
face-meh=F11A;gamepad=F11B;keyboard=F11C;flag-checkered=F11E;terminal=F120;code=F121;reply-all=F122;
location-arrow=F124;crop=F125;code-branch=F126;link-slash=F127;info=F129;exclamation=F12A;superscript=F12B;
subscript=F12C;eraser=F12D;puzzle-piece=F12E;microphone=F130;microphone-slash=F131;shield=F132;calendar=F133;
fire-extinguisher=F134;rocket=F135;circle-chevron-left=F137;circle-chevron-right=F138;circle-chevron-up=F139;
circle-chevron-down=F13A;anchor=F13D;unlock-keyhole=F13E;bullseye=F140;ellipsis=F141;ellipsis-vertical=F142;
square-rss=F143;ticket=F145;square-minus=F146;arrow-turn-up=F148;arrow-turn-down=F149;square-pen=F14B;
square-arrow-up-right=F14C;compass=F14E;square-caret-down=F150;square-caret-up=F151;square-caret-right=F152;
euro-sign=F153;sterling-sign=F154;dollar-sign=F155;rupee-sign=F156;yen-sign=F157;ruble-sign=F158;
won-sign=F159;arrow-down-a-z=F15D;arrow-up-a-z=F15E;arrow-down-wide-short=F160;arrow-up-wide-short=F161;
arrow-down-1-9=F162;arrow-up-1-9=F163;arrow-down-long=F175;arrow-up-long=F176;arrow-left-long=F177;
arrow-right-long=F178;person-dress=F182;person=F183;sun=F185;moon=F186;box-archive=F187;bug=F188;
circle-right=F18E;circle-left=F190;square-caret-left=F191;circle-dot=F192;wheelchair=F193;lira-sign=F195;
shuttle-space=F197;square-envelope=F199;building-columns=F19C;graduation-cap=F19D;language=F1AB;fax=F1AC;
child=F1AE;paw=F1B0;spoon=F1B1;cube=F1B2;cubes=F1B3;recycle=F1B8;car=F1B9;taxi=F1BA;tree=F1BB;database=F1C0;
file-pdf=F1C1;file-word=F1C2;file-excel=F1C3;file-powerpoint=F1C4;file-image=F1C5;file-zipper=F1C6;
file-audio=F1C7;file-video=F1C8;file-code=F1C9;life-ring=F1CD;circle-notch=F1CE;paper-plane=F1D8;
clock-rotate-left=F1DA;heading=F1DC;paragraph=F1DD;sliders=F1DE;share-nodes=F1E0;square-share-nodes=F1E1;
bomb=F1E2;futbol=F1E3;tty=F1E4;binoculars=F1E5;plug=F1E6;newspaper=F1EA;wifi=F1EB;calculator=F1EC;
bell-slash=F1F6;trash=F1F8;copyright=F1F9;eye-dropper=F1FB;paintbrush=F1FC;cake-candles=F1FD;chart-area=F1FE;
chart-pie=F200;chart-line=F201;toggle-off=F204;toggle-on=F205;bicycle=F206;bus=F207;closed-captioning=F20A;
shekel-sign=F20B;cart-plus=F217;cart-arrow-down=F218;diamond=F219;ship=F21A;user-secret=F21B;motorcycle=F21C;
street-view=F21D;heart-pulse=F21E;venus=F221;mars=F222;mercury=F223;mars-and-venus=F224;transgender=F225;
venus-double=F226;mars-double=F227;venus-mars=F228;mars-stroke=F229;mars-stroke-up=F22A;
mars-stroke-right=F22B;neuter=F22C;genderless=F22D;server=F233;user-plus=F234;user-xmark=F235;bed=F236;
train=F238;train-subway=F239;battery-full=F240;battery-three-quarters=F241;battery-half=F242;
battery-quarter=F243;battery-empty=F244;arrow-pointer=F245;i-cursor=F246;object-group=F247;
object-ungroup=F248;note-sticky=F249;clone=F24D;scale-balanced=F24E;hourglass=F250;hourglass-start=F251;
hourglass-half=F252;hourglass-end=F253;hand-back-fist=F255;hand=F256;hand-scissors=F257;hand-lizard=F258;
hand-spock=F259;hand-pointer=F25A;hand-peace=F25B;trademark=F25C;registered=F25D;tv=F26C;calendar-plus=F271;
calendar-minus=F272;calendar-xmark=F273;calendar-check=F274;industry=F275;map-pin=F276;signs-post=F277;
map=F278;message=F27A;comment-dots=F27B;circle-pause=F28B;circle-stop=F28D;bag-shopping=F290;
basket-shopping=F291;hashtag=F292;universal-access=F29A;person-walking-with-cane=F29D;audio-description=F29E;
phone-volume=F2A0;braille=F2A1;ear-listen=F2A2;hands-asl-interpreting=F2A3;ear-deaf=F2A4;hands=F2A7;
eye-low-vision=F2A8;font-awesome=F2B4;handshake=F2B5;envelope-open=F2B6;address-book=F2B9;address-card=F2BB;
circle-user=F2BD;id-badge=F2C1;id-card=F2C2;temperature-full=F2C7;temperature-three-quarters=F2C8;
temperature-half=F2C9;temperature-quarter=F2CA;temperature-empty=F2CB;shower=F2CC;bath=F2CD;podcast=F2CE;
window-maximize=F2D0;window-minimize=F2D1;window-restore=F2D2;square-xmark=F2D3;rectangle-xmark=F2D4;
microchip=F2DB;snowflake=F2DC;rotate-left=F2EA;rotate=F2F1;stopwatch=F2F2;right-from-bracket=F2F5;
right-to-bracket=F2F6;rotate-right=F2F9;poo=F2FE;images=F302;pen=F304;pen-clip=F305;down-long=F309;
left-long=F30A;right-long=F30B;up-long=F30C;file-pen=F31C;maximize=F31E;clipboard=F328;left-right=F337;
up-down=F338;up-right-from-square=F35D;square-up-right=F360;right-left=F362;repeat=F363;code-commit=F386;
code-merge=F387;gem=F3A5;turn-down=F3BE;turn-up=F3BF;lock-open=F3C1;location-dot=F3C5;microphone-lines=F3C9;
mobile-screen-button=F3CD;mobile=F3CE;mobile-screen=F3CF;money-bill-1=F3D1;phone-slash=F3DD;
image-portrait=F3E0;shield-halved=F3ED;tablet-screen-button=F3FA;tablet=F3FB;gauge-high=F3FD;
ticket-simple=F3FF;user-large=F406;down-left-and-up-right-to-center=F422;
up-right-and-down-left-from-center=F424;baseball-bat-ball=F432;baseball=F433;basketball=F434;
bowling-ball=F436;chess=F439;chess-bishop=F43A;chess-board=F43C;chess-king=F43F;chess-knight=F441;
chess-pawn=F443;chess-queen=F445;chess-rook=F447;dumbbell=F44B;football=F44E;golf-ball-tee=F450;
hockey-puck=F453;broom-ball=F458;square-full=F45C;table-tennis-paddle-ball=F45D;volleyball=F45F;
hand-dots=F461;bandage=F462;box=F466;boxes-stacked=F468;briefcase-medical=F469;fire-flame-simple=F46A;
capsules=F46B;clipboard-check=F46C;clipboard-list=F46D;person-dots-from-line=F470;dna=F471;dolly=F472;
cart-flatbed=F474;file-medical=F477;file-waveform=F478;kit-medical=F479;circle-h=F47E;id-card-clip=F47F;
notes-medical=F481;pallet=F482;pills=F484;prescription-bottle=F485;prescription-bottle-medical=F486;
bed-pulse=F487;truck-fast=F48B;smoking=F48D;syringe=F48E;tablets=F490;thermometer=F491;vial=F492;vials=F493;
warehouse=F494;weight-scale=F496;x-ray=F497;box-open=F49E;comment-slash=F4B3;couch=F4B8;
circle-dollar-to-slot=F4B9;dove=F4BA;hand-holding=F4BD;hand-holding-heart=F4BE;hand-holding-dollar=F4C0;
hand-holding-droplet=F4C1;hands-holding=F4C2;handshake-angle=F4C4;handshake-simple=F4C6;parachute-box=F4CD;
people-carry-box=F4CE;piggy-bank=F4D3;ribbon=F4D6;route=F4D7;seedling=F4D8;sign-hanging=F4D9;
face-smile-wink=F4DA;tape=F4DB;truck-ramp-box=F4DE;truck-moving=F4DF;video-slash=F4E2;wine-glass=F4E3;
user-large-slash=F4FA;user-astronaut=F4FB;user-check=F4FC;user-clock=F4FD;user-gear=F4FE;user-pen=F4FF;
user-group=F500;user-graduate=F501;user-lock=F502;user-minus=F503;user-ninja=F504;user-shield=F505;
user-slash=F506;user-tag=F507;user-tie=F508;users-gear=F509;scale-unbalanced=F515;scale-unbalanced-flip=F516;
blender=F517;book-open=F518;tower-broadcast=F519;broom=F51A;chalkboard=F51B;chalkboard-user=F51C;church=F51D;
coins=F51E;compact-disc=F51F;crow=F520;crown=F521;dice=F522;dice-five=F523;dice-four=F524;dice-one=F525;
dice-six=F526;dice-three=F527;dice-two=F528;divide=F529;door-closed=F52A;door-open=F52B;equals=F52C;
feather=F52D;frog=F52E;gas-pump=F52F;glasses=F530;greater-than=F531;greater-than-equal=F532;helicopter=F533;
infinity=F534;kiwi-bird=F535;less-than=F536;less-than-equal=F537;memory=F538;microphone-lines-slash=F539;
money-bill-wave=F53A;money-bill-1-wave=F53B;money-check=F53C;money-check-dollar=F53D;not-equal=F53E;
palette=F53F;square-parking=F540;diagram-project=F542;receipt=F543;robot=F544;ruler=F545;ruler-combined=F546;
ruler-horizontal=F547;ruler-vertical=F548;school=F549;screwdriver=F54A;shoe-prints=F54B;skull=F54C;
ban-smoking=F54D;store=F54E;shop=F54F;bars-staggered=F550;stroopwafel=F551;toolbox=F552;shirt=F553;
person-walking=F554;wallet=F555;face-angry=F556;archway=F557;book-atlas=F558;award=F559;delete-left=F55A;
bezier-curve=F55B;bong=F55C;brush=F55D;bus-simple=F55E;cannabis=F55F;check-double=F560;
martini-glass-citrus=F561;bell-concierge=F562;cookie=F563;cookie-bite=F564;crop-simple=F565;
tachograph-digital=F566;face-dizzy=F567;compass-drafting=F568;drum=F569;drum-steelpan=F56A;
feather-pointed=F56B;file-contract=F56C;file-arrow-down=F56D;file-export=F56E;file-import=F56F;
file-invoice=F570;file-invoice-dollar=F571;file-prescription=F572;file-signature=F573;file-arrow-up=F574;
fill=F575;fill-drip=F576;fingerprint=F577;fish=F578;face-flushed=F579;face-frown-open=F57A;martini-glass=F57B;
earth-africa=F57C;earth-americas=F57D;earth-asia=F57E;face-grimace=F57F;face-grin=F580;face-grin-wide=F581;
face-grin-beam=F582;face-grin-beam-sweat=F583;face-grin-hearts=F584;face-grin-squint=F585;
face-grin-squint-tears=F586;face-grin-stars=F587;face-grin-tears=F588;face-grin-tongue=F589;
face-grin-tongue-squint=F58A;face-grin-tongue-wink=F58B;face-grin-wink=F58C;grip=F58D;grip-vertical=F58E;
headphones-simple=F58F;headset=F590;highlighter=F591;hot-tub-person=F593;hotel=F594;joint=F595;face-kiss=F596;
face-kiss-beam=F597;face-kiss-wink-heart=F598;face-laugh=F599;face-laugh-beam=F59A;face-laugh-squint=F59B;
face-laugh-wink=F59C;cart-flatbed-suitcase=F59D;map-location=F59F;map-location-dot=F5A0;marker=F5A1;
medal=F5A2;face-meh-blank=F5A4;face-rolling-eyes=F5A5;monument=F5A6;mortar-pestle=F5A7;paint-roller=F5AA;
passport=F5AB;pen-fancy=F5AC;pen-nib=F5AD;pen-ruler=F5AE;plane-arrival=F5AF;plane-departure=F5B0;
prescription=F5B1;face-sad-cry=F5B3;face-sad-tear=F5B4;van-shuttle=F5B6;signature=F5B7;face-smile-beam=F5B8;
solar-panel=F5BA;spa=F5BB;splotch=F5BC;spray-can=F5BD;stamp=F5BF;star-half-stroke=F5C0;suitcase-rolling=F5C1;
face-surprise=F5C2;swatchbook=F5C3;person-swimming=F5C4;water-ladder=F5C5;droplet-slash=F5C7;face-tired=F5C8;
tooth=F5C9;umbrella-beach=F5CA;vector-square=F5CB;weight-hanging=F5CD;wine-glass-empty=F5CE;
spray-can-sparkles=F5D0;apple-whole=F5D1;atom=F5D2;bone=F5D7;book-open-reader=F5DA;brain=F5DC;car-rear=F5DE;
car-battery=F5DF;car-burst=F5E1;car-side=F5E4;charging-station=F5E7;diamond-turn-right=F5EB;draw-polygon=F5EE;
laptop-code=F5FC;layer-group=F5FD;location-crosshairs=F601;lungs=F604;microscope=F610;oil-can=F613;poop=F619;
shapes=F61F;star-of-life=F621;gauge=F624;gauge-simple=F629;teeth=F62E;teeth-open=F62F;masks-theater=F630;
traffic-light=F637;truck-monster=F63B;truck-pickup=F63C;rectangle-ad=F641;ankh=F644;book-bible=F647;
business-time=F64A;city=F64F;comment-dollar=F651;comments-dollar=F653;cross=F654;dharmachakra=F655;
envelope-open-text=F658;folder-minus=F65D;folder-plus=F65E;filter-circle-dollar=F662;gopuram=F664;hamsa=F665;
bahai=F666;jedi=F669;book-journal-whills=F66A;kaaba=F66B;khanda=F66D;landmark=F66F;envelopes-bulk=F674;
menorah=F676;mosque=F678;om=F679;spaghetti-monster-flying=F67B;peace=F67C;place-of-worship=F67F;
square-poll-vertical=F681;square-poll-horizontal=F682;person-praying=F683;hands-praying=F684;book-quran=F687;
magnifying-glass-dollar=F688;magnifying-glass-location=F689;socks=F696;square-root-variable=F698;
star-and-crescent=F699;star-of-david=F69A;synagogue=F69B;scroll-torah=F6A0;torii-gate=F6A1;vihara=F6A7;
volume-xmark=F6A9;yin-yang=F6AD;blender-phone=F6B6;book-skull=F6B7;campground=F6BB;cat=F6BE;chair=F6C0;
cloud-moon=F6C3;cloud-sun=F6C4;cow=F6C8;dice-d20=F6CF;dice-d6=F6D1;dog=F6D3;dragon=F6D5;drumstick-bite=F6D7;
dungeon=F6D9;file-csv=F6DD;hand-fist=F6DE;ghost=F6E2;hammer=F6E3;hanukiah=F6E6;hat-wizard=F6E8;
person-hiking=F6EC;hippo=F6ED;horse=F6F0;house-chimney-crack=F6F1;hryvnia-sign=F6F2;mask=F6FA;mountain=F6FC;
network-wired=F6FF;otter=F700;ring=F70B;person-running=F70C;scroll=F70E;skull-crossbones=F714;slash=F715;
spider=F717;toilet-paper=F71E;tractor=F722;user-injured=F728;vr-cardboard=F729;wand-sparkles=F72B;wind=F72E;
wine-bottle=F72F;cloud-meatball=F73B;cloud-moon-rain=F73C;cloud-rain=F73D;cloud-showers-heavy=F740;
cloud-sun-rain=F743;democrat=F747;flag-usa=F74D;hurricane=F751;landmark-dome=F752;meteor=F753;
person-booth=F756;poo-storm=F75A;rainbow=F75B;republican=F75E;smog=F75F;temperature-high=F769;
temperature-low=F76B;cloud-bolt=F76C;tornado=F76F;volcano=F770;check-to-slot=F772;water=F773;baby=F77C;
baby-carriage=F77D;biohazard=F780;blog=F781;calendar-day=F783;calendar-week=F784;candy-cane=F786;carrot=F787;
cash-register=F788;minimize=F78C;dumpster=F793;dumpster-fire=F794;ethernet=F796;gifts=F79C;
champagne-glasses=F79F;whiskey-glass=F7A0;earth-europe=F7A2;grip-lines=F7A4;grip-lines-vertical=F7A5;
guitar=F7A6;heart-crack=F7A9;holly-berry=F7AA;horse-head=F7AB;icicles=F7AD;igloo=F7AE;mitten=F7B5;
mug-hot=F7B6;radiation=F7B9;circle-radiation=F7BA;restroom=F7BD;satellite=F7BF;satellite-dish=F7C0;
sd-card=F7C2;sim-card=F7C4;person-skating=F7C5;person-skiing=F7C9;person-skiing-nordic=F7CA;sleigh=F7CC;
comment-sms=F7CD;person-snowboarding=F7CE;snowman=F7D0;snowplow=F7D2;tenge-sign=F7D7;toilet=F7D8;
screwdriver-wrench=F7D9;fire-flame-curved=F7E4;bacon=F7E5;book-medical=F7E6;bread-slice=F7EC;cheese=F7EF;
house-chimney-medical=F7F2;clipboard-user=F7F3;comment-medical=F7F5;crutch=F7F7;disease=F7FA;egg=F7FB;
folder-tree=F802;burger=F805;hand-middle-finger=F806;helmet-safety=F807;hospital-user=F80D;hotdog=F80F;
ice-cream=F810;laptop-medical=F812;pager=F815;pepper-hot=F816;pizza-slice=F818;sack-dollar=F81D;
book-tanakh=F827;bars-progress=F828;trash-arrow-up=F829;trash-can-arrow-up=F82A;user-nurse=F82F;
wave-square=F83E;person-biking=F84A;border-all=F84C;border-none=F850;border-top-left=F853;person-digging=F85E;
fan=F863;icons=F86D;phone-flip=F879;square-phone-flip=F87B;photo-film=F87C;text-slash=F87D;
arrow-down-z-a=F881;arrow-up-z-a=F882;arrow-down-short-wide=F884;arrow-up-short-wide=F885;arrow-down-9-1=F886;
arrow-up-9-1=F887;spell-check=F891;voicemail=F897;hat-cowboy=F8C0;hat-cowboy-side=F8C1;computer-mouse=F8CC;
radio=F8D7;record-vinyl=F8D9;walkie-talkie=F8EF;caravan=F8FF;firefox-browser=E007;ideal=E013;microblog=E01A;
square-pied-piper=E01E;unity=E049;dailymotion=E052;square-instagram=E055;mixer=E056;shopify=E057;deezer=E077;
edge-legacy=E078;google-pay=E079;rust=E07A;tiktok=E07B;unsplash=E07C;cloudflare=E07D;guilded=E07E;hive=E07F;
42-group=E080;instalod=E081;octopus-deploy=E082;perbyte=E083;uncharted=E084;watchman-monitoring=E087;
wodu=E088;wirsindhandwerk=E2D0;bots=E340;cmplid=E360;bilibili=E3D9;golang=E40F;pix=E43A;sitrox=E44A;
hashnode=E499;meta=E49B;padlet=E4A0;nfc-directional=E530;nfc-symbol=E531;screenpal=E570;space-awesome=E5AC;
square-font-awesome=E5AD;square-gitlab=E5AE;odysee=E5C6;stubber=E5C7;debian=E60B;shoelace=E60C;threads=E618;
square-threads=E619;square-x-twitter=E61A;x-twitter=E61B;opensuse=E62B;letterboxd=E62D;square-letterboxd=E62E;
mintbit=E62F;google-scholar=E63B;brave=E63C;brave-reverse=E63D;pixiv=E640;upwork=E641;webflow=E65C;
signal-messenger=E663;bluesky=E671;jxl=E67B;square-upwork=E67C;square-web-awesome=E683;
square-web-awesome-stroke=E684;dart-lang=E693;flutter=E694;files-pinwheel=E69F;css=E6A2;square-bluesky=E6A3;
square-twitter=F081;square-facebook=F082;linkedin=F08C;square-github=F092;twitter=F099;facebook=F09A;
github=F09B;pinterest=F0D2;square-pinterest=F0D3;square-google-plus=F0D4;google-plus-g=F0D5;linkedin-in=F0E1;
github-alt=F113;maxcdn=F136;html5=F13B;css3=F13C;btc=F15A;square-youtube=F166;youtube=F167;xing=F168;
square-xing=F169;dropbox=F16B;stack-overflow=F16C;instagram=F16D;flickr=F16E;adn=F170;bitbucket=F171;
tumblr=F173;square-tumblr=F174;apple=F179;windows=F17A;android=F17B;linux=F17C;dribbble=F17D;skype=F17E;
foursquare=F180;trello=F181;gratipay=F184;vk=F189;weibo=F18A;renren=F18B;pagelines=F18C;stack-exchange=F18D;
square-vimeo=F194;slack=F198;wordpress=F19A;openid=F19B;yahoo=F19E;google=F1A0;reddit=F1A1;square-reddit=F1A2;
stumbleupon-circle=F1A3;stumbleupon=F1A4;delicious=F1A5;digg=F1A6;pied-piper-pp=F1A7;pied-piper-alt=F1A8;
drupal=F1A9;joomla=F1AA;behance=F1B4;square-behance=F1B5;steam=F1B6;square-steam=F1B7;spotify=F1BC;
deviantart=F1BD;soundcloud=F1BE;vine=F1CA;codepen=F1CB;jsfiddle=F1CC;rebel=F1D0;empire=F1D1;square-git=F1D2;
git=F1D3;hacker-news=F1D4;tencent-weibo=F1D5;qq=F1D6;weixin=F1D7;slideshare=F1E7;twitch=F1E8;yelp=F1E9;
paypal=F1ED;google-wallet=F1EE;cc-visa=F1F0;cc-mastercard=F1F1;cc-discover=F1F2;cc-amex=F1F3;cc-paypal=F1F4;
cc-stripe=F1F5;lastfm=F202;square-lastfm=F203;ioxhost=F208;angellist=F209;buysellads=F20D;connectdevelop=F20E;
dashcube=F210;forumbee=F211;leanpub=F212;sellsy=F213;shirtsinbulk=F214;simplybuilt=F215;skyatlas=F216;
pinterest-p=F231;whatsapp=F232;viacoin=F237;medium=F23A;y-combinator=F23B;optin-monster=F23C;opencart=F23D;
expeditedssl=F23E;cc-jcb=F24B;cc-diners-club=F24C;creative-commons=F25E;gg=F260;gg-circle=F261;
odnoklassniki=F263;square-odnoklassniki=F264;get-pocket=F265;wikipedia-w=F266;safari=F267;chrome=F268;
firefox=F269;opera=F26A;internet-explorer=F26B;contao=F26D;500px=F26E;amazon=F270;houzz=F27C;vimeo-v=F27D;
black-tie=F27E;fonticons=F280;reddit-alien=F281;edge=F282;codiepie=F284;modx=F285;fort-awesome=F286;usb=F287;
product-hunt=F288;mixcloud=F289;scribd=F28A;bluetooth=F293;bluetooth-b=F294;gitlab=F296;wpbeginner=F297;
wpforms=F298;envira=F299;accessible-icon=F29B;glide=F2A5;glide-g=F2A6;viadeo=F2A9;square-viadeo=F2AA;
snapchat=F2AB;square-snapchat=F2AD;pied-piper=F2AE;first-order=F2B0;yoast=F2B1;themeisle=F2B2;
google-plus=F2B3;linode=F2B8;quora=F2C4;free-code-camp=F2C5;telegram=F2C6;bandcamp=F2D5;grav=F2D6;etsy=F2D7;
imdb=F2D8;ravelry=F2D9;sellcast=F2DA;superpowers=F2DD;wpexplorer=F2DE;meetup=F2E0;
square-font-awesome-stroke=F35C;accusoft=F369;adversal=F36A;affiliatetheme=F36B;algolia=F36C;amilia=F36D;
angrycreative=F36E;app-store=F36F;app-store-ios=F370;apper=F371;asymmetrik=F372;audible=F373;avianex=F374;
aws=F375;bimobject=F378;bitcoin=F379;bity=F37A;blackberry=F37B;blogger=F37C;blogger-b=F37D;
buromobelexperte=F37F;centercode=F380;cloudscale=F383;cloudsmith=F384;cloudversify=F385;cpanel=F388;
css3-alt=F38B;cuttlefish=F38C;d-and-d=F38D;deploydog=F38E;deskpro=F38F;digital-ocean=F391;discord=F392;
discourse=F393;dochub=F394;docker=F395;draft2digital=F396;square-dribbble=F397;dyalog=F399;earlybirds=F39A;
erlang=F39D;facebook-f=F39E;facebook-messenger=F39F;firstdraft=F3A1;fonticons-fi=F3A2;fort-awesome-alt=F3A3;
freebsd=F3A4;gitkraken=F3A6;gofore=F3A7;goodreads=F3A8;goodreads-g=F3A9;google-drive=F3AA;google-play=F3AB;
gripfire=F3AC;grunt=F3AD;gulp=F3AE;square-hacker-news=F3AF;hire-a-helper=F3B0;hotjar=F3B1;hubspot=F3B2;
itunes=F3B4;itunes-note=F3B5;jenkins=F3B6;joget=F3B7;js=F3B8;square-js=F3B9;keycdn=F3BA;kickstarter=F3BB;
kickstarter-k=F3BC;laravel=F3BD;line=F3C0;lyft=F3C3;magento=F3C4;medapps=F3C6;medrt=F3C8;microsoft=F3CA;
mix=F3CB;mizuni=F3CC;monero=F3D0;napster=F3D2;node-js=F3D3;npm=F3D4;ns8=F3D5;nutritionix=F3D6;page4=F3D7;
palfed=F3D8;patreon=F3D9;periscope=F3DA;phabricator=F3DB;phoenix-framework=F3DC;playstation=F3DF;pushed=F3E1;
python=F3E2;red-river=F3E3;wpressr=F3E4;replyd=F3E6;resolving=F3E7;rocketchat=F3E8;rockrms=F3E9;schlix=F3EA;
searchengin=F3EB;servicestack=F3EC;sistrix=F3EE;speakap=F3F3;staylinked=F3F5;steam-symbol=F3F6;
sticker-mule=F3F7;studiovinari=F3F8;supple=F3F9;uber=F402;uikit=F403;uniregistry=F404;untappd=F405;
ussunnah=F407;vaadin=F408;viber=F409;vimeo=F40A;vnv=F40B;square-whatsapp=F40C;whmcs=F40D;
wordpress-simple=F411;xbox=F412;yandex=F413;yandex-international=F414;apple-pay=F415;cc-apple-pay=F416;
fly=F417;node=F419;osi=F41A;react=F41B;autoprefixer=F41C;less=F41D;sass=F41E;vuejs=F41F;angular=F420;
aviato=F421;ember=F423;gitter=F426;hooli=F427;strava=F428;stripe=F429;stripe-s=F42A;typo3=F42B;
amazon-pay=F42C;cc-amazon-pay=F42D;ethereum=F42E;korvue=F42F;elementor=F430;flipboard=F44D;hips=F452;php=F457;
quinscape=F459;readme=F4D5;java=F4E4;pied-piper-hat=F4E5;creative-commons-by=F4E7;creative-commons-nc=F4E8;
creative-commons-nc-eu=F4E9;creative-commons-nc-jp=F4EA;creative-commons-nd=F4EB;creative-commons-pd=F4EC;
creative-commons-pd-alt=F4ED;creative-commons-remix=F4EE;creative-commons-sa=F4EF;
creative-commons-sampling=F4F0;creative-commons-sampling-plus=F4F1;creative-commons-share=F4F2;
creative-commons-zero=F4F3;ebay=F4F4;keybase=F4F5;mastodon=F4F6;r-project=F4F7;researchgate=F4F8;
teamspeak=F4F9;first-order-alt=F50A;fulcrum=F50B;galactic-republic=F50C;galactic-senate=F50D;jedi-order=F50E;
mandalorian=F50F;old-republic=F510;phoenix-squadron=F511;sith=F512;trade-federation=F513;
wolf-pack-battalion=F514;hornbill=F592;mailchimp=F59E;megaport=F5A3;nimblr=F5A8;rev=F5B2;shopware=F5B5;
squarespace=F5BE;themeco=F5C6;weebly=F5CC;wix=F5CF;ello=F5F1;hackerrank=F5F7;kaggle=F5FA;markdown=F60F;
neos=F612;zhihu=F63F;alipay=F642;the-red-yeti=F69D;critical-role=F6C9;d-and-d-beyond=F6CA;dev=F6CC;
fantasy-flight-games=F6DC;wizards-of-the-coast=F730;think-peaks=F731;reacteurope=F75D;artstation=F77A;
atlassian=F77B;canadian-maple-leaf=F785;centos=F789;confluence=F78D;dhl=F790;diaspora=F791;fedex=F797;
fedora=F798;figma=F799;intercom=F7AF;invision=F7B0;jira=F7B1;mendeley=F7B3;raspberry-pi=F7BB;redhat=F7BC;
sketch=F7C6;sourcetree=F7D3;suse=F7D6;ubuntu=F7DF;ups=F7E0;usps=F7E1;yarn=F7E3;airbnb=F834;battle-net=F835;
bootstrap=F836;buffer=F837;chromecast=F838;evernote=F839;itch-io=F83A;salesforce=F83B;speaker-deck=F83C;
symfony=F83D;waze=F83F;yammer=F840;git-alt=F841;stackpath=F842;cotton-bureau=F89E;buy-n-large=F8A6;mdb=F8CA;
orcid=F8D2;swift=F8E1;umbraco=F8E8;
]]
    local function utf8(cp)
        if cp < 0x80 then
            return string.char(cp)
        elseif cp < 0x800 then
            return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + cp % 0x40)
        end
        return string.char(0xE0 + math.floor(cp / 0x1000), 0x80 + math.floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
    end
    for name, code in icons:gmatch('([%w%-]+)=(%x+);') do
        local key = 'ICON_FA_' .. (name:upper():gsub('%-', '_'))
        if rawget(_G, key) == nil then
            rawset(_G, key, utf8(tonumber(code, 16)))
        end
    end
    if rawget(_G, 'ICON_MIN_FA') == nil then
        rawset(_G, 'ICON_MIN_FA', 0xE005)
        rawset(_G, 'ICON_MAX_FA', 0xF8FF)
        rawset(_G, 'ICON_MAX_16_FA', 0xF8FF)
    end
end
-- <<< font awesome
