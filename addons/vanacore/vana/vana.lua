--[[
vana: VanaCore's own window, loaded at start. The settings (data.lua: the tabs, their rows and the write path to
vanacore.ini) drawn on ImGui's draw lists (draw.lua): the pages in a list down the left with a search box over them,
the page's settings on the right in sections that reset on their own, a line each.

In game (//vana works the same):
  /vana                   opens or closes the window
  /vana <page>            opens it at a page (/vana graphics; the old tabs' names and aliases work)
  /vana close             closes it
  /vana reload            reloads the addon (/addon reload vana) and opens it again at the same page
  /vana scale 2           makes it twice its usual size (1 puts it back; 0.5 to 3, or 50% to 300%)
  /vana layout            the Addons page's Layout: addons' windows on screen
  /vana toggles           the Graphics page's Toggles: each effect switched on and off, one at a time (/vana layers too)
  /vana gamepad           the Client page's Gamepads (/gamepad too)
  /vana pivot             the Pivot page: the DAT overlays (/pivot too)
  /vana hud               the HUD page: the grid and the HUD presets
  /vana preset <name>     loads the Graphics, Effects and Experimental preset of that name (a scripts\boot.txt line can); alone, Presets
  /vana hudpreset <name>  loads the HUD and Game Options preset of that name; alone, the HUD page at its Presets
  /hud [element]          the HUD Manager (hud.lua): every game window's box, and the one picked's settings
  /hud addons             the HUD Manager in its Addon View: the addons' windows' boxes; /hud <addon> at that one
  /vana hud manager       the HUD Manager opened or closed, as /hud (the game's Config > HUD runs it)
  /vana commands          the Client page's Commands: VanaCore's commands, each switched on or off ([commands])
  /vana boot              the Client page's Boot.txt: scripts\boot.txt in a box to edit and save
  /vana install [file]    installs an addon package (.vcaddon): what's in it shown first; alone, Windows' Open dialog
  /vana reset ui          moves every addon's and plugin's window to the top-left corner (0,0)
  /vana grass capture     Mass Grass: the plants around you into the library (list, reload)
  /vana help <addon>      opens it at that addon's (or plugin's) read me and details
  /vana debug             opens or closes the Debug window (debug.lua): DLSS crash tests, frame recording
  /vana statsdump         the Equipment window's stats into the log, each step: items, modifiers, augments, sets, sums
  /vana help              the commands; stats, graph, debug <seconds>, abtest, telemetry, logs, trace, window and lua
                          are VanaCore's own (addons.cpp)

[vana] scale sizes the window; the screen's height sets the rest. Folded sections are kept in
config\addons\vana\settings.lua. Presets (named sets of settings) go to vanacore.presets.ini (Graphics) and vanacore.hudpresets.ini (HUD, Game Options' interface) beside vanacore.ini; Export
shows one as a string to copy (or saves a .json), Import takes either, with the addons' window places if asked (share.lua).
--]]

addon.name    = 'vana'
addon.author  = 'Vekien'
addon.version = '2.0'
addon.desc    = "VanaCore's window: what's running, and its settings."

local imgui = vana.imgui
if imgui == nil then
    print('vana needs a newer VanaCore.Core.dll (vana.imgui)')
    return
end

-------------------------------------------------------------------------------
-- The modules: draw.lua (the look and the controls) and data.lua (the settings), beside this file, given M
-------------------------------------------------------------------------------

local M = {}

local st = {
    open = false,
    minimised = false, -- the window shrunk to its tile, a 64 x 64 square with the logo in it (the minimize button); a click opens it again
    tile_at = nil,     -- { x, y }: where the tile goes at the next frame (its saved place or the window's top right corner, the first minimize)
    tile_placed = false, -- the tile has been put somewhere this session: ImGui keeps it there from then on
    tile_press = nil,  -- { x, y }: where the mouse pressed the tile (let go without moving: the window opens; moved: it was dragged)
    page = nil,        -- the page shown
    rows = {},         -- page id -> its rows as last built (tab.rows(), the marks attached, the hidden kinds out)
    rebuild_wanted = false, -- every page's rows built again at the next frame (a change that locks or unlocks others)
    anim = {},         -- id -> an eased 0..1 (draw.lua's ease)
    sidebar = true,    -- the list of pages shown at the left (hidden, the window narrows by it; settings.lua)
    size = nil,        -- { w, h }: the window's size dragged at its resize corner, unscaled as WIN_W and WIN_H (settings.lua); nil: theirs
    grab = nil,        -- { x, y }: the resize corner held, the mouse's distance from the window's corner
    sub = {},          -- page id -> the title of its section shown (its sub-tab in the list; settings.lua)
    navopen = {},      -- page id -> true / false: its sub-tabs open in the list or shut by its chevron (settings.lua)
    tabs = {},         -- a tab bar's id (row kind 'tabs') -> the tab picked, from 1 (settings.lua)
    groups = {},       -- a hidden group's id (M.toggle_group) -> true while its button has it shown (settings.lua)
    search = { '' },   -- the search box's text (ImGui writes it)
    last = 0, dt = 0,  -- the clock at the last frame, and the frame's length
    sliding = nil,     -- the slider id held (draw.lua)
    drag = nil,        -- { id, value }: where the held slider is, shown while it's held and written when it's let go
    picker = nil,      -- { kind = 'colour' | 'number', row, id, y, seen }: the picker open beside the window
    armed = nil,       -- { action, until }: a button that asks twice, pressed once
    copied = nil,      -- { action, until }: a Characters copy just sent (its button says Sent)
    hover_key = nil, hover_since = 0, hover_seen = false, -- the row the mouse rests on, for its tooltip
    live = {},         -- row -> { text, aside, mine, at }: a live value (About's, a status row's), read twice a second
    flash = nil,       -- { name, what, at }: "saved" / "loaded" by a preset for a moment
    confirm = nil,     -- { name, at }: a preset's Delete asked once
    presets = { list = {}, read = false, name = { '' } }, -- the user's presets of the kind shown (use_presets)
    number = nil,      -- the number box's buffer: { text }, and focus
    layout_next = 0,   -- when the Layout page looks at the windows again
    dlss5_next = 0,    -- when the Graphics page's DLSS 5 rows are checked against vana.graphics.info() again
    resolution_next = 0, -- when the Client page's Resolution is checked against the window's dragged size again
    fps = 0, frames = 0, since = 0,
}
M.st = st

-- this addon's folder, without a trailing backslash
M.folder = vana.paths.addon
if M.folder == nil or M.folder == '' then
    M.folder = vana.paths.addons .. '\\vanacore\\vana'
end
M.folder = M.folder:gsub('[\\/]+$', '')

local function load_module(name)
    local chunk, err = loadfile(M.folder .. '\\' .. name .. '.lua')
    if chunk == nil then
        error('vana: ' .. tostring(err))
    end
    chunk(M)
end

load_module('draw')
load_module('data')
load_module('share') -- a preset as a string, and addons' windows fitted to another screen
load_module('addons') -- the Addons and Plugins pages' list (in data.lua's place) and each one's help
load_module('install') -- the addon package prompt (a .vcaddon opened, or dropped in addons\install)
load_module('debug') -- the Debug window (/vana debug): debug views, DLSS crash tests, frame recording
load_module('gamepadtest') -- the Gamepad Test window (/gamepad test): buttons, sticks and stats of each controller
load_module('hud') -- the HUD Manager window (/hud): the game windows' boxes and the one picked's settings
load_module('copy') -- Client > Characters' copies of the Main's settings, addons and windows to the Alts
load_module('partypreview') -- Party / Alliance's Preview: made-up members in the party and alliance lists
load_module('zonename') -- the zone's name in the middle of the screen when you zone in (Mods > Zones > Zone Name)
do -- /vana2: a look at a redesigned window (vana2.lua, drawn with made-up values); one that fails to load never takes /vana with it
    local ok, err = pcall(load_module, 'vana2')
    if not ok then
        print('VanaCore: /vana2 isn\'t available: ' .. tostring(err))
    end
end

local S, P, fonts = M.S, M.P, M.fonts
local BODY, SMALL = M.BODY, M.SMALL

function M.rebuild()
    st.rebuild_wanted = true
end

-- the clock, as draw.lua's helpers read it too
local function clock()
    return vana.time.clock()
end

-------------------------------------------------------------------------------
-- The pages: the old window's tabs (Presets is Graphics' first sub-tab)
-------------------------------------------------------------------------------

-- a tab's page: its id (/vana <id>), icon and group; `presets` shows the Presets sub-tab
local PAGE_META = {
    Client = { id = 'client', icon = 'chip', group = 'Settings' }, -- (VanaCore, with About's banner and Info, is its first sub-tab: data.lua)
    Graphics = { id = 'graphics', icon = 'display', group = 'Settings', presets = true, preset_pick = true },
    -- (the HUD page isn't in the list: its Settings and Presets are windows of the HUD Manager's, hud.lua; its sections are the
    -- HUD Manager's elements)
    HUD = { id = 'hud', icon = 'hud', group = 'Settings', color = P.hud, preset_pick = true, hidden = true },
    ['Game Options'] = { id = 'options', icon = 'sliders', group = 'Settings', preset_pick = true },
    Pivot = { id = 'pivot', icon = 'layers', group = 'System' },
    Addons = { id = 'addons', icon = 'puzzle', group = 'System' },
    Plugins = { id = 'plugins', icon = 'plug', group = 'System' },
}
local GROUP_ORDER = { 'Settings', 'System' }

local PAGES = {}  -- in the list's order
local GROUPS = {} -- { name, pages }
do
    local by_group = {}
    for _, name in ipairs(GROUP_ORDER) do
        by_group[name] = { group = name, pages = {} }
        GROUPS[#GROUPS + 1] = by_group[name]
    end
    local function add(tab)
        local meta = PAGE_META[tab.name] or { id = tab.name:lower(), icon = 'gear', group = 'Settings' }
        local page = { id = meta.id, name = tab.name, icon = meta.icon, color = meta.color, tab = tab, presets = meta.presets, preset_pick = meta.preset_pick, aliases = tab.aliases or {},
                       hidden = meta.hidden, buttons = meta.buttons }
        if meta.id == 'graphics' then
            -- /vana presets: Graphics at its Presets sub-tab
            page.aliases = { 'presets', 'preset', unpack(page.aliases) }
        end
        PAGES[#PAGES + 1] = page
        local pages = by_group[meta.group].pages
        if not meta.hidden then
            pages[#pages + 1] = page
        end
        return pages
    end
    for _, tab in ipairs(M.TABS) do
        local pages = add(tab)
        if tab.name == 'Game Options' then
            -- Experimental, under Game Options: the Graphics tab's experimental panels, moved here (EXPERIMENTAL)
            local experimental = { id = 'experimental', name = 'Experimental', icon = 'flask', tab = M.tab_named('graphics'), virtual = true,
                                   aliases = { 'water', 'glow', 'elight', 'reflections', 'refl', 'normalmap', 'normals' }, buttons = {}, color = P.experimental, preset_pick = true }
            PAGES[#PAGES + 1] = experimental
            pages[#pages + 1] = experimental
            -- Mods, after Game Options: the Graphics tab's Mass Grass and Snow Trails and Game Options' Loading Screen, moved
            -- here (MOVED_PANELS, MOVED_OPTIONS); a tab in the header
            local mods = { id = 'mods', name = 'Mods', icon = 'wand', tab = M.tab_named('graphics'), virtual = true,
                           aliases = { 'grass', 'snow', 'loadscreen', 'zonelines', 'zones', 'zone', 'zonename' }, buttons = {}, preset_pick = true }
            PAGES[#PAGES + 1] = mods
            pages[#pages + 1] = mods
        end
    end
end

-- the Graphics tab's panels shown on another page instead, by their heading: that page and their order there (Reflections
-- and Glow Effects: Graphics' own). Mass Grass and Snow Trails: the Mods page's (Mass Grass was Game Options' last sub-tab
-- from 2026-11-06, the Experimental page's before)
local MOVED_PANELS = { ['Water Shader (Experimental)'] = { 'experimental', 1 }, ['Normal maps (Experimental)'] = { 'experimental', 2 },
                       ['Snow Trails (Experimental)'] = { 'mods', 2 }, ['Mass Grass'] = { 'mods', 1 } }
-- the Game Options tab's sections shown on the Mods page instead, by their heading (after the Graphics tab's)
local MOVED_OPTIONS = { ['Zones'] = true, ['Loading Screen'] = true, ['Sparkles'] = true }
-- the pages showing them, which a Graphics preset reads too (preset_keys)
local PANEL_HOSTS = {}
for _, to in pairs(MOVED_PANELS) do
    PANEL_HOSTS[to[1]] = true
end

local function page_named(name)
    name = (name or ''):lower()
    for _, page in ipairs(PAGES) do
        if page.id == name or page.name:lower() == name then
            return page
        end
        for _, alias in ipairs(page.aliases) do
            if alias == name then
                return page
            end
        end
    end
    return nil
end

-- A page's rows, built by its tab when first shown and again after a change that asks for it (M.rebuild): the marks
-- row attached to the slider after it, the kinds that are the old window's own drawing left out ('none', 'search').
local function page_rows(page)
    if page.tab == nil then
        return {}
    end
    local rows = st.rows[page.id]
    if rows ~= nil then
        return rows
    end
    if page.id == 'mods' then
        -- the Graphics tab's Mass Grass and Snow Trails, then Game Options' Zones and Loading Screen (each page builds its own
        -- rows first, which files these: graphics_to_mods, options_to_mods)
        page_rows(page_named('graphics'))
        page_rows(page_named('options'))
        rows = {}
        for _, list in ipairs({ st.rows.graphics_to_mods or {}, st.rows.options_to_mods or {} }) do
            for _, row in ipairs(list) do
                rows[#rows + 1] = row
            end
        end
        st.rows.mods = rows
        return rows
    end
    local ok, built = pcall(page.tab.rows)
    if not ok then
        built = { M.rows.header('This page failed to build'), { kind = 'warning', text = tostring(built) } }
    end
    rows = {}
    local marks = nil
    for _, row in ipairs(built) do
        if row.kind == 'marks' then
            marks = row
        elseif row.kind ~= 'none' and row.kind ~= 'search' then
            if marks ~= nil and row.kind == 'slider' then
                row.marks = marks.marks
            end
            marks = nil
            rows[#rows + 1] = row
        end
    end
    if page.tab.name == 'Graphics' then
        -- Mass Grass's library: an asset's switch and its Share slider as one row (the share is the switch: 0 is off),
        -- under a Grass Library sub-heading
        local merged, i, first = {}, 1, true
        local catalogue = {}
        local ok_assets, assets = pcall(vana.graphics.grass_assets)
        for _, a in ipairs(ok_assets and assets or {}) do
            catalogue[a.id] = a
        end
        while i <= #rows do
            local row, after = rows[i], rows[i + 1]
            if row.kind == 'toggle' and row.key ~= nil and row.key:match('^grass%.asset%.') and after ~= nil and after.kind == 'slider'
               and after.key == row.key .. '.amount' then
                local id = row.key:sub(#'grass.asset.' + 1)
                local asset = catalogue[id]
                after.asset_id = id
                after.asset_own = asset ~= nil and not asset.shipped and id ~= 'blades'
                if first then
                    merged[#merged + 1] = { kind = 'subheader', text = 'Grass Library', tag = 'what grows, by its share of the mix' }
                    first = false
                end
                -- a captured asset is named "Zone 132 plants 18": the zone by its name (the game's own list)
                local label = row.label:gsub('^Zone (%d+)', function (n)
                    local ok, name = false, nil
                    if vana.ashita ~= nil and vana.ashita.string ~= nil then
                        ok, name = pcall(vana.ashita.string, 'zones.names', tonumber(n))
                    end
                    return ok and type(name) == 'string' and name ~= '' and name or ('Zone ' .. n)
                end)
                after.label, after.asset, after.tip, after.needs_dx = label, row, row.tip, row.needs_dx or after.needs_dx
                merged[#merged + 1] = after
                i = i + 2
            else
                merged[#merged + 1] = row
                i = i + 1
            end
        end
        rows = merged
        -- the panels shown on another page (a heading and its rows to the next heading): the Experimental page's, and
        -- Mass Grass on Game Options (MOVED_PANELS)
        local kept, chunks, chunk = {}, {}, nil
        for _, row in ipairs(rows) do
            if row.kind == 'header' then
                local to = MOVED_PANELS[row.text]
                chunk = to ~= nil and { page = to[1], order = to[2] } or nil
                if chunk ~= nil then
                    chunks[#chunks + 1] = chunk
                end
            end
            local into = chunk or kept
            into[#into + 1] = row
        end
        table.sort(chunks, function (a, b) return a.order < b.order end)
        local moved = { experimental = {}, mods = {} }
        for _, c in ipairs(chunks) do
            local into = moved[c.page]
            for _, row in ipairs(c) do
                -- a Graphics preset's, filed as the Experimental page's wherever it's shown (preset_keys, load_preset)
                row.preset_page = 'experimental'
                if row.asset ~= nil then
                    row.asset.preset_page = 'experimental' -- (a library asset's switch: with_assets)
                end
                into[#into + 1] = row
            end
        end
        st.rows.graphics, st.rows.experimental, st.rows.graphics_to_mods = kept, moved.experimental, moved.mods
        return page.virtual and moved.experimental or kept
    end
    if page.id == 'options' then
        -- the sections shown on the Mods page (MOVED_OPTIONS): a heading and its rows to the next, out of this page's; a HUD
        -- preset's still (row.preset_page, as the Graphics panels' are filed as Graphics presets')
        local kept, moved, into = {}, {}, nil
        for _, row in ipairs(rows) do
            if row.kind == 'header' then
                into = MOVED_OPTIONS[row.text] and moved or nil
            end
            local list = into or kept
            list[#list + 1] = row
        end
        for _, row in ipairs(moved) do
            row.preset_page = 'options'
        end
        rows = kept
        st.rows.options_to_mods = moved
    end
    if page.id == 'client' then
        -- the window's scale ([vana] scale), under VanaCore's Info: after its Version (the big row), else first
        for i, row in ipairs(rows) do
            if row.kind == 'subheader' and row.text == 'Info' then
                local at = rows[i + 1] ~= nil and rows[i + 1].big and i + 2 or i + 1
                table.insert(rows, at, { kind = 'scale', label = 'Window scale', tip = M.TIPS.rows['vana.scale'] })
                break
            end
        end
    end
    st.rows[page.id] = rows
    return rows
end

-- A page's sections: a header row starts one (its rows until the next); rows before any header make a section with no
-- heading. A subheader stays a row of its section (drawn as a sub-heading that greys the rows after it by its switch).
local function sections_of(page, rows)
    local sections, current = {}, nil
    for _, row in ipairs(rows) do
        if row.kind == 'header' then
            local title = page.virtual and (row.text:gsub(' %([Ee]xperimental%)$', '')) or row.text
            current = { head = row, title = title, rows = {}, key = page.tab.name .. '/' .. row.text }
            sections[#sections + 1] = current
        else
            if current == nil then
                current = { head = nil, title = nil, rows = {}, key = page.tab.name .. '/' }
                sections[#sections + 1] = current
            end
            current.rows[#current.rows + 1] = row
        end
    end
    return sections
end

-- A page's sub-tabs: its sections with a heading, when it has two or more (the list shows them under the page, and the
-- page only the one picked). Returns them and the one picked (the first if none is, or the one picked has gone).
local ADDONS_ALL = { title = 'List', all = true } -- (the addons' list; each addon after it in italics)
local addons_layout = { at = -1, rows = {} } -- Layout's rows for the list while List is shown, built twice a second

-- The Addons page's: List, then each addon with a window on Layout, named without its kind ("partyfinder"), even one.
-- List is picked on List; an addon clicked shows Layout at it; on Layout, none picked shows every addon's windows.
local function addons_subtabs(page)
    local on_layout = M.layout.page == 'layout'
    local rows = addons_layout.rows
    if on_layout then
        rows = page_rows(page)
    elseif clock() - addons_layout.at >= 0.5 then
        addons_layout.at = clock()
        local ok, built = pcall(M.layout.rows)
        addons_layout.rows = ok and built or {}
        rows = addons_layout.rows
    end
    local subs = { ADDONS_ALL }
    for _, sec in ipairs(sections_of(page, rows)) do
        local windows = false
        for _, row in ipairs(sec.rows) do
            windows = windows or row.kind == 'layout'
        end
        if sec.head ~= nil and windows then
            sec.nav = (sec.title:gsub(' %([^()]*%)$', ''))
            subs[#subs + 1] = sec
        end
    end
    if not on_layout then
        return subs, ADDONS_ALL -- (List is picked on List)
    end
    for _, sec in ipairs(subs) do
        if sec ~= ADDONS_ALL and sec.title == st.sub[page.id] then
            return subs, sec
        end
    end
    return subs, nil -- (Layout with every addon's windows)
end

-- Graphics' first sub-tab: the user's presets (draw_presets_page), not one of its sections
local PRESETS_SUB = { title = 'Presets', presets = true, head = { nav_icon = 'bullet', nav_color = P.hud } }
-- ... and over its Settings, not a page: the HUD Manager window opened (hud.lua), this one hidden till it's closed
local HUD_OPEN_SUB = { title = 'HUD Manager', rows = {}, head = { nav_icon = 'open', nav_color = P.hud },
                       tip = "Every game window's box, to move it, and its settings (/hud)", run = function () M.hud.open_manager() end }

local function subtabs_of(page)
    -- (Pivot's Overlays and Available are one page, no sub-tabs)
    if page.tab == nil or page.id == 'pivot' or st.search[1] ~= '' then
        return nil, nil
    end
    if page.id == 'addons' then
        return addons_subtabs(page)
    end
    local subs = {}
    for _, sec in ipairs(sections_of(page, page_rows(page))) do
        if sec.head ~= nil and not sec.head.element then -- (the HUD's game windows: the HUD Manager's)
            subs[#subs + 1] = sec
        end
    end
    if #subs < 2 and page.id ~= 'hud' then
        return nil, nil
    end
    local first = subs[1] -- (shown when none is picked: Graphics' first section, not Presets or Toggles)
    if page.id == 'graphics' and first.title == 'Toggles' and subs[2] ~= nil then
        first = subs[2]
    end
    if page.id == 'graphics' then
        table.insert(subs, 1, PRESETS_SUB)
    elseif page.id == 'options' then
        table.insert(subs, 1, HUD_OPEN_SUB) -- (first in Game Options' list, in yellow: opens the HUD Manager)
    end
    for _, sec in ipairs(subs) do
        if sec.title == st.sub[page.id] then
            return subs, sec
        end
    end
    return subs, first
end

-- the tooltip of a row: its own, else TIPS.rows by its key
local function tip_of(row)
    if row.hides ~= nil then -- a Hide switch an addon holds on (data.lua's hides): who, and what it takes to bring the window back
        local by = M.hidden_by(row.hides)
        if by ~= nil then
            return 'Hidden by ' .. by .. '. It is shown again when that addon unhides it or is unloaded.'
        end
    end
    local text = row.tip
    if type(text) == 'function' then
        local ok, value = pcall(text)
        text = ok and value or nil
    end
    if text == nil and row.section ~= nil and row.key ~= nil then
        text = M.TIPS.rows[row.section .. '.' .. row.key]
    end
    return text
end

-- a live value (an info or status row's function), read again twice a second
local function live_value(row)
    local entry = st.live[row]
    local now = clock()
    if entry == nil or now - entry.at >= 0.5 then
        local ok, value, aside, mine = pcall(row.value)
        entry = { text = ok and tostring(value) or '?', aside = ok and aside ~= nil and tostring(aside) or nil, mine = ok and mine or false, at = now }
        st.live[row] = entry
    end
    return entry
end

-------------------------------------------------------------------------------
-- Folded sections (config\addons\vana\settings.lua)
-------------------------------------------------------------------------------

local SETTINGS_DIR = vana.paths.install .. '\\config\\addons\\vana'
local SETTINGS = SETTINGS_DIR .. '\\settings.lua'

local function load_settings()
    local chunk = loadfile(SETTINGS)
    if chunk == nil then
        return
    end
    setfenv(chunk, {})
    local ok, saved = pcall(chunk)
    if ok and type(saved) == 'table' and saved.sidebar == false then
        st.sidebar = false
    end
    if ok and type(saved) == 'table' and saved.layout == 'hybrid' then
        st.hybrid = true -- the header-and-tabs layout (/vana3), kept until /vana classic
    end
    if ok and type(saved) == 'table' and type(saved.size) == 'table' and tonumber(saved.size[1]) and tonumber(saved.size[2]) then
        st.size = { tonumber(saved.size[1]), tonumber(saved.size[2]) }
    end
    if ok and type(saved) == 'table' and type(saved.nav) == 'table' then
        for id, on in pairs(saved.nav) do
            if type(id) == 'string' and type(on) == 'boolean' then
                st.navopen[id] = on
            end
        end
    end
    if ok and type(saved) == 'table' and type(saved.sub) == 'table' then
        for id, title in pairs(saved.sub) do
            if type(id) == 'string' and type(title) == 'string' then
                st.sub[id] = title
            end
        end
    end
    if ok and type(saved) == 'table' and type(saved.tabs) == 'table' then
        for id, index in pairs(saved.tabs) do
            if type(id) == 'string' and type(index) == 'number' then
                st.tabs[id] = index
            end
        end
    end
    if ok and type(saved) == 'table' and type(saved.groups) == 'table' then
        for id, shown in pairs(saved.groups) do
            if type(id) == 'string' and shown == true then
                st.groups[id] = true
            end
        end
    end
end

local function save_settings()
    vana.fs.create_dir(SETTINGS_DIR)
    local f = io.open(SETTINGS, 'w')
    if not f then
        return
    end
    f:write("-- The /vana window's settings (written by it)\nreturn {\n")
    if not st.sidebar then
        f:write('    -- the list of pages hidden\n    sidebar = false,\n')
    end
    if st.hybrid then
        f:write("    -- the layout with the pages as tabs in a header (/vana3; /vana classic is the old one)\n    layout = 'hybrid',\n")
    end
    if st.size ~= nil then
        f:write(string.format("    -- the window's size, dragged at its resize corner (at 100%%)\n    size = { %d, %d },\n", st.size[1], st.size[2]))
    end
    local nav = {}
    for id in pairs(st.navopen) do
        nav[#nav + 1] = id
    end
    if #nav > 0 then
        table.sort(nav)
        f:write("    -- each page's sub-tabs open (true) or shut (false) in the list\n    nav = {\n")
        for _, id in ipairs(nav) do
            f:write(string.format('        [%q] = %s,\n', id, tostring(st.navopen[id])))
        end
        f:write('    },\n')
    end
    local ids = {}
    for id in pairs(st.sub) do
        ids[#ids + 1] = id
    end
    if #ids > 0 then
        table.sort(ids)
        f:write("    -- each page's sub-tab shown: page = 'heading'\n    sub = {\n")
        for _, id in ipairs(ids) do
            f:write(string.format('        [%q] = %q,\n', id, st.sub[id]))
        end
        f:write('    },\n')
    end
    local bars = {}
    for id in pairs(st.tabs) do
        bars[#bars + 1] = id
    end
    if #bars > 0 then
        table.sort(bars)
        f:write("    -- each tab bar's tab picked: bar = number\n    tabs = {\n")
        for _, id in ipairs(bars) do
            f:write(string.format('        [%q] = %d,\n', id, st.tabs[id]))
        end
        f:write('    },\n')
    end
    local shown = {}
    for id in pairs(st.groups) do
        shown[#shown + 1] = id
    end
    if #shown > 0 then
        table.sort(shown)
        f:write("    -- the groups shown by their button (Shadows' Advanced Settings)\n    groups = {\n")
        for _, id in ipairs(shown) do
            f:write(string.format('        [%q] = true,\n', id))
        end
        f:write('    },\n')
    end
    f:write('}\n')
    f:close()
end

-- A group hidden until its button shows it (Shadows' Advanced Settings: its heading's show_if reads st.groups, and
-- draw_section leaves a hidden group's box out): shown or hidden again, and kept. A field of M: data.lua's rows call it.
M.toggle_group = function (id)
    st.groups[id] = not st.groups[id] or nil
    save_settings()
end

-------------------------------------------------------------------------------
-- Quality presets: Low, Medium, High, Max over the Graphics settings that cost frames (not the resolution, DLSS or the
-- renderer), as vanacore.ini's text; '' is the default (the key unset). High is the defaults.
-------------------------------------------------------------------------------

local FX = M.FX
local QUALITY = {
    names = { 'Low', 'Medium', 'High', 'Max' },
    values = {
        [FX .. '.sun.enabled'] = { '0', '', '', '' },
        [FX .. '.sun_detail'] = { '1024', '2048', '', '8192' },
        [FX .. '.sun_distance'] = { '60', '80', '', '150' },
        [FX .. '.ao.enabled'] = { '0', '', '', '' },
        [FX .. '.ao_quality'] = { '0', '0', '', '4' }, -- (the default is Max, the 24 taps: Low and Medium keep the taps they had; 4 is 32, past Max: the box lights none)
        [FX .. '.ao_rings'] = { '', '', '', '0' },
        [FX .. '.sun_rings'] = { '', '', '', '0' },
        [FX .. '.fog.enabled'] = { '0', '', '', '' },
        [FX .. '.bloom.enabled'] = { '0', '', '', '' },
        [FX .. '.rays.enabled'] = { '0', '', '', '' },
        [FX .. '.elight.enabled'] = { '0', '', '', '' },
        [FX .. '.refl'] = { '', '', '', '0.5' },
        [FX .. '.refl_quality'] = { '', '', '', '3' },
        [FX .. '.filter'] = { '0', '', '', '' },
        [FX .. '.aniso'] = { '2', '8', '', '' },
        ['graphics.drawdistance'] = { '1', '3', '', '8' },
        ['graphics.drawdistance.mobs'] = { '1', '3', '', '8' },
        ['graphics.antialias'] = { '', '', '', 'fxaa' },
        ['graphics.glow.quality'] = { '0', '', '', '3' },
    },
}

-- a key's section and name: 'graphics.effects.sun_detail' is section graphics.effects, key sun_detail
local function split_key(full)
    local section, key = full:match('^(graphics%.effects)%.(.+)$')
    if section == nil then
        section, key = full:match('^([^.]+)%.(.+)$')
    end
    return section, key
end

-- the rows by 'section.key', from every page (for the presets' defaults and parsing)
local function rows_by_key()
    local map = {}
    for _, page in ipairs(PAGES) do
        for _, row in ipairs(page_rows(page)) do
            if row.section ~= nil and row.key ~= nil then
                map[row.section .. '.' .. row.key] = row
            end
        end
    end
    return map
end

-- a value as vanacore.ini's text, normalised through its row (so an unset key and one set to its default compare equal)
local function normal_text(row, text)
    if row == nil then
        return text or ''
    end
    -- read() goes through vana.config.get: for a moment, one that answers with `text` for this row's key
    local saved = vana.config.get
    local swapped = pcall(function ()
        vana.config.get = function (section, key)
            if section == row.section and key == row.key then
                return text
            end
            return saved(section, key)
        end
    end)
    local ok, got = pcall(M.read, row)
    if swapped then
        pcall(function () vana.config.get = saved end)
    end
    local value = ok and got or row.default
    return tostring(M.format_value(row, value))
end

local function quality_now()
    local rows = rows_by_key()
    for i = 1, #QUALITY.names do
        local all = true
        for full, levels in pairs(QUALITY.values) do
            local section, key = split_key(full)
            local now = vana.config.get(section, key)
            local want = levels[i]
            if key:match('%.enabled$') then
                -- an effect's switch: on unless 0
                if ((tonumber(now or '') or 1) ~= 0) ~= ((tonumber(want ~= '' and want or '') or 1) ~= 0) then
                    all = false
                end
            else
                local row = rows[full]
                if normal_text(row, now) ~= normal_text(row, want ~= '' and want or nil) then
                    all = false
                end
            end
            if not all then
                break
            end
        end
        if all then
            return i
        end
    end
    return nil
end

local function apply_quality(i)
    M.flush()
    for full, levels in pairs(QUALITY.values) do
        local section, key = split_key(full)
        local value = levels[i]
        vana.config.set(section, key, value ~= '' and value or nil)
    end
    vana.graphics.reload()
    M.rebuild()
end

-------------------------------------------------------------------------------
-- The user's presets, two kinds, each its own list: the Graphics and Experimental settings under a name, in
-- vanacore.presets.ini ([vana] preset the one loaded), and the HUD's and the interface's (Game Options' but what was
-- Options: `without`), in vanacore.hudpresets.ini ([vana] hud.preset). A section each: saved=<when>, then section.key=value for every setting of its pages, as vanacore.ini
-- has it, a setting left at its default written out as that default (so loading or importing one sets what it says,
-- whatever the other install has); a setting no row can give a default for (a colour picked or not) is left out when unset.
-- The kind is the page shown's (use_presets); what's below works on st.presets, the kind's list.
-------------------------------------------------------------------------------

local PRESET_KINDS = {
    graphics = { pages = { graphics = true, experimental = true }, file = 'vanacore.presets.ini', key = 'preset', state = st.presets,
                 ext = 'gfx', note = 'Named sets of the Graphics and Experimental settings.', what = 'Graphics and Experimental', where = 'Graphics' },
    hud = { pages = { hud = true, options = true }, without = 'Options', file = 'vanacore.hudpresets.ini', key = 'hud.preset', ext = 'hud',
            state = { list = {}, read = false, name = { '' } }, note = 'Named sets of the HUD and interface settings.',
            what = 'HUD and interface', where = 'HUD' },
}
local preset_kind = PRESET_KINDS.graphics
local PRESET_PAGES = preset_kind.pages

-- the presets of `kind` made the ones the rest works on (each kind keeps its own list and the one loaded)
local function use_kind(kind)
    if kind == preset_kind then
        return
    end
    preset_kind.loaded, preset_kind.loaded_read = st.loaded_preset, st.preset_read
    preset_kind = kind
    st.presets, PRESET_PAGES = kind.state, kind.pages
    st.loaded_preset, st.preset_read = kind.loaded, kind.loaded_read
end

-- ... `page`'s kind: the HUD's and Game Options', else Graphics'
local function use_presets(page)
    use_kind((page ~= nil and (page.id == 'hud' or page.id == 'options')) and PRESET_KINDS.hud or PRESET_KINDS.graphics)
end
-- not a preset's: what's this machine's or this install's
local PRESET_SKIP = { ['graphics.renderer'] = true, ['ffxi.gamepad.guid'] = true, ['window.position'] = true, ['mcp.enabled'] = true,
                      ['mcp.port'] = true, ['client.ini.upstream'] = true, ['vana.scale'] = true,
                      ['vana.hud.autosave'] = true, ['vana.tile.x'] = true, ['vana.tile.y'] = true }
-- still a preset's, on a page its kind doesn't hold: settings moved there from one it does (none now: Game Options'
-- nameplates are the HUD presets')
local PRESET_MOVED = {}

local function preset_moved(page_id, full)
    for _, prefix in ipairs(PRESET_MOVED[page_id] or {}) do
        if full:sub(1, #prefix) == prefix then
            return true
        end
    end
    return false
end

local function presets_file()
    return vana.paths.install .. '\\' .. preset_kind.file
end

local function read_presets()
    local presets = st.presets
    presets.read = true
    presets.list = {}
    local file = io.open(presets_file(), 'rb')
    if file == nil then
        return
    end
    local entry = nil
    for raw in file:lines() do
        local line = raw:gsub('\r$', '')
        local name = line:match('^%[(.-)%]%s*$')
        if name ~= nil then
            entry = { name = name, saved = '', values = {} }
            presets.list[#presets.list + 1] = entry
        elseif entry ~= nil and not line:match('^%s*;') then
            local key, value = line:match('^([^=]-)%s*=%s*(.-)%s*$')
            if key ~= nil and key ~= '' then
                if key == 'saved' then
                    entry.saved = value
                else
                    entry.values[key] = value
                end
            end
        end
    end
    file:close()
end

-- the preset the account has loaded: [vana] preset in its settings file, kept across sessions. Nil if none, or if the
-- preset has been deleted since.
local function current_preset()
    if st.loaded_preset == nil and not st.preset_read then
        st.preset_read = true -- once: the setting file isn't read every frame
        local name = vana.config.get('vana', preset_kind.key)
        if name ~= nil and name ~= '' then
            for _, entry in ipairs(st.presets.list) do
                if entry.name:lower() == name:lower() then
                    st.loaded_preset = entry.name
                end
            end
        end
    end
    return st.loaded_preset
end

local function set_current_preset(name)
    st.loaded_preset = name
    vana.config.set('vana', preset_kind.key, name) -- nil clears it
end

local function write_presets()
    local path = presets_file()
    local file, why = io.open(path, 'wb')
    if file == nil then
        print('VanaCore: can\'t write ' .. path .. ': ' .. tostring(why))
        return false
    end
    file:write('; VanaCore presets, saved by the /vana window: a section each, every setting as vanacore.ini has it (defaults too).\n')
    for _, entry in ipairs(st.presets.list) do
        file:write('\n[' .. entry.name .. ']\n')
        file:write('saved=' .. entry.saved .. '\n')
        local keys = {}
        for key in pairs(entry.values) do
            keys[#keys + 1] = key
        end
        table.sort(keys)
        for _, key in ipairs(keys) do
            file:write(key .. '=' .. entry.values[key] .. '\n')
        end
    end
    file:close()
    return true
end

-- a preset's name: letters, digits, spaces, - and _ (it names the file an export writes)
local function clean_name(name)
    name = name:gsub('[^%w _%-]', '')
    name = name:gsub('^%s+', '')
    name = name:gsub('%s+$', '')
    return name
end

-- `rows` with the switch row of each asset row beside it (merged into one on the page)
local function with_assets(rows)
    local out = {}
    for _, row in ipairs(rows) do
        out[#out + 1] = row
        if row.asset ~= nil then
            out[#out + 1] = row.asset
        end
    end
    return out
end

-- the keys a preset holds: every setting of its pages that resets, but PRESET_SKIP's and the kind's `without` tab's (`all`:
-- that tab's too, for Characters' copies); a Graphics panel shown on another page (Game Options' Mass Grass) is the page
-- its row.preset_page names
local function preset_keys(all)
    local keys, seen = {}, {}
    for _, page in ipairs(PAGES) do
        if PRESET_PAGES[page.id] or PRESET_MOVED[page.id] or PANEL_HOSTS[page.id] then
            for _, whole in ipairs(with_assets(page_rows(page))) do
                local home = whole.preset_page or page.id
                for _, row in ipairs(whole.parts or { whole }) do -- (a chat window's place: its sliders and switch)
                    if M.resettable(row) and row.section ~= 'layout' and row.section ~= 'gamepads'
                       and (all or whole.old_tab == nil or whole.old_tab ~= preset_kind.without) then
                        local full = row.section .. '.' .. row.key
                        if (PRESET_PAGES[home] or preset_moved(page.id, full)) and not PRESET_SKIP[full] and not seen[full] then
                            seen[full] = true
                            keys[#keys + 1] = { section = row.section, key = row.key, effect = row.effect, row = row, page = home }
                        end
                    end
                end
            end
        end
    end
    return keys
end

-- Client > Characters' copies (copy.lua): the settings each sends. 'graphics': a Graphics preset's (the Graphics and
-- Experimental pages, Mass Grass) and Client > Game's renderer, background resolution, frame rate and picture rows (not the
-- window's mode, resolution or place: the Alt's own). 'options': a HUD preset's and the rest of Game Options (the world,
-- camera, trusts, zone lines and damage numbers rows a HUD preset leaves out). With each, the loaded preset's name.
-- { { section, key, restart } }, an effect's switch (<key>.enabled) after its strength.
local COPY_EXTRA = {
    graphics = { { 'graphics', 'renderer', true }, { 'graphics', 'background', true }, { 'client', 'fps.max' },
                 { 'client', 'fps.cutscene' }, { 'client', 'fps.background' }, { 'graphics', 'vsync' }, { 'graphics', 'vsync.cutscene' },
                 { 'graphics', 'antialias' }, { 'graphics', 'aspect' }, { 'vana', 'preset' } },
    options = { { 'vana', 'hud.preset' } },
}
function M.copy_keys(kind)
    local was = preset_kind
    use_kind(kind == 'graphics' and PRESET_KINDS.graphics or PRESET_KINDS.hud)
    local ok, keys = pcall(preset_keys, true)
    use_kind(was)
    if not ok then
        error(keys, 0)
    end
    local out, seen = {}, {}
    local function add(section, key, restart)
        local full = section .. '.' .. key
        if not seen[full] then
            seen[full] = true
            out[#out + 1] = { section = section, key = key, restart = restart == true }
        end
    end
    for _, k in ipairs(keys) do
        add(k.section, k.key, k.row ~= nil and k.row.restart)
        if k.effect then
            add(k.section, k.key .. '.enabled')
        end
    end
    for _, k in ipairs(COPY_EXTRA[kind] or {}) do
        add(k[1], k[2], k[3])
    end
    return out
end

-- [vana] preset and hud.preset read again (a copy from the Main changed them), and the presets' lists with them
function M.presets_reread()
    for _, kind in pairs(PRESET_KINDS) do
        kind.loaded, kind.loaded_read = nil, false
        kind.state.read = false
    end
    st.loaded_preset, st.preset_read = nil, false
    st.presets.read = false
end

-- what a setting left unset runs with, as vanacore.ini's text; nil where its row can't say (kept elsewhere than the ini,
-- or a colour: unset is "none picked" there, which its default colour isn't)
local function default_text(row)
    if row == nil or row.get ~= nil or row.default == nil or row.kind == 'colour' then
        return nil
    end
    local ok, text = pcall(M.format_value, row, row.default)
    if ok and text ~= nil and text ~= '' then
        return tostring(text)
    end
    return nil
end

-- the settings a preset of the kind holds, as they are now: every one, a setting left unset as its default
local function preset_values()
    local values = {}
    for _, k in ipairs(preset_keys()) do
        local text = vana.config.get(k.section, k.key)
        if text == nil or text == '' then
            text = default_text(k.row)
        end
        if text ~= nil and text ~= '' then
            values[k.section .. '.' .. k.key] = text
        end
        if k.effect then
            local on = vana.config.get(k.section, k.key .. '.enabled')
            if on == nil or on == '' then
                on = '1' -- an effect's switch is on unless it says 0
            end
            values[k.section .. '.' .. k.key .. '.enabled'] = on
        end
    end
    return values
end

local function save_preset(name)
    name = clean_name(name)
    if name == '' then
        return
    end
    M.flush()
    local values = preset_values()
    local entry = nil
    for _, e in ipairs(st.presets.list) do
        if e.name:lower() == name:lower() then
            entry = e
        end
    end
    if entry == nil then
        entry = { name = name }
        st.presets.list[#st.presets.list + 1] = entry
    end
    entry.saved = os.date('%Y-%m-%d %H:%M')
    entry.values = values
    if write_presets() then
        st.flash = { name = entry.name, what = 'saved', at = clock() }
    end
end

-- a preset's settings written: every key it names, and every other key of its pages back to the default; except the
-- Experimental page's when the preset holds none of them (one saved before that page was part of a preset): those stay as they are
local function load_preset(entry)
    M.flush()
    local keys = preset_keys()
    local has_experimental = false
    for _, k in ipairs(keys) do
        local full = k.section .. '.' .. k.key
        if k.page == 'experimental' and (entry.values[full] ~= nil or entry.values[full .. '.enabled'] ~= nil) then
            has_experimental = true
            break
        end
    end
    for _, k in ipairs(keys) do
        if k.page ~= 'experimental' or has_experimental then
            local full = k.section .. '.' .. k.key
            vana.config.set(k.section, k.key, entry.values[full])
            if k.effect then
                vana.config.set(k.section, k.key .. '.enabled', entry.values[full .. '.enabled'])
            end
        end
    end
    vana.graphics.reload()
    M.rebuild()
    set_current_preset(entry.name)
    st.flash = { name = entry.name, what = 'loaded', at = clock() }
end

-- Vanilla UI (the HUD presets' save row): every setting a HUD preset holds back to its default, the game's own look: the
-- HUD Manager's windows' places, sizes, frames, Hide and Solid buttons switches, and Game Options' interface (the windows'
-- and buttons' looks, True Font, chat, menus, items, nameplates...); but the HUD Manager's own ([vana]: the grid, snap,
-- anchors). Until 2026-11-06 only the places, a few sizes and the Hide switches, so a flat button style or a hidden
-- frame stayed. No HUD preset is loaded after, so Auto save leaves the one that was as it is.
-- True Font off too: each area's switch at the end of its font line ([client] menu.font, buttons.font, chat.font... sharp or
-- game) taken out, which is off (the game's letters; chatfont.cpp read_want), on every page a HUD preset reads, Options'
-- rows too. A line's own reset and a preset leave a switch alone, so Vanilla UI kept True Font on (2026-11-10). Not
-- the nameplates' or the damage numbers' ([graphics] nameplates.font, damage.font): theirs stay as they are.
local function vanilla_ui()
    M.flush()
    use_kind(PRESET_KINDS.hud) -- (its button is on the HUD presets' row: already the kind)
    for _, k in ipairs(preset_keys()) do
        if k.section ~= 'vana' then
            vana.config.set(k.section, k.key, nil)
            if k.effect then
                vana.config.set(k.section, k.key .. '.enabled', nil)
            end
        end
    end
    local switched_off = {}
    for _, page in ipairs(PAGES) do
        if PRESET_PAGES[page.id] then
            for _, whole in ipairs(page_rows(page)) do
                for _, row in ipairs({ whole, unpack(whole.parts or {}) }) do
                    local switch = row.switch
                    if switch ~= nil and switch.section == 'client' and switch.key ~= nil and not switched_off[switch.key] then
                        switched_off[switch.key] = true
                        vana.config.set('client', switch.key, nil)
                    end
                end
            end
        end
    end
    vana.graphics.reload() -- (the nameplates' settings are [graphics])
    M.rebuild()
    set_current_preset(nil)
    st.flash = { name = '', what = 'vanilla', at = clock() }
end

local function delete_preset(entry)
    for i, e in ipairs(st.presets.list) do
        if e == entry then
            table.remove(st.presets.list, i)
            break
        end
    end
    if st.loaded_preset == entry.name then
        set_current_preset(nil)
    end
    write_presets()
end

-- Export and import: a preset as <install>\presets\<name>.json ({ "name", "saved", "settings": { "section.key": "value" } }),
-- the folder opened after an export; Import picks a file with Windows' own dialog (comdlg32, through FFI).
local PRESETS_DIR = vana.paths.install .. '\\presets'
local ffi_ok, ffi = pcall(require, 'ffi')
if ffi_ok then
    pcall(ffi.cdef, [[
        typedef struct {
            unsigned long lStructSize; void* hwndOwner; void* hInstance; const char* lpstrFilter; char* lpstrCustomFilter;
            unsigned long nMaxCustFilter; unsigned long nFilterIndex; char* lpstrFile; unsigned long nMaxFile;
            char* lpstrFileTitle; unsigned long nMaxFileTitle; const char* lpstrInitialDir; const char* lpstrTitle;
            unsigned long Flags; unsigned short nFileOffset; unsigned short nFileExtension; const char* lpstrDefExt;
            long lCustData; void* lpfnHook; const char* lpTemplateName; void* pvReserved; unsigned long dwReserved;
            unsigned long FlagsEx;
        } VANA_OPENFILENAMEA;
        int __stdcall GetOpenFileNameA(VANA_OPENFILENAMEA* ofn);
        void* __stdcall ShellExecuteA(void* hwnd, const char* op, const char* file, const char* params, const char* dir, int show);
        void* __stdcall GetForegroundWindow(void);
    ]])
end

local function json_string(text)
    text = tostring(text):gsub('[%c"\\]', function (c)
        if c == '"' then return '\\"' elseif c == '\\' then return '\\\\' elseif c == '\n' then return '\\n'
        elseif c == '\r' then return '\\r' elseif c == '\t' then return '\\t' end
        return string.format('\\u%04x', c:byte())
    end)
    return '"' .. text .. '"'
end

-- a small JSON reader: objects, arrays, strings, numbers, true, false, null; returns the value, or nil and why
local function json_decode(text)
    local pos = 1
    local function skip()
        pos = text:find('[^%s]', pos) or #text + 1
    end
    local value
    local function fail(why)
        error({ why = why .. ' at ' .. pos }, 0)
    end
    local function read_string()
        pos = pos + 1
        local out = {}
        while true do
            local c = text:sub(pos, pos)
            if c == '' then fail('an unfinished string') end
            if c == '"' then pos = pos + 1 break end
            if c == '\\' then
                local e = text:sub(pos + 1, pos + 1)
                local map = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }
                if e == 'u' then
                    local hex = text:sub(pos + 2, pos + 5)
                    local cp = tonumber(hex, 16) or 63
                    out[#out + 1] = cp < 128 and string.char(cp) or '?'
                    pos = pos + 6
                else
                    out[#out + 1] = map[e] or e
                    pos = pos + 2
                end
            else
                out[#out + 1] = c
                pos = pos + 1
            end
        end
        return table.concat(out)
    end
    value = function ()
        skip()
        local c = text:sub(pos, pos)
        if c == '{' then
            pos = pos + 1
            local object = {}
            skip()
            if text:sub(pos, pos) == '}' then pos = pos + 1 return object end
            while true do
                skip()
                if text:sub(pos, pos) ~= '"' then fail('a key expected') end
                local key = read_string()
                skip()
                if text:sub(pos, pos) ~= ':' then fail('a colon expected') end
                pos = pos + 1
                object[key] = value()
                skip()
                local d = text:sub(pos, pos)
                pos = pos + 1
                if d == '}' then return object end
                if d ~= ',' then fail('a comma expected') end
            end
        elseif c == '[' then
            pos = pos + 1
            local list = {}
            skip()
            if text:sub(pos, pos) == ']' then pos = pos + 1 return list end
            while true do
                list[#list + 1] = value()
                skip()
                local d = text:sub(pos, pos)
                pos = pos + 1
                if d == ']' then return list end
                if d ~= ',' then fail('a comma expected') end
            end
        elseif c == '"' then
            return read_string()
        elseif text:sub(pos, pos + 3) == 'true' then
            pos = pos + 4
            return true
        elseif text:sub(pos, pos + 4) == 'false' then
            pos = pos + 5
            return false
        elseif text:sub(pos, pos + 3) == 'null' then
            pos = pos + 4
            return nil
        else
            local number = text:match('^-?%d+%.?%d*[eE]?[-+]?%d*', pos)
            if number == nil or number == '' then fail('a value expected') end
            pos = pos + #number
            return tonumber(number)
        end
    end
    local ok, result = pcall(value)
    if not ok then
        return nil, type(result) == 'table' and result.why or tostring(result)
    end
    return result
end

local function open_folder(path)
    if not ffi_ok then
        return
    end
    pcall(function ()
        local shell32 = ffi.load('shell32')
        shell32.ShellExecuteA(nil, 'open', path, nil, nil, 1)
    end)
end

-- Addons' windows as they're placed now, for a preset to carry with the screen they're placed on ({ w, h }): every one that's
-- showing (moved or scaled or not) and those config\layout.json has, not this window's own. nil, nothing, when VanaCore can't say.
local function layout_now()
    if vana.layout == nil then
        return nil
    end
    local sw, sh = vana.ui.screen()
    if not sw or sw <= 0 or not sh or sh <= 0 then
        return nil
    end
    local windows = {}
    for _, w in ipairs(vana.layout.windows()) do
        local own = w.owner == 'vanacore/vana' or w.name:sub(-7) == '###vana'
        if not own and (w.shown or (w.saved and not w.pinned)) then -- (a window its addon places, not drawn now: its place is stale)
            windows[#windows + 1] = { name = w.name, x = w.x, y = w.y, w = w.w, h = w.h, scale = w.scale }
        end
    end
    return { sw, sh }, windows
end
M.layout_now = layout_now -- (Characters' Copy Addons/Plugins sends them: copy.lua)

-- what an export carries: the preset, and with `windows` the addons' windows as they are now (if any have been placed)
local function share_data(entry, windows)
    local data = { name = entry.name, saved = entry.saved, values = entry.values }
    if windows then
        local screen, list = layout_now()
        if list ~= nil and #list > 0 then
            data.screen, data.windows = screen, list
        end
    end
    return data
end

-- the JSON of an export: { name, saved, screen: [w, h], windows: [ { name, x, y, w, h, scale } ], settings: { key: value } }
local function json_text(data)
    local num = M.share.num
    local out = { '{\n  "name": ' .. json_string(data.name) .. ',\n  "saved": ' .. json_string(data.saved) }
    if data.windows ~= nil then
        out[#out + 1] = ',\n  "screen": [' .. num(data.screen[1]) .. ', ' .. num(data.screen[2]) .. '],\n  "windows": ['
        for i, w in ipairs(data.windows) do
            out[#out + 1] = (i > 1 and ',' or '') .. '\n    { "name": ' .. json_string(w.name) .. ', "x": ' .. num(w.x) .. ', "y": ' .. num(w.y)
                            .. ', "w": ' .. num(w.w) .. ', "h": ' .. num(w.h) .. ', "scale": ' .. num(w.scale) .. ' }'
        end
        out[#out + 1] = '\n  ]'
    end
    out[#out + 1] = ',\n  "settings": {\n'
    local keys = {}
    for key in pairs(data.values) do
        keys[#keys + 1] = key
    end
    table.sort(keys)
    for i, key in ipairs(keys) do
        out[#out + 1] = '    ' .. json_string(key) .. ': ' .. json_string(data.values[key]) .. (i < #keys and ',' or '') .. '\n'
    end
    out[#out + 1] = '  }\n}\n'
    return table.concat(out)
end

-- a preset as <install>\presets\<name>.<ext>.json (ext: gfx or hud; with `windows`, the addons' windows too), the folder opened
local function export_preset(entry, windows, ext)
    vana.fs.create_dir(PRESETS_DIR)
    local path = PRESETS_DIR .. '\\' .. entry.name .. '.' .. ext .. '.json'
    local file, why = io.open(path, 'wb')
    if file == nil then
        print('VanaCore: can\'t write ' .. path .. ': ' .. tostring(why))
        return false
    end
    file:write(json_text(share_data(entry, windows)))
    file:close()
    st.flash = { name = entry.name, what = 'exported', at = clock() }
    open_folder(PRESETS_DIR)
    return true
end

-- vana.window.block_mouse (VanaCore 0.8.x and later): no mouse button reaches the game. on: for `ms` more (the window
-- asks again every frame it's drawn); off: half a second more, then every button the game holds is let go.
local function block_game_mouse(on, ms)
    local window = vana.window
    if window ~= nil and window.block_mouse ~= nil then
        window.block_mouse(on, ms or 400)
    end
end

-- Windows' Open dialog: the path picked, or nil. The game's thread waits in it (the frame goes on when it closes).
local function pick_file()
    if not ffi_ok then
        print('VanaCore: no file dialog here (no FFI).')
        return nil
    end
    local ok, result = pcall(function ()
        vana.fs.create_dir(PRESETS_DIR)
        local comdlg32, user32 = ffi.load('comdlg32'), ffi.load('user32')
        local keep = { 'VanaCore preset (*.json)\0*.json\0All files (*.*)\0*.*\0\0', PRESETS_DIR, 'Import a VanaCore preset', 'json' }
        local buffer = ffi.new('char[?]', 2048)
        local ofn = ffi.new('VANA_OPENFILENAMEA')
        ofn.lStructSize = ffi.sizeof(ofn)
        ofn.hwndOwner = user32.GetForegroundWindow()
        ofn.lpstrFilter = keep[1]
        ofn.nFilterIndex = 1
        ofn.lpstrFile = buffer
        ofn.nMaxFile = 2048
        ofn.lpstrInitialDir = keep[2]
        ofn.lpstrTitle = keep[3]
        ofn.lpstrDefExt = keep[4]
        ofn.Flags = 0x1000 + 0x800 + 0x4 + 0x8 -- FILEMUSTEXIST, PATHMUSTEXIST, HIDEREADONLY, NOCHANGEDIR
        block_game_mouse(true, 600000) -- the dialog holds the game's thread: no frame renews it
        local picked = comdlg32.GetOpenFileNameA(ofn) ~= 0
        block_game_mouse(true, 400)
        local path = picked and ffi.string(buffer) or nil
        keep = nil
        return path
    end)
    if not ok then
        print('VanaCore: the file dialog failed: ' .. tostring(result))
        return nil
    end
    return result
end

-- a file's text, or nil (and why in the log)
local function read_text(path)
    local file, why = io.open(path, 'rb')
    if file == nil then
        print('VanaCore: can\'t read ' .. path .. ': ' .. tostring(why))
        return nil
    end
    local text = file:read('*a')
    file:close()
    return text
end

-- A preset in text: the string Export shows (share.lua's) or the .json it writes ({ name, saved, settings }, or a flat object
-- of settings; "screen" and "windows" when it carries the addons' windows). { name, saved, values, screen, windows } and how
-- many settings, or nil and what's wrong (nil, nil for nothing yet). `fallback`: the name for one that gives none.
local function parse_preset(text, fallback)
    text = (text or ''):gsub('^%s+', ''):gsub('%s+$', '')
    if text == '' then
        return nil, nil
    end
    local data
    if text:sub(1, 1) == '{' then
        local object, problem = json_decode(text)
        if type(object) ~= 'table' then
            return nil, 'That JSON can\'t be read: ' .. tostring(problem or 'not an object') .. '.'
        end
        local settings = type(object.settings) == 'table' and object.settings or object
        data = { name = type(object.name) == 'string' and object.name or fallback or '', saved = type(object.saved) == 'string' and object.saved or '', values = {} }
        for key, value in pairs(settings) do
            if type(key) == 'string' and key:find('.', 1, true) and (type(value) == 'string' or type(value) == 'number' or type(value) == 'boolean') then
                data.values[key] = type(value) == 'boolean' and (value and '1' or '0') or tostring(value)
            end
        end
        local screen = object.screen
        if type(screen) == 'table' and tonumber(screen[1]) and tonumber(screen[2]) and type(object.windows) == 'table' then
            local windows = {}
            for _, w in ipairs(object.windows) do
                if type(w) == 'table' and type(w.name) == 'string' and w.name ~= '' and tonumber(w.x) and tonumber(w.y) then
                    windows[#windows + 1] = { name = w.name, x = tonumber(w.x), y = tonumber(w.y), w = tonumber(w.w) or 0, h = tonumber(w.h) or 0,
                                              scale = tonumber(w.scale) or 1 }
                end
            end
            if #windows > 0 then
                data.screen, data.windows = { tonumber(screen[1]), tonumber(screen[2]) }, windows
            end
        end
    else
        local why
        data, why = M.share.unpack(text)
        if data == nil then
            return nil, why
        end
    end
    local n = 0
    for _ in pairs(data.values) do
        n = n + 1
    end
    if n == 0 and data.windows == nil then
        return nil, 'That preset holds no settings.'
    end
    data.name = clean_name(data.name)
    if data.name == '' then
        data.name = clean_name(fallback or '') ~= '' and clean_name(fallback) or 'Imported'
    end
    if data.saved == '' then
        data.saved = os.date('%Y-%m-%d %H:%M')
    end
    return data, nil, n
end

local function preset_named(name)
    for _, e in ipairs(st.presets.list) do
        if e.name:lower() == name:lower() then
            return e
        end
    end
    return nil
end

-- a parsed preset kept with the user's (one of the same name is replaced)
local function store_preset(data)
    local entry = preset_named(data.name)
    if entry == nil then
        entry = { name = data.name }
        st.presets.list[#st.presets.list + 1] = entry
    end
    entry.saved = data.saved
    entry.values = data.values
    if write_presets() then
        st.flash = { name = entry.name, what = 'imported', at = clock() }
    end
    return entry
end

-- A parsed preset's windows placed on this screen (share.fit: the same places in proportion to the screen). config\layout.json is
-- copied to config\layout-before-import-<date>-<time>.json first, for the layout before to have back. Returns how many were
-- placed, the factor their scales were multiplied by and the copy's name; or nil and why.
local function place_windows(data)
    if vana.layout == nil then
        return nil, 'This VanaCore can\'t move addons\' windows: update it.'
    end
    local sw, sh = vana.ui.screen()
    if not sw or sw <= 0 or not sh or sh <= 0 then
        return nil, 'The game\'s resolution isn\'t known yet.'
    end
    local dir = vana.paths.install .. '\\config\\'
    local kept = nil
    local from = io.open(dir .. 'layout.json', 'rb')
    if from ~= nil then
        local text = from:read('*a')
        from:close()
        local name = 'layout-before-import-' .. os.date('%Y-%m-%d-%H%M%S') .. '.json'
        local to = io.open(dir .. name, 'wb')
        if to ~= nil then
            to:write(text)
            to:close()
            kept = name
        end
    end
    local placed, factor = M.share.fit(data.windows, data.screen[1], data.screen[2], sw, sh)
    for _, p in ipairs(placed) do
        vana.layout.set(p.name, p.x, p.y, p.scale)
    end
    return #placed, factor, kept
end

-------------------------------------------------------------------------------
-- Rows
-------------------------------------------------------------------------------

local ROW_H = 27
local CONTROLS_H = 70 -- a controls row's panel (the HUD's Settings: what the mouse does to the boxes)
local FILTER_GAP = 8 -- above the Addons and Plugins pages' search row
local PAD = 18     -- the sidebar's margin, and the page's title line's
local LEFT = 12    -- the page's margin on the left: the names and the body
local RIGHT = 9    -- the page's margin on the right
local PAD_ROW = 40 -- a gamepad's line: its name, its id under it
local LAYOUT_TITLE_W = 96 -- the Layout page's window names, before their X, Y and Scale
-- Client > Characters (draw_transfer): the Main's and the Alt's cards, the arrows down between them and the copies' buttons
local TRANSFER_CARD, TRANSFER_ARROW, TRANSFER_BUTTON = 46, 24, 28
local TRANSFER_H = 4 + TRANSFER_CARD * 2 + TRANSFER_ARROW * 2 + TRANSFER_BUTTON + 6
-- Graphics > Toggles: a chip a layer, three to a line (LAYER_GAP between them)
local LAYER_H, LAYER_COLS, LAYER_GAP = 22, 3, 4
-- Client > Boot.txt (textfile.draw): the box's height and the line under it with Save; one table, its functions with it
-- (this chunk is near LuaJIT's 200 locals)
local textfile = { BOX = 380, BAR = 40, BOM = '\239\187\191' }
-- the GUID in a gamepad status row's value: that row is drawn as a pad
local function pad_guid(text)
    return text:match('^(%b{})')
end

-- `text` broken at word ends into lines that fit `room` pixels, kept on the row until the room changes
local NOTE_LINE = 20
local function note_lines(row, room)
    if row.wrap_room == room and row.wrap_lines ~= nil then
        return row.wrap_lines
    end
    local lines, line = {}, ''
    for word in tostring(row.text):gmatch('%S+') do
        local try = line == '' and word or line .. ' ' .. word
        if line ~= '' and M.measure(try, fonts.text, BODY - 1) > room then
            lines[#lines + 1] = line
            line = word
        else
            line = try
        end
    end
    lines[#lines + 1] = line
    row.wrap_room, row.wrap_lines = room, lines
    return lines
end

-- the room a page's notes wrap to: its width less the margins and the icon's column, the same for the height and the draw
local function note_width(w) return w - S(LEFT) - S(RIGHT) - S(20) end
-- THE BANNER STANDARD. Every notice on a page is this one banner (kinds warning, callout, alert, notice, hint; data.lua's
-- callout(title, text, tone) is the one to use): the rows' own width (lx to right), its tone's colour fading left to right
-- over the panel with a faint edge of it, no bar, no icon; a title in capitals in the tone's colour, the text wrapped under
-- it close up. Five tones (row.tone): 'warning' orange, 'success' green, 'basic' grey, 'notice' blue, 'danger' red. Don't
-- draw a box of any other style for a notice
local BANNER_KINDS = { warning = true, callout = true, alert = true, notice = true, hint = true }
local BANNER_TONES = { warning = 'amber', success = 'green', basic = 'dim', notice = 'accent', danger = 'rose' }
local BANNER_OLD = { warn = 'warning', muted = 'basic', grey = 'basic' } -- the tones' old names
local function banner_tone(row)
    local t = BANNER_OLD[row.tone] or row.tone
    if BANNER_TONES[t] then
        return t
    end
    return (row.kind == 'warning' or row.kind == 'alert') and 'danger' or row.kind == 'hint' and 'basic'
        or row.kind == 'notice' and 'notice' or 'warning'
end
local function banner_height(row, room)
    return math.max(1, #note_lines(row, room)) * S(NOTE_LINE) + (row.title and S(NOTE_LINE - 5) or 0) + S(18)
end

-- The notes without an icon that still show: what a page has or hasn't got right now, and a limit or warning that's true
-- now, not an explanation (those are the rows' tooltips). By how they start.
local SHOWN_NOTES = {
    "^This VanaCore can't", '^Nothing matches', '^None ', '^Every ', '^No addon or plugin is drawing', '^No overlays in',
    '^Top to bottom: the first', '^This is a resource intensive',
}

local function shown_note(row)
    if row.glyph ~= nil then
        return true
    end
    local text = tostring(row.text or '')
    for _, pattern in ipairs(SHOWN_NOTES) do
        if text:find(pattern) then
            return true
        end
    end
    return false
end

-- the room above a sub-panel's heading: less when it's the first row of its section (right under the section's title)
local function sub_room(row, sec)
    return (sec ~= nil and sec.rows[1] == row) and S(2) or S(16)
end

local function row_height(row, sec, room)
    if row.show_if ~= nil then -- a row shown only while show_if says so (looked at each frame)
        local ok, shown = pcall(row.show_if)
        if not (ok and shown) then
            return 0
        end
    end
    -- a tab bar (kind 'tabs') and its rows (row.tab_of): only the picked tab's rows; while searching, the bar left out and
    -- every tab's rows found shown, each tab's under its name (search_results)
    local searching = st.search[1] ~= ''
    if row.tab_of ~= nil and not searching and (st.tabs[row.tab_of.id] or 1) ~= row.tab_of.index then
        return 0
    elseif row.kind == 'tabs' then
        return searching and 0 or S(M.TABS_H) + S(10)
    end
    if row.kind == 'layers' then
        local lines = math.ceil(#row.items / LAYER_COLS)
        return S(ROW_H) + lines * S(LAYER_H) + math.max(0, lines - 1) * S(LAYER_GAP) + S(8)
    elseif row.kind == 'banner' then
        return S(M.BANNER_H) + S(12)
    elseif row.kind == 'transfer' then
        return S(TRANSFER_H)
    elseif row.kind == 'textfile' then
        return S(4) + S(textfile.BOX) + S(textfile.BAR)
    elseif row.kind == 'slider' and row.marks ~= nil then
        return S(ROW_H) + S(12)
    elseif row.kind == 'status' and sec ~= nil and sec.title == 'Gamepads' and pad_guid(live_value(row).text) then
        return S(PAD_ROW)
    elseif row.kind == 'note' and not shown_note(row) then
        return 0 -- explanations are the tooltips' (an iconed note stays, and the short state lines SHOWN_NOTES names)
    elseif BANNER_KINDS[row.kind] then
        return banner_height(row, room)
    elseif row.kind == 'note' then
        return math.max(1, #note_lines(row, room)) * S(NOTE_LINE) + S(7)
    elseif row.kind == 'controls' then
        return S(CONTROLS_H) + S(14)
    elseif row.kind == 'action' and row.big then
        return S(ROW_H) + S(14) -- a larger button alone on its line, in the middle (draw_row)
    elseif row.kind == 'action' and (row.action == 'gamepad_rescan' or row.action == 'reset_all') then
        return 0 -- the icon beside the Gamepad choice; no reset of everything here
    elseif row.kind == 'subheader' and row.headline then
        return S(8 + 36 + 2) + (st.hybrid and S(M.hy.TITLE_PAD) or 0) -- drawn as a section's heading, HEAD_H high (M.draw_headline); the hybrid layout's with room under
    elseif row.kind == 'subheader' and row.text == 'Reset' then
        return 0 -- the heading of that reset
    elseif row.kind == 'subheader' then
        return S(ROW_H) + sub_room(row, sec) + S(8) -- room above it (sub_room) and below it (8): draw_row
    elseif row.kind == 'extension_filter' then
        return S(ROW_H) + S(FILTER_GAP) -- room above it, at the top of the page
    elseif row.kind == 'columns' then
        -- (draw_row's columns: a title line, then each slider's name over its track, the longest column's)
        local n = 0
        for _, col in ipairs(row.columns) do
            n = math.max(n, #col.rows)
        end
        return S(24) + n * (S(20) + S(ROW_H)) + S(2)
    end
    return S(ROW_H)
end

-- how a row is drawn and whether it takes clicks: `enabled` from its section's switches and locks, faded (needs
-- DirectX 12 without it: half there, doing nothing), locked (needs DirectX 11/12 without them, or lock_if)
local function row_state(row, ctx)
    local alpha, enabled = 1, true
    if row.needs_dx12 and not M.dx12() then
        alpha, enabled = M.FADED, false
    end
    if row.needs_dx and not M.effects_available() then
        enabled = false
    end
    if row.lock_if ~= nil then
        local ok, locked = pcall(row.lock_if)
        if ok and locked then
            enabled = false
        end
    end
    if ctx.off then
        alpha = math.min(alpha, 0.55) -- its panel's effect or sub-panel's switch is off: greyed, still settable
    end
    return alpha, enabled
end

-- The renderer a row needs that the game isn't drawing with, as its pill says it ("DX12", "DX11+"), or nil. An effect's
-- heading needs DirectX 11 or 12 without saying so (its switch is locked without them).
local function dx_pill(row)
    if row.needs_dx12 and not M.dx12() then
        return 'DX12'
    end
    if (row.needs_dx or row.kind == 'header' and row.effect) and not M.effects_available() then
        return 'DX11+'
    end
    return nil
end

-- A row's pill (dx_pill) at x, its tooltip saying where the renderer is changed; none on a row whose section's heading
-- has the same one. Returns its width (0 without one) and whether the mouse is on it.
local function draw_dx_pill(dl, sec, row, x, cy)
    local label = dx_pill(row)
    if label == nil or sec ~= nil and sec.head ~= nil and sec.head ~= row and dx_pill(sec.head) == label then
        return 0, false
    end
    local pw, ph = M.pill(dl, x, cy, label)
    local over = M.over(x, cy - ph / 2, pw, ph)
    if over then
        M.tooltip((label == 'DX12' and 'Needs DirectX 12' or 'Needs DirectX 11 or 12')
                  .. '. The renderer is under Client > Game, and changes at the next start.')
    end
    return pw, over
end

-- a plant of the player's library deleted: its file (config\grass\<id>.json, and a picture beside it), its keys, and the
-- catalogue read again
local function delete_asset(id)
    local dir = vana.paths.install .. '\\config\\grass\\'
    os.remove(dir .. id .. '.json')
    pcall(os.remove, dir .. id .. '.png')
    M.flush()
    vana.config.set('graphics', 'grass.asset.' .. id, nil)
    vana.config.set('graphics', 'grass.asset.' .. id .. '.amount', nil)
    if vana.graphics.grass_reload ~= nil then
        pcall(vana.graphics.grass_reload)
    end
    vana.graphics.reload()
    M.rebuild()
end

-- Mass Grass's library: one plant grown alone, whole (its row's eye), or (nil) the mix again; never saved
-- (vana.graphics.grass_solo). st.grass_solo the one shown, taken from the game at load (an addon reload keeps it)
M.grass_solo = function (id)
    if vana.graphics.grass_solo == nil then
        return
    end
    local ok, now = pcall(vana.graphics.grass_solo, id)
    st.grass_solo = ok and now or nil
end
if vana.graphics.grass_solo ~= nil then
    local ok, now = pcall(vana.graphics.grass_solo)
    st.grass_solo = ok and now or nil
end

-- a change of `row` written, with what follows it (its own changed hook, the pages built again when it locks others;
-- a grass asset's switch follows its share: on from 1%, off at 0)
local function changed(row, value, now)
    if now and st.drag == nil and M.edit_note ~= nil then -- (a drag's release was noted when it began)
        M.edit_note(row, value)
    end
    M.write(row, value, now)
    if now and row.asset ~= nil then
        local on = value >= 1
        if M.read(row.asset) ~= on then
            M.write(row.asset, on, true)
        end
    end
    if now and row.changed ~= nil then
        pcall(row.changed)
    end
    if now and row.rebuild then
        M.rebuild()
    end
end

-- A setting's reset (its ↺, draw.lua M.reset_icon), every kind's as a slider's. It's lit while the setting isn't its
-- default (`shown`: the value shown, a slider's while it's dragged). A row that follows another setting while its own
-- key is unset (row.follows: a True Font area's font line and outline the area they follow, the Default Font's) isn't
-- its default while that key is set, and is put back by taking the key out, so it follows again; any other is written
-- its default by its own path (changed: its set, or vanacore.ini). A row of parts (a True Font line, a damage number's
-- colours, a chat window's place) is all of them; a part that is only a key ({ section, key }) is taken out.
function M.differs(row, shown)
    if row.parts ~= nil then
        for _, part in ipairs(row.parts) do
            if M.differs(part) then
                return true
            end
        end
        return false
    end
    if row.follows or row.kind == nil then
        local text = row.section ~= nil and row.key ~= nil and vana.config.get(row.section, row.key) or nil
        return text ~= nil and text ~= ''
    end
    if shown == nil then
        local ok, value = pcall(M.read, row)
        if not ok then
            return false
        end
        shown = value -- (false: a switch off)
    end
    if shown == nil or row.default == nil then
        return false
    end
    return M.format_value(row, shown) ~= M.format_value(row, row.default)
end

function M.put_back(row)
    M.flush()
    if row.parts ~= nil then
        for _, part in ipairs(row.parts) do
            if M.differs(part) then
                M.put_back(part)
            end
        end
        if row.rebuild then
            M.rebuild()
        end
        return
    end
    if row.follows or row.kind == nil then
        if row.set ~= nil then
            row.set(nil, true) -- (a follower's own set takes nil as taking its key out)
        else
            vana.config.set(row.section, row.key, nil)
        end
        if M.LIVE_GRAPHICS[row.section] then
            vana.graphics.reload()
        end
        if row.changed ~= nil then
            pcall(row.changed)
        end
        if row.rebuild then
            M.rebuild()
        end
        return
    end
    changed(row, row.default, true)
end

-- The ↺ of a setting (or a row of parts) at x, its control after it (M.RESET_W): none for one with no default (no key:
-- the NVIDIA overlay's), one a reset leaves as it is (kept: the renderer), a controller's own (the gamepads') or a
-- no_reset. Returns the room it took.
function M.row_reset(row, id, x, cy, enabled, alpha)
    if row.no_reset or row.keep or row.section == 'gamepads'
       or (row.parts == nil and (row.section == nil or row.key == nil or row.default == nil)) then
        return 0
    end
    if M.reset_icon(id .. '.reset', x, cy, M.differs(row), enabled, alpha) then
        M.put_back(row)
        st.picker = nil
    end
    return S(M.RESET_W)
end

-- the number box opened for a slider's typed value
local function open_number(row, id, y)
    st.picker = { kind = 'number', row = row, id = id, y = y, seen = true }
    st.number = { M.format_value(row, M.read(row)) }
    st.number_focus = true
end

-- a choice's options as the segments' words and the dropdown's list; segments when they're few and short. A dropdown alone
-- on its row is one width whatever its options, its reset where a slider's is (a slider and its reset are M.SLIDER_W)
local function choice_shape(row)
    local labels = {}
    for _, option in ipairs(row.options) do
        if not option.sep then
            labels[#labels + 1] = option[2]
        end
    end
    local total = M.segments_width(labels)
    local as_segments = not row.dropdown and #labels <= 4 and total <= S(290)
    return labels, as_segments, total, S(M.SLIDER_W - M.RESET_W)
end

-- a setting's control at the right of its row: toggle, choice, slider, colour
-- A slider row's control at sx: its reset icon (unless no_reset), the track, then the value (a press on it opens a typed
-- number). `width` and `value_w` size it (draw.lua's usual when nil)
local function draw_slider(row, id, sx, cy, enabled, alpha, width, value_w)
    local value = M.read(row)
    -- while it's held: where it's been dragged to, not the setting. A row that writes only once it's let go (DLSS's and
    -- FSR's Quality, the chat's and camera's previews) still reads its old value, and the knob snapped back to it each
    -- frame and was let go there: it never moved off 35%
    if st.drag ~= nil and st.drag.id == id and st.sliding == id then
        value = st.drag.value
    end
    local fmt = row.fmt or '%.2f'
    local shown = fmt:find('%%d') and math.floor(value + 0.5) or value
    local text = row.format ~= nil and row.format(value) or string.format(fmt, shown)
    local can_reset = M.differs(row, value)
    local v, moved, released, value_clicked, reset_clicked = M.slider(id, sx, cy, value, row.min, row.max, row.step, enabled,
        { text = text, reset = not row.no_reset, can_reset = can_reset, marks = row.marks, alpha = alpha, width = width, value_w = value_w })
    if moved then
        if st.drag == nil and M.edit_note ~= nil then
            M.edit_note(row) -- (the setting as it is before the drag's first write; the HUD Manager's undo)
        end
        st.drag = { id = id, value = v }
        M.write(row, v, false)
        st.slid = row
    end
    if released then
        changed(row, v, true)
        st.slid = nil
        st.drag = nil
    end
    if value_clicked then
        if st.picker ~= nil and st.picker.id == id then
            st.picker = nil
        else
            open_number(row, id, cy)
        end
    end
    if reset_clicked then
        M.put_back(row)
        st.picker = nil
    end
    if st.picker ~= nil and st.picker.id == id then
        st.picker.y, st.picker.seen = cy, true
    end
end

-- a colour's swatch, its right edge at `right` (or its left at `left`): opens the picker beside the window. Returns its left.
local function draw_colour(row, id, right, cy, enabled, alpha, left, bare)
    local value = M.read(row)
    local label = value ~= '' and value or (row.none or "Game's") -- (row.none: what an unset colour is, when not the game's)
    label = bare and '' or label -- (bare: its swatch alone)
    local w = S(23) + M.measure(label, fonts.text, SMALL + 0.5) -- (M.swatch's box, gap and text)
    if left ~= nil then
        right = left + w
    end
    left = right - w
    local open = st.picker ~= nil and st.picker.id == id
    if M.swatch(id, right, cy, value ~= '' and value or row.game, label, enabled, open, alpha) then
        if open then
            st.picker = nil
        else
            st.picker = { kind = 'colour', row = row, id = id, y = cy, seen = true, rgb = nil }
        end
    end
    if st.picker ~= nil and st.picker.id == id then
        st.picker.y, st.picker.seen = cy, true
    end
    return left
end

-- A setting's control at the right of its row (toggle, choice, slider, colour), at the row's end as ever, and its reset
-- (M.row_reset) just before it, as a slider's is before its track. `label_end`: where the row's name ends; segments that
-- would run into it are a drop-down instead (the Keyboard hook thread's three ran over "(Linux / Proton)")
local function draw_control(page, sec, row, id, right, cy, enabled, alpha, label_end)
    local dl = imgui.GetWindowDrawList()
    local left = nil -- (the control's left edge: its reset goes before it)
    if row.kind == 'toggle' then
        local value = M.read(row)
        left = right - S(M.SW_W)
        local on, flipped = M.switch(id, left, cy, value, enabled, alpha, row.colour)
        if flipped then
            changed(row, on, true)
        end
    elseif row.kind == 'choice' then
        local value = M.read(row)
        local labels, as_segments, total, width = choice_shape(row)
        if as_segments and label_end ~= nil and right - total - S(M.RESET_W) - S(12) < label_end then
            as_segments = false
        end
        if row.arrows then
            local w = M.dropdown_arrows_width(labels)
            left = right - w
            local picked, moved = M.dropdown_arrows(id, right - w, cy, w, row.options, value, enabled, alpha)
            if moved then
                changed(row, picked, true)
            end
        elseif as_segments then
            left = right - total
            local index = 0
            local k = 0
            for _, option in ipairs(row.options) do
                if not option.sep then
                    k = k + 1
                    if option[1] == value then
                        index = k
                    end
                end
            end
            local picked, moved, over = M.segments(id, right - total, cy, labels, index, enabled, nil, alpha)
            -- an option's own tooltip (its tip) while the mouse is on it
            k = 0
            for _, option in ipairs(row.options) do
                if not option.sep then
                    k = k + 1
                    if k == over and option.tip ~= nil then
                        M.tooltip(option.tip)
                    end
                end
            end
            if moved then
                k = 0
                for _, option in ipairs(row.options) do
                    if not option.sep then
                        k = k + 1
                        if k == picked then
                            changed(row, option[1], true)
                        end
                    end
                end
            end
        else
            -- row.also: a second drop-down on the line, beside the first and as wide (Zone Name's Slide To after its Slide From);
            -- row.prefix (and its): a word dim in the box before the pick
            local also = row.also
            left = right - width - (also ~= nil and width + S(8) or 0)
            if also ~= nil then
                local picked2, moved2 = M.dropdown(id .. '.also', right - width, cy, width, also.options, M.read(also), enabled, alpha,
                                                   true, true, also.prefix ~= nil and { prefix = also.prefix } or nil)
                if moved2 then
                    changed(also, picked2, true)
                end
            end
            local picked, moved = M.dropdown(id, left, cy, width, row.options, value, enabled, alpha, true, true,
                                             row.prefix ~= nil and { prefix = row.prefix } or nil)
            if moved then
                changed(row, picked, true)
            end
        end
    elseif row.kind == 'slider' then
        draw_slider(row, id, right - S(M.SLIDER_W), cy, enabled, alpha) -- (its own reset, before its track)
    elseif row.kind == 'colour' then
        left = draw_colour(row, id, right, cy, enabled, alpha)
    end
    if left ~= nil then
        M.row_reset(row, id, left - S(M.RESET_W), cy, enabled, alpha)
    end
end

-- A number setting as a small drop-down ending at `right` (a True Font line's size and outline): part.icon (the outline's
-- hollow A) or part.short in the box before its value (its " px" left out then; a size is "16 px" alone), the list every value
-- from its min to its max at its step (40 or so at most), as its format has it less the zeros after the point, part.zero
-- naming 0 ("Off"); one set outside them (typed into vanacore.ini) in it too, in its place. Every row's box is as wide for
-- one kind of part, so a page's line up. No reset of its own: its line's puts it back. Returns the box's left.
function M.number_drop(part, id, right, cy, enabled, alpha)
    local function label(v)
        if v == 0 and part.zero ~= nil then
            return part.zero
        end
        local fmt = part.fmt or '%.2f'
        local text = part.format ~= nil and part.format(v) or string.format(fmt, fmt:find('%%d') and math.floor(v + 0.5) or v)
        text = (part.short ~= nil or part.icon ~= nil) and text:gsub(' px$', '') or text
        return (text:gsub('(%.%d-)0+(%D*)$', '%1%2'):gsub('%.(%D*)$', '%1'))
    end
    local scale = S(100)
    if part.drop_options == nil or part.drop_scale ~= scale then
        local options, longest = {}, 0
        local n = math.floor((part.max - part.min) / part.step + 0.5)
        for i = 0, n, math.max(1, math.ceil((n + 1) / 40)) do
            local v = part.min + i * part.step
            options[#options + 1] = { M.format_value(part, v), label(v) }
            longest = math.max(longest, M.measure(options[#options][2], fonts.text, M.SEG_SIZE))
        end
        if part.step < 1 then -- (room for a value between its steps, from vanacore.ini or before them: an outline's 1.25 was "1...")
            longest = math.max(longest, M.measure(label(part.min + part.step / 2), fonts.text, M.SEG_SIZE))
        end
        part.drop_options, part.drop_scale = options, scale
        part.drop_look = { prefix = part.icon == nil and part.short or nil, icon = part.icon, search = false, to_pick = true, compact = true }
        -- (as M.dropdown fits a compact box to its text: its icon or word, the padding both sides, the chevron, a hair of slack)
        local fit = M.DROP_COMPACT
        local prefix_w = part.icon ~= nil and S(M.DROP_ICON_W) + S(fit.gap)
                         or part.short ~= nil and M.measure(part.short, fonts.text, M.SEG_SIZE) + S(fit.gap) or 0
        part.drop_w = prefix_w + longest + S(fit.pad) * 2 + S(fit.chevron) + S(2)
    end
    local value = M.read(part)
    local current = M.format_value(part, value)
    local list = part.drop_options
    local found = false
    for _, option in ipairs(list) do
        found = found or option[1] == current
    end
    if not found then
        list = {}
        for _, option in ipairs(part.drop_options) do
            if not found and (tonumber(option[1]) or 0) > value then
                list[#list + 1], found = { current, label(value) }, true
            end
            list[#list + 1] = option
        end
        if not found then
            list[#list + 1] = { current, label(value) }
        end
    end
    local left = right - part.drop_w
    local picked, moved = M.dropdown(id, left, cy, part.drop_w, list, current, enabled, alpha, true, true, part.drop_look)
    if moved and tonumber(picked) ~= nil then
        changed(part, tonumber(picked), true)
    end
    if M.over(left, cy - S(12), part.drop_w, S(24)) and not (imgui.IsPopupOpen ~= nil and imgui.IsPopupOpen(id .. '.list')) then
        M.tooltip(part.tip or part.label)
    end
    return left
end

-- A True Font line (data.lua: font_line): its label (row.column: the widest of the lines sharing it, so they line up), one reset for the
-- whole line (its outline's too), the font's list, Bold, Italic and (an area's) Uppercase, its size and (an area's) outline
-- as small drop-downs (M.number_drop), and on True Font's page the area's switch at the end (row.switch: off, the line greyed,
-- still settable, as a switched-off sub-panel's rows were). Each part is its own setting, its id the row's and its key.
local function draw_font_row(row, id, lx, right, cy, enabled, alpha)
    local dl = imgui.GetWindowDrawList()
    local font, size, italic, bold, upper = row.font, row.size, row.italic, row.bold, row.upper
    -- a part's own lock_if on top of the row's (the inventory count's size is every letters', its font True Font's alone)
    local function on(part)
        if not enabled or part.lock_if == nil then
            return enabled
        end
        local ok, locked = pcall(part.lock_if)
        return not (ok and locked)
    end
    if row.switch ~= nil then
        local ok, now = pcall(row.switch.get)
        now = ok and now or false
        local new, flipped = M.switch(id .. '.switch', right - S(M.SW_W), cy, now, enabled, alpha)
        if flipped then
            M.flush()
            pcall(row.switch.set, new)
            M.rebuild()
        end
        alpha = now and alpha or math.min(alpha, 0.55)
        right = right - S(M.SW_W) - S(12)
    end
    M.text_mid(dl, lx, cy, row.label, P.text, fonts.text, BODY, alpha)
    local sx = right
    for _, part in ipairs({ row.outline or false, size }) do
        if part then
            sx = M.number_drop(part, id .. '.' .. part.key, sx, cy, on(part), alpha) - S(4)
        end
    end
    -- Bold, Italic and Uppercase joined as one control (M.chip_bar), each its own button
    local chips = {}
    for _, c in ipairs({ { bold, 'B', fonts.semi }, { italic, 'I', fonts.semi_italic or fonts.italic or fonts.text }, { upper or false, 'AA', fonts.semi } }) do
        local part = c[1]
        if part then
            chips[#chips + 1] = { c[2], c[3], M.read(part), on(part), id = id .. '.' .. part.key, part = part }
        end
    end
    local x = sx - S(6) - M.chip_bar_width(chips)
    local pressed, over = M.chip_bar(x, cy, chips, alpha)
    if pressed ~= nil then
        changed(chips[pressed].part, not chips[pressed][3], true)
    end
    if over ~= nil then
        M.tooltip(chips[over].part.label)
    end
    -- the line's reset before its font's list: every part back, the size and outline too
    local label_w = M.measure(row.label, fonts.text, BODY) + S(14)
    if row.column ~= nil then
        local scale = S(100)
        if row.column.scale ~= scale then
            row.column.scale, row.column.w = scale, 0
        end
        row.column.w = math.max(row.column.w, label_w)
        label_w = row.column.w
    end
    -- what the area mostly draws (row.tip: True Font's Fonts lines) once the mouse has rested on its name, as a row's tooltip
    if row.tip ~= nil and M.over(lx, cy - S(10), label_w - S(14), S(20)) then
        local now = clock()
        if st.hover_key ~= id .. '.label' then
            st.hover_key, st.hover_since = id .. '.label', now
        elseif now - st.hover_since > 0.4 then
            M.tooltip(tip_of(row))
        end
        st.hover_seen = true
    end
    local dx = lx + label_w
    dx = dx + M.row_reset(row, id, dx, cy, enabled, alpha)
    local picked, moved = M.dropdown(id .. '.' .. font.key, dx, cy, x - S(12) - dx, font.options, M.read(font), on(font), alpha, true, true)
    if moved then
        changed(font, picked, true)
    end
end

-- A damage number kind's colours on one line (data.lua: damage_colours): its label, its colour, the gradient's second
-- colour (greyed while Gradient is off: one colour then, the first) and the Gradient checkbox at the row's end, the colours
-- in columns down the page. Each part its own setting, its id the row's and its key, its lock_if on top of the row's.
-- (On M: the main chunk is near LuaJIT's 200 locals and draw_row near its 60 upvalues.)
function M.draw_colours_row(row, id, lx, right, cy, enabled, alpha)
    if row.line then
        M.draw_colours_line(row, id, lx, right, cy, enabled, alpha)
        return
    end
    local dl = imgui.GetWindowDrawList()
    local function on(part)
        if not enabled or part.lock_if == nil then
            return enabled
        end
        local ok, locked = pcall(part.lock_if)
        return not (ok and locked)
    end
    local first, gradient, second = row.parts[1], row.parts[2], row.parts[3]
    M.text_mid(dl, lx, cy, row.label, enabled and P.text or P.dim, fonts.text, BODY, alpha)
    local bx = right - M.checkbox_width(gradient.label)
    local set, flipped = M.checkbox(id .. '.' .. gradient.key, bx, cy, gradient.label, M.read(gradient), on(gradient), alpha)
    if flipped then
        changed(gradient, set, true)
    end
    if M.over(bx, cy - S(10), right - bx, S(20)) then
        M.tooltip(tip_of(gradient) or gradient.label)
    end
    -- each colour's column as wide as a swatch and its #RRGGBB, an arrow from the first to the second
    local col = S(23) + M.measure('#DDDDDD', fonts.text, SMALL + 0.5)
    local x2 = bx - S(22) - col
    local x1 = x2 - S(26) - col
    M.row_reset(row, id, x1 - S(M.RESET_W), cy, enabled, alpha) -- (both colours and Gradient back)
    M.icon_centred(dl, x2 - S(13), cy, 'right', P.dim, 8, (on(second) and 1 or 0.4) * alpha)
    for _, at in ipairs({ { first, x1 }, { second, x2 } }) do
        local part, x = at[1], at[2]
        draw_colour(part, id .. '.' .. part.key, nil, cy, on(part), alpha, x)
        if M.over(x, cy - S(10), col, S(20)) then
            M.tooltip(tip_of(part) or part.label)
        end
    end
end

-- Colours on one line (data.lua: colours_line; the Status window's HP / MP / TP): the name, the reset (every part back),
-- the colours in their order with their values (their swatches alone where the line hasn't room, the value then in the
-- tooltip), and the switch that turns them on at the line's end (row.toggle, none for some). Each part its own setting,
-- its id the row's and its key, its lock_if (the switch off) on top of the row's.
function M.draw_colours_line(row, id, lx, right, cy, enabled, alpha)
    local dl = imgui.GetWindowDrawList()
    local function on(part)
        if not enabled or part.lock_if == nil then
            return enabled
        end
        local ok, locked = pcall(part.lock_if)
        return not (ok and locked)
    end
    M.text_mid(dl, lx, cy, row.label, enabled and P.text or P.dim, fonts.text, BODY, alpha)
    local switch = row.toggle
    if switch ~= nil then
        local sx = right - S(M.SW_W)
        local set, flipped = M.switch(id .. '.' .. switch.key, sx, cy, M.read(switch), on(switch), alpha)
        if flipped then
            changed(switch, set, true)
        end
        if M.over(sx, cy - S(10), S(M.SW_W), S(20)) then
            M.tooltip(tip_of(switch) or switch.label)
        end
        right = sx - S(16)
    end
    local colours, text, need = {}, {}, 0
    for _, part in ipairs(row.parts) do
        if part ~= switch then
            local value = M.read(part)
            colours[#colours + 1] = part
            text[#colours] = value ~= '' and value or (part.none or "Game's")
            need = need + S(35) + M.measure(text[#colours], fonts.text, SMALL + 0.5)
        end
    end
    local bare = need > right - (lx + M.measure(row.label, fonts.text, BODY) + S(M.RESET_W) + S(16))
    local x = right
    for i = #colours, 1, -1 do
        local part = colours[i]
        local w = S(23) + (bare and 0 or M.measure(text[i], fonts.text, SMALL + 0.5))
        draw_colour(part, id .. '.' .. part.key, x, cy, on(part), alpha, nil, bare)
        if M.over(x - w, cy - S(10), w, S(20)) then
            M.tooltip(part.label .. (bare and (': ' .. text[i]) or ''))
        end
        x = x - w - S(12)
    end
    M.row_reset(row, id, x + S(12) - S(M.RESET_W), cy, enabled, alpha)
end

-- A row's switches and colours after its sliders (row.extras; data.lua: a tooltip part's Size line): each switch a chip from x
-- (its letters row.chip: B, I, AA), the colours at the row's end, with their values where there's room, else their swatches alone
-- (the value in the tooltip). Each its own setting and lock, under the row's; one reset before them puts them all back (the
-- sliders have their own).
local function draw_extras(row, id, x, right, cy, enabled, alpha)
    local function on(part)
        if not enabled or part.lock_if == nil then
            return enabled
        end
        local ok, locked = pcall(part.lock_if)
        return not (ok and locked)
    end
    row.extras_reset = row.extras_reset or { parts = row.extras }
    x = x + M.row_reset(row.extras_reset, id .. '.extras', x, cy, enabled, alpha)
    local colours, text, chips = {}, {}, {}
    for _, part in ipairs(row.extras) do
        if part.kind == 'colour' then
            colours[#colours + 1] = part
        else
            local face = part.chip == 'I' and (fonts.semi_italic or fonts.italic or fonts.text) or fonts.semi
            chips[#chips + 1] = { part.chip, face, M.read(part), on(part), id = id .. '.' .. part.key, part = part }
        end
    end
    if #chips > 0 then -- (its switches joined as one control, as a True Font line's: M.chip_bar)
        local pressed, over = M.chip_bar(x, cy, chips, alpha)
        if pressed ~= nil then
            changed(chips[pressed].part, not chips[pressed][3], true)
        end
        if over ~= nil then
            M.tooltip(chips[over].part.label)
        end
        x = x + M.chip_bar_width(chips) + S(4)
    end
    local need = 0
    for i, part in ipairs(colours) do
        local value = M.read(part)
        text[i] = value ~= '' and value or (part.none or "Game's")
        need = need + S(35) + M.measure(text[i], fonts.text, SMALL + 0.5)
    end
    local bare = need > right - x - S(10)
    for i = #colours, 1, -1 do
        local part = colours[i]
        local w = S(23) + (bare and 0 or M.measure(text[i], fonts.text, SMALL + 0.5))
        draw_colour(part, id .. '.' .. part.key, right, cy, on(part), alpha, nil, bare)
        if M.over(right - w, cy - S(10), w, S(20)) then
            M.tooltip(part.label .. (bare and (': ' .. text[i]) or ''))
        end
        right = right - w - S(12)
    end
end

-- an action's button: its text, or its confirm text in red once pressed, for 5 seconds
local function draw_action(row, id, right, cy, enabled, alpha)
    -- a row.release row runs once the click is let go: Windows' own loop (a dialog) mustn't open with a button held
    if st.release == row and not imgui.IsMouseDown(0) then
        st.release = nil
        pcall(row.run)
        if row.rebuild then
            M.rebuild()
        end
    end
    local armed = st.armed
    local asking = row.confirm ~= nil and armed ~= nil and armed[1] == (row.action or row.text) and clock() < armed[2]
    local label = asking and row.confirm or row.text
    local w = M.button_width(label, row.icon)
    -- row.value: what the button set (a picked file's name) left of it, with an x that clears it (row.clear)
    if row.value ~= nil then
        local dl = imgui.GetWindowDrawList()
        local ok, value = pcall(row.value)
        value = ok and value or ''
        if #value > 32 then
            value = value:sub(1, 29) .. '...'
        end
        local tw = M.measure(value ~= '' and value or 'None', fonts.text, BODY)
        local tx = right - w - S(12) - tw
        M.text_mid(dl, tx, cy, value ~= '' and value or 'None', value ~= '' and P.text2 or P.faint, fonts.text, BODY, alpha)
        if value ~= '' and row.clear ~= nil then
            local gone, hov = M.icon_button(id .. '.clear', tx - S(14), cy, 'close', 11, P.rose, P.faint, enabled)
            if hov then
                M.tooltip(row.clear_tip or 'Remove')
            end
            if gone then
                pcall(row.clear)
                if row.rebuild then
                    M.rebuild()
                end
            end
        end
    end
    -- row.also: a second button, left of this one ({ text, icon, action }: Zone Name's Show beside Preview)
    if row.also ~= nil then
        local also = row.also
        local also_w = M.button_width(also.text, also.icon)
        if M.button(id .. '.also', right - w - S(8) - also_w, cy, also.text, also.icon, 'ghost', enabled, alpha)
            and also.action ~= nil and M[also.action] ~= nil then
            pcall(M[also.action])
        end
    end
    local clicked = M.button(id, right - w, cy, label, row.icon, asking and 'danger' or 'ghost', enabled, alpha)
    if clicked then
        if row.confirm ~= nil and not asking then
            st.armed = { row.action or row.text, clock() + 5 }
            return
        end
        st.armed = nil
        if row.run ~= nil and row.release then
            st.release = row
        elseif row.run ~= nil then
            pcall(row.run)
            if row.rebuild then
                M.rebuild()
            end
        elseif row.action ~= nil and M[row.action] ~= nil then
            pcall(M[row.action])
        end
    end
end

-- the widest kind's name (VanaCore, Ashita, Windower) at this scale: the kinds' column in the Addons and Plugins lists
local kind_column = { scale = nil, w = 0 }
local function kind_width()
    if kind_column.scale ~= M.scale then
        kind_column.scale, kind_column.w = M.scale, 0
        for _, kind in pairs(M.KIND_NAMES) do
            kind_column.w = math.max(kind_column.w, M.measure(kind, fonts.text, SMALL))
        end
    end
    return kind_column.w
end

-- An addon or plugin, in columns that line up down the list: its name (a click shows its panel), ? for its panel, Reload
-- while it's loaded, the kind it is, and the switch that loads or unloads it (on and locked for those CatsEyeXI
-- requires). The row whose help is open is tinted.
local function draw_extension(row, id, x, w, lx, right, cy, alpha)
    local dl = imgui.GetWindowDrawList()
    local reading = M.help_showing(row)
    if reading then
        M.fill(dl, x + S(4), cy - S(ROW_H) / 2, w - S(8), S(ROW_H), P.accent_bg, S(3), 0, 0.9)
    end
    local sx = right - S(M.SW_W)
    local on, flipped = M.switch(id, sx, cy, row.on, not row.fixed, alpha)
    if imgui.IsItemHovered() then
        M.tooltip(row.fixed and 'Required by client configuration: it stays loaded' or row.on and 'Switch off (unload it)' or 'Switch on (load it)')
    end
    if flipped then
        row.on = on
        M.set_extension(row, on)
    end
    local kind = M.KIND_NAMES[row.source] or row.source
    local kind_color = ({ windower = P.teal, ashita = P.amber, vanacore = P.accent })[row.source] or P.dim
    M.text_right_mid(dl, sx - S(12), cy, kind, row.on and kind_color or P.faint, fonts.text, SMALL, alpha)
    local rx = sx - S(12) - kind_width() - S(18)
    if row.on and not row.fixed then
        local clicked, hovered = M.icon_button(id .. '.reload', rx, cy, 'reload', 10, P.text, P.dim)
        if hovered then
            M.tooltip('Reload: unload it and load it again')
        end
        if clicked then
            M.reload_extension(row)
        end
    end
    local hx = rx - S(24)
    local help_clicked, help_hovered = M.icon_button(id .. '.help', hx, cy, 'question', 10, P.accent2, reading and P.accent or P.faint)
    if help_hovered then
        M.tooltip('Read me and details')
    end
    local room = hx - S(16) - lx
    local name = M.fit(row.label, room, fonts.text, BODY)
    local name_hovered, name_clicked = M.hit(id .. '.name', lx, cy - S(10), M.measure(name, fonts.text, BODY), S(20))
    local color = reading and P.accent or name_hovered and P.accent2 or row.on and P.text or P.dim
    M.text_mid(dl, lx, cy, name, color, fonts.text, BODY, alpha)
    if help_clicked or name_clicked then
        if reading then
            M.help_close()
        else
            M.help_open(row)
        end
    end
end

-- a Pivot overlay: its name; in use, arrows to move it and a switch to take it out; the server's own locked on
local function draw_overlay(row, id, lx, right, cy, alpha)
    local dl = imgui.GetWindowDrawList()
    if row.server then
        M.switch(id, right - S(M.SW_W), cy, true, false, alpha)
        M.icon_centred(dl, lx + S(6), cy, 'lock', P.server, 9)
        M.text_mid(dl, lx + S(20), cy, row.label, P.server, fonts.text, BODY, alpha)
        if M.over(lx, cy - S(12), right - lx, S(24)) then
            M.tooltip(row.tip)
        end
        return
    end
    local on, flipped = M.switch(id, right - S(M.SW_W), cy, row.on, true, alpha)
    if flipped then
        M.set_overlay(row.name, on)
        M.rebuild()
    end
    if row.on then
        local bx = right - S(M.SW_W) - S(16)
        local down_ok, up_ok = row.index < row.count, row.index > 1
        local down = M.icon_button(id .. '.down', bx - S(8), cy, 'down', 9, P.text, P.dim, down_ok)
        local up = M.icon_button(id .. '.up', bx - S(30), cy, 'up', 9, P.text, P.dim, up_ok)
        if down then
            M.move_overlay(row.index, 1)
            M.rebuild()
        elseif up then
            M.move_overlay(row.index, -1)
            M.rebuild()
        end
    end
    M.icon_centred(dl, lx + S(6), cy, 'folder', row.on and P.accent or P.faint, 9)
    M.text_mid(dl, lx + S(20), cy, row.label, row.on and P.text or P.dim, fonts.text, BODY, alpha)
end

-- The Characters block, top to bottom: the Main's card, arrows down to the three copies (Graphics Settings, Game Options,
-- Addons/Plugins: each asks twice, then shows Sent for a moment), arrows down to the Alt's card. With no Alt, one Refresh
-- button in the copies' place. (TRANSFER_*: its parts' heights.)
local function draw_transfer(row, id, lx, right, y, h)
    local dl = imgui.GetWindowDrawList()
    local w = right - lx
    local function card(item, top)
        local ch = S(TRANSFER_CARD)
        M.fill(dl, lx, top, w, ch, P.well, S(4))
        M.outline(dl, lx, top, w, ch, P.well_edge, S(4))
        M.text(dl, lx + S(10), top + S(5), item.label, P.dim, fonts.text, SMALL - 1)
        local entry = live_value(item)
        M.text(dl, lx + S(10), top + S(21), entry.text, entry.mine and P.mine or P.text, fonts.text, BODY - 0.5)
        if entry.aside ~= nil then
            M.text(dl, lx + S(10) + M.measure(entry.text, fonts.text, BODY - 0.5) + S(6), top + S(21), entry.aside, P.dim, fonts.text, BODY - 0.5)
        end
    end
    local top = y + S(4)
    card(row.main, top)
    local arrows1 = top + S(TRANSFER_CARD) + S(TRANSFER_ARROW) / 2
    local by = top + S(TRANSFER_CARD) + S(TRANSFER_ARROW)
    local bcy = by + S(TRANSFER_BUTTON) / 2
    local arrows2 = by + S(TRANSFER_BUTTON) + S(TRANSFER_ARROW) / 2
    card(row.alt, by + S(TRANSFER_BUTTON) + S(TRANSFER_ARROW))
    if row.refresh then
        -- one game running: the button looks for the other
        local cx = lx + w / 2
        M.icon_centred(dl, cx, arrows1, 'arrow_down', P.faint, 9, 0.5)
        M.icon_centred(dl, cx, arrows2, 'arrow_down', P.faint, 9, 0.5)
        local bw = M.button_width('Refresh', 'reload')
        if M.button(id .. '.refresh', cx - bw / 2, bcy, 'Refresh', 'reload', 'ghost', nil, nil, TRANSFER_BUTTON - 4) then
            M.rebuild()
        end
        if M.over(cx - bw / 2, by, bw, S(TRANSFER_BUTTON)) and row.tip then
            M.tooltip(row.tip)
        end
        return
    end
    local copies = row.copies
    local gap = S(10)
    local cw = math.floor((w - gap * (#copies - 1)) / #copies)
    local now = clock()
    local busy = M.copy ~= nil and M.copy.busy()
    for i, c in ipairs(copies) do
        local bx = lx + (i - 1) * (cw + gap)
        local cx = bx + cw / 2
        local armed = st.armed ~= nil and st.armed[1] == c.action and now < st.armed[2]
        local sent = st.copied ~= nil and st.copied[1] == c.action and now < st.copied[2]
        local label, icon, kind = c.label, c.icon, 'ghost'
        if sent then
            label, icon, kind = 'Sent', 'check', 'success'
        elseif armed then
            label, icon, kind = 'Click again to copy', nil, 'danger'
        end
        if icon ~= nil and M.button_width(label, icon) > cw then
            icon = nil
        end
        label = M.fit(label, cw - S(18), fonts.text, SMALL + 0.5)
        local lit = M.over(bx, by, cw, S(TRANSFER_BUTTON))
        M.icon_centred(dl, cx, arrows1, 'arrow_down', lit and P.accent2 or P.faint, 9)
        M.icon_centred(dl, cx, arrows2, 'arrow_down', lit and P.accent2 or P.faint, 9)
        local clicked = M.button(id .. '.' .. c.action, bx, bcy, label, icon, kind, not busy, nil, TRANSFER_BUTTON - 4, cw)
        if lit then
            M.tooltip(busy and 'A copy is under way: wait for it to finish' or c.tip)
        end
        if clicked then
            if not armed then
                st.armed = { c.action, now + 5 }
            else
                st.armed = nil
                local ok, done = pcall(row.run, c.kind)
                if ok and done then
                    st.copied = { c.action, now + 2.5 }
                end
                M.rebuild()
            end
        end
    end
end

-- The banner at the top of Client > VanaCore, as the README's: banner.png (the logo and VANACORE) centred and the line
-- under it, on a dark card
M.BANNER_H = 126 -- (unscaled: 16 + the picture 64 + 8 + the line 22 + 16)
local function draw_banner(row, lx, right, y)
    local dl = imgui.GetWindowDrawList()
    local bx, bw, r = lx, right - lx, S(10)
    M.fill(dl, bx, y, bw, S(M.BANNER_H), P.well, r)
    M.outline(dl, bx, y, bw, S(M.BANNER_H), P.well_edge, r)
    local cx, iy, ih = bx + bw / 2, y + S(16), S(64)
    if M.logo.banner ~= nil then
        local iw = ih * 4 -- banner.png is 1024 x 256
        dl:AddImage(M.logo.banner, { cx - iw / 2, iy }, { cx + iw / 2, iy + ih })
    else -- (banner.png couldn't be loaded)
        M.text_centred(dl, bx, iy, bw, ih, 'SOULSPIREXI CLIENT', P.accent, fonts.semi, BODY + 18)
    end
    local ty = iy + ih + S(8)
    M.text_centred(dl, bx, ty, bw, S(22), row.tagline or '', P.white, fonts.semi, BODY)
end

-- Graphics > Toggles (data.lua: layers): a line with how many are on and All off and Put back, then a chip a layer, two to a
-- line, on a dark well with a green border and tick while it's on and grey with a cross while it's off (eased between); a
-- click switches it. One
-- that can't be switched now (it needs DirectX 12, or a card it doesn't run on) is grey with a lock, and one that was on
-- before All off and is off now has an amber dot at its end. The tooltips wait for the mouse to rest, and again after a
-- click, so a video of the clicks doesn't show them.
local function draw_layers(row, id, lx, right, y)
    local dl = imgui.GetWindowDrawList()
    local now = clock()
    local function rested(key, clicked, text)
        if st.hover_key ~= key or clicked then
            st.hover_key, st.hover_since = key, now
        elseif now - st.hover_since > 0.6 and text ~= '' then
            M.tooltip(text)
        end
        st.hover_seen = true
    end
    local items, kept = row.items, M.layers.kept()
    local on, why, count, any_kept = {}, {}, 0, false
    for k, item in ipairs(items) do
        why[k] = item.why()
        on[k] = why[k] == nil and item.get()
        count = count + (on[k] and 1 or 0)
        any_kept = any_kept or kept[item.id] == true
    end
    local cy = y + S(ROW_H) / 2
    M.text_mid(dl, lx, cy, count .. ' of ' .. #items .. ' on', P.text2, fonts.text, BODY - 1)
    local bw = M.button_width('Put back', 'reset')
    local back = M.button(id .. '.back', right - bw, cy, 'Put back', 'reset', 'ghost', any_kept)
    if imgui.IsItemHovered() then
        rested(id .. '.back', back, any_kept and 'The layers that were on before All off on again, the others off'
                                    or 'Nothing to put back: All off remembers the layers that were on')
    end
    local ow = M.button_width('All off', 'power')
    local off = M.button(id .. '.off', right - bw - S(8) - ow, cy, 'All off', 'power', 'ghost', count > 0)
    if imgui.IsItemHovered() then
        rested(id .. '.off', off, 'Every layer off. The ones on now are remembered for Put back')
    end
    local gap, ch = S(LAYER_GAP), S(LAYER_H)
    local cw = math.floor((right - lx - gap * (LAYER_COLS - 1)) / LAYER_COLS)
    local top = y + S(ROW_H) + S(2)
    local flip = nil
    for k, item in ipairs(items) do
        local col, line = (k - 1) % LAYER_COLS, math.floor((k - 1) / LAYER_COLS)
        local x0, y0 = lx + col * (cw + gap), top + line * (ch + gap)
        local locked, was = why[k] ~= nil, kept[item.id] == true and not on[k]
        local hovered, clicked = M.hit(id .. '.' .. item.id, x0, y0, cw, ch)
        local t = M.ease(id .. '.' .. item.id .. '.on', on[k] and 1 or 0, 16)
        local lit = M.ease(id .. '.' .. item.id .. '.lit', hovered and not locked and 1 or 0, 14)
        local hue = locked and P.faint or M.mix(P.dim, P.green, t)
        local round = S(3)
        M.fill(dl, x0, y0, cw, ch, P.well, round, 0, 0.85)
        M.fill(dl, x0, y0, cw, ch, hue, round, 0, 0.06 * lit)
        if t > 0.01 then
            -- on: green from the left, fading out to the right (inset a pixel, inside the rounded outline)
            local l, r = M.u32(P.green, 0.42 * t), M.u32(P.green, 0.03 * t)
            dl:AddRectFilledMultiColor({ x0 + 1, y0 + 1 }, { x0 + cw - 1, y0 + ch - 1 }, l, r, r, l)
        end
        M.outline(dl, x0, y0, cw, ch, hue, round, 0, 1, 0.30 + 0.10 * t + 0.25 * lit)
        -- the tick or cross centred in a square as tall as the chip, the name a clear gap after it
        local ax, ay = x0 + ch / 2 + S(2), y0 + ch / 2
        M.icon_centred(dl, ax, ay, locked and 'lock' or on[k] and 'check' or 'close', hue, 9)
        local nx = ax + S(11)
        local name = M.fit(item.label, x0 + cw - S(was and 16 or 6) - nx, fonts.text, BODY - 2)
        M.text_mid(dl, nx, ay, name, locked and P.dim or M.mix(on[k] and P.text or P.text2, P.white, lit), fonts.text, BODY - 2)
        if was then
            M.circle(dl, x0 + cw - S(8), ay, S(2.5), P.amber)
        end
        if clicked and not locked then
            flip = k
        end
        if hovered then
            local lines = {}
            if locked then
                lines[#lines + 1] = why[k]
            end
            if was then
                lines[#lines + 1] = 'On before All off.'
            end
            if type(item.tip) == 'string' then
                lines[#lines + 1] = item.tip
            end
            rested(id .. '.' .. item.id, clicked, table.concat(lines, '\n'))
        end
    end
    if off then
        M.layers.all_off(items)
    elseif back then
        M.layers.put_back(items)
    elseif flip ~= nil then
        M.layers.switch({ items[flip] }, function () return not on[flip] end)
    end
end

-- a gamepad found (a status row of the Gamepads section; data.lua: "<guid>\t<kind>\t<use|idle|gone|off|disabled>"): its
-- name over what it is and its id, a tile for its icon, "In use" and a teal edge while the game reads it now; "Gamepad off"
-- or "Disabled" in rose, the row greyed, while the game reads no controller at all
local function draw_pad(row, lx, right, y)
    local PAD_STATES = { use = 'In use', idle = 'Available', gone = 'Gone', off = 'Gamepad off', disabled = 'Disabled' }
    local dl = imgui.GetWindowDrawList()
    local cy = y + S(PAD_ROW) / 2
    local entry = live_value(row)
    local guid = pad_guid(entry.text) or entry.text
    local kind = entry.text:match('^%b{}\t([^\t]*)\t') or ''
    local state = entry.text:match('%s+(%a+)$')
    local in_use = state == 'use'
    -- gone, or no controller read at all (gamepad off, disabled): the name greyed, the state in rose
    local dead = state == 'gone' or state == 'off' or state == 'disabled'
    local tile = S(28)
    if in_use then
        M.fill(dl, lx - S(8), y + S(3), right - lx + S(16), S(PAD_ROW) - S(6), P.teal, S(6), 0, 0.07)
    end
    -- in use: the tile and its icon green
    M.fill(dl, lx, cy - tile / 2, tile, tile, in_use and M.mix(P.well, P.teal, 0.18) or P.well, S(7))
    M.outline(dl, lx, cy - tile / 2, tile, tile, in_use and M.mix(P.well_edge, P.teal, 0.7) or P.well_edge, S(7))
    -- (the icon font's glyph sits a little left and high of its box: nudged to the tile's middle)
    M.icon_centred(dl, lx + tile / 2 + S(1.2), cy + S(1.2), 'gamepad', in_use and P.teal or (dead and P.faint or P.dim), 11)
    local text_x = lx + tile + S(10)
    local state_text = PAD_STATES[state] or 'Available'
    local state_colour = in_use and P.teal or (dead and P.rose or P.dim)
    local state_w = M.measure(state_text, fonts.text, SMALL)
    local room = right - text_x - state_w - S(28)
    M.text_mid(dl, text_x, cy - S(8), M.fit(row.label, room, fonts.text, BODY - 0.5), dead and P.dim or P.text, fonts.text, BODY - 0.5)
    local sub = kind ~= '' and (kind .. '  ·  ' .. guid) or guid
    M.text_mid(dl, text_x, cy + S(9), M.fit(sub, room, fonts.text, SMALL - 1), P.faint, fonts.text, SMALL - 1)
    M.text_right_mid(dl, right, cy, state_text, state_colour, fonts.text, SMALL)
    M.circle(dl, right - state_w - S(8), cy, S(2.5), in_use and P.teal or (dead and P.rose or P.faint))
end

-- a Gamepads status line's value colour: teal when it's working, rose when it's off, grey when there's nothing
local function gamepad_status_tone(label, text)
    if label == 'Game uses' then
        if text:find('^none') or text:find('^Nothing') then
            return P.rose
        end
        return (text == 'DirectInput' or text == 'XInput') and P.teal or P.dim
    elseif label == 'XInput' then
        return text:find('^slot') and P.teal or P.faint
    end
    return nil
end

-- a field in a sunken box, its text in the body font (the Export and Import dialog's, Client > Boot.txt's)
local function dialog_box(id, x, y, w, h, buffer, size, flags, focus)
    local dl = imgui.GetWindowDrawList()
    M.fill(dl, x, y, w, h, P.well, S(5))
    M.outline(dl, x, y, w, h, P.well_edge, S(5))
    imgui.SetCursorScreenPos({ x + S(2), y + S(2) })
    imgui.PushStyleColor(M.COL.FrameBg, M.rgba(P.well, 0))
    imgui.PushStyleColor(M.COL.FrameBgHovered, M.rgba(P.well, 0))
    imgui.PushStyleColor(M.COL.FrameBgActive, M.rgba(P.well, 0))
    imgui.PushStyleVar(M.VAR.FramePadding, { S(8), S(6) })
    imgui.PushStyleVar(M.VAR.FrameBorderSize, 0)
    M.push(fonts.text, SMALL + 0.5)
    if focus then
        imgui.SetKeyboardFocusHere()
    end
    local changed = imgui.InputTextMultiline(id, buffer, size, { w - S(4), h - S(4) }, flags)
    M.pop()
    imgui.PopStyleVar(2)
    imgui.PopStyleColor(3)
    return changed
end

-- Client > Boot.txt (data.lua): a text file to edit in a box (row.path: scripts\boot.txt), Save and Revert under it. What's
-- typed stays (st.files, by path) across page switches and rebuilds till it's saved or reverted. The file is read again
-- each second while it shows: a change made to it meanwhile (an Addons switch, a typed /addon load, the launcher) is taken
-- into a box not edited, and an edited box says so (Revert takes it up). Saved with the line ends and BOM the file had
-- (CRLF for a new one); Ctrl+S saves while typing in it. Its table is with the rows' sizes (textfile).

-- the file's text with \n line ends, whether it had \r\n and a BOM; nil when it isn't there
function textfile.read(path)
    local file = io.open(path, 'rb')
    if file == nil then
        return nil
    end
    local raw = file:read('*a') or ''
    file:close()
    local bom = raw:sub(1, 3) == textfile.BOM
    if bom then
        raw = raw:sub(4)
    end
    return (raw:gsub('\r\n', '\n')), raw:find('\r\n', 1, true) ~= nil, bom
end

-- the file as it is now into f.disk; a box not edited (and not being typed in) takes it up
function textfile.check(f, path)
    local text, crlf, bom = textfile.read(path)
    f.missing = text == nil
    if text ~= nil then
        f.crlf, f.bom = crlf, bom
    end
    f.disk = text or ''
    if f.text[1] == f.base and not f.active then
        f.text[1], f.base = f.disk, f.disk
    end
end

function textfile.save(f, path)
    local text = f.text[1]
    local out = (f.bom and textfile.BOM or '') .. (f.crlf ~= false and (text:gsub('\n', '\r\n')) or text)
    local folder = path:match('^(.+)\\[^\\]+$')
    if folder ~= nil and vana.fs ~= nil and vana.fs.create_dir ~= nil then
        pcall(vana.fs.create_dir, folder)
    end
    local file, why = io.open(path, 'wb')
    local ok = file ~= nil
    if file ~= nil then
        ok, why = file:write(out)
        file:close()
    end
    if not ok then
        f.error, f.saved = (tostring(why or 'the file is in use'):gsub('^.*: ', '')), nil -- (the reason, not the path)
        return
    end
    f.base, f.disk, f.missing, f.error, f.saved, f.checked = text, text, false, nil, clock(), clock()
end

function textfile.draw(row, id, lx, right, y)
    local dl = imgui.GetWindowDrawList()
    st.files = st.files or {}
    local f = st.files[row.path]
    if f == nil then
        f = { text = { '' }, base = '' }
        st.files[row.path] = f
    end
    local now = clock()
    if f.checked == nil or now - f.checked >= 1 then
        f.checked = now
        textfile.check(f, row.path)
    end
    local w, top, box_h = right - lx, y + S(4), S(textfile.BOX)
    dialog_box(id .. '.text', lx, top, w, box_h, f.text, #f.text[1] + 65536, 0)
    f.active = imgui.IsItemActive()
    local save = false
    if f.active and imgui.IsKeyPressed ~= nil then
        local ok, keys = pcall(imgui.GetIO)
        save = ok and keys ~= nil and keys.KeyCtrl and not keys.KeyAlt and imgui.IsKeyPressed(0x53, false) -- (Ctrl+S)
    end
    -- under the box: what state it's in on the left, Revert and Save on the right
    local cy = top + box_h + S(textfile.BAR) / 2
    local edited = f.text[1] ~= f.base
    local stale = edited and f.disk ~= f.base
    local sw = M.button_width('Save', 'save') + S(8)
    local rw = M.button_width('Revert', 'reset') + S(8)
    if M.button(id .. '.save', right - sw, cy, 'Save', 'save', 'accent', edited or f.missing, nil, 26, sw) then
        save = true
    end
    if M.over(right - sw, cy - S(13), sw, S(26)) then
        M.tooltip('Saves ' .. row.label .. ' (Ctrl+S while typing). VanaCore runs it at the next start')
    end
    if M.button(id .. '.revert', right - sw - S(8) - rw, cy, 'Revert', 'reset', 'ghost', edited, nil, 26, rw) then
        textfile.check(f, row.path)
        f.text[1], f.base, f.error = f.disk, f.disk, nil
    end
    if M.over(right - sw - S(8) - rw, cy - S(13), rw, S(26)) then
        M.tooltip('Puts the box back to ' .. row.label .. ' as it is on disk, dropping what was typed')
    end
    if save and (edited or f.missing) then
        textfile.save(f, row.path)
        edited, stale = false, false
    end
    local status, colour
    if f.error ~= nil then
        status, colour = "Couldn't save: " .. f.error, P.rose
    elseif stale then
        status, colour = 'Changed on disk while you were editing: Revert takes that up', P.amber
    elseif edited then
        status, colour = 'Unsaved changes', P.amber
    elseif f.saved ~= nil and now - f.saved < 3 then
        status, colour = 'Saved: it runs at the next start', P.green
    elseif f.missing then
        status, colour = "Not there yet: Save makes it", P.dim
    else
        local _, breaks = f.text[1]:gsub('\n', '')
        local lines = breaks + ((f.text[1] ~= '' and f.text[1]:sub(-1) ~= '\n') and 1 or 0)
        status, colour = lines .. (lines == 1 and ' line' or ' lines'), P.faint
    end
    local room = right - sw - S(8) - rw - S(12) - lx
    M.text_mid(dl, lx + S(2), cy, M.fit(status, room, fonts.text, SMALL), colour, fonts.text, SMALL)
end

-- A row: its name on the left (a dot in the margin when it takes effect at the next start), its control on the right;
-- the kinds that aren't settings drawn their own way. Returns its height.
local function draw_row(page, sec, row, x, y, w, ctx)
    local dl = imgui.GetWindowDrawList()
    local h = row_height(row, sec, note_width(w))
    if h == 0 then
        return 0
    end
    -- a row scrolled out of the page's box costs nothing: only its height is kept. A sub-panel's heading is drawn
    -- anyway, it sets whether the rows under it are greyed.
    local view_top, view_bottom = st.view_top, st.view_bottom
    if view_top ~= nil and row.kind ~= 'subheader' and (y + h < view_top or y > view_bottom) then
        return h
    end
    -- (a row with no setting of its own, a mover or a pair, goes by its first slider's too: the Main Menu's Sub-menu rows,
    -- one name in one section, were one control, their switches and eyes flipping together, 2026-10-22)
    local first = row.key == nil and row.sliders ~= nil and row.sliders[1] ~= nil and row.sliders[1].key or nil
    local key = page.id .. '.' .. (row.section or sec.title or '') .. '.' .. tostring(row.key or row.label or row.text or row.name or '')
                .. (first ~= nil and ('.' .. first) or '') .. (row.source ~= nil and ('.' .. row.source) or '')
                -- (a sub-panel's heading has its own: an effect_sub's key is its strength slider's too, and its switch at the
                -- line's end took the slider with it to its far end, Day & Night's Warmth to 2 and Darkness to 1, 2026-11-04)
                .. (row.kind == 'subheader' and '.sub' or '')
    -- (ImGui reads an id as a C string: a key with a NUL in it, a Layout slider's window and X/Y/Scale, ended there, and a window's
    -- three sliders were one control)
    local id = '##' .. (key:gsub('%z', '|'))
    local lx = x + S(LEFT)
    local right = x + w - S(RIGHT)
    local cy = y + S(ROW_H) / 2
    -- a sub-panel's heading is greyed by its panel's switch (nested, by the sub-panel it's in), never by the sub-panel before
    -- it: on True Font, an area switched off greyed the next area's switch (reported 2026-11-06)
    if row.kind == 'subheader' and row.nested ~= true then
        ctx.off = ctx.panel_off == true
        if row.own_panel and row.switch ~= nil then
            -- a headline that's a panel of its own (FSR, beside DLSS): its rows are greyed by its switch, not the section's
            local ok, on = pcall(row.switch.get)
            ctx.off = not (ok and on)
            ctx.panel_off = ctx.off
        end
    end
    local alpha, enabled = row_state(row, ctx)

    if row.kind == 'banner' then
        draw_banner(row, lx, right, y + S(4))
        return h
    elseif row.kind == 'layers' then
        draw_layers(row, id, lx, right, y)
        return h
    elseif row.kind == 'transfer' then
        draw_transfer(row, id, lx, right, y, h)
        return h
    elseif row.kind == 'textfile' then
        textfile.draw(row, id, lx, right, y)
        return h
    elseif row.kind == 'tabs' then
        -- a tab bar: the tab clicked picked (its rows shown in place of the others: row_height), kept in settings.lua
        local picked, changed = M.tabs(id, lx - S(M.TAB_PAD), y + S(2), right, row.labels, st.tabs[row.id] or 1, alpha)
        if changed then
            st.tabs[row.id] = picked
            st.picker = nil
            save_settings()
        end
        return h
    end

    local hovered = M.over(x, y, w, h)
    -- no hover band on a sub-panel's heading, nor on the Addons and Plugins pages' search row (controls, not a setting)
    if hovered and row.kind ~= 'subheader' and row.kind ~= 'extension_filter' and row.kind ~= 'controls' and row.kind ~= 'columns'
        and not BANNER_KINDS[row.kind] and not row.big then
        M.fill(dl, x + S(4), y, w - S(8), h, P.hover, S(3), 0, 0.8)
        if row.window ~= nil and vana.layout ~= nil then
            vana.layout.highlight(row.window) -- a Layout window's row: it's outlined in the game
        end
    end

    if row.kind == 'subheader' and row.headline then
        M.draw_headline(row, id, x, y, w, alpha, enabled, ctx.panel_head == true and st.hybrid == true)
        return h
    elseif row.kind == 'subheader' then
        -- a sub-panel's heading (sub_room below the row above, 8 above the next: row_height): its title in capitals, semibold
        -- italic and spaced, in teal (rose for danger), then a tag, a line in the title's colour and its switch
        -- (a group's heading, Shadows' Advanced Settings, in rose too: asked for in game, 2026-11-09)
        local color = (row.danger or row.group) and P.rose or P.teal
        cy = y + sub_room(row, sec) + S(ROW_H) / 2
        local line_end = right
        local title, spacing = row.text:upper(), S(1.2)
        local sub_font = fonts.semi_italic or fonts.semi
        local sw_on = nil
        if row.switch ~= nil then
            local ok, on = pcall(row.switch.get)
            sw_on = ok and on or false
            local new, flipped = M.switch(id, right - S(M.SW_W), cy, sw_on, enabled, alpha)
            if flipped then
                M.flush()
                pcall(row.switch.set, new)
                if row.section == nil or M.LIVE_GRAPHICS[row.section] then
                    vana.graphics.reload()
                end
                M.rebuild()
            end
            line_end = right - S(M.SW_W) - S(10)
        end
        -- row.modes (Lighting's Night: Off, Dark, Bright; data.lua) in place of a switch: segments at the line's end, each
        -- its own tooltip (modes.tips)
        local over_mode = nil
        if row.modes ~= nil then
            local ok, index = pcall(row.modes.get)
            local total = M.segments_width(row.modes.labels, SMALL)
            local picked, moved, over = M.segments(id .. '.modes', right - total, cy, row.modes.labels, ok and index or 1, enabled, SMALL, alpha)
            if moved then
                M.flush()
                pcall(row.modes.set, picked)
                vana.graphics.reload()
                M.rebuild()
            end
            over_mode = over ~= nil and row.modes.tips ~= nil and row.modes.tips[over] or nil
            if over_mode ~= nil then
                M.tooltip(over_mode)
            end
            line_end = right - total - S(10)
        end
        -- reset: this sub-panel's rows (to the next sub-panel) back to their defaults, its switches left as they are (the
        -- panel's Reset, draw_heading, puts those back too; Position without the colours)
        local sub_rows, after = {}, false
        for _, other in ipairs(ctx.list or sec.rows) do
            if after then
                if other.kind == 'subheader' then
                    break
                end
                sub_rows[#sub_rows + 1] = other
            elseif other == row then
                after = true
            end
        end
        -- (the hybrid layout's green headings have no reset: the section's own resets them all; the red ones, a group's, keep theirs)
        local no_reset = st.hybrid == true and not (row.danger or row.group)
        for _, other in ipairs(M.ROW_RESETS and not M.hide_resets and not no_reset and sub_rows or {}) do
            if M.resettable(other) or other.reset ~= nil then
                local reset_clicked, reset_hovered = M.icon_button(id .. '.reset', line_end - S(8), cy, 'reset', 9, P.text, P.faint)
                if reset_clicked then
                    M.reset_rows(sub_rows)
                    st.picker = nil
                end
                if reset_hovered then
                    M.tooltip('Reset ' .. row.text .. ' to the defaults')
                end
                line_end = line_end - S(24)
                break
            end
        end
        local tx = lx
        -- (a card's heading in the hybrid layout, M.hy.row: the line across under it in place of the one after the title)
        local card_head = ctx.panel_head == true and st.hybrid == true
        tx = tx + M.text_spaced(dl, tx, cy, title, color, sub_font, SMALL, alpha, spacing) + S(10)
        if row.tag ~= nil then
            M.text_mid(dl, tx, cy, row.tag, P.dim, fonts.text, SMALL - 0.5, alpha)
            tx = tx + M.measure(row.tag, fonts.text, SMALL - 0.5) + S(8)
        end
        local pill_w, over_pill = draw_dx_pill(dl, sec, row, tx, cy)
        if pill_w > 0 then
            tx = tx + pill_w + S(8)
        end
        if card_head then
            M.hline(dl, lx, right, cy + S(18), M.hy.LINE, 0.16)
        elseif line_end > tx then
            M.hline(dl, tx, line_end, cy, color, 0.8 * alpha)
        end
        local panel_tip = row.tip or M.TIPS.panels[row.text] -- (its own, where two tabs' sub-panels share a title)
        if hovered and not over_pill and over_mode == nil and panel_tip ~= nil then
            M.tooltip(panel_tip)
        end
        -- its rows greyed with its panel (ctx.off from the top of draw_row) or, a sub-panel inside another (row.nested), with
        -- that one (DLSS 5's Advanced Settings while DLSS 5 is off); and while its own switch is off
        ctx.off = ctx.off == true or (row.switch ~= nil and sw_on == false)
        return h
    elseif BANNER_KINDS[row.kind] then
        -- the one banner (banner_tone, banner_height): the rows' own width (lx to right, as the Layers cards), its tone fading
        -- left to right, a faint edge, the title in capitals over the wrapped text
        local tone = banner_tone(row)
        local color = P[BANNER_TONES[tone]]
        local lines = note_lines(row, note_width(w))
        local bx, by, bw, bh, r = lx, y + S(4), right - lx, h - S(8), S(4)
        local steps = 24
        M.fill(dl, bx, by, r * 2, bh, color, r, M.CORNERS.left, 0.18 * alpha)
        M.fill(dl, bx + bw - r * 2, by, r * 2, bh, color, r, M.CORNERS.right, 0.03 * alpha)
        for k = 0, steps - 1 do
            local x1 = bx + r * 2 + math.floor((bw - r * 4) * k / steps)
            local x2 = bx + r * 2 + math.floor((bw - r * 4) * (k + 1) / steps)
            M.fill(dl, x1, by, math.max(1, x2 - x1), bh, color, 0, 0, (0.18 - 0.15 * (k + 0.5) / steps) * alpha)
        end
        M.outline(dl, bx, by, bw, bh, color, r, 0, 1, 0.22 * alpha)
        local tx = bx + S(10)
        local ly = by + S(5) + S(NOTE_LINE) / 2
        if row.title then
            M.text_mid(dl, tx, ly, tostring(row.title):upper(), color, fonts.semi, BODY - 1, alpha)
            ly = ly + S(NOTE_LINE - 5)
        end
        for _, line in ipairs(lines) do
            M.text_mid(dl, tx, ly, line, P.text, fonts.text, BODY - 1, alpha)
            ly = ly + S(NOTE_LINE)
        end
        return h
    elseif row.kind == 'note' then
        -- an icon, then the text wrapped over as many lines as it takes
        local color = row.glyph_color == 'danger' and P.rose or row.glyph_color == 'experimental' and P.amber or P.dim
        local icon = row.glyph == 'warning' and 'warning' or row.glyph == 'favorite' and 'heart' or row.glyph == 'lock' and 'lock' or 'info'
        local lines = note_lines(row, note_width(w))
        local ly = y + S(4) + S(NOTE_LINE) / 2
        M.icon_centred(dl, lx + S(6), ly, icon, color, 10)
        for _, line in ipairs(lines) do
            M.text_mid(dl, lx + S(20), ly, line, P.text2, fonts.text, BODY - 1, alpha)
            ly = ly + S(NOTE_LINE)
        end
        return h
    elseif row.kind == 'scale' then
        -- the window's scale: a left / right selector over the usual sizes, the one set shown even when it isn't one of them
        M.text_mid(dl, lx, cy, row.label, P.text, fonts.text, BODY)
        local SCALES = { 75, 90, 100, 110, 125, 150, 175, 200 }
        local percent = math.floor(M.user_scale() * 100 + 0.5)
        local step = M.arrows(id, right - S(M.ARROWS_W), cy, percent .. '%', true)
        -- its reset before it, as every setting's: 100% (the key taken out)
        if M.reset_icon(id .. '.reset', right - S(M.ARROWS_W) - S(M.RESET_W), cy, percent ~= 100, true) then
            vana.config.set('vana', 'scale', nil)
            M.compute_scale(clock(), st.screen_h or 1080, 100, true)
        end
        if step ~= 0 then
            local next_percent = nil
            if step > 0 then
                for _, p in ipairs(SCALES) do
                    if p > percent and next_percent == nil then next_percent = p end
                end
            else
                for _, p in ipairs(SCALES) do
                    if p < percent then next_percent = p end
                end
            end
            if next_percent ~= nil then
                vana.config.set('vana', 'scale', next_percent ~= 100 and tostring(next_percent) or nil)
                M.compute_scale(clock(), st.screen_h or 1080, next_percent, true)
            end
        end
        local mx = imgui.GetMousePos()
        if hovered and mx < x + w * 0.5 and row.tip ~= nil then
            M.tooltip(row.tip)
        end
        return h
    elseif row.kind == 'info' then
        local entry = live_value(row)
        -- row.delete ({ tip, run }): a delete icon at the end of the row (True Font's Custom Fonts), its text before it
        local edge = row.delete ~= nil and right - S(26) or right
        if not row.big then
            M.text_mid(dl, lx, cy, row.label, P.dim, fonts.text, BODY)
        end
        local room = edge - lx - M.measure(row.label, fonts.text, BODY) - S(16)
        local full = entry.text .. (entry.aside ~= nil and ('  ' .. entry.aside) or '')
        local size = M.measure(full, fonts.text, BODY) > room and SMALL or BODY
        if row.big then -- (Client > VanaCore's Version: its label in grey and its value larger, bold, in bright yellow, centred)
            local bold, gap = fonts.bold or fonts.semi, S(8)
            local lw, vw = M.measure(row.label, fonts.text, BODY), M.measure(entry.text, bold, BODY + 4)
            local x = math.floor((lx + edge - lw - gap - vw) / 2)
            M.text_mid(dl, x, cy, row.label, P.dim, fonts.text, BODY)
            M.text_mid(dl, x + lw + gap, cy, entry.text, '#FFE04A', bold, BODY + 4)
        elseif entry.aside ~= nil then
            M.text_right_mid(dl, edge, cy, entry.aside, P.dim, fonts.text, size)
            M.text_right_mid(dl, edge - M.measure(entry.aside, fonts.text, size) - S(6), cy, entry.text, P.text, fonts.text, size)
        else
            M.text_right_mid(dl, edge, cy, entry.text, P.text, fonts.text, size)
        end
        if row.delete ~= nil then
            -- asked twice: the first click turns the icon red, a second within 3 seconds deletes
            local now = clock()
            local confirm = st.confirm
            local asking = confirm ~= nil and confirm.name == 'delete:' .. row.label and now - confirm.at < 3
            local clicked, hov = M.icon_button(id .. '.delete', right - S(10), cy, 'trash', 13, P.rose, asking and P.rose or P.faint)
            if hov then
                M.tooltip(asking and 'Click again to delete it' or row.delete.tip)
            end
            if clicked then
                if asking then
                    st.confirm = nil
                    pcall(row.delete.run)
                    M.rebuild()
                else
                    st.confirm = { name = 'delete:' .. row.label, at = now }
                end
            end
        end
        return h
    elseif row.kind == 'status' then
        if sec.title == 'Gamepads' and pad_guid(live_value(row).text) then
            draw_pad(row, lx, right, y)
            return h
        end
        local entry = live_value(row)
        M.text_mid(dl, lx, cy, row.label, P.dim, fonts.text, BODY, alpha)
        local room = right - lx - M.measure(row.label, fonts.text, BODY) - S(16)
        local size = M.measure(entry.text, fonts.text, BODY) > room and SMALL or BODY
        local tone = sec.title == 'Gamepads' and gamepad_status_tone(row.label, entry.text) or nil
        local shown = M.fit(entry.text, room - (tone ~= nil and S(14) or 0), fonts.text, size)
        M.text_right_mid(dl, right, cy, shown, tone or P.text, fonts.text, size, alpha)
        if tone ~= nil then -- a dot before the value: teal working, rose off, grey nothing
            M.circle(dl, right - M.measure(shown, fonts.text, size) - S(9), cy, S(3), tone, alpha)
        end
        return h
    elseif row.kind == 'layout' then
        -- an addon's window on the Layout page: its name, then its X, Y and Scale sliders along the row (data.lua:
        -- layout.rows). The name's tooltip says when it isn't shown or its addon places it. A chat window's place (data.lua:
        -- mover) is drawn the same, its Move switch at the row's end locking the sliders while it's off
        local title_w = S(row.title_w or LAYOUT_TITLE_W)
        local name = M.fit(row.label, title_w - S(8), fonts.text, BODY)
        local on = true
        if row.toggle ~= nil then
            local ok, value = pcall(row.toggle.get)
            on = ok and value or false
            local new, flipped = M.switch(id .. '.move', right - S(M.SW_W), cy, on, enabled, alpha)
            if flipped then
                M.flush()
                pcall(row.toggle.set, new)
                on = new
            end
            right = right - S(M.SW_W)
        elseif row.modes ~= nil then
            -- (data.lua: placed) its modes at the row's end in place of the switch: Game, Chat, Custom
            local ok, index = pcall(row.modes.get)
            index = ok and index or 1
            local total = M.segments_width(row.modes.labels, SMALL)
            local picked, moved = M.segments(id .. '.mode', right - total, cy, row.modes.labels, index, enabled, SMALL, alpha)
            if moved then
                M.flush()
                pcall(row.modes.set, picked)
            end
            right = right - total
        end
        if row.toggle ~= nil or row.modes ~= nil then
            -- the row's reset before its switch: the edges (no room for one each) and the switch back
            right = right - M.row_reset(row, id, right - S(M.RESET_W), cy, enabled, alpha) - S(14)
        end
        if row.flags ~= nil then -- (an addon's window: its Lock and No Pointer Events at the row's end)
            right = right - M.icon_switches(row.flags, id .. '.flags', right, cy, enabled) - S(8)
        end
        M.text_mid(dl, lx, cy, name, (row.tag == 'Not shown' or not on) and P.dim or P.text, fonts.text, BODY, alpha)
        if (row.tag ~= nil or row.tip ~= nil or name ~= row.label) and M.over(lx, y, title_w, h) then
            M.tooltip(row.tag ~= nil and (row.label .. ' (' .. row.tag:lower() .. ')') or row.tip or row.label)
        end
        -- the labels and the values take their widths, the tracks share what's left
        local sx, fixed = lx + title_w, 0
        for i, sub in ipairs(row.sliders) do
            sub.label_w = M.measure(sub.label, fonts.text, SMALL) + S(row.label_gap or 5)
            sub.value_w = S(sub.value_w_base or (sub.label == 'Scale' and 38 or 32))
            fixed = fixed + sub.label_w + sub.value_w + S(8) + (sub.no_reset and 0 or S(20)) + (i > 1 and S(10) or 0)
        end
        local slots = math.max(row.slots or 0, #row.sliders)
        if slots > #row.sliders then -- room kept for the slots it hasn't, as wide as its last (labelled row.slot_label)
            local last = row.sliders[#row.sliders]
            local label_w = row.slot_label ~= nil and M.measure(row.slot_label, fonts.text, SMALL) + S(row.label_gap or 5) or last.label_w
            fixed = fixed + (slots - #row.sliders) * (label_w + last.value_w + S(8) + (last.no_reset and 0 or S(20)) + S(10))
        end
        local track = math.max(S(24), (right - sx - fixed) / slots)
        for i, sub in ipairs(row.sliders) do
            if i > 1 then
                sx = sx + S(10)
            end
            local sub_on = enabled
            if sub.lock_if ~= nil then
                local ok, locked = pcall(sub.lock_if)
                sub_on = sub_on and not (ok and locked)
            end
            M.text_mid(dl, sx, cy, sub.label, P.dim, fonts.text, SMALL, alpha)
            sx = sx + sub.label_w
            local width = track + sub.value_w + S(8) + (sub.no_reset and 0 or S(20))
            -- (by its setting: two rows of a name in one section, the Chat window's Opacity and the Chat input's, were one control)
            local sub_id = id .. '.' .. (tostring(sub.key or sub.label):gsub('%z', '|'))
            if sub.kind == 'colour' then -- (a colour pair: its reset and swatch where a slider's are, so the rows line up)
                draw_colour(sub, sub_id, nil, cy, sub_on, alpha, sx + M.row_reset(sub, sub_id, sx, cy, sub_on, alpha))
            else
                draw_slider(sub, sub_id, sx, cy, sub_on, alpha, width, sub.value_w)
            end
            if M.TIPS.rows[(sub.section or '') .. '.' .. (sub.key or '')] ~= nil and M.over(sx - sub.label_w, y, sub.label_w, h) then
                M.tooltip(M.TIPS.rows[sub.section .. '.' .. sub.key])
            end
            sx = sx + width
        end
        if row.extras ~= nil then -- (its switches and colours in the slots it hasn't sliders for)
            draw_extras(row, id, sx + S(10), right, cy, enabled, alpha)
        end
        return h
    elseif row.kind == 'columns' then
        -- sliders in columns side by side (Lighting's Night: Dark night | Bright night; data.lua), each column its title,
        -- then each slider's name over its track. A column whose dim_if says so is drawn at 40% (the night not picked; 60%
        -- till fainter was asked for, 2026-11-07), still settable, as a greyed sub-panel's rows are
        local gap = S(28)
        local cw = (right - lx - gap * (#row.columns - 1)) / #row.columns
        for c, col in ipairs(row.columns) do
            local cx = lx + (c - 1) * (cw + gap)
            local ca = alpha
            if col.dim_if ~= nil then
                local ok, dim = pcall(col.dim_if)
                if ok and dim then
                    ca = math.min(ca, 0.4)
                end
            end
            M.text_mid(dl, cx, y + S(12), col.title, P.text, fonts.semi, SMALL, ca)
            if col.tip ~= nil and M.over(cx, y, M.measure(col.title, fonts.semi, SMALL), S(24)) then
                M.tooltip(col.tip)
            end
            local iy = y + S(24)
            for _, sub in ipairs(col.rows) do
                M.text_mid(dl, cx, iy + S(10), sub.label, P.text2, fonts.text, SMALL + 0.5, ca)
                local tip = M.TIPS.rows[(sub.section or '') .. '.' .. (sub.key or '')]
                if tip ~= nil and M.over(cx, iy, M.measure(sub.label, fonts.text, SMALL + 0.5), S(20)) then
                    M.tooltip(tip)
                end
                draw_slider(sub, id .. '.' .. sub.key, cx, iy + S(20) + S(ROW_H) / 2, enabled, ca, cw, S(38))
                iy = iy + S(20) + S(ROW_H)
            end
        end
        return h
    elseif row.kind == 'controls' then
        -- a panel of what the mouse does, side by side: each its keys as key caps, what it does under them
        local py, ph = y + S(2), S(CONTROLS_H)
        M.fill(dl, lx, py, right - lx, ph, P.well, S(5), 0, 0.6 * alpha)
        M.outline(dl, lx, py, right - lx, ph, P.well_edge, S(5), 0, 1, alpha)
        local n = #row.items
        local cw = (right - lx) / n
        local kh, kcy, tcy = S(22), py + S(24), py + S(52)
        for i, item in ipairs(row.items) do
            local cx = lx + cw * (i - 1)
            if i > 1 then
                M.vline(dl, cx, py + S(12), py + ph - S(12), P.well_edge)
            end
            -- the keys' width, + between them, to centre them in the column
            local total = 0
            for k, key in ipairs(item.keys) do
                total = total + M.measure(key, fonts.semi, SMALL) + S(16) + (k > 1 and S(18) or 0)
            end
            local kx = cx + (cw - total) / 2
            for k, key in ipairs(item.keys) do
                if k > 1 then
                    M.text_mid(dl, kx + S(5), kcy, '+', P.dim, fonts.text, SMALL, alpha)
                    kx = kx + S(18)
                end
                local kw = M.measure(key, fonts.semi, SMALL) + S(16)
                M.fill(dl, kx, kcy - kh / 2, kw, kh, P.bg, S(4), 0, alpha)
                M.fill(dl, kx, kcy + kh / 2 - S(2), kw, S(2), P.well_edge, S(4), 0, alpha) -- a key cap's lower lip
                M.outline(dl, kx, kcy - kh / 2, kw, kh, P.border, S(4), 0, 1, alpha)
                M.text_mid(dl, kx + S(8), kcy, key, P.hud, fonts.semi, SMALL, alpha)
                kx = kx + kw
            end
            local tw = M.measure(item.text, fonts.text, SMALL)
            M.text_mid(dl, cx + (cw - tw) / 2, tcy, item.text, P.text2, fonts.text, SMALL, alpha)
        end
        return h
    elseif row.kind == 'font' then
        draw_font_row(row, id, lx, right, cy, enabled, alpha)
        return h
    elseif row.kind == 'colours' then
        M.draw_colours_row(row, id, lx, right, cy, enabled, alpha)
        return h
    elseif row.kind == 'action' and row.big then
        -- a button alone on its line, in the middle of the page: taller, its text semibold in the danger red (Shadows'
        -- Toggle Advanced Settings; asked for, 2026-11-12). Its row.run only
        local bw = M.measure(row.text, fonts.semi, BODY + 1) + S(40)
        if M.button(id, math.floor((lx + right - bw) / 2), y + h / 2, row.text, nil, 'danger', enabled, alpha, 30, bw, fonts.semi, BODY + 1) then
            pcall(row.run)
        end
        if hovered and tip_of(row) ~= nil then
            M.tooltip(tip_of(row))
        end
        return h
    elseif row.kind == 'action' then
        local label = row.label ~= row.text and row.label or ''
        M.text_mid(dl, lx, cy, label, row.value ~= nil and enabled and P.text or P.dim, fonts.text, BODY, alpha) -- (a value: a setting's name)
        local _, over_pill = draw_dx_pill(dl, sec, row, lx + (label ~= '' and M.measure(label, fonts.text, BODY) + S(8) or 0), cy)
        draw_action(row, id, right, cy, enabled, alpha)
        if row.more ~= nil then
            -- row.more: more buttons on its line before its own, in their order (the damage numbers' tests), each its own
            -- id, and each button's tooltip its own
            local main_w = M.button_width(row.text, row.icon)
            local bx = right - main_w
            for _, more in ipairs(row.more) do
                bx = bx - S(8) - M.button_width(more.text, more.icon)
            end
            local tipped = M.over(right - main_w, cy - S(12), main_w, S(24)) and tip_of(row) or nil
            for i, more in ipairs(row.more) do
                local bw = M.button_width(more.text, more.icon)
                draw_action(more, id .. '.more' .. i, bx + bw, cy, enabled, alpha)
                if M.over(bx, cy - S(12), bw, S(24)) then
                    tipped = tip_of(more)
                end
                bx = bx + bw + S(8)
            end
            if hovered and tipped ~= nil then
                M.tooltip(tipped)
            end
        elseif hovered and not over_pill and tip_of(row) ~= nil then
            M.tooltip(tip_of(row))
        end
        return h
    elseif row.kind == 'extension' then
        draw_extension(row, id, x, w, lx, right, cy, alpha)
        return h
    elseif row.kind == 'overlay' then
        draw_overlay(row, id, lx, right, cy, alpha)
        return h
    elseif row.kind == 'extension_filter' then
        M.draw_extension_filter(id, row.what, lx, right, cy + S(FILTER_GAP), alpha) -- the Addons and Plugins pages' search and filters (addons.lua)
        return h
    end

    -- a setting: its name (and "restart" after it when it takes effect at the next start), its control
    -- (a library plant: lit with a share; while one is shown alone, only that one)
    local lit = enabled and (row.asset == nil or M.read(row) >= 1)
    if row.asset_id ~= nil and st.grass_solo ~= nil then
        lit = enabled and row.asset_id == st.grass_solo
    end
    if row.sub then
        -- an option under the row above: a bent arrow, the name set in; dimmer only while it's locked (its lock_if: the
        -- row above off), its name as bright as any other while it can be set
        M.sub_arrow(dl, lx + S(4), cy - S(ROW_H) / 2, cy, 12, P.faint, alpha)
        M.text_mid(dl, lx + S(22), cy, row.label, lit and (row.colour or P.text) or P.dim, row.bold and fonts.semi or fonts.text, BODY, lit and alpha or alpha * 0.6)
    else
        -- (row.colour: its name in that colour, its switch's track too, draw_control: the Buttons tabs' Enable; row.bold: its
        -- name in the semibold, True Font's Disable)
        M.text_mid(dl, lx, cy, row.label, lit and (row.colour or P.text) or P.dim, row.bold and fonts.semi or fonts.text, BODY, alpha)
    end
    if row.restart then
        -- (after the name where it's drawn: a sub row's is set in)
        M.text_mid(dl, (row.sub and lx + S(22) or lx) + M.measure(row.label, row.bold and fonts.semi or fonts.text, BODY) + S(5), cy - S(1), '*', P.amber,
                   fonts.semi, BODY + 1, alpha)
    end
    if row.asset_own then
        -- a red × beside the name: the plant out of the library, asked twice
        local now = clock()
        local confirm = st.confirm
        local asking = confirm ~= nil and confirm.name == 'asset:' .. row.asset_id and now - confirm.at < 3
        local xx = lx + M.measure(row.label, fonts.text, BODY) + S(16)
        local clicked, hov = M.icon_button(id .. '.delete', xx, cy, 'close', 11, P.rose, asking and P.rose or P.faint)
        if hov then
            M.tooltip('Delete this plant from your library (config\\grass\\' .. row.asset_id .. '.json)')
        end
        if clicked then
            if asking then
                st.confirm = nil
                delete_asset(row.asset_id)
            else
                st.confirm = { name = 'asset:' .. row.asset_id, at = now }
            end
        end
    end
    -- a library plant's eye, after its name (and ×): it alone, whole, till clicked again or the window closes (M.grass_solo)
    local after = row.asset_own and 22 or 0
    if row.asset_id ~= nil and vana.graphics.grass_solo ~= nil then
        local alone = st.grass_solo == row.asset_id
        local ex = lx + M.measure(row.label, fonts.text, BODY) + S(after + 16)
        local clicked, hov = M.icon_button(id .. '.solo', ex, cy, 'eye', 11, P.teal, alone and P.teal or P.faint, enabled)
        if hov then
            M.tooltip(alone and 'Showing only this one: click to put the mix back' or 'Show only this one, to see what it looks like')
        end
        if clicked then
            M.grass_solo(not alone and row.asset_id or nil)
        end
        after = after + 22
    end
    -- the renderer it needs, after the name (and its "*", × or eye)
    local name_end = (row.sub and lx + S(22) or lx) + M.measure(row.label, fonts.text, BODY)
    local pill_x = name_end + S(after > 0 and after + 8 or row.restart and 18 or 8)
    local pill_w, over_pill = draw_dx_pill(dl, sec, row, pill_x, cy)
    -- where the name and what's after it (its "*", ×, eye, pill) end: the control mustn't run into it
    local label_end = pill_w > 0 and pill_x + pill_w or name_end + S(after > 0 and after or row.restart and 14 or 0)
    if row.hint ~= nil then
        -- a short note after all that, dim and smaller (data.lua's hinted: Max FPS's "before frame gen")
        local hx = label_end + S(8)
        M.text_mid(dl, hx, cy, row.hint, P.dim, fonts.text, SMALL - 0.5, alpha)
        label_end = hx + M.measure(row.hint, fonts.text, SMALL - 0.5)
    end
    if sec.title == 'Gamepads' and row.key == 'gamepad.guid' then
        -- the rescan beside the choice (the Rescan gamepads action's), locked while gamepads are disabled
        local pads_off = (tonumber(vana.config.get('input', 'gamepad.disable') or '') or 0) ~= 0
        local clicked, hov = M.icon_button(id .. '.rescan', right - S(10), cy, 'reload', 11, P.text, P.dim, not pads_off)
        if hov and not pads_off then
            M.tooltip('Looks for gamepads again, as plugging one in does')
        end
        if clicked then
            pcall(M.gamepad_rescan)
        end
        right = right - S(26)
    end
    draw_control(page, sec, row, id, right, cy, enabled, alpha, label_end)

    -- the description as a tooltip, once the mouse has rested on the name's side of the row (the pill has its own)
    local mx = imgui.GetMousePos()
    if hovered and not over_pill and mx < x + w * 0.5 then
        local text = tip_of(row)
        if text ~= nil then
            local now = clock()
            if st.hover_key ~= key then
                st.hover_key, st.hover_since = key, now
            elseif now - st.hover_since > 0.4 then
                M.tooltip(text)
            end
            st.hover_seen = true
        end
    end
    return h
end

-------------------------------------------------------------------------------
-- Sections
-------------------------------------------------------------------------------

local HEAD_H = 36
-- a section's title: bold italic, white, bigger than the rows' (a sub-panel's is small capitals in a band)
local function head_font() return fonts.bold_italic or fonts.semi, BODY + 5 end

-- A section's heading: its title, a tag, a line to the right, its reset and its switch at the end. Returns whether the
-- panel's effect or switch is off.
local function draw_heading(page, sec, x, y, w, over_title)
    local dl = imgui.GetWindowDrawList()
    local head = sec.head
    local cy = y + S(HEAD_H) / 2 + S(2)
    local alpha, enabled = row_state(head, {})
    local off = false
    local line_end = x + w - S(RIGHT)
    local switch_id = '##' .. sec.key .. '.switch'
    if head.effect then
        -- an effect's switch: on while it isn't switched off and its strength is above 0
        local on = M.effect_on(head)
        local new, flipped = M.switch(switch_id, line_end - S(M.SW_W), cy, on, enabled and M.effects_available(), alpha)
        if flipped then
            M.flush()
            M.set_effect(head, new)
            vana.graphics.reload()
            M.rebuild()
        end
        off = not on
        line_end = line_end - S(M.SW_W) - S(10)
    elseif head.switch ~= nil then
        local ok, on = pcall(head.switch.get)
        on = ok and on or false
        local new, flipped = M.switch(switch_id, line_end - S(M.SW_W), cy, on, enabled, alpha)
        if flipped then
            M.flush()
            pcall(head.switch.set, new)
            if head.section == nil or M.LIVE_GRAPHICS[head.section] then
                vana.graphics.reload()
            end
            M.rebuild()
        end
        off = not on
        line_end = line_end - S(M.SW_W) - S(10)
    end
    -- reset: this section's settings back to their defaults
    local has_settings = false
    for _, row in ipairs(sec.rows) do
        if M.resettable(row) or row.reset ~= nil then
            has_settings = true
            break
        end
    end
    -- Reset: everything in the panel back to its default, its switches too (data.lua reset_rows with switches) and the
    -- settings its heading names that aren't on the panel (reset_also); the heading's own switch (the panel's on/off) stays as
    -- it is. A button with its name, asked twice as Vanilla UI is. Till 2026-11-23 an arrow that left every switch, so a
    -- player's "reset your shadows" kept Blend with the game's shading on ("the main reset on the top should reset
    -- EVERYTHING for that category"). A sub-panel's arrow (draw_row's, on its line) still leaves its switches.
    if has_settings or M.resettable(head) then
        local now, confirm_id = clock(), '##reset.' .. sec.key
        local asking = st.confirm ~= nil and st.confirm.name == confirm_id and now - st.confirm.at < 3
        local bw = math.max(M.button_width('Reset', 'reset'), M.button_width('Sure?', 'reset'))
        local clicked = M.button('##' .. sec.key .. '.reset', line_end - bw, cy, asking and 'Sure?' or 'Reset', 'reset',
                                 asking and 'danger' or 'neutral', true, nil, nil, bw)
        if imgui.IsItemHovered() then
            M.tooltip(asking and 'Click again to put everything in ' .. sec.title .. ' back to its default'
                      or 'Everything in ' .. sec.title .. ' back to its default, its switches too.'
                         .. ((head.effect or head.switch ~= nil) and '\nThe switch beside this stays as it is.' or ''))
        end
        if clicked then
            if asking then
                st.confirm = nil
                local rows = { head }
                for _, row in ipairs(with_assets(sec.rows)) do
                    rows[#rows + 1] = row
                end
                M.reset_rows(rows, true)
                st.picker = nil
            else
                st.confirm = { name = confirm_id, at = now }
            end
        end
        line_end = line_end - bw - S(10)
    end
    -- an addon's page (Layout's section): Unload beside the reset, what its switch on the List does when switched off
    -- (unloaded, and not loaded at the next start either). Its windows go with it, so the List shows after.
    local ext = page.id == 'addons' and head.extension or nil
    if ext ~= nil and not M.extension_fixed(ext.what, ext.name) then
        local unload_clicked, unload_hovered = M.icon_button('##' .. sec.key .. '.unload', line_end - S(8), cy, 'eject', 9, P.rose, P.faint)
        if unload_clicked then
            M.flush()
            M.set_extension({ what = ext.what, name = ext.name, source = ext.source }, false)
            st.sub[page.id] = ADDONS_ALL.title
            M.layout.page = 'list'
            st.picker = nil
            save_settings()
            M.rebuild()
        end
        if unload_hovered then
            M.tooltip('Unload ' .. ext.name .. ' (as its switch on the List: it stays off at the next start)')
        end
        line_end = line_end - S(24)
    end
    -- the title lines up with the rows' names
    local tx = x + S(LEFT)
    local title_color = head.danger and P.rose or head.warn and P.amber or P.white
    local head_face, head_size = head_font()
    -- the hybrid layout's panel (M.hy.card): a line under the heading in a card's (a page's title, 'title' mode, is a card of its
    -- own with only the title in it: no line)
    local card = st.hybrid == true and M.hy.mode ~= nil
    local titled = card and M.hy.mode == 'title'
    local function title_draw(px, text, colour)
        text = M.hy.caps(text) -- (the hybrid layout's titles in capitals)
        M.text_mid(dl, px, cy, text, colour, head_face, head_size, alpha)
        return M.measure(text, head_face, head_size)
    end
    local title_w = M.measure(M.hy.caps(sec.title), head_face, head_size)
    if over_title ~= nil then
        M.text_mid(dl, tx, cy, over_title, P.dim, fonts.text, SMALL)
        tx = tx + M.measure(over_title, fonts.text, SMALL) + S(6)
    end
    local hovered = M.over(tx, cy - S(12), title_w, S(24)) -- (the title: its tooltip)
    if head.parts ~= nil then
        -- a title in parts, each green while it's the one running (DLSS / FSR: one or the other), else white: no title is greyed
        local px = tx
        for _, part in ipairs(head.parts) do
            local ok, active = pcall(part[2] or function () return false end)
            local colour = (ok and active) and P.green or title_color
            px = px + title_draw(px, part[1], colour)
        end
        title_w = px - tx
    else
        title_draw(tx, sec.title, off and M.mix(title_color, P.dim, 0.5) or title_color)
    end
    tx = tx + title_w + S(8)
    if head.icon ~= nil then
        M.icon_centred(dl, tx + S(6), cy, head.icon, P.dim, 10, alpha)
        tx = tx + S(20)
    end
    if head.warn and not head.danger then
        M.text_mid(dl, tx, cy, 'in progress', P.amber, fonts.text, SMALL - 0.5, alpha)
        tx = tx + M.measure('in progress', fonts.text, SMALL - 0.5) + S(8)
    end
    local pill_w, over_pill = draw_dx_pill(dl, sec, head, tx, cy)
    if pill_w > 0 then
        tx = tx + pill_w + S(8)
    end
    -- a tag after the title (a string, or a function that says it as it's drawn: the Addons list's "7 of 31 loaded")
    local tag = head.tag
    if type(tag) == 'function' then
        local ok, text = pcall(tag)
        tag = ok and text or nil
    end
    if type(tag) == 'string' and tag ~= '' then
        M.text_mid(dl, tx, cy, tag, P.dim, fonts.text, SMALL - 0.5, alpha)
        tx = tx + M.measure(tag, fonts.text, SMALL - 0.5) + S(8)
    end
    if hovered and not over_pill and M.TIPS.panels[sec.title] ~= nil then
        M.tooltip(M.TIPS.panels[sec.title])
    end
    if not card and line_end > tx then -- (the hybrid layout's main titles have no line)
        M.hline(dl, tx, line_end, cy, P.line)
    end
    return off
end

-- A sub-panel's heading drawn as a section's is (row.headline: DLSS 5 in the DLSS panel): the bold title, its Reset
-- button (row.reset, asked twice) and its switch at the line's end, a line between. It greys nothing after it. Its height
-- is row_height's (8 above, HEAD_H, 2 below). A field of M: this chunk is near LuaJIT's 200 locals.
M.draw_headline = function (row, id, x, y, w, alpha, enabled, carded)
    local dl = imgui.GetWindowDrawList()
    local cy = y + S(8) + S(HEAD_H) / 2 + S(2)
    local line_end = x + w - S(RIGHT)
    local off = false
    if row.switch ~= nil then
        local ok, on = pcall(row.switch.get)
        on = ok and on or false
        local new, flipped = M.switch(id .. '.switch', line_end - S(M.SW_W), cy, on, enabled, alpha)
        if flipped then
            M.flush()
            pcall(row.switch.set, new)
            if row.section == nil or M.LIVE_GRAPHICS[row.section] then
                vana.graphics.reload()
            end
            M.rebuild()
        end
        off = not on
        line_end = line_end - S(M.SW_W) - S(10)
    end
    if row.reset ~= nil then
        local now, confirm_id = clock(), id .. '.reset'
        local asking = st.confirm ~= nil and st.confirm.name == confirm_id and now - st.confirm.at < 3
        local bw = math.max(M.button_width('Reset', 'reset'), M.button_width('Sure?', 'reset'))
        local clicked = M.button(id .. '.button', line_end - bw, cy, asking and 'Sure?' or 'Reset', 'reset',
                                 asking and 'danger' or 'neutral', true, nil, nil, bw)
        if imgui.IsItemHovered() then
            M.tooltip(asking and 'Click again to put everything in ' .. row.text .. ' back to its default'
                      or 'Everything in ' .. row.text .. ' back to its default.\nThe switch beside this stays as it is.')
        end
        if clicked then
            if asking then
                st.confirm = nil
                pcall(row.reset)
                st.picker = nil
            else
                st.confirm = { name = confirm_id, at = now }
            end
        end
        line_end = line_end - bw - S(10)
    end
    local head_face, head_size = head_font()
    local tx = x + S(LEFT)
    local title = M.hy.caps(row.text) -- (the hybrid layout's titles in capitals)
    local title_w = M.measure(title, head_face, head_size)
    local hovered = M.over(tx, cy - S(12), title_w, S(24))
    -- (green_on: its title green while its switch is on)
    local colour = (row.green_on and not off) and P.green or P.white
    M.text_mid(dl, tx, cy, title, colour, head_face, head_size) -- (full alpha: a panel's heading isn't greyed with its rows)
    tx = tx + title_w + S(8)
    if hovered and M.TIPS.panels[row.text] ~= nil then
        M.tooltip(M.TIPS.panels[row.text])
    end
    if not carded and line_end > tx then -- (carded, the hybrid layout's: no line)
        M.hline(dl, tx, line_end, cy, P.line)
    end
end

-- a section: its heading (none for the rows before the first), then its rows; returns its height
-- A group's rows (list[a..b], under its heading: Shadows' Advanced Settings) inset in a sunken box, as the status rows
-- are: their heights first, then the box, then the rows drawn narrower inside it. A field of M, not a local: this chunk is
-- near LuaJIT's 200 locals. Returns the y after.
M.draw_group = function (page, sec, list, a, b, x, y, w, ctx)
    -- the box from the page's names to its controls' ends (lx to right: draw_row), as its heading's line runs, so it stands
    -- off the page's edges (it ran to them, the divider and the scrollbar: asked for in game, 2026-11-09); the rows set
    -- in so their hover band (4 in from a row's edges: draw_row) stands 6 off the box on every side
    local inset, inset_right = S(LEFT) + S(2), S(RIGHT) + S(2)
    local room = note_width(w - inset - inset_right)
    local total = 0
    for k = a, b do
        total = total + row_height(list[k], sec, room)
    end
    local bx, by = x + S(LEFT), y + S(2)
    local bw, bh = w - S(LEFT) - S(RIGHT), total + S(14)
    local view_top, view_bottom = st.view_top, st.view_bottom
    if view_top == nil or (by + bh >= view_top and by <= view_bottom) then
        local dl = imgui.GetWindowDrawList()
        -- (darker than the status rows' box: P.side_l, the list of pages' left edge, solid; asked for in game, 2026-11-09)
        M.fill(dl, bx, by, bw, bh, P.side_l, S(5), 0, 1)
        M.outline(dl, bx, by, bw, bh, P.line, S(5))
    end
    local ry = y + S(8)
    for k = a, b do
        ry = ry + draw_row(page, sec, list[k], x + inset, ry, w - inset - inset_right, ctx)
    end
    return math.max(ry, by + bh) + S(6)
end

-- `hy`: in the hybrid layout, where the sections are panels (M.hy.begin: ctx.cards)
local function draw_section(page, sec, x, y, w, over_title, rows, hy)
    local ry = y
    M.hide_resets = sec.title == 'DLSS' -- (no reset icons beside the DLSS panel's settings: draw.lua reset_icon)
    local ctx = { off = false, panel_off = false } -- (panel_off: the heading's switch or effect, for each sub-panel's: draw_row)
    -- status rows (what the panel reports, not a setting: DLSS's Resolution and Frame generation, the gamepads) sit in
    -- a sunken box, a run of them in one
    local list = rows or sec.rows
    ctx.list = list -- (a sub-panel's reset: the rows under it, as drawn)
    M.hy.mode = nil
    local cards = hy and M.hy.begin(ctx, page, sec, list, x, w, rows) or nil
    -- (a bare heading isn't drawn but in a search's results: Client > VanaCore's, the banner under it says it)
    if sec.head ~= nil and not (sec.head.bare and st.search[1] == '') then
        -- (the heading in a card: with the rows after it, or alone when the cards are the sub-panels' under it)
        if cards ~= nil then
            M.hy.open(cards, cards.key .. '|head', y, S(HEAD_H) + S(2) + (cards.mode == 'card' and M.hy.span(cards, 1, false) + S(2) or 0))
        end
        local off = draw_heading(page, sec, x, y, w, over_title)
        ctx.off, ctx.panel_off = off, off
        ry = y + S(HEAD_H) + S(2)
        if cards ~= nil and cards.mode == 'title' then
            M.hy.close(cards, ry)
            ry = ry + S(10) -- (room before the cards)
        elseif cards ~= nil then
            ry = ry + S(M.hy.TITLE_PAD) -- (room under the title)
        end
    end
    local i = 1
    while i <= #list do
        local row = list[i]
        if cards ~= nil then
            M.hy.row(ctx, row, i, ry)
        end
        if row.kind == 'status' then
            local j, total = i, 0
            while j <= #list and list[j].kind == 'status' do
                total = total + row_height(list[j], sec, note_width(w))
                j = j + 1
            end
            if total > 0 then
                local bx, bw = x + S(LEFT) - S(8), w - S(LEFT) - S(RIGHT) + S(16)
                local by, bh = ry + S(2), total + S(4)
                local view_top, view_bottom = st.view_top, st.view_bottom
                if view_top == nil or (by + bh >= view_top and by <= view_bottom) then
                    local dl = imgui.GetWindowDrawList()
                    M.fill(dl, bx, by, bw, bh, P.well, S(5), 0, 0.7)
                    M.outline(dl, bx, by, bw, bh, P.line, S(5))
                end
                ry = ry + S(4)
                for k = i, j - 1 do
                    ry = ry + draw_row(page, sec, list[k], x, ry, w, ctx)
                end
                ry = ry + S(4)
            end
            i = j
        elseif row.boxed and st.search[1] == '' then
            -- one row alone in the Advanced Settings' box (Disable True Font)
            ry = M.draw_group(page, sec, list, i, i, x, ry, w, ctx)
            i = i + 1
        elseif row.group and st.search[1] == '' then
            -- a group of sub-panels (Shadows' Advanced Settings): its heading, then its rows inset in a box (M.draw_group)
            -- up to a sub-panel that isn't in it, a heading or the status rows. A heading hidden by its show_if (no
            -- height: M.toggle_group's button hasn't shown it) takes its rows with it; a search finds them all the same
            local hidden = row_height(row, sec, note_width(w)) == 0
            ry = ry + draw_row(page, sec, row, x, ry, w, ctx)
            local j = i + 1
            while j <= #list and list[j].kind ~= 'status' and list[j].kind ~= 'header'
                and not (list[j].kind == 'subheader' and not list[j].in_group) do
                j = j + 1
            end
            if not hidden then
                ry = M.draw_group(page, sec, list, i + 1, j - 1, x, ry, w, ctx)
            end
            i = j
        else
            local rh = draw_row(page, sec, row, x, ry, w, ctx)
            ry = ry + rh
            -- (a panel's rows in the hybrid layout: a faint line between one and the next)
            if cards ~= nil and cards.cur ~= nil and rh > 0 and i < #list and not M.hy.NOSEP[row.kind] and list[i + 1].kind ~= 'subheader' then
                M.hline(imgui.GetWindowDrawList(), x + S(LEFT), x + w - S(RIGHT), ry, M.hy.LINE, 0.07)
            end
            i = i + 1
        end
    end
    if cards ~= nil then
        M.hy.finish(cards, ry)
    end
    M.hide_resets = false
    return ry - y + S(4)
end


-------------------------------------------------------------------------------
-- The Presets page
-------------------------------------------------------------------------------

-- Bundled presets: Low, Medium, High and Max (QUALITY) as a list under the saved ones on the Presets page, each with
-- a Load; the one the settings are at in cyan, as the loaded saved preset is. Returns the height drawn.
local BUNDLED_WORDS = {
    'Effects off, draw distance 1x',
    'Lighter shadows, draw distance 3x',
    "VanaCore's defaults",
    'Everything up, draw distance 8x, FXAA',
}

local function draw_bundled_presets(x, y, w)
    local dl = imgui.GetWindowDrawList()
    local now = clock()
    local lx, right = x + S(LEFT), x + w - S(RIGHT)
    local at = y
    local title = M.hy.caps('Bundled presets')
    M.text_mid(dl, lx, at + S(HEAD_H) / 2 + S(2), title, P.white, head_font())
    if not st.hybrid then
        M.hline(dl, lx + M.measure(title, head_font()) + S(8), right, at + S(HEAD_H) / 2 + S(2), P.line)
    end
    at = at + S(HEAD_H) + S(2) + (st.hybrid and S(M.hy.TITLE_PAD) or 0)
    local now_at = quality_now()
    local enabled = M.effects_available()
    for i, name in ipairs(QUALITY.names) do
        local cy = at + S(ROW_H) / 2
        if M.over(x, at, w, S(ROW_H)) then
            M.fill(dl, x + S(4), at, w - S(8), S(ROW_H), P.hover, S(3), 0, 0.8)
        end
        -- its name loads it, as a saved preset's does: white and underlined under the mouse
        local name_w = M.measure(name, fonts.text, BODY)
        local name_over, name_click = M.hit('##bundled.' .. name .. '.name', lx - S(4), at, name_w + S(8), S(ROW_H))
        local colour = (name_over and enabled) and P.white or P.cyan
        M.text_mid(dl, lx, cy, name, colour, fonts.text, BODY)
        if i == now_at then -- (the settings as they are: a green tick after its name)
            M.icon_centred(dl, lx + name_w + S(10), cy, 'check', P.green, 11)
            name_w = name_w + S(16)
        end
        if name_over then
            if enabled then
                M.hline(dl, lx, lx + name_w, cy + S(BODY) / 2 + S(1), colour, 0.7)
            end
            M.tooltip(enabled and ('Load ' .. name .. ': shadows, occlusion, fog, bloom, god rays, reflections, texture filtering, draw distance, anti-aliasing and glow together.\nNot the resolution, DLSS or the renderer.')
                      or "Needs VanaCore's renderer")
        end
        M.text_mid(dl, lx + name_w + S(8), cy, BUNDLED_WORDS[i], P.faint, fonts.text, SMALL)
        local flash = st.flash
        if flash ~= nil and flash.name == name and flash.what == 'loaded' and now - flash.at < 2.5 then
            M.text_right_mid(dl, right - S(10), cy, 'loaded', P.teal, fonts.text, SMALL)
        end
        if name_click and enabled then
            apply_quality(i)
            st.flash = { name = name, what = 'loaded', at = now }
        end
        at = at + S(ROW_H)
    end
    return at - y
end

local SAVE_H = 30 -- the name field and its buttons

local function draw_save_row(id, lx, right, cy)
    local dl = imgui.GetWindowDrawList()
    local now = clock()
    -- the name field in a well of its own, then Save, Import and (the HUD presets') Vanilla UI at the end of the line,
    -- all one height
    local gap = S(10)
    local iw = M.button_width('Import', 'downloading') + S(8)
    local sw_ = M.button_width('Save', 'check') + S(8)
    local vanilla = preset_kind == PRESET_KINDS.hud
    local vw = vanilla and M.button_width('Vanilla UI', 'reset') + S(8) or 0
    local fh = S(SAVE_H)
    local input_w = right - lx - sw_ - iw - gap * 2 - (vanilla and vw + gap or 0) -- (the same gap between each)
    local fy = cy - fh / 2
    M.fill(dl, lx, fy, input_w, fh, P.well, S(5))
    M.outline(dl, lx, fy, input_w, fh, P.text2, S(5), 0, 1, 0.45) -- (as bright as Vanilla UI's)
    local text_h = S(BODY - 0.5)
    imgui.SetCursorScreenPos({ lx + S(4), fy + 1 })
    imgui.SetNextItemWidth(input_w - S(8))
    imgui.PushStyleColor(M.COL.FrameBg, M.rgba(P.well, 0))
    imgui.PushStyleColor(M.COL.FrameBgHovered, M.rgba(P.well, 0))
    imgui.PushStyleColor(M.COL.FrameBgActive, M.rgba(P.well, 0))
    imgui.PushStyleVar(M.VAR.FramePadding, { S(8), math.max(0, (fh - 2 - text_h) / 2) })
    imgui.PushStyleVar(M.VAR.FrameRounding, S(4))
    imgui.PushStyleVar(M.VAR.FrameBorderSize, 0)
    M.push(fonts.text, BODY - 0.5)
    -- 64: EnterReturnsTrue (ImGui 1.92's), so the call is true on Enter alone; and Enter itself is checked, so a keystroke never saves
    local entered = imgui.InputTextWithHint('##preset.name', 'Preset name', st.presets.name, 48, 64)
    entered = entered and imgui.IsKeyPressed ~= nil and imgui.IsKeyPressed(525)
    M.pop()
    imgui.PopStyleVar(3)
    imgui.PopStyleColor(3)
    local named = clean_name(st.presets.name[1]) ~= ''
    local clicked = M.button(id .. '.save', lx + input_w + gap, cy, 'Save', 'check', 'success', named, nil, SAVE_H, sw_)
    if clicked or (entered and named) then
        save_preset(st.presets.name[1])
        st.presets.name[1] = ''
    end
    -- Import: a pasted string, or a .json picked with Windows' dialog (the dialog below)
    local import = M.button(id .. '.import', lx + input_w + gap + sw_ + gap, cy, 'Import', 'downloading', 'secondary', true, nil, SAVE_H, iw)
    if imgui.IsItemHovered() then
        M.tooltip('A preset from a string someone gave you, or from a .json file (one Export wrote, in ' .. PRESETS_DIR .. ')')
    end
    if import then
        st.dialog = { kind = 'import', text = { '' }, place = true, opens = true, focus = true }
    end
    -- Vanilla UI: asked twice, as Delete is
    if vanilla then
        local confirm = st.confirm
        local asking = confirm ~= nil and confirm.name == '##vanilla' and now - confirm.at < 3
        local reset = M.button(id .. '.vanilla', right - vw, cy, asking and 'Sure?' or 'Vanilla UI', 'reset', asking and 'danger' or 'neutral',
                               true, nil, SAVE_H, vw)
        if imgui.IsItemHovered() then
            M.tooltip(asking and 'Click again to put the UI back to the game\'s own'
                      or 'Every HUD and interface setting back to its default: windows in the game\'s own places, frames shown,\nbuttons and letters the game\'s. The HUD Manager\'s grid and snap stay. Save a preset first to come back to this one.')
        end
        if reset then
            if asking then
                st.confirm = nil
                vanilla_ui()
            else
                st.confirm = { name = '##vanilla', at = now }
            end
        end
    end
    local flash = st.flash
    if flash ~= nil and (flash.what == 'saved' or flash.what == 'vanilla') and now - flash.at < 2.5 then
        -- in the name field's well, at its right (the field is empty after a save)
        M.text_right_mid(dl, lx + input_w - S(10), cy, flash.what == 'vanilla' and 'UI back to vanilla' or 'saved ' .. flash.name, P.teal, fonts.text, SMALL)
    end
end

local function preset_count(entry)
    local n = 0
    for _ in pairs(entry.values) do
        n = n + 1
    end
    return n
end

local function draw_preset_row(id, entry, lx, right, cy)
    local dl = imgui.GetWindowDrawList()
    local now = clock()
    -- its name loads it, asked first (the dialog below): white and underlined under the mouse, the loaded one cyan
    local name_w = M.measure(entry.name, fonts.text, BODY)
    local name_over, name_click = M.hit(id .. '.name', lx - S(4), cy - S(ROW_H) / 2, name_w + S(8), S(ROW_H))
    local active = entry.name == current_preset()
    local colour = name_over and P.white or P.cyan
    M.text_mid(dl, lx, cy, entry.name, colour, fonts.text, BODY)
    if active then -- (the loaded one: a green tick after its name)
        M.icon_centred(dl, lx + name_w + S(10), cy, 'check', P.green, 11)
        name_w = name_w + S(16)
    end
    if name_over then
        M.hline(dl, lx, lx + name_w, cy + S(BODY) / 2 + S(1), colour, 0.7)
        M.tooltip('Load ' .. entry.name)
    end
    if name_click then
        st.dialog = { kind = 'load', entry = entry, presets = preset_kind, opens = true }
    end
    M.text_mid(dl, lx + name_w + S(8), cy, entry.saved .. '  ·  ' .. preset_count(entry) .. ' settings', P.faint, fonts.text, SMALL)
    -- its buttons, icons only, no frame (each its tooltip), from the right: Delete (asked twice), Export, and Update first,
    -- in green; each brightens under the mouse (Load is the name)
    local confirm = st.confirm
    local asking = confirm ~= nil and confirm.name == entry.name and now - confirm.at < 3
    local step, size = S(28), 13
    local cx = right - S(10)
    local del, del_over = M.icon_button(id .. '.delete', cx, cy, 'trash', size, '#FF8A9A', asking and '#FF8A9A' or P.rose)
    if del_over then
        M.tooltip(asking and 'Click again to delete ' .. entry.name or 'Delete ' .. entry.name)
    end
    cx = cx - step
    local export, export_over = M.icon_button(id .. '.export', cx, cy, 'share', size, P.white, P.text2)
    if export_over then
        M.tooltip('Export: shows ' .. entry.name .. ' as a string to copy, or saves it as a .json file in ' .. PRESETS_DIR)
    end
    cx = cx - step
    local update, update_over = M.icon_button(id .. '.update', cx, cy, 'save', size, P.white, P.green)
    if update_over then
        M.tooltip('Update: saves the settings you have now into ' .. entry.name)
    end
    local at = cx - S(10)
    if asking then
        M.text_right_mid(dl, at - S(4), cy, 'Click the bin again to delete', P.rose, fonts.text, SMALL)
    end
    local flash = st.flash
    if not asking and flash ~= nil and flash.name == entry.name and flash.what ~= 'saved' and now - flash.at < 2.5 then
        M.text_right_mid(dl, at - S(4), cy, flash.what, P.teal, fonts.text, SMALL)
    end
    if update then
        save_preset(entry.name)
        st.flash = { name = entry.name, what = 'updated', at = now }
    end
    if export then
        st.dialog = { kind = 'export', entry = entry, windows = false, text = { '' }, opens = true, ext = preset_kind.ext }
    end
    if del then
        if asking then
            st.confirm = nil
            delete_preset(entry)
        else
            st.confirm = { name = entry.name, at = now }
        end
    end
end

-------------------------------------------------------------------------------
-- Export and Import: the dialog over the window (st.dialog)
--   { kind = 'export', entry, windows (the box ticked) }  the preset as a string to copy, or a .json to save
--   { kind = 'import', text = { the box }, place (the box ticked) }  a string pasted, or a .json opened
--   { kind = 'load', entry, presets (its kind) }  a saved preset's name clicked: "Load the preset: <name>", Cancel or Load
-------------------------------------------------------------------------------

local DIALOG_W, DIALOG_H = 560, 372
-- NoTitleBar, NoResize, NoMove, NoScrollbar, NoScrollWithMouse, NoCollapse, NoSavedSettings
local DIALOG_FLAGS = 1 + 2 + 4 + 8 + 16 + 32 + 256
local B64_SAMPLE = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
local INPUT_READ_ONLY = 512 -- (ImGui 1.92's number)

-- "2560 x 1440"
local function size_text(size)
    return M.share.num(size[1]) .. ' x ' .. M.share.num(size[2])
end

-- `text` broken into lines of `n` characters (the box doesn't wrap: a string is one line that scrolls); Base64 decoding
-- ignores the breaks, so what's copied from the box by hand works as well as the Copy button's
local function broken(text, n)
    return ((text:gsub(('.'):rep(n), '%0\n'):gsub('\n$', '')))
end

-- the Export box's string, again for the ticked option and the box's width
local function export_refresh(d, per_line)
    local data = share_data(d.entry, d.windows)
    d.packed = M.share.pack(data)
    d.text[1] = broken(d.packed, per_line)
    d.state = tostring(d.windows) .. '/' .. per_line
end

-- Load's question, a small dialog: its two lines, its size (as wide as the name needs), then its contents (true when
-- it's answered)
local function load_question(d)
    return 'Load the preset: ' .. d.entry.name, 'Sets your ' .. d.presets.what .. ' settings to the ' .. preset_count(d.entry) .. ' it holds.'
end

local function load_size(d)
    local title, note = load_question(d)
    local w = math.max(S(360), M.measure(title, fonts.semi, BODY + 1) + S(44) + S(28), M.measure(note, fonts.text, SMALL) + S(44))
    return w, S(128)
end

local function draw_load_dialog(d, px, py, dw, dh)
    local dl = imgui.GetWindowDrawList()
    local pad = S(22)
    local x, right = px + pad, px + dw - pad
    local button_cy = py + dh - S(32)
    local title, note = load_question(d)
    M.text_mid(dl, x, py + S(26), title, P.text, fonts.semi, BODY + 1)
    M.text_mid(dl, x, py + S(50), note, P.dim, fonts.text, SMALL)
    local done = M.icon_button('##dlg.close', right - S(2), py + S(26), 'close', 11, P.rose)
    local lw = M.button_width('Load', 'download') + S(8)
    local cw = M.button_width('Cancel', nil) + S(8)
    if M.button('##dlg.cancel', right - lw - S(8) - cw, button_cy, 'Cancel', nil, 'ghost', true, nil, 28, cw) then
        done = true
    end
    if M.button('##dlg.load', right - lw, button_cy, 'Load', 'download', 'accent', true, nil, 28, lw) then
        use_kind(d.presets) -- (the kind the name was clicked in: the page's)
        load_preset(d.entry)
        done = true
    end
    return done
end

local function draw_dialog(wx, wy, ww, wh)
    local d = st.dialog
    if d == nil then
        return
    end
    d.frames = (d.frames or 0) + 1
    block_game_mouse(true) -- no click reaches the game while this window is open (its own buttons still work)
    local dw, dh = S(DIALOG_W), S(DIALOG_H)
    if d.kind == 'load' then
        dw, dh = load_size(d)
    end
    local export = d.kind == 'export'
    if d.opens then
        d.opens = false
        imgui.OpenPopup('##presetdlg')
        if export then
            local screen, list = layout_now()
            d.screen, d.count = screen, list ~= nil and #list or 0
        end
    end
    imgui.SetNextWindowPos({ wx + ww / 2, wy + wh / 2 }, 8, { 0.5, 0.5 }) -- (Appearing)
    imgui.SetNextWindowSize({ dw, dh }, 0)
    M.push_popup_style()
    imgui.PushStyleColor(M.COL.ModalWindowDimBg, { 0, 0, 0, 0.4 }) -- the screen dimmed behind the dialog: the game takes no click
    local shown = imgui.BeginPopupModal('##presetdlg', nil, DIALOG_FLAGS)
    imgui.PopStyleColor(1)
    local done = false
    if shown and d.kind == 'load' then
        local px, py = imgui.GetWindowPos()
        done = draw_load_dialog(d, px, py, dw, dh)
        if done then
            imgui.CloseCurrentPopup()
        end
        imgui.EndPopup()
    elseif shown then
        local px, py = imgui.GetWindowPos()
        local dl = imgui.GetWindowDrawList()
        local pad = S(22)
        local x, right = px + pad, px + dw - pad
        local bw, bh = right - x, S(150)
        local by = py + S(70)
        local info_cy = by + bh + S(22)
        local option_cy = info_cy + S(30)
        local button_cy = py + dh - S(32)

        M.text_mid(dl, x, py + S(26), export and ('Export "' .. d.entry.name .. '"') or 'Import a preset', P.text, fonts.semi, BODY + 1)
        M.text_mid(dl, x, py + S(50), export and 'Copy the string and send it. Import, on Graphics > Presets, turns it back into this preset.'
                                      or 'Paste a preset string, or open a .json file that Export saved.', P.dim, fonts.text, SMALL)
        if M.icon_button('##dlg.close', right - S(2), py + S(26), 'close', 11, P.rose) then
            done = true
        end

        if export then
            local inner = bw - S(4) - S(16) - S(12)
            local per_line = math.max(16, math.floor(inner / (M.measure(B64_SAMPLE, fonts.text, SMALL + 0.5) / #B64_SAMPLE)) - 1)
            if d.state ~= tostring(d.windows) .. '/' .. per_line then
                export_refresh(d, per_line)
            end
            dialog_box('##dlg.text', x, by, bw, bh, d.text, #d.text[1] + 16, INPUT_READ_ONLY)
            M.text_mid(dl, x, info_cy, #d.packed .. ' characters  ·  ' .. (d.windows and d.count > 0 and 'settings and addon positions' or 'settings only'), P.faint, fonts.text, SMALL)
            if d.copied and clock() - d.copied < 2.5 then
                M.text_right_mid(dl, right, info_cy, 'copied', P.teal, fonts.text, SMALL)
            end
            -- the addons' windows: ticked to go with it (only if some are showing or have been placed)
            local can = d.count > 0
            local on, switched = M.switch('##dlg.windows', x, option_cy, d.windows and can, can)
            if switched then
                d.windows = on
            end
            local label_x = x + S(M.SW_W) + S(10)
            M.text_mid(dl, label_x, option_cy, 'Include addon positions', can and P.text or P.dim, fonts.text, BODY - 1)
            local label_w = M.measure('Include addon positions', fonts.text, BODY - 1)
            M.text_mid(dl, label_x + label_w + S(8), option_cy,
                       can and ('(' .. d.count .. (d.count == 1 and ' window' or ' windows') .. ' placed on ' .. size_text(d.screen) .. ')')
                           or '(no addon window is showing)', P.faint, fonts.text, SMALL)
            if M.over(x, option_cy - S(10), bw, S(20)) then
                M.tooltip('Where the addons\' windows are, and the resolution they are placed on. Whoever imports it gets them in the same\nplaces in proportion to their screen, and sized for it.')
            end

            local cw = M.button_width('Copy', 'copy') + S(8)
            local jw = M.button_width('Save JSON', 'save') + S(8)
            local xw = M.button_width('Close', nil) + S(8)
            if M.button('##dlg.close2', right - xw, button_cy, 'Close', nil, 'ghost', true, nil, 28, xw) then
                done = true
            end
            if M.button('##dlg.json', right - xw - S(8) - jw, button_cy, 'Save JSON', 'save', 'success', true, nil, 28, jw) then
                if export_preset(d.entry, d.windows and can, d.ext) then
                    done = true
                end
            end
            if imgui.IsItemHovered() then
                M.tooltip('Writes ' .. d.entry.name .. '.' .. d.ext .. '.json to ' .. PRESETS_DIR .. ' and opens the folder')
            end
            if M.button('##dlg.copy', right - xw - S(8) - jw - S(8) - cw, button_cy, 'Copy', 'copy', 'accent', true, nil, 28, cw) then
                imgui.SetClipboardText(d.packed)
                d.copied = clock()
            end
        else
            dialog_box('##dlg.text', x, by, bw, bh, d.text, 65536, 0, d.focus)
            d.focus = false
            if d.text[1] == '' then
                M.text_mid(dl, x + S(12), by + S(16), 'Paste the string here', P.faint, fonts.text, SMALL + 0.5)
            end
            if d.parsed_for ~= d.text[1] then
                d.parsed_for = d.text[1]
                d.data, d.problem, d.count = parse_preset(d.text[1], d.file_name)
            end
            local data = d.data
            if data ~= nil then
                local here = preset_named(data.name) ~= nil
                M.text_mid(dl, x, info_cy, '"' .. data.name .. '": ' .. d.count .. (d.count == 1 and ' setting' or ' settings')
                                         .. (data.windows ~= nil and (', ' .. #data.windows .. (#data.windows == 1 and ' addon window' or ' addon windows')) or ''), P.teal, fonts.text, SMALL + 0.5)
                if here then
                    M.text_right_mid(dl, right, info_cy, 'replaces your preset of the same name', P.amber, fonts.text, SMALL)
                end
            elseif d.problem ~= nil then
                M.text_mid(dl, x, info_cy, d.problem, P.rose, fonts.text, SMALL + 0.5)
            end
            -- the addons' windows it carries: placed now, in proportion to this screen
            if data ~= nil and data.windows ~= nil then
                local sw, sh = vana.ui.screen()
                local on, switched = M.switch('##dlg.place', x, option_cy, d.place, true)
                if switched then
                    d.place = on
                end
                local label_x = x + S(M.SW_W) + S(10)
                M.text_mid(dl, label_x, option_cy, 'Place the addon windows too', P.text, fonts.text, BODY - 1)
                local label_w = M.measure('Place the addon windows too', fonts.text, BODY - 1)
                local _, factor = M.share.fit({}, data.screen[1], data.screen[2], sw or 0, sh or 0)
                local note = size_text(data.screen) == size_text({ sw or 0, sh or 0 }) and '(same resolution as yours)'
                             or ('(made on ' .. size_text(data.screen) .. '; this is ' .. size_text({ sw or 0, sh or 0 }) .. ', sizes x' .. M.share.num(factor) .. ')')
                M.text_mid(dl, label_x + label_w + S(8), option_cy, note, P.faint, fonts.text, SMALL)
            end

            local pw_ = M.button_width('Paste', 'paste') + S(8)
            local fw = M.button_width('Open file', 'folder') + S(8)
            local cw = M.button_width('Cancel', nil) + S(8)
            local iw = M.button_width('Import', 'downloading') + S(8)
            if M.button('##dlg.paste', x, button_cy, 'Paste', 'paste', 'secondary', true, nil, 28, pw_) then
                d.text[1] = (imgui.GetClipboardText() or ''):sub(1, 60000)
            end
            if imgui.IsItemHovered() then
                M.tooltip('The text you last copied, into the box')
            end
            if M.button('##dlg.file', x + pw_ + S(8), button_cy, 'Open file', 'folder', 'ghost', true, nil, 28, fw) then
                d.pick = true
            end
            -- the dialog once the click is let go (it's taken on the press): Windows' own loop mustn't open with a button held
            if d.pick and not imgui.IsMouseDown(0) then
                d.pick = nil
                local path = pick_file()
                local text = path ~= nil and read_text(path) or nil
                if text ~= nil then
                    d.text[1] = text:sub(1, 60000)
                    d.file_name = path:match('([^\\/]+)%.%w+$')
                    d.file_name = d.file_name and (d.file_name:gsub('%.gfx$', ''):gsub('%.hud$', ''))
                end
            end
            if imgui.IsItemHovered() then
                M.tooltip('A .json file Export saved (in ' .. PRESETS_DIR .. ')')
            end
            if M.button('##dlg.cancel', right - iw - S(8) - cw, button_cy, 'Cancel', nil, 'ghost', true, nil, 28, cw) then
                done = true
            end
            if M.button('##dlg.import', right - iw, button_cy, 'Import', 'downloading', 'accent', data ~= nil, nil, 28, iw) and data ~= nil then
                local entry = store_preset(data)
                if d.place and data.windows ~= nil then
                    local placed, factor, kept = place_windows(data)
                    if placed ~= nil then
                        print('VanaCore: ' .. placed .. (placed == 1 and ' addon window' or ' addon windows') .. ' placed for this screen ('
                              .. size_text(data.screen) .. ' to ' .. size_text({ vana.ui.screen() }) .. ').'
                              .. (kept ~= nil and (' The layout before is in config\\' .. kept .. '.') or ''))
                    else
                        print('VanaCore: ' .. tostring(factor))
                    end
                end
                done = entry ~= nil
            end
        end
        if done then
            imgui.CloseCurrentPopup()
        end
        imgui.EndPopup()
    end
    M.pop_popup_style()
    if done or (not shown and d.frames > 2) then
        st.dialog = nil -- (or closed by Escape)
        block_game_mouse(false) -- the game gets the mouse back after half a second, every button up
    end
end

-- the Presets page's sections: Save, the saved presets, then the bundled ones; returns the height drawn
local function draw_presets_page(page, x, y, w)
    local dl = imgui.GetWindowDrawList()
    if not st.presets.read then
        read_presets()
    end
    local lx, right = x + S(LEFT), x + w - S(RIGHT)
    local at = y + S(6)
    draw_save_row('##presets.save', lx, right, at + S(SAVE_H) / 2)
    at = at + S(SAVE_H) + S(14)
    local title = M.hy.caps('Saved presets')
    M.text_mid(dl, lx, at + S(HEAD_H) / 2 + S(2), title, P.white, head_font())
    if not st.hybrid then
        M.hline(dl, lx + M.measure(title, head_font()) + S(8), right, at + S(HEAD_H) / 2 + S(2), P.line)
    end
    at = at + S(HEAD_H) + S(2) + (st.hybrid and S(M.hy.TITLE_PAD) or 0)
    if #st.presets.list == 0 then
        M.icon_centred(dl, lx + S(6), at + S(ROW_H) / 2, 'info', P.dim, 10)
        M.text_mid(dl, lx + S(20), at + S(ROW_H) / 2, 'No presets yet: name the settings you have now and save them.', P.text2, fonts.text, BODY - 1)
        at = at + S(ROW_H)
    end
    for _, entry in ipairs(st.presets.list) do
        local cy = at + S(ROW_H) / 2
        if M.over(x, at, w, S(ROW_H)) then
            M.fill(dl, x + S(4), at, w - S(8), S(ROW_H), P.hover, S(3), 0, 0.8)
        end
        draw_preset_row('##preset.' .. entry.name, entry, lx, right, cy)
        at = at + S(ROW_H)
    end
    if page.presets then
        at = at + S(10)
        at = at + draw_bundled_presets(x, at, w)
    end
    return at - y + S(4)
end

-------------------------------------------------------------------------------
-- Search: the rows of every page whose name, tooltip or section title has the text
-------------------------------------------------------------------------------

local function search_results(query)
    query = query:lower()
    local results = {}
    for _, page in ipairs(PAGES) do
        if page.tab ~= nil and not page.hidden then
            for _, sec in ipairs(sections_of(page, page_rows(page))) do
                local whole = sec.title ~= nil and sec.title:lower():find(query, 1, true) ~= nil
                local rows = {}
                local tab = nil -- (a tab bar's rows found: each tab's under its name, row.tab_of.head)
                for _, row in ipairs(sec.rows) do
                    if row.kind ~= 'subheader' and row.kind ~= 'note' and row.kind ~= 'tabs' then
                        local name = (row.label or row.text or row.name or ''):lower()
                        local tip = tip_of(row)
                        local found = whole or name:find(query, 1, true) or (type(tip) == 'string' and tip:lower():find(query, 1, true))
                        -- (a True Font line by its parts' names too: "outline", "size", "bold"; a colours line, "tp colour")
                        for _, part in ipairs(not found and (row.kind == 'font' or row.kind == 'colours') and row.parts or {}) do
                            found = found or (part.label or ''):lower():find(query, 1, true) ~= nil
                        end
                        if found then
                            if row.tab_of ~= nil and row.tab_of ~= tab and row.tab_of.head ~= nil then
                                rows[#rows + 1] = row.tab_of.head
                            end
                            tab = row.tab_of
                            rows[#rows + 1] = row
                        end
                    end
                end
                if #rows > 0 then
                    results[#results + 1] = { page = page, section = sec, rows = rows }
                end
            end
        end
    end
    return results
end

-------------------------------------------------------------------------------
-- The window: the list of pages and the page
-------------------------------------------------------------------------------

local SIDE_W = 159           -- fits the widest name (Experimental, 125 across at 100%), the sub-tabs' and the margin
local WIN_W, WIN_H = 651, 540 -- SIDE_W + the page's 492: the window's size until it's dragged at its resize corner (st.size)
-- the resize corner, bottom right (the page stops above it), and the least the window's dragged to: WIN_W across (the pages are
-- laid out for 492), MIN_H down
local RESIZE = { GRIP = 18, MIN_H = 400 }
local MARGIN = 6
local TITLE_H = 36
local TILE = 64              -- the minimized window's tile, a square with the logo in it
local SEARCH_H = 30

-- The hybrid layout (/vana3): the pages are the tabs of a header across the top (M.hy.header), so the list at the left is the
-- shown tab's own sub-tabs, drawn as they are under a page in the list of pages (the same rows, the same scrollbar). The
-- System pages (Plugins, Addons, Pivot) are Client's: its list goes on with a line, then them with their icons, then the tools
M.hy = { H = 44, PAD = 11 } -- H: the header's height; PAD: the list's margin at the left (the classic layout's is PAD)
-- the panels' lines (M.hy.card, at a low alpha) and the kinds of row that have no line under them
M.hy.LINE = '#96C3FF'
M.hy.FLAT = true -- no box round a panel, only its heading's line (false: the cards)
M.hy.TITLE_PAD = 6 -- the room under a main title (a section's, a headline's, the presets' lists), where it has no line
-- a main title (a page's section, a headline, the presets' lists) in the hybrid layout is in capitals
function M.hy.caps(text)
    return st.hybrid == true and text:upper() or text
end
M.hy.NOSEP = {}
for kind in ('subheader banner warning callout alert notice hint note tabs layers transfer textfile status controls columns extension_filter header'):gmatch('%S+') do
    M.hy.NOSEP[kind] = true
end

-- the tab a page is under: the System pages are under Client, Experimental under Graphics
function M.hy.home(page)
    if page.id == 'experimental' then
        return page_named('graphics') or page
    end
    for _, group in ipairs(GROUPS) do
        if group.group == 'System' then
            for _, other in ipairs(group.pages) do
                if other == page then
                    return page_named('client') or page
                end
            end
        end
    end
    return page
end

function M.hy.nav(x, top, w, bottom, searching)
    imgui.SetCursorScreenPos({ x, top })
    if imgui.BeginChild('##nav', { w, bottom - top }, 0, M.NO_NAV) then
        local cdl = imgui.GetWindowDrawList()
        local _, ny = imgui.GetCursorScreenPos()
        w = (imgui.GetContentRegionAvail())
        local pad, sh, ih = S(M.hy.PAD), S(22), S(25)
        local home = M.hy.home(st.page)

        -- a page's sub-tabs from `ny`, as under its row in the list of pages; returns where they end. `lined`: the line down from
        -- the page's icon, as under a page row
        local function sub_rows(page, ny0, lined)
            local subs, shown = nil, nil
            if not searching then
                subs, shown = subtabs_of(page)
            end
            local active = st.page == page and not searching
            if subs == nil then
                return ny0, nil, nil
            end
            local gx = x + pad + S(6)
            if lined then
                M.vline(cdl, gx, ny0 + S(2), ny0 + S(2) + #subs * sh, P.line)
            end
            ny0 = ny0 + S(2)
            for _, sec in ipairs(subs) do
                local on = active and sec == shown
                local shov, sclick = M.hit('##sub.' .. page.id .. '.' .. sec.title, x, ny0, w, sh)
                if sclick and sec.run ~= nil then -- (not a section: the HUD's Open Manager)
                    sec.run()
                elseif sclick and not on then
                    st.sub[page.id] = sec.title
                    st.page = page
                    st.search[1] = ''
                    st.picker = nil
                    -- (List is the addons' list; an addon's windows are on Layout)
                    local want = sec == ADDONS_ALL and 'list' or 'layout'
                    if page.id == 'addons' and M.layout.page ~= want then
                        M.layout.page = want
                        M.rebuild()
                    end
                    save_settings()
                end
                if shov and not on then
                    M.fill(cdl, x, ny0, w, sh, P.hover, 0, 0, 0.7)
                end
                if on then
                    M.fill(cdl, gx - S(1), ny0 + S(4), S(3), sh - S(8), P.cyan, S(1.5))
                end
                local full = sec.nav or sec.title
                local an_addon = page.id == 'addons' and sec ~= ADDONS_ALL
                local nav_icon = sec.head ~= nil and sec.head.nav_icon or nil
                local font = an_addon and fonts.italic or fonts.text
                local sub_color = on and P.cyan or (shov and P.text or P.dim)
                local tx = x + pad + S(19)
                if an_addon then
                    M.icon_centred(cdl, tx + S(4), ny0 + sh / 2, 'cube', on and P.cyan or (shov and P.dim or P.faint), 8)
                    tx = tx + S(13)
                elseif nav_icon ~= nil then
                    sub_color = sec.head.nav_color or sub_color
                    if nav_icon == 'bullet' then
                        M.circle(cdl, tx + S(4), ny0 + sh / 2, S(2.5), sub_color)
                    else
                        M.icon_centred(cdl, tx + S(4), ny0 + sh / 2, nav_icon, sub_color, 8)
                    end
                    tx = tx + S(10)
                end
                local name = M.fit(full, w - (tx - x) - S(4), font, BODY - 0.5)
                M.text_mid(cdl, tx, ny0 + sh / 2, name, sub_color, font, BODY - 0.5)
                if shov and (name ~= full or sec.tip ~= nil) then
                    M.tooltip(sec.tip or full)
                end
                ny0 = ny0 + sh
            end
            return ny0 + S(4), subs, shown
        end

        ny = ny + S(2)
        ny = sub_rows(home, ny, false)
        local experimental = home.id == 'graphics' and page_named('experimental') or nil
        if experimental ~= nil then
            -- Graphics' experimental panels (Water Shader, Normal maps, Snow Trails), under a red heading
            ny = ny + S(6)
            M.hline(cdl, x + pad, x + w - pad, ny, P.border, 0.8)
            ny = ny + S(8)
            M.text_spaced(cdl, x + pad + S(6), ny + S(7), 'EXPERIMENTAL', P.rose, fonts.semi, SMALL - 0.5, 1, S(1.4))
            ny = ny + S(16)
            ny = sub_rows(experimental, ny, false)
        end
        if home.id == 'client' then
            -- Client's: Plugins, Addons and Pivot, with their icons, under a line
            local function line()
                ny = ny + S(5)
                M.hline(cdl, x + pad, x + w - pad, ny, P.border, 0.8)
                ny = ny + S(6)
            end
            line()
            for _, group in ipairs(GROUPS) do
                if group.group == 'System' then
                    for _, page in ipairs(group.pages) do
                        local active = st.page == page and not searching
                        local hov, clicked = M.hit('##hy.page.' .. page.id, x, ny, w, ih)
                        local subs, shown = nil, nil
                        if active then
                            subs, shown = subtabs_of(page)
                        end
                        if clicked and not active then
                            st.page = page
                            st.search[1] = ''
                            st.picker = nil
                        end
                        local lit = active and not (subs ~= nil and shown ~= nil)
                        if lit then
                            M.fill(cdl, x, ny, w, ih, P.accent_bg, 0, 0, 0.9)
                            M.fill(cdl, x, ny, S(2), ih, P.accent)
                        elseif hov and not active then
                            M.fill(cdl, x, ny, w, ih, P.hover, 0, 0, 0.7)
                        end
                        M.icon_centred(cdl, x + pad + S(6), ny + ih / 2, page.icon, active and P.accent or P.faint, 10.5)
                        M.text_mid(cdl, x + pad + S(19), ny + ih / 2, page.name, page.color or P.white, fonts.text, BODY + 0.5)
                        ny = ny + ih
                        if active and subs ~= nil then
                            ny = sub_rows(page, ny, true)
                        end
                    end
                end
            end
            -- the tools: Stats, Graphs and Debug (/vana stats, /vana graph, /vana debug)
            line()
            local tools = {
                { 'Stats', 'stats', '/vana stats', 'Where a frame goes (/vana stats)' },
                { 'Graphs', 'chart', '/vana graph', 'FPS and more, 1 minute (/vana graph)' },
                { 'Debug', 'bug', '/vana debug', 'DLSS crash tests, frame recording (/vana debug)' },
            }
            for i, tool in ipairs(tools) do
                local hov, clicked = M.hit('##hy.tool.' .. i, x, ny, w, ih)
                if hov then
                    M.fill(cdl, x, ny, w, ih, P.hover, 0, 0, 0.7)
                    M.tooltip(tool[4])
                end
                M.icon_centred(cdl, x + pad + S(6), ny + ih / 2, tool[2], hov and P.accent or P.faint, 10.5)
                M.text_mid(cdl, x + pad + S(19), ny + ih / 2, tool[1], P.white, fonts.text, BODY + 0.5)
                if clicked then
                    vana.chat.run(tool[3])
                end
                ny = ny + ih
            end
        end
        imgui.SetCursorScreenPos({ x, ny })
        imgui.Dummy({ 1, S(4) })
    end
    imgui.EndChild()
end

local function draw_sidebar(x, y, w, h)
    local dl = imgui.GetWindowDrawList()
    local pad = S(st.hybrid and M.hy.PAD or PAD)

    -- the search box at the top (the name and version are Client > VanaCore's): what's typed finds settings on every
    -- page. A well with a shadow drawn here; ImGui's field sits in it, see-through, left of the red × that clears it
    -- (the × is its own item, outside the field, so it takes the click)
    local sy = y + S(12)
    local sw_, sh_ = w - pad * 2, S(SEARCH_H)
    local sx = x + pad
    M.fill(dl, sx + S(1), sy + S(4), sw_, sh_, '#000000', S(6), 0, 0.18)
    M.fill(dl, sx + S(1), sy + S(2), sw_, sh_, '#000000', S(6), 0, 0.3)
    M.fill(dl, sx, sy, sw_, sh_, P.search, S(6))
    M.outline(dl, sx, sy, sw_, sh_, P.search_edge, S(6))
    local searching = st.search[1] ~= ''
    local field_w = sw_ - S(4) - (searching and S(28) or S(4))
    local text_h = S(SMALL + 1)
    imgui.SetCursorScreenPos({ sx + S(4), sy + 1 })
    imgui.SetNextItemWidth(field_w)
    imgui.PushStyleColor(M.COL.FrameBg, M.rgba(P.well, 0))
    imgui.PushStyleColor(M.COL.FrameBgHovered, M.rgba(P.well, 0))
    imgui.PushStyleColor(M.COL.FrameBgActive, M.rgba(P.well, 0))
    imgui.PushStyleVar(M.VAR.FramePadding, { S(8), math.max(0, (sh_ - 2 - text_h) / 2) })
    imgui.PushStyleVar(M.VAR.FrameRounding, S(5))
    imgui.PushStyleVar(M.VAR.FrameBorderSize, 0)
    M.push(fonts.text, SMALL + 1)
    imgui.InputTextWithHint('##search', 'Search', st.search, 64)
    M.pop()
    imgui.PopStyleVar(3)
    imgui.PopStyleColor(3)
    if searching then
        if M.icon_button('##search.clear', sx + sw_ - S(15), sy + sh_ / 2, 'close', 12, '#FFA7AE', P.rose) then
            st.search[1] = ''
            searching = false
        end
    end

    if st.hybrid then -- (the tools are icons in the header, the pages its tabs: only this page's sub-tabs are listed)
        M.hy.nav(x, sy + S(SEARCH_H) + S(8), w, y + h - S(6), searching)
        return
    end

    -- the tools at the bottom: Stats and Graphs (the old window's Frame stats and Performance graph: /vana stats and
    -- /vana graph)
    local TOOLS = {
        { 'Stats', 'stats', '/vana stats', 'Where a frame goes (/vana stats)' },
        { 'Graphs', 'chart', '/vana graph', 'FPS and more, 1 minute (/vana graph)' },
        { 'Debug', 'bug', '/vana debug', 'DLSS crash tests, frame recording (/vana debug)' },
    }
    local ih = S(25)
    local tools_h = S(18) + #TOOLS * ih + S(10)
    local ty = y + h - tools_h
    M.hline(dl, x + pad, x + w - pad, ty, P.border, 0.8)
    M.text_mid(dl, x + pad, ty + S(14), 'TOOLS', P.faint, fonts.semi, SMALL - 1.5)
    local ry = ty + S(22)
    for i, tool in ipairs(TOOLS) do
        local hov, clicked = M.hit('##tool.' .. i, x, ry, w, ih)
        if hov then
            M.fill(dl, x, ry, w, ih, P.hover, 0, 0, 0.7)
            M.tooltip(tool[4])
        end
        M.icon_centred(dl, x + pad + S(6), ry + ih / 2, tool[2], hov and P.accent or P.faint, 10.5)
        M.text_mid(dl, x + pad + S(19), ry + ih / 2, tool[1], P.white, fonts.text, BODY + 0.5)
        if clicked then
            vana.chat.run(tool[3])
        end
        ry = ry + ih
    end

    -- the pages, in their groups' order, the page shown with its sub-tabs under it; they scroll, the rows narrowed by
    -- the scrollbar when it's there
    local ny0 = sy + S(SEARCH_H) + S(8)
    imgui.SetCursorScreenPos({ x, ny0 })
    if imgui.BeginChild('##nav', { w, ty - ny0 - S(6) }, 0, M.NO_NAV) then
        local cdl = imgui.GetWindowDrawList()
        local _, ny = imgui.GetCursorScreenPos()
        w = (imgui.GetContentRegionAvail())
        local sh = S(22)
        for gi, group in ipairs(GROUPS) do
            for _, page in ipairs(group.pages) do
                local active = st.page == page and not searching
                local subs, shown = subtabs_of(page)
                -- the chevron opens or shuts its sub-tabs in the list, any number of pages' at once; the page shown,
                -- clicked again, does too, and another page clicked opens its own. Never set: open only while shown
                local cw = subs ~= nil and S(22) or 0
                local hov, clicked = M.hit('##nav.' .. page.id, x, ny, w - cw, ih)
                local chov, cclick = false, false
                if subs ~= nil then
                    chov, cclick = M.hit('##navopen.' .. page.id, x + w - cw, ny, cw, ih)
                end
                hov = hov or chov
                local open = subs ~= nil and (st.navopen[page.id] == true or (st.navopen[page.id] == nil and active))
                if cclick or (clicked and active and subs ~= nil) then
                    open = not open
                    st.navopen[page.id] = open
                    save_settings()
                elseif clicked and not active then
                    if subs ~= nil and not open then
                        open = true
                        st.navopen[page.id] = true
                        save_settings()
                    end
                    st.page = page
                    st.search[1] = ''
                    st.picker = nil
                end
                local t = M.ease('nav.' .. page.id, active and 1 or 0, 20)
                -- the page shown is lit only while no sub-tab under it shows it
                local lit = M.ease('nav.lit.' .. page.id, active and not (open and shown ~= nil) and 1 or 0, 20)
                if lit > 0.01 then
                    M.fill(cdl, x, ny, w, ih, P.accent_bg, 0, 0, lit * 0.9)
                    M.fill(cdl, x, ny, S(2), ih, P.accent, 0, 0, lit)
                end
                if hov and not active then
                    M.fill(cdl, x, ny, w, ih, P.hover, 0, 0, 0.7)
                end
                -- the names in white (Experimental in light purple), the icon lit in the accent on the page shown
                local color = page.color or P.white
                M.icon_centred(cdl, x + pad + S(6), ny + ih / 2, page.icon, page.color or M.mix(P.faint, P.accent, t), 10.5)
                M.text_mid(cdl, x + pad + S(19), ny + ih / 2, page.name, color, fonts.text, BODY + 0.5)
                if subs ~= nil then
                    M.icon_centred(cdl, x + w - pad - S(4), ny + ih / 2, open and 'down' or 'right', chov and P.text or (hov and P.dim or P.faint), 6)
                end
                ny = ny + ih
                -- its sub-tabs: its sections by their headings, on a line down from its icon; the one shown lit (a
                -- click on another page's shows that page at it)
                if open then
                    local gx = x + pad + S(6)
                    M.vline(cdl, gx, ny + S(2), ny + S(2) + #subs * sh, P.line)
                    ny = ny + S(2)
                    for _, sec in ipairs(subs) do
                        local on = active and sec == shown
                        local shov, sclick = M.hit('##sub.' .. page.id .. '.' .. sec.title, x, ny, w, sh)
                        if sclick and sec.run ~= nil then -- (not a section: the HUD's Open Manager)
                            sec.run()
                        elseif sclick and not on then
                            st.sub[page.id] = sec.title
                            st.page = page
                            st.search[1] = ''
                            st.picker = nil
                            -- (List is the addons' list; an addon's windows are on Layout)
                            local want = sec == ADDONS_ALL and 'list' or 'layout'
                            if page.id == 'addons' and M.layout.page ~= want then
                                M.layout.page = want
                                M.rebuild()
                            end
                            save_settings()
                        end
                        if shov and not on then
                            M.fill(cdl, x, ny, w, sh, P.hover, 0, 0, 0.7)
                        end
                        if on then
                            M.fill(cdl, gx - S(1), ny + S(4), S(3), sh - S(8), P.cyan, S(1.5))
                        end
                        local full = sec.nav or sec.title
                        local an_addon = page.id == 'addons' and sec ~= ADDONS_ALL
                        -- a heading's own icon and colour (the HUD's Settings: a bullet, in yellow; its Open Manager: an
                        -- open-in-a-window icon)
                        local nav_icon = sec.head ~= nil and sec.head.nav_icon or nil
                        local font = an_addon and fonts.italic or fonts.text
                        local sub_color = on and P.cyan or (shov and P.text or P.dim)
                        local tx = x + pad + S(19)
                        if an_addon then
                            -- an addon's: a small cube before its name (on the line's middle, as the page icons: there its
                            -- middle is level with the name's capitals', where S(1) lower sat it 1.5 px low at a 4K scale)
                            M.icon_centred(cdl, tx + S(4), ny + sh / 2, 'cube', on and P.cyan or (shov and P.dim or P.faint), 8)
                            tx = tx + S(13)
                        elseif nav_icon ~= nil then
                            sub_color = sec.head.nav_color or sub_color
                            if nav_icon == 'bullet' then -- a dot, as a list's bullet (on the line's middle: a glyph's pixel lower is too low)
                                M.circle(cdl, tx + S(4), ny + sh / 2, S(2.5), sub_color)
                            else
                                M.icon_centred(cdl, tx + S(4), ny + sh / 2, nav_icon, sub_color, 8) -- (as the cube)
                            end
                            tx = tx + S(10) -- (the icon's 8 wide: a small gap before the title)
                        end
                        local name = M.fit(full, w - (tx - x) - S(4), font, BODY - 0.5)
                        M.text_mid(cdl, tx, ny + sh / 2, name, sub_color, font, BODY - 0.5)
                        if shov and (name ~= full or sec.tip ~= nil) then
                            M.tooltip(sec.tip or full)
                        end
                        ny = ny + sh
                    end
                    ny = ny + S(4)
                end
            end
            ny = ny + (gi < #GROUPS and S(6) or 0)
        end
        imgui.SetCursorScreenPos({ x, ny })
        imgui.Dummy({ 1, S(4) })
    end
    imgui.EndChild()
end

-- THE PANELS, in the hybrid layout (/vana3): a section is drawn as cards, a faint fill with a hairline edge, 6 round, 8 in from
-- the page's edges. A section with sub-panels (its subheaders that aren't a group's or a headline: Client > Game's Display,
-- Resolution ...) is a title and a card for each sub-panel; any other is one card, its heading and rows in it, and a headline
-- (DLSS 5, FSR beside DLSS) starts another. What isn't in a card (the rows before the first sub-panel, a section with no
-- heading) is on the page. A card's height is what it was drawn at last (the rows set it as they go: M.hy.card_h); the first
-- time, its rows' heights added up. draw_section keeps its state in ctx.cards.
M.hy.card_h = {}

-- the section drawn in the panels' way; returns its height with the gap under it. Inside it the names stand 16 from a card's
-- left edge and the controls 16 from its right (the card is 4 out on the left of draw_section's x, 7 wider than its w)
function M.hy.card(page, sec, x, y, w, over_title, rows)
    return draw_section(page, sec, x + S(12), y, w - S(27), over_title, rows, true) + S(10)
end

-- a row that starts a card of its own: 'sub' (a sub-panel's heading) or 'headline', else nil
function M.hy.start_kind(cards, row)
    if row.kind ~= 'subheader' or row.nested or row.group or row.in_group or row.text == 'Reset'
        or row_height(row, cards.sec, note_width(cards.w)) == 0 then
        return nil
    end
    return row.headline and 'headline' or 'sub'
end

-- the state of a section's cards (ctx.cards), and the way it's drawn: 'card' (the heading in the first card, with the rows
-- up to the first sub-panel) or 'title' (no rows before it: the heading alone in a card); M.hy.mode says it to the headings
-- (draw_heading, draw_row)
function M.hy.begin(ctx, page, sec, list, x, w, rows)
    local cards = { x = x, w = w, sec = sec, list = list, mode = 'card',
                    key = page.id .. '|' .. tostring(sec.key or sec.title) .. (rows ~= nil and '|search' or '') }
    ctx.cards = cards
    -- (a heading with no rows before its first sub-panel is a card of its own, the page's title: no line under it)
    if M.hy.span(cards, 1, false) == 0 then
        cards.mode = 'title'
    end
    M.hy.mode = cards.mode
    return cards
end

-- the heights of the rows from list[from] up to the next card's (list[from] itself is the card's first when `own`)
function M.hy.span(cards, from, own)
    local total = 0
    for j = from, #cards.list do
        local row = cards.list[j]
        if not (own and j == from) and M.hy.start_kind(cards, row) ~= nil then
            break
        end
        total = total + row_height(row, cards.sec, note_width(cards.w))
    end
    return total
end

-- a card from `y0`, drawn before its rows (so it's under them) at the height it had last time, or `est`. With M.hy.FLAT there's
-- no box round the card (boxes in boxes: the Advanced Settings, the status rows): its heading's line and the rows' only.
function M.hy.open(cards, key, y0, est)
    local h = M.hy.card_h[key] or est
    local top, bottom = st.view_top, st.view_bottom
    if not M.hy.FLAT and (top == nil or (y0 + h >= top and y0 <= bottom)) then
        local dl = imgui.GetWindowDrawList()
        local bx, bw = cards.x - S(4), cards.w + S(11)
        M.fill(dl, bx, y0, bw, h, '#FFFFFF', S(6), 0, 0.04)
        M.outline(dl, bx, y0, bw, h, M.hy.LINE, S(6), 0, 1, 0.16)
    end
    cards.cur = { key = key, y0 = y0 }
end

-- the card open ends at `bottom`: that's its height for the next frame
function M.hy.close(cards, bottom)
    if cards.cur ~= nil then
        M.hy.card_h[cards.cur.key] = bottom - cards.cur.y0
        cards.cur = nil
    end
end

-- called for each row at `ry`: one that starts a card closes the one open and opens its own (ctx.panel_head says the row's
-- heading is a card's: draw_row, M.draw_headline). The card's top is 4.5 above the heading's line less its room above
-- (draw_row's subheader: sub_room, the heading's line at ROW_H / 2 below), a headline's 10 down (M.draw_headline)
function M.hy.row(ctx, row, i, ry)
    local cards = ctx.cards
    local kind = M.hy.start_kind(cards, row)
    ctx.panel_head = kind ~= nil
    if kind == nil then
        return
    end
    M.hy.close(cards, ry + (kind == 'sub' and S(3) or S(2)))
    local key = cards.key .. '|' .. i .. '|' .. tostring(row.text)
    if kind == 'sub' then
        local room = sub_room(row, cards.sec)
        M.hy.open(cards, key, ry + room + S(ROW_H) / 2 - S(18), M.hy.span(cards, i, true) + S(8.5) - room)
    else
        M.hy.open(cards, key, ry + S(10), M.hy.span(cards, i, true) - S(8))
    end
end

function M.hy.finish(cards, ry)
    M.hy.close(cards, ry + S(2))
    M.hy.mode = nil
end

local function draw_page(x, y, w, h, hy)
    local dl = imgui.GetWindowDrawList()
    local page = st.page
    local pad = S(PAD)
    local query = st.search[1]
    local searching = query ~= ''
    local results = searching and search_results(query) or nil
    local rows = page.tab ~= nil and page_rows(page) or {}
    -- with sub-tabs, the one picked (Graphics' Presets is the presets page)
    local subs, shown = nil, nil
    if not searching then
        subs, shown = subtabs_of(page)
    end
    local on_presets = shown ~= nil and shown.presets == true
    use_presets(page)

    -- the hybrid layout: the header above has the title line's jobs, but the page's own buttons (Refresh ...) are drawn in a
    -- line at the top of the page
    local hy_buttons = {}
    if hy and not searching and page.tab ~= nil then
        for _, b in ipairs(page.buttons or page.tab.buttons or {}) do
            if b[1] ~= 'Frame stats' and b[1] ~= 'Performance graph' then -- (those are Client's tools)
                hy_buttons[#hy_buttons + 1] = b
            end
        end
    end
    local title_h = hy and (#hy_buttons > 0 and S(34) or 0) or S(TITLE_H)
    -- the title line: the switch for the list of pages, the page's name, its note, its tab's buttons, Reset at the
    -- right, a line under
    if not hy then
        local cy = y + S(TITLE_H) / 2
        -- the list of pages hidden or shown again (the window narrows by it; a search is cleared with the box it's typed in)
        local switched, over_switch = M.icon_button('##sidebar', x + S(15), cy, 'bars', 10.5, P.text, P.dim)
        if switched then
            st.sidebar = not st.sidebar
            if not st.sidebar then
                st.search[1] = ''
            end
            save_settings()
        end
        if over_switch then
            M.tooltip(st.sidebar and 'Hide the list of pages' or 'Show the list of pages')
        end
        local tx = x + S(30)
        local title = searching and 'Search' or page.name
        M.text_mid(dl, tx, cy, title, P.white, fonts.semi, BODY + 3)
        local title_w = M.measure(title, fonts.semi, BODY + 3)
        local note = nil
        if searching then
            note = #results == 0 and 'Nothing matches.' or string.format('%d section%s', #results, #results == 1 and '' or 's')
        elseif on_presets then
            note = preset_kind.note
        else
            note = page.note
        end
        local rx = x + w - pad - S(48) -- (the minimize and close buttons at the right)
        if not searching and page.tab ~= nil then
            -- (no reset for the whole page: each section has its own)
            rx = rx + S(8)
            -- the tab's buttons: Frame stats, Performance graph, Refresh
            local buttons = {}
            for _, b in ipairs(page.buttons or page.tab.buttons or {}) do
                if b[1] ~= 'Frame stats' and b[1] ~= 'Performance graph' then -- those are the sidebar's Stats and Graphs
                    buttons[#buttons + 1] = b
                end
            end
            for i = #buttons, 1, -1 do
                local b = buttons[i]
                local icon = b[3] == 'refresh' and 'reload' or b[3] == 'show_chart' and 'chart' or b[3] == 'account_tree' and 'stats' or b[3] == 'eye' and 'eye' or b[3] == 'hud' and 'hud' or nil
                local bw = M.button_width(b[1], icon)
                rx = rx - S(8) - bw
                local clicked2 = M.button('##tabbutton.' .. page.id .. i, rx, cy, b[1], icon, 'ghost')
                if imgui.IsItemHovered() and b[4] then
                    M.tooltip(b[4])
                end
                if clicked2 then
                    if b[2] ~= nil then
                        pcall(b[2])
                    else
                        M.rebuild()
                    end
                end
            end
        end
        if note then
            local nx = tx + title_w + S(10)
            local room = rx - S(12) - nx
            if room > S(40) then
                M.text_mid(dl, nx, cy, M.fit(note, room, fonts.text, SMALL), P.faint, fonts.text, SMALL)
            end
        end
        if page.preset_pick and not searching and not on_presets then
            -- "Preset:" and the saved presets to pick from: the one picked is loaded
            if not st.presets.read then
                read_presets()
            end
            local px = tx + title_w + S(24)
            M.text_mid(dl, px, cy, 'Preset:', P.dim, fonts.text, SMALL + 0.5)
            px = px + M.measure('Preset:', fonts.text, SMALL + 0.5) + S(8)
            local options = {}
            for _, entry in ipairs(st.presets.list) do
                options[#options + 1] = { entry.name, entry.name }
            end
            local none = #options == 0
            if none then
                options[1] = { '', 'No presets saved' }
            end
            local shown = current_preset() or (none and '' or nil)
            local value = shown or 'None'
            local pick_w = math.min(S(220), rx - S(12) - px)
            local picked, moved = M.dropdown('##preset.pick', px, cy, pick_w, options, value, not none, nil, true)
            if moved then
                for _, entry in ipairs(st.presets.list) do
                    if entry.name == picked then
                        load_preset(entry)
                    end
                end
            end
        end
        M.hline(dl, x, x + w, y + S(TITLE_H), P.border)
    end
    if #hy_buttons > 0 then
        local by, bx = y + title_h / 2, x + w - pad
        for i = #hy_buttons, 1, -1 do
            local b = hy_buttons[i]
            local icon = b[3] == 'refresh' and 'reload' or b[3] == 'show_chart' and 'chart' or b[3] == 'account_tree' and 'stats' or b[3] == 'eye' and 'eye' or b[3] == 'hud' and 'hud' or nil
            bx = bx - M.button_width(b[1], icon)
            local clicked = M.button('##hy.button.' .. page.id .. i, bx, by, b[1], icon, 'ghost')
            if imgui.IsItemHovered() and b[4] then
                M.tooltip(b[4])
            end
            if clicked then
                if b[2] ~= nil then
                    pcall(b[2])
                else
                    M.rebuild()
                end
            end
            bx = bx - S(8)
        end
        M.hline(dl, x, x + w, y + title_h, P.border)
    end

    -- the sections, scrolling: with sub-tabs, the one picked (and any rows before the first heading), each its own scroll.
    -- They stop above the resize corner (a child over it would take its mouse)
    local bottom = y + h - S(RESIZE.GRIP) - S(2)
    imgui.SetCursorScreenPos({ x, y + title_h + 1 })
    st.view_top, st.view_bottom = y + title_h + 1, bottom
    local child = searching and 'search' or page.id .. (shown ~= nil and '.' .. shown.title or '')
    if imgui.BeginChild('##page.' .. child, { w, bottom - y - title_h - 1 }, 0, M.NO_NAV) then
        local cx, cy0 = imgui.GetCursorScreenPos()
        local cw = (imgui.GetContentRegionAvail())
        local at = cy0 + S(hy and 8 or 3)
        -- (the hybrid layout's sections are panels: M.hy.card)
        local function section(pg, sec, over, rws)
            if hy then
                return M.hy.card(pg, sec, cx, at, cw, over, rws)
            end
            return draw_section(pg, sec, cx, at, cw, over, rws)
        end
        if searching then
            for _, r in ipairs(results) do
                at = at + section(r.page, r.section, r.page.name .. ' ›', r.rows)
            end
        elseif on_presets then
            at = at + draw_presets_page(page, cx, at, cw)
        else
            for _, sec in ipairs(sections_of(page, rows)) do
                if shown == nil or shown.all or sec.head == nil or sec.title == shown.title then
                    at = at + section(page, sec)
                end
            end
        end
        imgui.SetCursorScreenPos({ cx, at })
        imgui.Dummy({ 1, S(3) })
    end
    imgui.EndChild()
    st.view_top, st.view_bottom = nil, nil
end

-------------------------------------------------------------------------------
-- The pickers beside the window: ImGui's colour picker, and a typed number for a slider
-------------------------------------------------------------------------------

-- NoAlpha, DisplayRGB, DisplayHex, PickerHueBar
local PICKER_FLAGS = 2 + 1048576 + 4194304 + 33554432

local function hex_of(rgb)
    local function c(v) return math.floor(math.max(0, math.min(1, v or 0)) * 255 + 0.5) end
    return string.format('#%02X%02X%02X', c(rgb[1]), c(rgb[2]), c(rgb[3]))
end

local function rgb_of(hex)
    return { tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255 }
end

local function draw_pickers(wx, wy, ww, wh, screen_w, screen_h)
    local p = st.picker
    if p == nil then
        return
    end
    if not p.seen then
        st.picker = nil -- its row isn't drawn any more (scrolled off, another page)
        return
    end
    p.seen = false
    local row = p.row
    local pw, ph = p.kind == 'colour' and S(300) or S(280), p.kind == 'colour' and S(340) or S(120)
    -- placed in ImGui's pixels, where this window and it are drawn k times their size from their top-left corners
    -- (M.display): beside the window, or left of it if that's off the screen
    local dw, dh, k = M.display(screen_w, screen_h)
    local x = wx + (ww + S(8)) * k
    if x + pw * k > dw then
        x = math.max(0, wx - (pw + S(8)) * k)
    end
    local y = math.max(0, math.min(dh - ph * k, wy + (p.y - wy - S(20)) * k))
    imgui.SetNextWindowPos({ x, y }, 1)
    M.push_popup_style()
    M.push(fonts.text, SMALL + 1)
    if p.kind == 'colour' then
        if p.rgb == nil then
            local value = M.read(row)
            p.rgb = rgb_of(value ~= '' and value or row.game)
            p.last = value
        end
        if imgui.Begin('##vana_colour', nil, M.POPUP_WINDOW) then
            imgui.Text(row.label)
            imgui.SetNextItemWidth(S(230))
            if imgui.ColorPicker3('##colour', p.rgb, PICKER_FLAGS) then
                local hex = hex_of(p.rgb)
                if hex ~= p.last then
                    p.last = hex
                    changed(row, hex, true)
                end
            end
            if imgui.Button(row.none or "Game's colour") then
                changed(row, '', true)
                st.picker = nil
            end
            imgui.SameLine()
            if imgui.Button('Done') then
                st.picker = nil
            end
        end
        imgui.End()
    else
        if imgui.Begin('##vana_number', nil, M.POPUP_WINDOW) then
            imgui.Text(row.label)
            imgui.SetNextItemWidth(S(150))
            if st.number_focus then
                imgui.SetKeyboardFocusHere()
                st.number_focus = false
            end
            -- CharsDecimal, EnterReturnsTrue, AutoSelectAll (ImGui 1.92's numbers)
            local entered = imgui.InputText('##number', st.number, 24, 1 + 64 + 4096)
            local typed = tonumber(st.number[1])
            imgui.SameLine()
            local set = imgui.Button('Set')
            imgui.SameLine()
            local cancel = imgui.Button('Cancel')
            local reset = false
            if not row.no_reset then
                imgui.SameLine()
                reset = imgui.Button('Reset')
                if imgui.IsItemHovered() then
                    imgui.SetTooltip('Back to the default, ' .. M.format_value(row, row.default))
                end
            end
            if row.free and (row.low ~= nil or row.high ~= nil) then
                imgui.TextDisabled(string.format('Any value from %s to %s', row.low ~= nil and M.format_value(row, row.low) or 'any',
                                                 row.high ~= nil and M.format_value(row, row.high) or 'any'))
            elseif row.free then
                imgui.TextDisabled('Any value works')
            else
                imgui.TextDisabled(string.format('%s to %s', M.format_value(row, row.min), M.format_value(row, row.max)))
            end
            if (set or entered) and typed ~= nil then
                typed = M.within(row, typed)
                if row.step >= 1 then
                    typed = math.floor(typed + 0.5)
                end
                changed(row, typed, true)
                st.picker = nil
            elseif reset then
                M.put_back(row) -- (a follower's key taken out: it follows again)
                st.picker = nil
            elseif cancel then
                st.picker = nil
            end
        end
        imgui.End()
    end
    M.pop()
    M.pop_popup_style()
end

-------------------------------------------------------------------------------
-- The window
-------------------------------------------------------------------------------

-- the minimize button: the window shrinks to its tile (draw_tile), at the window's top right corner
local function minimise()
    st.minimised = true
    -- the tile's place: where it was last dragged to ([vana] tile.x and tile.y, saved as it's let go), else the window's
    -- top right corner; once shown, ImGui keeps it where it is for the session (seen 2026-10-21: it went back to the
    -- corner at every minimize)
    if not st.tile_placed then
        local tx, ty = tonumber(vana.config.get('vana', 'tile.x') or ''), tonumber(vana.config.get('vana', 'tile.y') or '')
        if tx ~= nil and ty ~= nil then
            st.tile_at = { tx, ty }
        else
            st.tile_at = { (st.win_x or 0) + (st.win_w or 0) - (S(TILE) + S(MARGIN) * 2) * (st.win_k or 1), st.win_y or 0 }
        end
    end
    -- the tile comes up under the mouse (the window's top right corner, where the button is) while the button's press is
    -- still down: false, the press that minimised, let go without opening (seen 2026-10-21: minimised and opened again at once)
    st.tile_press = nil
    if imgui.IsMouseDown(0) then
        st.tile_press = false
    end
    st.picker = nil
    M.help_close()
    M.flush()
end

-- The resize corner, bottom right, as API Explorer's: three short lines across it, lit under the mouse; dragged, the window's
-- corner follows the mouse (taken at the next frame, no smaller than WIN_W by MIN_H), saved as it's let go (settings.lua).
-- `side`: the list of pages' width now (shut, the window is narrower by it than its size)
function RESIZE.corner(dl, x, y, pw, ph, side, screen_w, screen_h)
    local g = S(RESIZE.GRIP)
    local hovered, _, active = M.hit('##resize', x + pw - g, y + ph - g, g, g)
    if active then
        local mx, my = imgui.GetMousePos()
        st.grab = st.grab or { x + pw - mx, y + ph - my }
        local w = (mx + st.grab[1] - x + S(SIDE_W) - side) / M.scale
        local h = (my + st.grab[2] - y) / M.scale
        st.size = { math.floor(math.max(WIN_W, math.min(w, screen_w / M.scale)) + 0.5),
                    math.floor(math.max(RESIZE.MIN_H, math.min(h, screen_h / M.scale)) + 0.5) }
    elseif st.grab ~= nil then
        st.grab = nil
        save_settings()
    end
    if hovered or active then
        imgui.SetMouseCursor(6) -- ImGuiMouseCursor_ResizeNWSE
    end
    local colour = M.u32((hovered or active) and P.accent or P.dim)
    local right, bottom = x + pw - S(5), y + ph - S(5)
    for _, k in ipairs({ 4, 8, 12 }) do
        dl:AddLine({ right - S(k), bottom }, { right, bottom - S(k) }, colour, math.max(1, S(1.25)))
    end
    if hovered and not active then
        M.tooltip('Drag to resize')
    end
end

-- a rounded rectangle shaded left to right, { colour, alpha } to { colour, alpha }: one gradient rectangle for each pixel row of the
-- corners and one for the middle, so there is no seam to see
function M.hy.gradient(dl, x, y, w, h, left, right, radius)
    local function at(t)
        return M.mix(left[1], right[1], t), left[2] + (right[2] - left[2]) * t
    end
    local function strip(top, bottom, inset)
        local c0, a0 = at(inset / w)
        local c1, a1 = at((w - inset) / w)
        local l, r = M.u32(c0, a0), M.u32(c1, a1)
        dl:AddRectFilledMultiColor({ x + inset, top }, { x + w - inset, bottom }, l, r, r, l)
    end
    local r, top, rows = math.max(1, math.floor(radius + 0.5)), math.floor(y + 0.5), math.floor(h + 0.5)
    for k = 0, r - 1 do
        local inset = r - math.sqrt(r * r - (r - k - 0.5) ^ 2)
        strip(top + k, top + k + 1, inset)
        strip(top + rows - k - 1, top + rows - k, inset)
    end
    strip(top + r, top + rows - r, 0)
end

-- The hybrid layout's header (/vana3), across the top: the pages as tabs (as the /vana2 prototype's categories were), the
-- System pages left to Client's list, and at the right minimize and close. A tab shows its icon and name; with no room for
-- all of them, the ones not shown keep their icon.
function M.hy.header(dl, x, y, pw, radius)
    local hh = S(M.hy.H)
    local cy = y + hh / 2
    local searching = st.search[1] ~= ''
    local home = M.hy.home(st.page)
    M.fill(dl, x, y, pw, hh, P.side, radius, M.CORNERS.top)
    M.hline(dl, x, x + pw, y + hh, P.border)

    local rx = x + pw - S(PAD)
    local minimised, over_minimise = M.icon_button('##minimise', rx - S(22), cy, 'minus', 11, P.accent)
    if over_minimise then
        M.tooltip('Minimize: a tile that opens the window again')
    end
    if M.icon_button('##close', rx, cy, 'close', 11, P.rose) then
        st.open = false
    elseif minimised then
        minimise()
    end
    rx = rx - S(22) - S(18)

    local tabs = {}
    for _, group in ipairs(GROUPS) do
        if group.group ~= 'System' then
            for _, tab in ipairs(group.pages) do
                if tab.id ~= 'experimental' then -- (Graphics' list has its panels)
                    tabs[#tabs + 1] = { page = tab, label = tab.name:upper() }
                end
            end
        end
    end
    local FS = 12.5
    local function width(tab, full)
        return S(14) + S(14) + (full and (S(8) + M.text_spaced(nil, 0, 0, tab.label, P.white, fonts.semi, FS, 1, S(1.2))) or 0) + S(14)
    end
    local start = x + S(M.hy.PAD - 2) -- (a tab's lit patch begins 2 in: in line with the list's search box)
    local room = rx - S(12) - start
    -- every tab shows its name where there's room: the one shown first, then the others in their order; the rest keep their icon
    for _, tab in ipairs(tabs) do
        tab.active = tab.page == home and not searching
        tab.full = false
        room = room - width(tab, false)
    end
    local order = {}
    for _, tab in ipairs(tabs) do
        if tab.active then
            table.insert(order, 1, tab)
        else
            order[#order + 1] = tab
        end
    end
    for _, tab in ipairs(order) do
        local extra = width(tab, true) - width(tab, false)
        if extra > room then
            break
        end
        tab.full, room = true, room - extra
    end
    local at = start
    for _, tab in ipairs(tabs) do
        local active, full = tab.active, tab.full
        local tw = width(tab, full)
        local hov, clicked = M.hit('##hy.tab.' .. tab.page.id, at, y, tw, hh)
        -- the tab shown: a lit patch behind it, as the list's rows have, and its name in white
        if active then
            M.hy.gradient(dl, at + S(2), cy - S(14), tw - S(4), S(28), { '#243462', 1 }, { P.accent_bg, 0.9 }, S(5)) -- (dark blue, left to right, subtle)
        elseif hov then
            M.fill(dl, at + S(2), cy - S(14), tw - S(4), S(28), P.hover, S(5), 0, 0.7)
        end
        local color = active and P.cyan or (hov and P.white or P.dim)
        M.icon_centred(dl, at + S(14) + S(6), cy, tab.page.icon, color, 12)
        if full then
            M.text_spaced(dl, at + S(14) + S(14) + S(8), cy, tab.label, color, fonts.semi, FS, 1, S(1.2))
        elseif hov then
            M.tooltip(tab.page.name)
        end
        if clicked and not active then
            st.page = tab.page
            st.search[1] = ''
            st.picker = nil
        end
        at = at + tw
    end
end

local function draw_window()
    local now = clock()
    st.dt = math.max(0, math.min(0.1, now - st.last))
    st.last = now
    local screen_w, screen_h = vana.ui.screen()
    screen_w, screen_h = (screen_w and screen_w > 0) and screen_w or 1920, (screen_h and screen_h > 0) and screen_h or 1080
    st.screen_h = screen_h
    M.compute_scale(now, screen_h, M.user_scale() * 100)
    if not M.logo.tried then
        pcall(M.load_logo)
    end

    local size = st.size or { WIN_W, WIN_H }
    local pw, ph = S(math.max(WIN_W, size[1])), S(math.max(RESIZE.MIN_H, size[2]))
    local m = S(MARGIN)
    -- the list of pages slides shut (and open) and the window narrows by it; its contents draw once it's fully open
    local side = S(SIDE_W) * M.ease('sidebar', (st.sidebar or st.hybrid) and 1 or 0, 14)
    pw = pw - S(SIDE_W) + side
    local ww, wh = math.min(pw + m * 2, screen_w), math.min(ph + m * 2, screen_h)
    pw, ph = ww - m * 2, wh - m * 2

    -- placed in ImGui's pixels, where it's drawn ww * k by wh * k (the game window smaller than the interface: k < 1)
    local dw, dh, k = M.display(screen_w, screen_h)
    imgui.SetNextWindowPos({ (dw - ww * k) / 2, (dh - wh * k) / 2 }, M.FIRST_USE_EVER)
    imgui.SetNextWindowSize({ ww, wh }, 0)

    local COL, VAR = M.COL, M.VAR
    local colours = {
        { COL.Text, P.text }, { COL.TextDisabled, P.faint }, { COL.WindowBg, P.bg }, { COL.ChildBg, P.bg },
        { COL.PopupBg, '#0F1118' }, { COL.Border, P.border }, { COL.FrameBg, P.well }, { COL.ScrollbarBg, P.bg },
        { COL.ScrollbarGrab, '#3A4158' }, { COL.ScrollbarGrabHovered, '#4A5270' }, { COL.ScrollbarGrabActive, P.accent_lo },
    }
    for _, c in ipairs(colours) do
        imgui.PushStyleColor(c[1], M.rgba(c[2], (c[1] == COL.ChildBg or c[1] == COL.ScrollbarBg) and 0 or 1))
    end
    imgui.PushStyleVar(VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleVar(VAR.WindowRounding, 0)
    imgui.PushStyleVar(VAR.WindowBorderSize, 0)
    imgui.PushStyleVar(VAR.ChildRounding, 0)
    imgui.PushStyleVar(VAR.ChildBorderSize, 0)
    imgui.PushStyleVar(VAR.PopupRounding, S(4))
    imgui.PushStyleVar(VAR.PopupBorderSize, 1)
    imgui.PushStyleVar(VAR.FramePadding, { S(8), S(4) })
    imgui.PushStyleVar(VAR.FrameRounding, S(3))
    imgui.PushStyleVar(VAR.ItemSpacing, { 0, 0 })
    imgui.PushStyleVar(VAR.ScrollbarSize, S(6))
    imgui.PushStyleVar(VAR.ScrollbarRounding, S(3))

    local open = { true }
    local wx, wy = 0, 0
    if imgui.Begin('VanaCore###vana', open, M.WINDOW_FLAGS) then
        wx, wy = imgui.GetWindowPos()
        -- kept on the screen: a place saved on a bigger one (imgui.ini), a smaller resolution, or dragged past an edge.
        -- Held there before it's drawn: put back a frame later (SetNextWindowPos), a drag took it out again each other
        -- frame and it shook
        local cx = math.max(0, math.min(wx, dw - ww * k))
        local cy = math.max(0, math.min(wy, dh - wh * k))
        local clipped = false
        if cx ~= wx or cy ~= wy then
            imgui.SetWindowPos({ cx, cy }, 0)
            wx, wy = imgui.GetWindowPos()
            -- Begin set the window's clip rect where the drag had put it: moved here, everything past that rect is
            -- cut off (the window faded away at the edge while it was dragged). The whole screen is the clip now
            imgui.PushClipRect({ 0, 0 }, { dw, dh }, false)
            clipped = true
        end
        st.win_x, st.win_y, st.win_w, st.win_k = wx, wy, ww * k, k
        local x, y = wx + m, wy + m
        local dl = imgui.GetWindowDrawList()
        local radius = S(5)

        M.fill(dl, x - 1, y, pw + 2, ph + 2, '#000000', radius + 1, 0, 0.35)
        M.fill(dl, x, y, pw, ph, P.bg, radius)
        local px = x
        -- the hybrid layout (/vana3): a header across the top, the list of pages and the page under it
        local hy = st.hybrid == true
        local top = hy and S(M.hy.H) or 0
        if side > 0 then
            -- shaded left to right, darker to lighter: the rounded corners in the left colour, the rest one gradient
            M.fill(dl, x, y + top, math.min(side, radius * 2), ph - top, P.side_l, radius, hy and 64 or M.CORNERS.left)
            if side > radius then
                local l, r = M.u32(P.side_l), M.u32(P.side)
                dl:AddRectFilledMultiColor({ x + radius, y + top }, { x + side, y + ph }, l, r, r, l)
            end
            M.vline(dl, x + side, y + top, y + ph, P.border)
            px = x + side + 1
        end
        M.outline(dl, x, y, pw, ph, P.border, radius)

        if side >= S(SIDE_W) then
            draw_sidebar(x, y + top, side, ph - top)
        end
        draw_page(px, y + top, x + pw - px, ph - top, hy)
        RESIZE.corner(dl, x, y, pw, ph, side, screen_w, screen_h)
        if not st.hover_seen then
            st.hover_key = nil
        end
        st.hover_seen = false

        if hy then
            M.hy.header(dl, x, y, pw, radius) -- (with its minimize and close)
        else
            local minimised, over_minimise = M.icon_button('##minimise', x + pw - S(PAD) - S(22), y + S(TITLE_H) / 2, 'minus', 11, P.accent)
            if over_minimise then
                M.tooltip('Minimize: a tile that opens the window again')
            end
            if M.icon_button('##close', x + pw - S(PAD), y + S(TITLE_H) / 2, 'close', 11, P.rose) then
                st.open = false
            elseif minimised then
                minimise()
            end
        end
        if st.dialog == nil or st.dialog.host ~= 'hud' then -- (the HUD Manager's Presets window has its own: hud.presets_body)
            draw_dialog(x, y, pw, ph) -- Export and Import over the window, if one is open
        end
        if clipped then
            imgui.PopClipRect()
        end
    end
    imgui.End()
    imgui.PopStyleVar(12)
    imgui.PopStyleColor(#colours)

    if not M.hud_owns(st.picker) then -- (the HUD Manager's own is beside it: hud.lua)
        draw_pickers(wx, wy, ww, wh, screen_w, screen_h)
    end
    M.draw_help(wx + m * k, wy + m * k, pw, ph, screen_w, screen_h)
end

-- Minimized: the tile, a 64 x 64 square (at the window's scale) with the logo in it, put at the window's top right corner and
-- dragged anywhere from there (ImGui moves it by its background, as the window). A click opens the window again, where
-- it was.
local function draw_tile()
    local now = clock()
    st.dt = math.max(0, math.min(0.1, now - st.last))
    st.last = now
    if not M.logo.tried then
        pcall(M.load_logo)
    end
    local screen_w, screen_h = vana.ui.screen()
    screen_w, screen_h = (screen_w and screen_w > 0) and screen_w or 1920, (screen_h and screen_h > 0) and screen_h or 1080
    M.compute_scale(now, screen_h, M.user_scale() * 100)

    local m = S(MARGIN)
    local tw = S(TILE)
    local ww = tw + m * 2
    local dw, dh, k = M.display(screen_w, screen_h)
    if st.tile_at ~= nil then
        imgui.SetNextWindowPos({ st.tile_at[1], st.tile_at[2] }, 0)
        st.tile_at = nil
        st.tile_placed = true
    end
    imgui.SetNextWindowSize({ ww, ww }, 0)
    local VAR = M.VAR
    imgui.PushStyleVar(VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleVar(VAR.WindowRounding, 0)
    imgui.PushStyleVar(VAR.WindowBorderSize, 0)
    if imgui.Begin('##vana_tile', nil, M.WINDOW_FLAGS) then
        local wx, wy = imgui.GetWindowPos()
        local cx = math.max(0, math.min(wx, dw - ww * k)) -- kept on the screen, as the window is
        local cy = math.max(0, math.min(wy, dh - ww * k))
        if cx ~= wx or cy ~= wy then
            imgui.SetWindowPos({ cx, cy }, 0)
            wx, wy = imgui.GetWindowPos()
        end
        local x, y = wx + m, wy + m
        local dl = imgui.GetWindowDrawList()
        local r = S(10)
        local hovered = M.over(x, y, tw, tw)
        local t = M.ease('tile', hovered and 1 or 0, 22)
        M.fill(dl, x - 1, y, tw + 2, tw + 2, '#000000', r + 1, 0, 0.35)
        M.fill(dl, x, y, tw, tw, P.bg, r)
        M.fill(dl, x, y, tw, tw, P.accent, r, 0, 0.1 * t)
        M.outline(dl, x, y, tw, tw, M.mix(P.border, P.accent, t), r)
        if M.logo.texture ~= nil then -- the logo (About's), a V where it couldn't be loaded
            local pad = S(6)
            dl:AddImage(M.logo.texture, { x + pad, y + pad }, { x + tw - pad, y + tw - pad })
        else
            M.text_centred(dl, x, y, tw, tw, 'V', M.mix(P.accent, P.white, t), fonts.semi, 40)
        end
        if hovered then
            M.tooltip('VanaCore: click to open the window')
        end
        -- let go where it was pressed: a click, the window opens; moved first: the tile was dragged
        local down = imgui.IsMouseDown(0)
        if down and hovered and st.tile_press == nil then
            local mx, my = imgui.GetMousePos()
            st.tile_press = { mx, my }
        elseif not down and st.tile_press ~= nil then
            local mx, my = imgui.GetMousePos()
            local press = st.tile_press -- (false: the minimize button's own press, let go here)
            local moved = press == false or math.abs(mx - press[1]) + math.abs(my - press[2]) > S(3)
            st.tile_press = nil
            if hovered and not moved then
                st.minimised = false
                st.last = clock()
            elseif press ~= false then -- dragged: where it was left, for the next minimize and the next game
                vana.config.set('vana', 'tile.x', tostring(math.floor(wx + 0.5)))
                vana.config.set('vana', 'tile.y', tostring(math.floor(wy + 0.5)))
            end
        end
    end
    imgui.End()
    imgui.PopStyleVar(3)
end

-------------------------------------------------------------------------------
-- Events and /vana
-------------------------------------------------------------------------------

local failed_at = 0

-- The HUD's Settings ([vana] hud.*, data.lua's rows): its grid and the grid a moved window snaps to; and the HUD Manager
-- window's (hud.lua) boxes, every game window's place in the game. M.hud: hud.lua's way in
local hud = {}
M.hud = hud
-- what hud.lua draws its rows with, as the pages do
M.draw_slider, M.draw_colour, M.changed, M.tip_of, M.row_state, M.draw_pickers = draw_slider, draw_colour, changed, tip_of, row_state, draw_pickers

-- the HUD's switches that are on unless switched off (data.lua's Settings rows have the same defaults)
hud.ON = { ['hud.grid'] = true, ['hud.grid.background'] = true, ['hud.snap'] = true, ['hud.names'] = true, ['hud.autosave'] = true }

function hud.on(key)
    return (tonumber(vana.config.get('vana', key) or '') or (hud.ON[key] and 1 or 0)) ~= 0
end

-- Auto save ([vana] hud.autosave, on by default): the HUD preset loaded ([vana] hud.preset) takes every change of its
-- settings (the HUD's and Game Options'), checked while /vana or the HUD Manager is open: as either opens, and after this
-- state writes a setting (hud.changed), at most every second and a half; none loaded, none saved. The kind of presets
-- shown is put back after. A check reads every one of those settings (~700, ~60 ms), so it isn't run on a timer.
function hud.autosave()
    if not hud.on('hud.autosave') then
        return
    end
    local was = preset_kind
    use_kind(PRESET_KINDS.hud)
    local ok, why = pcall(function ()
        if not st.presets.read then
            read_presets()
        end
        local name = current_preset()
        local entry = name ~= nil and preset_named(name) or nil
        if entry == nil then
            return
        end
        local values = preset_values()
        local same = true
        for key, value in pairs(values) do
            if entry.values[key] ~= value then
                same = false
                break
            end
        end
        for key in pairs(entry.values) do
            if values[key] == nil then
                same = false
                break
            end
        end
        if not same then
            entry.values = values
            entry.saved = os.date('%Y-%m-%d %H:%M')
            write_presets()
        end
    end)
    use_kind(was)
    if not ok then
        print('VanaCore: HUD auto save: ' .. tostring(why))
    end
end

-- every setting this state writes (the /vana rows, the HUD Manager, presets) marks the auto save's check due
do
    local config_set = vana.config.set
    vana.config.set = function(...)
        hud.changed = true
        return config_set(...)
    end
end

-- a box's name in the game with Show element names on (vana.layout.outline's names -> the HUD Manager's)
hud.NAMES = { action = 'Action Menu', actionmagic = 'Action Menu: Magic Sub-Menu', actionability = 'Action Menu: Abilities Sub-Menu', actionlist = 'Action Menu: Magic/Abilities', actionmount = 'Action Menu: Mounts', jobchange = 'Change Jobs', alliance1 = 'Alliance List 1', alliance2 = 'Alliance List 2', auction = 'Auction Menu', castbar = 'Cast Bar',
              chat = 'Chat: Window 1 (Main)', chat2 = 'Chat: Window 2 (Secondary)', input = 'Chat: Input Line', compass = 'Compass', clock = 'Clock',
              macrobar = 'Macro Bar', macroedit = 'Macro Bar: Editing', macroeditoptions = 'Macro Bar: Edit Options', menus = 'Interaction Menu', party = 'Party List', buffs = 'Status Icons',
              target = 'Target Window', map = 'Map', mainmenu = 'Main Menu', statusmenu = 'Main Menu: Status Sub-Menu',
              missions = 'Main Menu: Mission', magicmenu = 'Main Menu: Magic', abilitymenu = 'Main Menu: Abilities',
              synthmenu = 'Main Menu: Synthesis', partymenu = 'Main Menu: Party', sortmenu = 'Main Menu: Options',
              sortconfirm = 'Main Menu: Options Commands',
              fulllog = 'Chat: Maximized', menucontent = 'Menu Content', inventory = 'Inventory', helptitle = 'Help Bar: Title Box', helpbar = 'Help Bar',
              netwin = 'Network Window', searchmenu = 'Main Menu: Search', regionmenu = 'Main Menu: Region Info',
              housemenu = 'Main Menu: View House', bazaarmenu = 'Main Menu: Bazaar', macromenu = 'Main Menu: Macro', macrooptions = 'Main Menu: Macro Options',
              configmenu = 'Main Menu: Config', storagemenu = 'Storage List', checkwin = 'Check Window', commentwin = 'Comment Window',
              itemdetails = 'Item Details', hpgauge = 'HP Gauge', expwin = 'EXP Window', spellinfo = 'Spell Info', regionmap = 'Region Map',
              conqbalance = 'Conquest: Balance', conqpoints = 'Conquest: Points', conqcontrol = 'Conquest: Regional Control',
              besorders = 'Besieged: Orders', besstanding = 'Besieged: Standing', colorank = 'Colonization: Rank',
              colobayld = 'Colonization: Bayld', mapcontrols = 'Map: Controls', ['equip.stats'] = 'Equipment Stats',
              ['equip.stats.check'] = 'Check Window Stats', battletimer = 'Battle Timer', shopconfirm = 'Shop Confirm' }
-- which side of a box snaps to the grid ([vana] hud.anchor.<id>, data.lua's Anchor rows): each its own; the windows at the
-- right of the screen snap their right edges unless set. A box can go by another's instead (its id -> the other's): none do
hud.ANCHOR_OF = {}
hud.RIGHT = { party = true, alliance1 = true, alliance2 = true, mainmenu = true, statusmenu = true, auction = true, missions = true,
              magicmenu = true, abilitymenu = true, synthmenu = true, partymenu = true, sortmenu = true, sortconfirm = true, searchmenu = true,
              regionmenu = true, housemenu = true, bazaarmenu = true, macromenu = true, macrooptions = true, macroeditoptions = true, configmenu = true, storagemenu = true, spellinfo = true,
              conqbalance = true, conqpoints = true, besstanding = true, colobayld = true, mapcontrols = true }
function hud.anchor(what)
    local id = hud.ANCHOR_OF[what] or what
    local value = vana.config.get('vana', 'hud.anchor.' .. id)
    if value == 'left' or value == 'right' then
        return value
    end
    return hud.RIGHT[id] and 'right' or 'left'
end

-- the HUD Manager's element a box's left click picks (vana.layout.clicked), where it isn't its name
hud.SECTIONS = { clock = 'Compass' }

-- a window hidden by its switch ([client] <id>.hide; the Network window's is network.panel=0): its box isn't drawn in the
-- game (nothing there to place). One an addon has hidden (vana.gameui.hide, fancychat's pin: data.lua's hides rows show the
-- switch on and locked) keeps its box: the player can still place it for when the addon lets go, and a window the addon put
-- off the screen has its box at the top left (outline.cpp)
hud.NO_HIDE = { map = true, regionmap = true, mapcontrols = true, menucontent = true, statusmenu = true, missions = true,
                magicmenu = true, abilitymenu = true, synthmenu = true, partymenu = true, sortmenu = true, sortconfirm = true, fulllog = true,
                searchmenu = true, regionmenu = true, housemenu = true, bazaarmenu = true, macromenu = true, macrooptions = true, configmenu = true,
                ['equip.stats'] = true, ['equip.stats.check'] = true } -- (off with equip.stats=0 or .check=0: then it has no box)
function hud.hidden(what)
    if what == 'netwin' then
        return (tonumber(vana.config.get('client', 'network.panel') or '') or 1) == 0
    end
    if what == 'helptitle' then -- (helptitle.hide, or helpbar.title=0 as hid it before)
        return (tonumber(vana.config.get('client', 'helptitle.hide') or '') or 0) ~= 0
            or (tonumber(vana.config.get('client', 'helpbar.title') or '') or 1) == 0
    end
    if hud.NO_HIDE[what] then
        return false
    end
    return (tonumber(vana.config.get('client', what .. '.hide') or '') or 0) ~= 0
end

-- a box's move widget shown in the game while the HUD Manager is open ([vana] hud.widget.<id>=0: not drawn, so it
-- can't be dragged or nudged; its element still picks and its sliders still move it), and those of an element's boxes
-- shown or not (hud.lua's Toggle Move Widget)
function hud.widget_shown(what)
    return (tonumber(vana.config.get('vana', 'hud.widget.' .. what) or '') or 1) ~= 0
end
function hud.show_widgets(ids, on)
    for _, what in ipairs(ids) do
        vana.config.set('vana', 'hud.widget.' .. what, not on and '0' or nil)
    end
end

-- the HUD Manager's elements: the HUD page's sections of a game window (data.lua's element headings), in its order
function hud.elements()
    local page = page_named('hud')
    local list = {}
    for _, sec in ipairs(page ~= nil and sections_of(page, page_rows(page)) or {}) do
        if sec.head ~= nil and sec.head.element then
            list[#list + 1] = sec
        end
    end
    return list
end

-- the HUD Manager's preset drop-down (hud.lua), as a page's title line has it: the HUD presets, the one picked loaded.
-- The kind of presets shown is put back after. Returns the box's right edge.
function hud.preset_pick(id, x, cy, w)
    local was = preset_kind
    use_kind(PRESET_KINDS.hud)
    local right = x + w
    local ok, why = pcall(function ()
        if not st.presets.read then
            read_presets()
        end
        local options = {}
        for _, entry in ipairs(st.presets.list) do
            options[#options + 1] = { entry.name, entry.name }
        end
        local none = #options == 0
        if none then
            options[1] = { '', 'No presets saved' }
        end
        local picked, moved
        picked, moved, right = M.dropdown(id, x, cy, w, options, current_preset() or (none and '' or 'None'), not none, nil, true)
        if moved then
            for _, entry in ipairs(st.presets.list) do
                if entry.name == picked then
                    load_preset(entry)
                end
            end
        end
    end)
    use_kind(was)
    if not ok then
        error(why, 0)
    end
    return right
end

-- each frame /vana or the HUD Manager is open: the grid over the game while the HUD page (not at its Presets) or the HUD
-- Manager is shown; the grid size a dragged box snaps to (vana.layout.outline's second argument), or nil
function hud.grid()
    local page = st.open and st.page ~= nil and st.page.id == 'hud' and (st.sub.hud ~= 'Presets' or st.search[1] ~= '')
    local shown = (page or M.hudman.open) and hud.on('hud.grid')
    local snap = hud.on('hud.snap')
    if not shown and not snap then
        return nil
    end
    local size = math.max(4, math.floor(tonumber(vana.config.get('vana', 'hud.grid.size') or '') or 50))
    if shown and vana.layout.grid ~= nil then
        local color = vana.config.get('vana', 'hud.grid.color') or ''
        local fill = hud.on('hud.grid.background') and (vana.config.get('vana', 'hud.grid.background.color') or '') or nil
        if fill ~= nil and not fill:match('^#%x%x%x%x%x%x$') then
            fill = '#3D5781'
        end
        pcall(vana.layout.grid, size, color:match('^#%x%x%x%x%x%x$') and color or '#A3A3A3',
              (tonumber(vana.config.get('vana', 'hud.grid.opacity') or '') or 16) / 100,
              fill, (tonumber(vana.config.get('vana', 'hud.grid.background.opacity') or '') or 15) / 100,
              (tonumber(vana.config.get('vana', 'hud.grid.center.opacity') or '') or 60) / 100)
    end
    return snap and size or nil
end

-- Undo for the HUD Manager's moves: Ctrl+Z puts back what the last move changed, Ctrl+Y (or Ctrl+Shift+Z) does it again.
-- A step is the settings one move wrote, each as { section, key, text it held before (nil: not set) }: the boxes let go
-- in one frame (hud.boxes; several picked boxes are one step), an X or Y slider dragged, typed or reset (edit_note, from
-- draw_slider and changed). An addon's window moved in the Addon View is { layout = its name, x, y: where it was }. Kept
-- while the game runs, the HUD Manager shut or open.
local UNDO_MAX = 200
hud.history, hud.redo = {}, {}

-- the text `section`/`key` holds now, nil when it isn't set
function hud.held(section, key)
    local text = vana.config.get(section, key)
    return text ~= '' and text or nil
end

function hud.undo_push(step)
    if #step == 0 then
        return
    end
    local list = hud.history
    list[#list + 1] = step
    if #list > UNDO_MAX then
        table.remove(list, 1)
    end
    hud.redo = {}
end

-- An X or Y slider of a place is about to change in the HUD Manager (`value`: the one it gets; nil at the start of a
-- drag, which gets its first write next): its setting as it is now is a step
function hud.edit_note(row, value)
    -- (an addon window's X or Y in the Addon View: where the window is, a step)
    if M.hudman.open and row.kind == 'slider' and row.section == 'layout' and row.window ~= nil and (row.key or ''):match('|[xy]$') then
        local at = hud.layout_at(row.window)
        if at ~= nil and (value == nil or math.floor(value + 0.5) ~= math.floor(row.get() + 0.5)) then
            hud.undo_push({ { layout = row.window, x = at.x, y = at.y } })
        end
        return
    end
    if not M.hudman.open or row.kind ~= 'slider' or row.section ~= 'client' or not (row.key or ''):match('%.[xy]$') then
        return
    end
    if value ~= nil and M.format_value(row, value) == M.format_value(row, M.read(row)) then
        return
    end
    hud.undo_push({ { row.section, row.key, hud.held(row.section, row.key) } })
end
M.edit_note = hud.edit_note

-- the last step undone (`again`: the last undone, done again); false when there is none. What each setting holds now
-- goes the other way.
function hud.undo(again)
    local from, to = hud.history, hud.redo
    if again then
        from, to = to, from
    end
    local step = table.remove(from)
    if step == nil then
        return false
    end
    M.flush() -- (a slider's write meanwhile would land after)
    local back = {}
    for _, entry in ipairs(step) do
        if entry.layout ~= nil then -- an addon's window (the Addon View): put back where it was
            local at = hud.layout_at(entry.layout) or { x = entry.x, y = entry.y }
            back[#back + 1] = { layout = entry.layout, x = at.x, y = at.y }
            vana.layout.set(entry.layout, entry.x, entry.y)
        else
            back[#back + 1] = { entry[1], entry[2], hud.held(entry[1], entry[2]) }
            vana.config.set(entry[1], entry[2], entry[3])
        end
    end
    to[#to + 1] = back
    M.rebuild()
    return true
end

-- each frame the HUD Manager is open: Ctrl+Z and Ctrl+Y / Ctrl+Shift+Z. Not while a text box has the keyboard, the chat
-- line is open or the mouse is held (a box or slider being dragged).
function hud.undo_keys()
    if imgui.IsKeyPressed == nil or imgui.IsMouseDown(0) then
        return
    end
    local ok, io = pcall(imgui.GetIO)
    if not ok or io == nil or not io.KeyCtrl or io.KeyAlt or io.WantTextInput then
        return
    end
    local z, y = imgui.IsKeyPressed(0x5A), imgui.IsKeyPressed(0x59)
    if not z and not y then
        return
    end
    local world_ok, world = pcall(vana.game.world)
    if world_ok and type(world) == 'table' and world.chat_open then
        return
    end
    hud.undo(y or io.KeyShift)
end

-- The HUD Manager's Open only ([vana] hud.open_only, its title line's filter): only the windows open in the game now have
-- their boxes, and its element list has only theirs. hud.up_now: each box's window up or not (vana.layout.up; one it can't
-- tell, the chat's, the compass's, the clock's, up), worked out once a frame by hud.boxes; nil while it's off, or with a
-- VanaCore.Core.dll that hasn't vana.layout.up
hud.up_now = nil
function hud.open_only()
    return vana.layout.up ~= nil and hud.on('hud.open_only')
end

-- each frame the HUD Manager is open: the boxes in the game, the game windows' (hud.game_boxes) or with Addon View the
-- addons' (hud.addon_boxes), their names and hints in this window's font, as big on the screen as its small text (M.scale
-- and M.display's k)
function hud.boxes(snap)
    local label_font = fonts.semi
    if label_font ~= nil then
        local sw, sh = vana.ui.screen()
        sw, sh = (sw and sw > 0) and sw or 1920, (sh and sh > 0) and sh or 1080
        local _, _, k = M.display(sw, sh)
        imgui.PushFont(label_font, math.max(8, math.floor(SMALL * M.scale * k + 0.5)))
    end
    local ok, err = pcall(hud.addon_view() and hud.addon_boxes or hud.game_boxes, snap)
    if label_font ~= nil then
        imgui.PopFont()
    end
    if not ok then
        error(err, 0)
    end
end

-- every game window's box in the game but a hidden one's (hud.hidden), one whose move widget is toggled off
-- (hud.widget_shown) and, with Open only, one whose window isn't open (hud.up_now), Shift + drag moving it (to the grid
-- hud.grid gives, `snap`); one let go is saved, and one clicked is picked in the HUD Manager
function hud.game_boxes(snap)
    local up = nil
    if hud.open_only() then
        up = {}
        for what in pairs(hud.NAMES) do
            local ok, open = pcall(vana.layout.up, what)
            up[what] = not ok or open ~= false
        end
    end
    hud.up_now = up
    local names = hud.on('hud.names')
    local let_go = {}
    for what, name in pairs(hud.NAMES) do
        if not hud.hidden(what) and hud.widget_shown(what) and (up == nil or up[what]) then
            local ok, _, x, y = pcall(vana.layout.outline, what, snap, names and name or nil, hud.anchor(what))
            if ok and x ~= nil then
                let_go[what] = { x, y }
            end
        end
    end
    local step = {} -- what the boxes let go this frame held before (Ctrl+Z puts it back)
    for what, at in pairs(let_go) do -- let go: saved, and switched on (the sliders show it)
        local switch = what ~= 'map' and what ~= 'regionmap' -- (the maps have no switch: Full screen off is their place)
        local x, y = string.format('%g', at[1]), string.format('%g', at[2])
        local was_x, was_y = hud.held('client', what .. '.x'), hud.held('client', what .. '.y')
        local was_move = switch and hud.held('client', what .. '.move') or nil
        if tonumber(was_x or '') ~= tonumber(x) or tonumber(was_y or '') ~= tonumber(y)
           or (switch and (tonumber(was_move or '') or 0) == 0) then -- (a click that didn't move it isn't a step)
            if switch then
                step[#step + 1] = { 'client', what .. '.move', was_move }
            end
            step[#step + 1] = { 'client', what .. '.x', was_x }
            step[#step + 1] = { 'client', what .. '.y', was_y }
        end
        if switch then
            vana.config.set('client', what .. '.move', '1')
        end
        vana.config.set('client', what .. '.x', x)
        vana.config.set('client', what .. '.y', y)
        M.rebuild()
    end
    hud.undo_push(step)
    local clicked = vana.layout.clicked ~= nil and vana.layout.clicked() or nil
    if clicked ~= nil and (hud.SECTIONS[clicked] or hud.NAMES[clicked]) ~= nil then
        M.hud_pick(hud.SECTIONS[clicked] or hud.NAMES[clicked], clicked)
    end
end

-- The HUD Manager's Addon View ([vana] hud.addons, its title line's puzzle piece): yellow boxes over the addons' and
-- plugins' windows and their text and shapes (vana.layout.box) in place of the game windows' blue ones, and its list the
-- addons. Their windows are Addons > Layout's (data.lua's M.layout), moved the same way (config\layout.json). Needs a
-- VanaCore.Core.dll with vana.layout.box.
function hud.addon_view()
    return vana.layout ~= nil and vana.layout.box ~= nil and hud.on('hud.addons')
end

-- an addon's or plugin's name as its folder spells it, by its owner ('ashita/xiui' -> 'XIUI'); the loaded ones looked at
-- again every two seconds
hud.addon_names = { at = -10, by = {} }
function hud.addon_name(owner)
    local cache = hud.addon_names
    local now = clock()
    if now - cache.at > 2 then
        cache.at, cache.by = now, {}
        for _, entry in ipairs(vana.addons.loaded and vana.addons.loaded() or {}) do
            cache.by[entry.source .. '/' .. entry.name:lower()] = entry.name
        end
        for _, entry in ipairs(vana.plugins.loaded and vana.plugins.loaded() or {}) do
            cache.by['plugin/' .. entry.name:lower()] = entry.name
        end
    end
    local kind, name = owner:match('^(%w+)/(.+)$')
    if kind == nil then
        return owner
    end
    return cache.by[kind .. '/' .. name:lower()] or name
end

-- a window's own name in the Addon View: ImGui's without its id ("Party Finder"), "Text" for an addon's text and shapes,
-- "Drawing" for a plugin's (what it draws itself, its minimap, with its fonts and primitives)
function hud.window_title(w)
    if w.name:find('##objects/', 1, true) ~= nil then
        return w.owner:match('^plugin/') and 'Drawing' or 'Text'
    end
    return (M.layout.title(w.name))
end

-- The Addon View's addons: each owner of a window Addons > Layout has (not this window's), as { owner, name, kind
-- ('ashita', 'plugin', 'vanacore', 'windower'), shown (something on the screen now), windows (those on the screen first,
-- then by name) }, by name; and the same by owner
function hud.addons()
    local list, by = {}, {}
    for _, w in ipairs((M.layout.windows())) do
        if w.owner ~= '' then
            local addon = by[w.owner]
            if addon == nil then
                addon = { owner = w.owner, name = hud.addon_name(w.owner), kind = w.owner:match('^(%w+)/') or '', shown = false, windows = {} }
                by[w.owner] = addon
                list[#list + 1] = addon
            end
            addon.windows[#addon.windows + 1] = w
            addon.shown = addon.shown or w.shown
        end
    end
    table.sort(list, function (a, b)
        if a.name:lower() ~= b.name:lower() then
            return a.name:lower() < b.name:lower()
        end
        return a.owner < b.owner
    end)
    for _, addon in ipairs(list) do
        table.sort(addon.windows, function (a, b)
            if a.shown ~= b.shown then
                return a.shown
            end
            return a.name:lower() < b.name:lower()
        end)
    end
    return list, by
end

-- where window `name` is now (vana.layout.windows()'s x and y), or nil
function hud.layout_at(name)
    local _, by_name = M.layout.windows()
    local w = by_name[name]
    return w ~= nil and { x = w.x, y = w.y } or nil
end

-- each frame of the Addon View: a box over every addon window and text and shapes on the screen, its addon's name over its
-- corner (with Show element names; an addon with more than one, the window's name after it), Shift + drag moving it (to
-- the grid hud.grid gives, `snap`) and the picked addon's (hudman.addon) the arrow keys'. A move let go is a step for Ctrl+Z,
-- and a box clicked picks its addon in the HUD Manager
function hud.addon_boxes(snap)
    local names = hud.on('hud.names')
    local step = {}
    for _, addon in ipairs((hud.addons())) do
        local shown = 0
        for _, w in ipairs(addon.windows) do
            shown = shown + (w.shown and 1 or 0)
        end
        local picked = addon.owner == M.hudman.addon
        for _, w in ipairs(addon.windows) do
            if w.shown then
                local label = names and (shown > 1 and addon.name .. ': ' .. hud.window_title(w) or addon.name) or nil
                local ok, _, x, _, from_x, from_y = pcall(vana.layout.box, w.name, label, snap, picked)
                if ok and x ~= nil then
                    step[#step + 1] = { layout = w.name, x = from_x, y = from_y }
                end
            end
        end
    end
    hud.undo_push(step)
    local clicked = vana.layout.box_clicked()
    if clicked ~= nil then
        local _, by_name = M.layout.windows()
        local w = by_name[clicked]
        if w ~= nil and w.owner ~= '' then
            M.hud_pick_addon(w.owner)
        end
    end
end

-- The Addon View's settings for an addon (hud.lua): each of its windows by name (and "Text" for its text and shapes), its X,
-- Y and Scale under it, as Addons > Layout has them; built again when its windows change. As a HUD page section,
-- { title, owner, rows }.
hud.addon_built = { owner = nil, signature = nil, sec = nil }
function hud.addon_section(addon)
    local built = hud.addon_built
    local signature = M.layout.signature_of(addon.windows)
    if built.owner == addon.owner and built.signature == signature then
        return built.sec
    end
    local sw, sh = vana.ui.screen()
    local titles = {}
    for _, w in ipairs(addon.windows) do
        local title = hud.window_title(w)
        titles[title] = (titles[title] or 0) + 1
    end
    local rows = {}
    for _, w in ipairs(addon.windows) do
        local title = hud.window_title(w)
        local _, id = M.layout.title(w.name)
        if titles[title] > 1 and id ~= nil then
            title = title .. ' (' .. id .. ')' -- two of its windows go by one name
        end
        local sliders = { M.layout.slider('X', w, 'x', 0, math.max(1, sw or 1920), 1, '%d'),
                          M.layout.slider('Y', w, 'y', 0, math.max(1, sh or 1080), 1, '%d'),
                          M.layout.slider('Scale', w, 'scale', 0.5, 2, 0.05, '%.2fx') }
        -- (ImGui's ids end at a NUL: Addons > Layout's keys have one between the window and the X, Y or Scale)
        for _, sub in ipairs(sliders) do
            sub.key = w.name:gsub('[%z#]', '_') .. '|' .. sub.label:lower()
        end
        rows[#rows + 1] = { kind = 'layout', label = title .. (w.shown and '' or ' (not shown)'), window = w.name, sliders = sliders,
                            flags = M.layout.flags_of(w), -- (its Lock and No Pointer Events, at its name's end)
                            tip = not w.shown and 'Not on the screen now: its place is kept for when it is' or w.pinned
                                  and 'Placed by its addon: moved, it keeps that far from where the addon puts it' or nil,
                            reset = function () vana.layout.reset(w.name) end }
    end
    built.owner, built.signature = addon.owner, signature
    built.sec = { title = addon.name, owner = addon.owner, rows = rows }
    return built.sec
end

-- the Addon View's reset: every window of the addon forgotten (Addons > Layout's reset: its scale 1 again, one its addon
-- places back there), where each was a step for Ctrl+Z
function hud.addon_reset(addon)
    local step = {}
    for _, w in ipairs(addon.windows) do
        if w.saved then
            step[#step + 1] = { layout = w.name, x = w.x, y = w.y }
            vana.layout.reset(w.name)
        end
    end
    hud.undo_push(step)
end

local debug_failed_at = 0
local gptest_failed_at = 0

local function present()
    local now = clock()
    do -- an addon package's prompt (install.lua), with this window open or not
        local ok, err = pcall(M.install.frame, now)
        if not ok and now - (M.install.failed_at or 0) > 2 then
            M.install.failed_at = now
            print('VanaCore: addon package: ' .. tostring(err))
        end
    end
    st.frames = st.frames + 1
    if now - st.since >= 1 then
        st.fps = math.floor(st.frames / (now - st.since) + 0.5)
        M.fps = st.fps
        st.frames, st.since = 0, now
    end
    if M.layout ~= nil then
        M.layout.tick = M.layout.tick + 1
    end
    -- a change that locks or unlocks others: every page's rows built again (the HUD Manager's too, /vana shut)
    if st.rebuild_wanted then
        st.rebuild_wanted = false
        st.rows = {}
    end
    if M.debug.open then -- the Debug window (debug.lua), with this one open or not
        local ok, err = pcall(M.draw_debug, now)
        if not ok and now - debug_failed_at > 2 then
            debug_failed_at = now
            print('VanaCore: debug: ' .. tostring(err))
        end
    end
    if M.gptest.recording then -- the Gamepad Test's recording runs with its window shut too
        local ok, err = pcall(M.gamepadtest_tick, now)
        if not ok and now - gptest_failed_at > 2 then
            gptest_failed_at = now
            print('VanaCore: gamepad recording: ' .. tostring(err))
        end
    end
    if M.gptest.open then -- the Gamepad Test window (gamepadtest.lua), with this one open or not
        local ok, err = pcall(M.draw_gamepadtest, now)
        if not ok and now - gptest_failed_at > 2 then
            gptest_failed_at = now
            print('VanaCore: gamepad test: ' .. tostring(err))
        end
    end
    local managing = M.hudman.open
    if st.open or managing then
        if not hud.was_open then -- just opened: what changed while shut (commands, other addons) is taken up now
            hud.was_open, hud.changed = true, true
        end
        if hud.changed and now - (hud.saved_at or 0) >= 1.5 then
            hud.changed, hud.saved_at = false, now
            hud.autosave()
        end
    else
        hud.was_open = false
    end
    if not st.open and st.grass_solo ~= nil then
        M.grass_solo(nil) -- a library plant shown alone (its eye) goes back into the mix with the window closed
    end
    if not st.open and not managing then
        return
    end
    local grid_ok, snap = pcall(hud.grid)
    snap = grid_ok and snap or nil
    if managing then -- the HUD Manager (hud.lua), with this one open or not
        local ok, err = pcall(hud.boxes, snap)
        if ok then
            ok, err = pcall(hud.undo_keys)
        end
        if ok then
            ok, err = pcall(M.draw_hud, now)
        end
        if not ok and now - (hud.failed_at or 0) > 2 then
            hud.failed_at = now
            print('VanaCore: HUD Manager: ' .. tostring(err))
        end
    end
    if not st.open then
        if next(M.pending) ~= nil then
            M.flush() -- (the HUD Manager's sliders)
        end
        return
    end
    if not fonts.ready then
        fonts.ready = true
        local ok, err = pcall(M.load_fonts)
        if not ok then
            print('VanaCore: fonts: ' .. tostring(err))
        end
        return
    end
    if M.refresh_at ~= nil and now >= M.refresh_at then
        M.refresh_at = nil
        M.rebuild()
    end
    -- the Graphics page's DLSS 5 rows: built again when whether it runs here changes (data.lua's dlss5_seen; set again
    -- when they're next built)
    if M.dlss5_built ~= nil and now >= st.dlss5_next then
        st.dlss5_next = now + 0.5
        local ok, seen = pcall(M.dlss5_seen)
        if ok and seen ~= M.dlss5_built then
            M.dlss5_built = nil
            M.rebuild()
        end
    end
    -- the Client page's Resolution: built again when the window's edges were dragged (window.cpp's [window] lastsize) or it's
    -- maximized or restored
    if M.resolution_built ~= nil and now >= st.resolution_next then
        st.resolution_next = now + 0.5
        if (M.resolution_shown()) ~= M.resolution_built then
            M.resolution_built = nil
            M.rebuild()
        elseif M.monitor_built ~= nil and M.monitor_seen() ~= M.monitor_built then
            -- the window on another monitor: its Monitor, and the resolutions' screen sizes
            M.monitor_built = nil
            M.rebuild()
        end
    end
    -- the Addons page's Layout: built again when a window opens or closes, the mouse's window outlined in the game
    if st.page ~= nil and st.page.id == 'addons' and M.layout.page == 'layout' and vana.layout ~= nil then
        if now >= st.layout_next then
            st.layout_next = now + 0.1
            local signature = M.layout.state((M.layout.windows())) -- (the windows, and the addons loaded)
            if signature ~= M.layout.signature and st.sliding == nil then
                M.rebuild()
            end
        end
        if st.slid ~= nil and st.slid.window ~= nil then
            vana.layout.highlight(st.slid.window)
        end
    end
    -- an addon package's prompt up (install.lua): this window is left undrawn meanwhile. A window drawn under an open prompt
    -- came out over it (the game dimmed with the prompt hidden behind the window), and a prompt is answered before anything else
    local ok, err = true, nil
    if M.install.prompt == nil then
        ok, err = pcall(st.minimised and draw_tile or draw_window)
    end
    if not ok then
        if now - failed_at > 2 then
            failed_at = now
            print('VanaCore: ' .. tostring(err))
        end
    end
    if next(M.pending) ~= nil then
        M.flush() -- a slider's writes while it's dragged, once a frame
    end
end

local function open(page)
    st.page = page or st.page or PAGES[1]
    st.open = true
    st.minimised = false
    st.last = clock()
    st.rows = {}
end

local function close()
    st.open = false
    st.minimised = false
    st.picker = nil
    M.help_close()
    M.flush()
end

-- the HUD Manager (hud.lua) opened (/hud, the HUD page's Open Manager): this window hidden meanwhile, and shown again at
-- the page it was on when the HUD Manager is closed (minimized, its tile is left where it is)
function hud.open_manager()
    if M.hudman.open then
        return
    end
    M.hudman.back = nil
    if st.open and not st.minimised then
        M.hudman.back = st.page
        close()
    end
    M.hud_open()
end

function hud.close_manager()
    if not M.hudman.open then
        return
    end
    M.hud_close()
    if M.hudman.back ~= nil and not st.open then
        open(M.hudman.back)
    end
    M.hudman.back = nil
end

-- the HUD Manager's Presets window (hud.lua), over it: its edit button's, /vana hudpreset's. The HUD Manager is opened
-- first when it isn't (the window belongs to it).
function hud.edit_presets()
    if not M.hudman.open then
        hud.open_manager()
    end
    M.hudman.presets_open = true
end

-- The HUD page's Settings, the section the HUD Manager's Settings window draws (nil: the page has none)
function hud.settings_section()
    local page = page_named('hud')
    for _, sec in ipairs(page ~= nil and sections_of(page, page_rows(page)) or {}) do
        if sec.head ~= nil and sec.title == 'Settings' then
            return sec
        end
    end
    return nil
end

-- the HUD presets (the list, Save and Import, each a row) as the Presets page has them, in the HUD Manager's window at
-- x, y, w; returns the height drawn. The kind of presets shown is put back after.
function hud.presets_body(x, y, w)
    local was = preset_kind
    use_kind(PRESET_KINDS.hud)
    local ok, height = pcall(function ()
        local h = draw_presets_page({}, x, y, w)
        if st.dialog ~= nil and st.dialog.host == nil then
            st.dialog.host = 'hud' -- (opened here: drawn here, not over the /vana window)
        end
        return h
    end)
    use_kind(was)
    if not ok then
        error(height, 0)
    end
    return height
end

-- ... and its Export, Import and Load dialog, over the window at wx, wy, ww, wh
function hud.presets_dialog(wx, wy, ww, wh)
    if st.dialog == nil or st.dialog.host ~= 'hud' then
        return
    end
    local was = preset_kind
    use_kind(PRESET_KINDS.hud)
    local ok, why = pcall(draw_dialog, wx, wy, ww, wh)
    use_kind(was)
    if not ok then
        error(why, 0)
    end
end

-- /vana reload: the addon reloaded (/addon reload vana waits for the game's Present, so a /vana after it would reach
-- this copy), and the new one opens itself at the page this one was on, named in a file it reads and deletes at load
local REOPEN = SETTINGS_DIR .. '\\reopen.txt'

local function reload()
    vana.fs.create_dir(SETTINGS_DIR)
    local f = io.open(REOPEN, 'w')
    if f then
        f:write(st.page and st.page.id or '')
        f:close()
    end
    vana.chat.run('/addon reload vana')
end

vana.events.on('load', function ()
    load_settings()
    st.since = clock()
    local f = io.open(REOPEN, 'r')
    if f then
        local id = f:read('*a') or ''
        f:close()
        os.remove(REOPEN)
        open(page_named(id))
    end
end)

vana.events.on('present', present)

-- /vana grass capture [name]: the plant sheets drawn near the player go into the library (config\grass) and the
-- catalogue; list, reload. A capture's answer comes some frames later (waiting).
local grass = { waiting = false }

function grass.command(words)
    local sub = words[3] and words[3]:lower() or ''
    if sub == 'capture' then
        local ok, message = vana.graphics.grass_capture(table.concat(words, ' ', 4))
        print(message)
        grass.waiting = ok
    elseif sub == 'reload' then
        vana.graphics.grass_reload()
        print('Mass Grass: the catalogue and the zones\' dirt levels were read again.')
        M.rebuild()
    elseif sub == 'list' then
        for _, a in ipairs(vana.graphics.grass_assets()) do
            print(string.format('%s (%s%s%s)', a.name, a.id, a.shipped and '' or ', yours', a.picture and ', a picture' or ''))
        end
    else
        print('//vana grass capture [name] | list | reload: capture finds the plants around you in your game files and adds them to your library (config\\grass).')
    end
end

vana.events.on('present', function ()
    if not grass.waiting then
        return
    end
    local status = vana.graphics.grass_capture_status()
    if status ~= nil then
        grass.waiting = false
        print(status)
        M.rebuild()
    end
end, 'grass_capture')

-- /vana scale <size>: [vana] scale, 0.5 to 3 (or 50% to 300%); reset or default puts it back
local function scale_command(size)
    if size == nil then
        print('VanaCore: the window\'s scale is ' .. math.floor(M.user_scale() * 100 + 0.5) .. '%. /vana scale 2 makes it twice the usual size, /vana scale 1 puts it back (0.5 to 3).')
        return
    end
    size = size:lower()
    local percent = nil
    if size == 'reset' or size == 'default' then
        percent = 100
    else
        local number, unit = size:match('^(%d*%.?%d+)([x%%]?)$')
        number = tonumber(number)
        if number ~= nil then
            percent = math.floor(((unit == 'x' or (unit == '' and number <= 10)) and number * 100 or number) + 0.5)
        end
    end
    if percent == nil or percent < 50 or percent > 300 then
        print('VanaCore: /vana scale takes 0.5 to 3 (or 50% to 300%): /vana scale 2 makes the window twice the usual size.')
        return
    end
    vana.config.set('vana', 'scale', percent ~= 100 and tostring(percent) or nil) -- 100: the default
    print('VanaCore: window scale ' .. percent .. '% ([vana] scale)' .. (st.open and '.' or '; /vana opens it.'))
end

-- /vana preset <name> (a Graphics and Effects preset) and /vana hudpreset <name> (a HUD and Game Options one): the preset
-- of that name loaded, as its Load in the window does, so a line in scripts\boot.txt or a macro can load one (asked
-- 2026-10-24). The name is the rest of the line, in any case; one that isn't saved lists those that are. The kind of
-- presets shown is put back after (hud.autosave's way), so the window's own list stays as it was.
local function preset_command(kind, name)
    local was = preset_kind
    use_kind(kind)
    local ok, why = pcall(function ()
        if not st.presets.read then
            read_presets()
        end
        local entry = preset_named(name)
        if entry ~= nil then
            load_preset(entry)
            print('VanaCore: ' .. kind.what .. ' preset ' .. entry.name .. ' loaded.')
            return
        end
        local names = {}
        for _, e in ipairs(st.presets.list) do
            names[#names + 1] = e.name
        end
        if #names == 0 then
            print('VanaCore: no ' .. kind.what .. ' preset is saved yet: save one at /vana > ' .. kind.where .. ' > Presets.')
        else
            print('VanaCore: ' .. name .. ' isn\'t a saved ' .. kind.what .. ' preset. Saved: ' .. table.concat(names, ', ') .. '.')
        end
    end)
    use_kind(was)
    if not ok then
        print('VanaCore: preset: ' .. tostring(why))
    end
end

-- /vana help (or a word that isn't a command): a line a command, the command in the colour addons give their name (\30 and
-- a colour: FFXI's). stats to lua are VanaCore's own (addons.cpp).
local function print_commands(command, unknown)
    local ids = {}
    for _, page in ipairs(PAGES) do
        ids[#ids + 1] = page.id
    end
    local help = {
        { '', 'open or close the VanaCore window' },
        { ' <page>', 'open it on ' .. table.concat(ids, ', ') },
        { ' layout', 'move addons\' windows on screen' },
        { ' toggles', 'switch each effect on and off, one at a time' },
        { ' gamepad', 'the gamepads (or /gamepad)' },
        { ' pivot', 'the DAT overlays (or /pivot)' },
        { ' hud', 'the HUD page: grid and presets (/hud [element]: the HUD Manager)' },
        { ' hud manager', 'open or close the HUD Manager, as /hud does' },
        { ' commands', 'VanaCore\'s commands, each switched on or off' },
        { ' boot', 'edit scripts\\boot.txt, what runs at each start' },
        { ' install [file]', 'install an addon package (.vcaddon), shown first' },
        { ' preset <name>', 'load a Graphics, Effects and Experimental preset (a boot.txt line can)' },
        { ' hudpreset <name>', 'load a HUD and Game Options preset' },
        { ' reset ui', 'every addon\'s window to the top-left corner' },
        { ' close', 'close the window' },
        { ' minimize', 'shrink it to a tile (the logo) that opens it again' },
        { ' reload', 'reload the window\'s addon and open it again' },
        { ' scale <size>', 'the window\'s size: 2 twice as big, 1 the usual' },
        { ' help <addon>', 'an addon\'s or plugin\'s read me and details' },
        { ' stats [on|off|save]', 'where each frame\'s time goes' },
        { ' graph [on|off|save]', 'FPS and frame time, last minute' },
        { ' fps [<1-60>|capped|uncapped]', 'the most FPS the game draws' },
        { ' fps cutscene [<1-60>|uncapped|off]', 'the FPS in cutscenes' },
        { ' debug', 'the Debug window: DLSS crash tests, frame recording' },        { ' debug <seconds>|stop', 'record frames and find hitches (logs folder)' },
        { ' abtest <setting>=<a>,<b>', 'compare two values\' FPS' },
        { ' telemetry [on|off]', 'clocks, temperatures and power to a CSV' },
        { ' logs [on|off]', 'VanaCore\'s log file' },
        { ' window help', 'move and size the game window' },
        { ' grass capture|list|reload', 'Mass Grass plant library' },
        { ' lua <code>', 'run a line of Lua' },
        { 'old', 'the old VanaCore window' },
    }
    local lines = {}
    if unknown ~= nil then
        lines[1] = '\30\68' .. command .. ' ' .. unknown .. ' isn\'t a command.\30\1'
    end
    lines[#lines + 1] = '\30\81[\30\6VanaCore\30\81]\30\1 ' .. command .. ' commands:'
    for _, h in ipairs(help) do
        lines[#lines + 1] = '  \30\6' .. command .. h[1] .. '\30\1 - ' .. h[2]
    end
    print(table.concat(lines, '\n'))
end

-- /vana fps [<1-60>|capped|uncapped]: [client] fps.max. /vana fps cutscene [<1-60>|uncapped|off]: [client] fps.cutscene,
-- the rate while the player is in an event (framerate.cpp). Both apply at once and are kept in the settings file.
function M.fps_command(words)
    local function fps_rate(word)
        if word == 'uncapped' or word == 'unlocked' or word == 'unlimited' or word == '0' then
            return 0
        elseif word == 'capped' then
            return 60
        end
        local n = tonumber(word)
        if n ~= nil and n >= 1 and n <= 60 then
            return math.floor(n)
        end
        return nil
    end
    local function fps_text(rate)
        return rate == 0 and 'uncapped' or string.format('%d FPS', rate)
    end
    local function read(key)
        return tonumber(vana.config.get('client', key) or '')
    end
    local sub = words[3] and words[3]:lower() or nil
    if sub == 'cutscene' or sub == 'cs' then
        local word = words[4] and words[4]:lower() or nil
        if word == 'off' or word == 'same' or word == 'clear' then
            vana.config.set('client', 'fps.cutscene', nil)
            print('VanaCore: cutscene FPS is off (fps.max holds in a cutscene too).')
            return
        end
        local rate = word ~= nil and fps_rate(word) or nil
        if rate ~= nil then
            vana.config.set('client', 'fps.cutscene', tostring(rate))
            print('VanaCore: cutscenes now run ' .. fps_text(rate) .. ' (fps.cutscene).')
        else
            local now = read('fps.cutscene')
            print('VanaCore: cutscene FPS is ' .. (now ~= nil and now >= 0 and fps_text(now) or 'off')
                .. '. /vana fps cutscene <1-60>|uncapped|off')
        end
        return
    end
    local rate = sub ~= nil and fps_rate(sub) or nil
    if rate ~= nil then
        vana.config.set('client', 'fps.max', tostring(rate))
        print('VanaCore: max FPS ' .. (rate == 0 and 'uncapped (above 60 FPS the game runs fast)' or fps_text(rate)) .. '.')
        return
    end
    local max, cutscene = read('fps.max') or 60, read('fps.cutscene')
    print(string.format('VanaCore: max FPS %s, cutscenes %s, now %d FPS.', fps_text(max),
        cutscene ~= nil and cutscene >= 0 and fps_text(cutscene) or 'the same', M.fps or 0))
    print('  /vana fps <1-60>|capped|uncapped   /vana fps cutscene <1-60>|uncapped|off')
end

-- The window's own commands besides /vana, each switched off in Client > Commands ([commands] <name>=0, data.lua's
-- COMMANDS) for a player whose own addon or plugin has one of that name: left to it then
local OWN_COMMANDS = { hud = true, gamepad = true, gamepads = true, addons = true, plugins = true, pivot = true,
                       api = true, apiexplorer = true }

vana.events.on('command', function (e)
    local words = {}
    for word in e.command:gmatch('%S+') do
        words[#words + 1] = word
    end
    local command = words[1] and words[1]:lower() or nil
    local name = command and command:gsub('^/+', '') or nil
    if name ~= nil and OWN_COMMANDS[name] then
        local value = (vana.config.get('commands', name) or ''):gsub('%s', ''):lower()
        if value == '0' or value == 'off' or value == 'false' then -- (as commands.cpp reads it)
            return false
        end
    end
    -- /addons, /plugins and /pivot: the window at that page
    if name == 'addons' or name == 'plugins' or name == 'pivot' then
        local page = page_named((command:gsub('^/+', '')))
        if page ~= nil then
            st.search[1] = ''
            open(page)
            return true
        end
    end
    -- /hud: the HUD Manager (hud.lua) opened, or closed; /hud <element> opens it at that one (its name or the start of it:
    -- /hud chat, /hud party)
    if command == '//hud' or command == '/hud' then
        local want = table.concat(words, ' ', 2):lower()
        if want == 'open' then -- (opened, never closed: the game's Config > HUD Manager, configmenu.cpp)
            hud.open_manager()
        elseif (want == 'addons' or want == 'addon') and vana.layout.box ~= nil then -- in the Addon View
            vana.config.set('vana', 'hud.addons', '1')
            hud.open_manager()
        elseif want ~= '' then
            for _, sec in ipairs(hud.elements()) do
                if sec.title:lower():sub(1, #want) == want then
                    vana.config.set('vana', 'hud.addons', '0')
                    hud.open_manager()
                    M.hud_pick(sec.title)
                    return true
                end
            end
            -- an addon by its name (or the start of it): the Addon View at that one
            if vana.layout.box ~= nil then
                for _, addon in ipairs((hud.addons())) do
                    if addon.name:lower():sub(1, #want) == want then
                        vana.config.set('vana', 'hud.addons', '1')
                        hud.open_manager()
                        M.hud_pick_addon(addon.owner)
                        return true
                    end
                end
            end
            print('VanaCore: no HUD element or addon is called ' .. want .. ' (/hud opens the HUD Manager with them all).')
        elseif M.hudman.open then
            hud.close_manager()
        else
            hud.open_manager()
        end
        return true
    end
    -- /gamepad: the window at the Client page's Gamepads; /gamepad test the Gamepad Test window
    if command == '//gamepad' or command == '/gamepad' or command == '//gamepads' or command == '/gamepads' then
        if words[2] ~= nil and words[2]:lower() == 'test' then
            M.gamepadtest_toggle()
            return true
        elseif words[2] ~= nil and words[2]:lower() == 'record' then -- starts or stops the Gamepad Test's recording
            M.gamepadtest_record_toggle()
            return true
        end
        local page = page_named('gamepads')
        if page ~= nil then
            st.search[1] = ''
            st.sub[page.id] = 'Gamepads'
            open(page)
            return true
        end
    end
    -- /loadscreen alone: the window at Mods > Loading Screen (the loadscreen addon passes it on; /loadscreen preview is its)
    if (command == '//loadscreen' or command == '/loadscreen') and words[2] == nil then
        local page = page_named('loadscreen')
        if page ~= nil then
            st.search[1] = ''
            st.sub[page.id] = 'Loading Screen'
            open(page)
            return true
        end
    end
    -- /api (or /apiexplorer) with the API Explorer not loaded: it's loaded and shown (/api hide: nothing to hide). Once
    -- it's loaded the apiexplorer addon answers /api itself, so it's passed on (the same patterns as its own)
    local api = e.command:match('^/api%s*(%a*)%s*$') or e.command:match('^/apiexplorer%s*(%a*)%s*$')
    if (api == '' or api == 'show' or api == 'hide') and not M.apiexplorer_loaded() then
        if api ~= 'hide' then
            M.open_apiexplorer()
        end
        return true
    end
    -- /vana3: the window in the hybrid layout (a header with the pages as tabs: M.hy.header), opened, or closed when it's up.
    -- The layout is kept (settings.lua): /vana opens it that way until /vana classic
    if command == '//vana3' or command == '/vana3' then
        if st.hybrid and st.open and not st.minimised then
            close()
        else
            st.hybrid = true
            save_settings()
            open()
        end
        return true
    end
    if command ~= '//vana' and command ~= '/vana' then
        return false
    end
    local what = words[2] and words[2]:lower() or nil
    if what == nil then
        if st.open and not st.minimised then close() else open() end -- (minimized: its tile's click, opened again)
    elseif what == 'classic' then -- the old layout, kept
        st.hybrid = false
        save_settings()
        open()
    elseif what == 'open' then -- opened, never closed (the game's Config > VanaCore, configmenu.cpp)
        if not st.open or st.minimised then open() end
    elseif what == 'close' then
        close()
    elseif what == 'minimize' or what == 'minimise' then
        if st.open and not st.minimised then minimise() end
    elseif what == 'debug' then -- /vana debug alone (VanaCore records frames for /vana debug <seconds> or stop)
        M.debug_toggle()
    elseif what == 'reload' then
        reload()
    elseif what == 'scale' then
        scale_command(words[3])
    elseif what == 'reset' then
        M.layout.reset_command(words[3])
    elseif what == 'layout' then -- the Addons page's Layout
        M.layout.page = 'layout'
        st.search[1] = ''
        open(page_named('addons'))
    elseif what == 'fps' then
        M.fps_command(words)
    elseif what == 'statsdump' then -- the Equipment window's stats, each step, into the log (equipstats_dump.h)
        local n = vana.gameui.stats_dump ~= nil and vana.gameui.stats_dump() or nil
        print(n == nil and 'VanaCore: statsdump needs a newer VanaCore.Core.dll.'
            or n == -2 and 'VanaCore: the equipment data is still being read: try again in a moment.'
            or n == -1 and 'VanaCore: equip_mods.json is missing: nothing to dump (see the log).'
            or string.format('VanaCore: the stats of %d equipped item%s written to the log (Stats dump:).', n, n == 1 and '' or 's'))
    elseif what == 'grass' and words[3] ~= nil then -- /vana grass alone: Mods at its Mass Grass (below)
        grass.command(words)
    elseif what == 'hud' and (words[3] or ''):lower() == 'manager' then
        -- the HUD Manager opened or closed, as /hud does (the game's Config > HUD runs this: /hud may be switched off)
        if M.hudman.open then hud.close_manager() else hud.open_manager() end
    elseif (what == 'preset' or what == 'presets') and words[3] ~= nil then -- alone: the Graphics page at its Presets
        preset_command(PRESET_KINDS.graphics, table.concat(words, ' ', 3))
    elseif (what == 'hudpreset' or what == 'hudpresets') and words[3] ~= nil then -- alone: the HUD page at its Presets
        preset_command(PRESET_KINDS.hud, table.concat(words, ' ', 3))
    elseif what == 'install' then
        -- /vana install [path]: an addon package's prompt (install.lua); alone, Windows' Open dialog asks for one
        M.install.command(e.command:match('^%S+%s+%S+%s+(.+)$'))
    elseif what == 'help' and words[3] ~= nil then
        -- /vana help <name>: the window at its page with its panel open (addons.lua)
        local row = M.help_command(words[3])
        if row ~= nil then
            if row.what == 'addon' then
                M.layout.page = 'list'
            end
            st.search[1] = ''
            open(page_named(row.what == 'addon' and 'addons' or 'plugins'))
            M.help_open(row)
        end
    elseif what == 'check' then
        -- every tab's rows built and every setting read, as the window does: what fails is listed
        local rows_n, settings_n, bad = 0, 0, {}
        for _, tab in ipairs(M.TABS) do
            local ok, rows = pcall(tab.rows)
            if not ok then
                bad[#bad + 1] = tab.name .. ': rows: ' .. tostring(rows)
            else
                for _, row in ipairs(rows) do
                    rows_n = rows_n + 1
                    if row.kind == 'toggle' or row.kind == 'choice' or row.kind == 'slider' or row.kind == 'colour' then
                        settings_n = settings_n + 1
                        local read_ok, value = pcall(M.read, row)
                        local fmt_ok, text = read_ok and pcall(M.format_value, row, value) or false, nil
                        if not read_ok or not fmt_ok then
                            bad[#bad + 1] = tab.name .. ' > ' .. tostring(row.label) .. ': ' .. tostring(read_ok and text or value)
                        end
                    elseif row.kind == 'info' or row.kind == 'status' then
                        local ok2, v = pcall(row.value)
                        if not ok2 then
                            bad[#bad + 1] = tab.name .. ' > ' .. tostring(row.label) .. ': ' .. tostring(v)
                        end
                    end
                    if row.lock_if ~= nil and not pcall(row.lock_if) then
                        bad[#bad + 1] = tab.name .. ' > ' .. tostring(row.label) .. ': lock_if failed'
                    end
                end
            end
        end
        print(string.format('VanaCore: %d rows, %d settings read, %d problem%s', rows_n, settings_n, #bad, #bad == 1 and '' or 's'))
        for i = 1, math.min(#bad, 12) do
            print('  ' .. bad[i])
        end
    else
        local asked = what == 'help' or what == '?'
        local hud_presets = what == 'hudpreset' or what == 'hudpresets' -- alone: the HUD page at its Presets
        local dlss = what == 'dlss' or what == 'fsr' -- alone: the Graphics page at its DLSS / FSR
        local page = not asked and page_named(hud_presets and 'hud' or dlss and 'graphics' or what) or nil
        if not asked and (what or '') ~= '' and not (page ~= nil and (page.id == what or page.name:lower() == what)) then
            -- a sub-tab's name alone (/vana windows, /vana shadows, /vana game): its page at it
            st.search[1] = ''
            local found = false
            for _, candidate in ipairs(PAGES) do
                for _, sub in ipairs(candidate.tab ~= nil and subtabs_of(candidate) or {}) do
                    -- (a name of two words without the space: /vana drawdistance)
                    if not found and sub.run == nil and (sub.title:lower() == what or sub.title:lower():gsub('[%s/]+', '') == what) then
                        page, st.sub[candidate.id], found = candidate, sub.title, true
                    end
                end
            end
        end
        if page ~= nil and page.hidden then -- (the HUD page: the HUD Manager's windows now)
            if hud_presets then
                hud.edit_presets()
            else
                hud.open_manager()
            end
        elseif page ~= nil then
            st.search[1] = ''
            if what == 'about' or what == 'vanacore' then
                st.sub[page.id] = 'VanaCore' -- the Client page at its VanaCore (with About's banner and Info)
            elseif what == 'layers' or what == 'toggles' then
                st.sub[page.id] = 'Toggles' -- the Graphics page at its Toggles
            elseif what == 'gamepad' or what == 'gamepads' then
                st.sub[page.id] = 'Gamepads' -- the Client page at its Gamepads
            elseif what == 'commands' then
                st.sub[page.id] = 'Commands' -- the Client page at its Commands
            elseif what == 'boot' then
                st.sub[page.id] = 'Boot.txt' -- the Client page at its Boot.txt
            elseif dlss then
                st.sub[page.id] = 'DLSS / FSR'
            elseif what == 'presets' or what == 'preset' or hud_presets then
                st.sub[page.id] = 'Presets' -- the Graphics page at its Presets, or the HUD page at its
            elseif what == 'truefont' or what == 'font' or what == 'fonts' then
                st.sub[page.id] = 'True Font' -- Game Options at its True Font
            elseif what == 'inventory' or what == 'items' then
                st.sub[page.id] = 'Inventory' -- Game Options at its Inventory
            elseif what == 'equipment' or what == 'equip' then
                st.sub[page.id] = 'Equipment' -- Game Options at its Equipment
            elseif what == 'nameplates' or what == 'names' then
                st.sub[page.id] = 'Nameplates' -- Game Options at its Nameplates
            elseif what == 'zone' or what == 'zonename' then
                st.sub[page.id] = 'Zones' -- Mods at its Zones (Zone Lines, then Zone Name)
            elseif what == 'grass' then
                st.sub[page.id] = 'Mass Grass' -- Mods at its Mass Grass
            elseif what == 'snow' then
                st.sub[page.id] = 'Snow Trails' -- Mods at its Snow Trails
            elseif what == 'loadscreen' then
                st.sub[page.id] = 'Loading Screen' -- Mods at its Loading Screen
            end
            open(page)
        else
            print_commands(command, not asked and words[2] or nil)
        end
    end
    return true
end)

vana.events.on('unload', function ()
    if M.hudman.open then
        M.hud_close()
    end
    close()
    M.release_logo()
end)
