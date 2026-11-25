--[[
vana\vana2.lua: /vana2, a look at a redesigned /vana window. A visual prototype: the pages' rows are drawn with made-up
values kept in this file (nothing here reads or writes a setting), so it can be looked at and clicked through, no more.
Loaded by vana.lua with the shared table M (a failure to load it never takes /vana with it); drawn each frame it's open,
with the /vana window open or not.

  Top bar      the categories across the top (Client, Graphics, HUD, Game Options), the Tools as three icons (they open
               /vana stats, /vana graph and /vana debug), the preset, and the close
  Left         the search box and the pages of the open category
  Page         a title, then panels: a heading with its Reset (and a switch), rows of switches, sliders, segments,
               drop-downs and colours. Only DLSS / FSR and Nameplates are built; the others say so
  Search       typed in the box, it finds options by name on the built pages and shows them, working, in place of the page

/vana2 opens it and closes it (/vana2 open, /vana2 close).
--]]

local M = ...
local imgui = vana.imgui
local S, P, fonts = M.S, M.P, M.fonts
local BODY = M.BODY
local st = M.st
local COL, VAR = M.COL, M.VAR

local v2 = { open = false, top = 'Graphics', page = 'DLSS / FSR', search = { '' }, v = {} }
M.vana2 = v2

-- the sizes (before the window's scale): everything the density hangs on is here
local WIN_W, WIN_H, MARGIN = 800, 560, 8 -- the window's size until it's dragged at its resize corner (v2.size)
local MIN_W, MIN_H, GRIP = 700, 380, 16   -- no smaller than that; the resize corner's size
local TOP_H, SIDE_W, PAD = 44, 176, 16
local BLUR = 1.0 -- how blurred the glass is behind the panel, 0 to 1 (vana.ui.backdrop's strength: 0.5 is the usual); [vana] vana2.blur.strength=<n> overrides
local HEAD_H = 36       -- a panel's heading
local ROW_H = 38        -- a row; ROW_MARKS_H one with marks under its slider; GRID_H a cell of a two-column panel
local ROW_MARKS_H, GRID_H = 52, 34
local CARD_PAD, GAP = 16, 10 -- inside a panel's edges; between panels
local LBL = 14.5        -- a row's label

-- the size the window was dragged to, kept between games (config\addons\vana\vana2.size: width and height at 100%)
local SIZE_FILE = vana.paths.install .. '\\config\\addons\\vana\\vana2.size'
pcall(function ()
    local f = io.open(SIZE_FILE, 'r')
    if f ~= nil then
        local w, h = (f:read('*a') or ''):match('(%d+)%s+(%d+)')
        f:close()
        if w ~= nil then
            v2.size = { tonumber(w), tonumber(h) }
        end
    end
end)

local function save_size()
    pcall(function ()
        local f = io.open(SIZE_FILE, 'w')
        if f ~= nil then
            f:write(string.format('%d %d\n', v2.size[1], v2.size[2]))
            f:close()
        end
    end)
end

-- the crystal glass look: the game shows through the panel, a cool light on what's active
local G = {
    bg = '#0C1322', bg_a = 0.90, bg_glass = 0.52, side = '#060A14', -- (bg_glass: the fill over a blurred backdrop)
    line = '#96C3FF', -- the hairlines and edges, at a low alpha
    crystal = '#62D8EA', deep = '#1D7F93', lit = '#8EE9F6',
    ink = '#E9EFFF', dim = '#8E9BB8', faint = '#5D6A86',
    amber = '#F2D27A',
}

-------------------------------------------------------------------------------
-- What the pages hold (made-up values; the numbers in each row are only drawn)
-------------------------------------------------------------------------------

local FMT = {
    pct = function (v) return string.format('%d%%', math.floor(v + 0.5)) end,
    px = function (v) return string.format('%d px', math.floor(v + 0.5)) end,
    xx = function (v) return string.format('%.2fx', v) end,
    sgn = function (v) v = math.floor(v + 0.5); return (v > 0 and '+' or '') .. string.format('%d', v) end,
    n2 = function (v) return string.format('%.2f', v) end,
    tenths = function (v) local n = math.floor(v * 10 + 0.5); return n < 1 and 'Off' or tostring(n) end, -- 0.1 to 1 shown 1 to 10
}

local function tog(id, label, def, tip) return { id = id, label = label, type = 'toggle', def = def, tip = tip } end
local function rng(id, label, min, max, step, def, fmt, tip, marks)
    return { id = id, label = label, type = 'range', min = min, max = max, step = step, def = def, fmt = fmt, tip = tip, marks = marks }
end
local function col(id, label, def, tip) return { id = id, label = label, type = 'colour', def = def, tip = tip } end
local function sel(id, label, def, opts, tip) return { id = id, label = label, type = 'select', def = def, opts = opts, tip = tip } end
local function seg(id, label, def, names, tip)
    local opts = {}
    for i, name in ipairs(names) do
        opts[i] = { name, name }
    end
    return { id = id, label = label, type = 'seg', def = def, opts = opts, tip = tip }
end

local QUALITY_MARKS = { 'Performance', 'Balanced', 'Quality', 'Native' }
local FRAMEGEN = { { 'off', 'Off' }, { '2x', '2x (RTX 40)' }, { '3x', '3x (RTX 50)' }, { '4x', '4x (RTX 50)' }, { 'fsr', 'AMD FSR 2x (any card)' } }
local PALETTE = { '#FFFFFF', '#C2FFC2', '#FFFFC2', '#FF82FF', '#FF8282', '#C2FFFF', '#FFC27A', '#9AB7FF', '#E0B3FF', '#7DE3C5' }

-- DLSS and FSR are one page, one or the other (the panels' switches: set_switch), DLSS 5 on DLSS
local PAGES = {
    ['DLSS / FSR'] = { sub = 'Upscaling and frame generation', secs = {
        { title = 'DLSS', tag = 'NVIDIA', switch = 'dlss_on', on = true, items = {
            rng('quality', 'Quality', 0, 100, 5, 60, FMT.pct,
                'How far down the game draws before DLSS brings it back up to full size. Lower is faster, higher is sharper.', QUALITY_MARKS),
            sel('framegen', 'Frame generation', '2x', FRAMEGEN,
                'Makes extra frames between the real ones for smoother motion. NVIDIA\'s needs an RTX 40 or 50 card.') } },
        { title = 'DLSS 5', tag = 'Neural rendering', switch = 'dlss5_on', on = false, items = {
            seg('style', 'DLSS 5 Style', 'Cinematic', { 'Natural', 'Cinematic', 'Default' },
                'The look DLSS 5 gives the picture.'),
            seg('res', 'DLSS 5 Resolution', 'Max', { 'Low', 'Medium', 'High', 'Max' },
                'The size DLSS 5 works at. Lower costs less GPU time.'),
            seg('od', 'DLSS 5 Overdrive', '1x', { '1x', '2x', '3x' },
                'Runs the picture through DLSS 5 again, each pass on the one before.') } },
        { title = 'FSR', tag = 'AMD', switch = 'fsr_on', on = false, items = {
            rng('fsr_quality', 'Quality', 0, 100, 5, 35, FMT.pct, 'How far down the game draws before FSR brings it back up to full size.', QUALITY_MARKS),
            rng('fsr_sharp', 'Sharpening', 0, 1, 0.1, 0, FMT.tenths, 'How much sharpening FSR adds after upscaling.'),
            sel('fsr_fg', 'Frame generation', '2x', { { 'off', 'Off' }, { '2x', '2x' } }, 'Makes an extra frame between the real ones. Any DirectX 12 card.') } },
    } },
    Nameplates = { sub = 'The names over characters\' heads', preview = true, secs = {
        { title = 'Text', items = {
            sel('np_family', 'Font', '', { { '', 'Default font' }, { 'Inter', 'Inter' }, { 'Cinzel', 'Cinzel' }, { 'JetBrains Mono', 'JetBrains Mono' } },
                'Which font True Font uses.'),
            rng('np_fontsize', 'Font size', 6, 40, 1, 18, FMT.px, 'How big names are up close, in pixels.'),
            tog('np_italic', 'Italic', false, 'Makes True Font italic.'), tog('np_bold', 'Bold', true, 'Makes True Font bold.'),
            tog('np_upper', 'Uppercase', false, 'Writes names in capital letters.') } },
        { title = 'Display', items = {
            rng('np_size', 'Size', 0.5, 2, 0.05, 1, FMT.xx, 'How big names over heads are.'),
            rng('np_distance', 'Distance scaling', 0, 100, 5, 100, FMT.pct, 'How much names shrink with distance. 0% keeps them all one size.'),
            tog('np_order', 'Near names on top', true, 'Draws nearer names over farther ones.'),
            tog('np_blink', 'Target name blinks', true, 'The name of what you target fades in and out, as the game has it.'),
            tog('np_framegen', 'Smooth with frame generation', true, 'With frame generation on, draws names in the frames it makes where they are at that moment.') } },
        { title = 'Shown', grid = true, items = {
            tog('np_hideown', 'Hide own nameplate', false, 'Hides the name over your own head.'),
            tog('np_hideplayers', 'Hide other players\' names', false, 'Hides the names over other players.'),
            tog('np_hidenpcs', 'Hide NPCs\' names', false, 'Hides the names over NPCs.'),
            tog('np_hidemonsters', 'Hide monsters\' names', false, 'Hides the names over monsters.'),
            tog('np_hidetrusts', 'Hide trusts\' names', false, 'Hides the names over trusts.'),
            tog('np_hidepets', 'Hide pets\' names', false, 'Hides the names over players\' pets.'),
            tog('np_bazaar', 'Bazaar bag', true, 'Shows the bag icon by players with a bazaar.'),
            tog('np_linkshell', 'Linkshell pearl', true, 'Shows the linkshell pearl by names.'),
            tog('np_mastery', 'Mastery stars', true, 'Shows the mastery stars by names.'),
            tog('np_gm', 'Staff GM icons', true, 'Server staff icons in place of the game\'s GM icon.'),
            sel('np_starstyle', 'Star style', 'game', { { 'game', 'Game\'s' }, { 'one', 'One star' }, { 'small', 'Small stars' } }, 'The mastery stars as the game draws them.') } },
        { title = 'Positioning', items = {
            tog('np_center', 'Centre name', true, 'Centres the name\'s letters over the head.'),
            rng('np_platex', 'Plate X', -100, 100, 1, 0, FMT.sgn, 'Moves the whole nameplate right or left.'),
            rng('np_platey', 'Plate Y', -100, 100, 1, 0, FMT.sgn, 'Moves the whole nameplate down or up.'),
            rng('np_namex', 'Name X', -100, 100, 1, 0, FMT.sgn, 'Moves the name\'s letters right or left.'),
            rng('np_namey', 'Name Y', -100, 100, 1, 0, FMT.sgn, 'Moves the name\'s letters down or up.'),
            rng('np_pearlx', 'Pearl X', -100, 100, 1, 38, FMT.sgn, 'Moves the linkshell pearl right or left.'),
            rng('np_pearly', 'Pearl Y', -100, 100, 1, -1, FMT.sgn, 'Moves the linkshell pearl down or up.'),
            rng('np_pearlsize', 'Pearl size', 0.1, 2, 0.05, 0.45, FMT.xx, 'How big the linkshell pearl is.'),
            rng('np_starsize', 'Star size', 0.25, 2, 0.05, 0.65, FMT.xx, 'How big the mastery stars are.'),
            rng('np_starspace', 'Star spacing', 0, 150, 5, 40, FMT.pct, 'How far apart the mastery stars are.') } },
        { title = 'Colours', grid = true, items = {
            col('np_c_players', 'Players', '#FFFFFF', 'Other players\' name colour.'), col('np_c_npcs', 'NPCs', '#C2FFC2', 'NPC name colour.'),
            col('np_c_monsters', 'Monsters (unclaimed)', '#FFFFC2', 'Name colour of monsters nobody has claimed.'),
            col('np_c_claimed', 'Monsters (claimed)', '#FF82FF', 'Name colour of monsters someone else has claimed.'),
            col('np_c_mine', 'Monsters (claimed by you)', '#FF8282', 'Name colour of monsters you have claimed.'),
            col('np_c_trusts', 'Trusts', '#C2FFC2', 'Trust name colour.'), col('np_c_pets', 'Pets', '#C2FFFF', 'Pet name colour.') } },
    } },
}
local PAGE_ORDER = { 'DLSS / FSR', 'Nameplates' }

local TOPS = {
    { 'Client', 'chip' }, { 'Graphics', 'display' }, { 'HUD', 'hud' }, { 'Game Options', 'sliders' },
}
local NAV = {
    Client = { 'VanaCore', 'Game', 'Characters', 'Gamepads', 'Commands', 'Boot.txt', 'Developer' },
    Graphics = { 'Presets', 'Toggles', 'DLSS / FSR', 'Draw distance', 'Shadows', 'Lighting', 'Ambient occlusion', 'Reflections', 'Bounce light', 'Height fog' },
    HUD = { 'Settings', 'Action Menu', 'Chat', 'Combat', 'Maps', 'Party', 'Windows', 'Misc' },
    ['Game Options'] = { 'Windows', 'Buttons', 'Chat', 'Menus', 'Party / Alliance', 'Macro Bar', 'Maps', 'Inventory', 'Equipment', 'Item Tooltips', 'Nameplates', 'Target', 'Status Window', 'Network' },
}
local TOPOF = {}
for _, top in ipairs(TOPS) do
    for _, name in ipairs(NAV[top[1]]) do
        if TOPOF[name] == nil then
            TOPOF[name] = top[1]
        end
    end
end
local DEFAULT_PAGE = { Graphics = 'DLSS / FSR', ['Game Options'] = 'Nameplates' }

-- the plates the Nameplates page previews: a name, its colour key, its Hide switch, whether it wears the pearl
local PLATES = {
    { 'Nog', 'players', 'np_hideplayers', true }, { 'Porter Moogle', 'npcs', 'np_hidenpcs', false },
    { 'Rock Spriggan', 'monsters', 'np_hidemonsters', false }, { 'Ayame', 'trusts', 'np_hidetrusts', false },
    { 'Carbuncle', 'pets', 'np_hidepets', false },
}

local function reset_values()
    for _, page in pairs(PAGES) do
        for _, sec in ipairs(page.secs) do
            if sec.switch then
                v2.v[sec.switch] = sec.on
            end
            for _, item in ipairs(sec.items) do
                v2.v[item.id] = item.def
            end
            if sec.extra then
                for _, item in ipairs(sec.extra.items) do
                    v2.v[item.id] = item.def
                end
            end
        end
    end
end
reset_values()

-------------------------------------------------------------------------------
-- Small drawing helpers
-------------------------------------------------------------------------------

-- a heading's face: a serif in capitals when Windows has one, the window's semibold if not
local function display_font()
    return M.face('constanb.ttf') or fonts.semi
end

local function edge(dl, x, y, w, h, radius, alpha, hex)
    M.outline(dl, x, y, w, h, hex or G.line, radius, 0, 1, alpha)
end

-- a chevron pointing down, drawn with lines
local function chevron_down(dl, cx, cy, hex, alpha)
    local c = M.u32(hex, alpha)
    local hw, hh = S(4.5), S(2.5)
    dl:AddLine({ cx - hw, cy - hh }, { cx, cy + hh }, c, S(1.5))
    dl:AddLine({ cx, cy + hh }, { cx + hw, cy - hh }, c, S(1.5))
end

-- a switch right-aligned to `right`... drawn from its left edge `x`; returns the value and whether it changed
local function switch(id, x, cy, on, big, a, en)
    local w, h = S(big and 38 or 34), S(big and 21 or 18)
    local y = cy - h / 2
    local dl = imgui.GetWindowDrawList()
    local hov, clicked = M.hit(id, x, y, w, h)
    local changed = false
    if clicked and en then
        on = not on
        changed = true
    end
    if on then
        M.fill(dl, x - S(2), y - S(2), w + S(4), h + S(4), G.crystal, h / 2 + S(2), 0, 0.14 * a)
        if a >= 0.99 then
            M.hgradient(dl, x, y, w, h, G.deep, G.crystal, h / 2)
        else
            M.fill(dl, x, y, w, h, M.mix(G.deep, G.crystal, 0.5), h / 2, 0, a)
        end
    else
        M.fill(dl, x, y, w, h, '#FFFFFF', h / 2, 0, 0.08 * a)
        edge(dl, x, y, w, h, h / 2, 0.16 * a)
    end
    if hov and en then
        M.fill(dl, x, y, w, h, '#FFFFFF', h / 2, 0, 0.07)
    end
    local r = (h - S(6)) / 2
    local kx = on and (x + w - S(3) - r) or (x + S(3) + r)
    M.circle(dl, kx, cy, r, on and '#FFFFFF' or '#CFD8EE', a)
    return on, changed
end

-- a button with a border: `label` and an icon before it; returns clicked
local function ghost(id, right, cy, label, icon)
    local tw = M.measure(label, fonts.text, 11)
    local w, h = tw + S(20) + (icon and S(13) or 0), S(22)
    local x, y = right - w, cy - S(11)
    local dl = imgui.GetWindowDrawList()
    local hov, clicked = M.hit(id, x, y, w, h)
    M.fill(dl, x, y, w, h, '#FFFFFF', S(7), 0, hov and 0.09 or 0.04)
    edge(dl, x, y, w, h, S(7), hov and 0.42 or 0.16)
    local at = x + S(10)
    if icon then
        M.icon_centred(dl, at + S(5), cy, icon, hov and G.ink or G.dim, 8.5)
        at = at + S(13)
    end
    M.text_mid(dl, at, cy, label, hov and G.ink or G.dim, fonts.text, 11)
    return clicked
end

-------------------------------------------------------------------------------
-- Controls, each drawn with its right edge on `right`, centred on `cy`; `a` fades it, `en` lets it take a click
-------------------------------------------------------------------------------

local function ctl_toggle(item, right, cy, a, en)
    local on, changed = switch('##v2.' .. item.id, right - S(34), cy, v2.v[item.id], false, a, en)
    if changed then
        v2.v[item.id] = on
    end
end

local function ctl_range(item, right, cy, avail, a, en)
    local dl = imgui.GetWindowDrawList()
    local id = '##v2.' .. item.id
    local total = math.max(S(200), math.min(S(400), avail))
    local value_w = S(70)
    local track_w = total - value_w - S(14)
    local x = right - total
    local _, _, active = M.hit(id, x - S(7), cy - S(11), track_w + S(14), S(22))
    local value = v2.v[item.id]
    if active and en then
        local mx = imgui.GetMousePos()
        local f = math.max(0, math.min(1, (mx - x) / track_w))
        local nv = item.min + (item.max - item.min) * f
        nv = math.floor((nv - item.min) / item.step + 0.5) * item.step + item.min
        value = math.max(item.min, math.min(item.max, nv))
        v2.v[item.id] = value
    end
    local f = math.max(0, math.min(1, (value - item.min) / (item.max - item.min)))
    local th = S(4)
    M.fill(dl, x, cy - th / 2, track_w, th, '#FFFFFF', th / 2, 0, 0.10 * a)
    local fw = math.max(th, track_w * f)
    M.fill(dl, x - S(2), cy - th / 2 - S(2), fw + S(4), th + S(4), G.crystal, th, 0, 0.13 * a)
    if a >= 0.99 then
        M.hgradient(dl, x, cy - th / 2, fw, th, G.deep, G.crystal, th / 2)
    else
        M.fill(dl, x, cy - th / 2, fw, th, M.mix(G.deep, G.crystal, 0.5), th / 2, 0, a)
    end
    local kx = x + track_w * f
    M.circle(dl, kx, cy, S(10.5), G.crystal, 0.22 * a)
    M.circle(dl, kx, cy, S(7), '#FFFFFF', a)
    if item.marks ~= nil then
        local widths, sum = {}, 0
        for i, mark in ipairs(item.marks) do
            widths[i] = M.measure(mark:upper(), fonts.text, 9.5)
            sum = sum + widths[i]
        end
        local gap = math.max(S(4), (track_w - sum) / math.max(1, #item.marks - 1))
        local mx = x
        for i, mark in ipairs(item.marks) do
            M.text(dl, mx, cy + S(8), mark:upper(), G.faint, fonts.text, 9.5, a)
            mx = mx + widths[i] + gap
        end
    end
    M.text_right_mid(dl, right, cy, (item.fmt or FMT.n2)(value), G.crystal, fonts.semi, 15, a)
end

local function ctl_seg(item, right, cy, a, en)
    local dl = imgui.GetWindowDrawList()
    local size, h = 13, S(28)
    local widths, total = {}, S(4)
    for i, opt in ipairs(item.opts) do
        widths[i] = M.measure(opt[2], fonts.text, size) + S(26)
        total = total + widths[i]
    end
    local x, y = right - total, cy - h / 2
    M.fill(dl, x, y, total, h, '#000000', S(8), 0, 0.25 * a)
    edge(dl, x, y, total, h, S(8), 0.16 * a)
    local at = x + S(2)
    for i, opt in ipairs(item.opts) do
        local hov, clicked = M.hit('##v2.' .. item.id .. '.' .. i, at, y + S(2), widths[i], h - S(4))
        local on = v2.v[item.id] == opt[1]
        if on then
            M.fill(dl, at - S(1), y + S(1), widths[i] + S(2), h - S(2), G.crystal, S(7), 0, 0.16 * a)
            M.fill(dl, at, y + S(2), widths[i], h - S(4), G.lit, S(6), 0, a)
        elseif hov and en then
            M.fill(dl, at, y + S(2), widths[i], h - S(4), '#FFFFFF', S(6), 0, 0.06)
        end
        M.text_centred(dl, at, y + S(2), widths[i], h - S(4), opt[2], on and '#04131A' or (hov and G.ink or G.dim),
                       on and fonts.semi or fonts.text, size, a)
        if clicked and en then
            v2.v[item.id] = opt[1]
        end
        at = at + widths[i]
    end
end

-- the room a control takes (its right edge on the row's, `avail` what the row has for a wide one), so the label gives way
local function ctl_width(item, avail)
    if item.type == 'toggle' then
        return S(34)
    elseif item.type == 'range' then
        return math.max(S(200), math.min(S(400), avail))
    elseif item.type == 'seg' then
        local total = S(4)
        for _, opt in ipairs(item.opts) do
            total = total + M.measure(opt[2], fonts.text, 13) + S(26)
        end
        return total
    elseif item.type == 'select' then
        return math.min(S(210), math.max(S(110), avail))
    elseif item.type == 'colour' then
        return S(3) + S(26) + S(8) + M.measure(v2.v[item.id], fonts.text, 11.5) + S(10)
    end
    return 0
end

-- a drop-down (a click goes to the next choice: the list itself isn't built)
local function ctl_select(item, right, cy, avail, a, en)
    local dl = imgui.GetWindowDrawList()
    local w, h = ctl_width(item, avail), S(28)
    local x, y = right - w, cy - h / 2
    local hov, clicked = M.hit('##v2.' .. item.id, x, y, w, h)
    local at, shown = 1, item.opts[1]
    for i, opt in ipairs(item.opts) do
        if opt[1] == v2.v[item.id] then
            at, shown = i, opt
        end
    end
    M.fill(dl, x, y, w, h, '#FFFFFF', S(7), 0, hov and 0.08 or 0.05)
    edge(dl, x, y, w, h, S(7), hov and 0.4 or 0.16)
    M.text_mid(dl, x + S(11), cy, M.fit(shown[2], w - S(36), fonts.text, 13.5), G.ink, fonts.text, 13.5, a)
    chevron_down(dl, x + w - S(16), cy, G.dim, a)
    if clicked and en then
        v2.v[item.id] = item.opts[at % #item.opts + 1][1]
    end
end

-- a colour (a click goes to the next of ten: the picker isn't built)
local function ctl_colour(item, right, cy, a, en)
    local dl = imgui.GetWindowDrawList()
    local hex = v2.v[item.id]
    local tw = M.measure(hex, fonts.text, 11.5)
    local w, h = S(3) + S(26) + S(8) + tw + S(10), S(24)
    local x, y = right - w, cy - h / 2
    local hov, clicked = M.hit('##v2.' .. item.id, x, y, w, h)
    M.fill(dl, x, y, w, h, '#FFFFFF', S(7), 0, hov and 0.08 or 0.04)
    edge(dl, x, y, w, h, S(7), hov and 0.4 or 0.16)
    M.fill(dl, x + S(3), y + S(3), S(26), h - S(6), hex, S(4), 0, a)
    edge(dl, x + S(3), y + S(3), S(26), h - S(6), S(4), 0.25 * a, '#FFFFFF')
    M.text_mid(dl, x + S(37), cy, hex, G.dim, fonts.text, 11.5, a)
    if clicked and en then
        local next_at = 1
        for i, c in ipairs(PALETTE) do
            if c == hex then
                next_at = i % #PALETTE + 1
            end
        end
        v2.v[item.id] = PALETTE[next_at]
    end
end

local function control(item, right, cy, avail, a, en)
    if item.type == 'toggle' then
        ctl_toggle(item, right, cy, a, en)
    elseif item.type == 'range' then
        ctl_range(item, right, cy, avail, a, en)
    elseif item.type == 'seg' then
        ctl_seg(item, right, cy, a, en)
    elseif item.type == 'select' then
        ctl_select(item, right, cy, avail, a, en)
    elseif item.type == 'colour' then
        ctl_colour(item, right, cy, a, en)
    end
end

-------------------------------------------------------------------------------
-- Panels
-------------------------------------------------------------------------------

local function rows_height(items, grid)
    if grid then
        return S(8) + math.ceil(#items / 2) * S(GRID_H)
    end
    local h = 0
    for _, item in ipairs(items) do
        h = h + (item.marks and S(ROW_MARKS_H) or S(ROW_H))
    end
    return h
end

local function card_height(sec, items, searching)
    local h = S(HEAD_H) + rows_height(items, sec.grid) + S(6)
    if not searching then
        if sec.status then
            h = h + S(8) + S(14) + #sec.status * S(18) + S(4)
        end
        if sec.extra then
            h = h + S(34) + rows_height(sec.extra.items, false)
        end
    end
    return h
end

-- the rows of a panel from y; `hl`: the search's words, lit in each label. Returns the height
local function draw_rows(items, grid, x, y, w, a, en, hl)
    local dl = imgui.GetWindowDrawList()
    local pad = S(CARD_PAD)
    local lx, rx = x + pad, x + w - pad
    local function label(item, at, cy, room)
        local shown = M.fit(item.label, room, fonts.text, LBL)
        if hl ~= nil and shown == item.label then
            local i = item.label:lower():find(hl, 1, true)
            if i ~= nil then
                local before = i > 1 and M.measure(item.label:sub(1, i - 1), fonts.text, LBL) or 0
                local mw = M.measure(item.label:sub(i, i + #hl - 1), fonts.text, LBL)
                M.fill(dl, at + before - S(2), cy - S(9), mw + S(4), S(18), G.crystal, S(4), 0, 0.22)
            end
        end
        M.text_mid(dl, at, cy, shown, G.ink, fonts.text, LBL, a)
    end
    if grid then
        local gap = S(28)
        local cw = (rx - lx - gap) / 2
        local rows = math.ceil(#items / 2)
        for i, item in ipairs(items) do
            local c, r = (i - 1) % 2, math.floor((i - 1) / 2)
            local cx = lx + c * (cw + gap)
            local top = y + S(4) + r * S(GRID_H)
            local cy = top + S(GRID_H) / 2
            local avail = cw * 0.55
            label(item, cx, cy, cw - ctl_width(item, avail) - S(12))
            control(item, cx + cw, cy, avail, a, en)
            if r < rows - 1 then
                M.hline(dl, cx, cx + cw, top + S(GRID_H), G.line, 0.07)
            end
            if item.tip ~= nil and M.over(cx, top, cw, S(GRID_H)) then
                M.tooltip(item.tip)
            end
        end
        return S(8) + rows * S(GRID_H)
    end
    local at = y
    for i, item in ipairs(items) do
        local rh = item.marks and S(ROW_MARKS_H) or S(ROW_H)
        local cy = at + rh / 2 - (item.marks and S(5) or 0)
        local avail = rx - lx - S(180)
        label(item, lx, cy, rx - lx - ctl_width(item, avail) - S(12))
        control(item, rx, cy, avail, a, en)
        if i < #items then
            M.hline(dl, lx, rx, at + rh, G.line, 0.07)
        end
        if item.tip ~= nil and M.over(lx - S(6), at, rx - lx + S(12), rh) then
            M.tooltip(item.tip)
        end
        at = at + rh
    end
    return at - y
end

-- one panel: its heading (title, what it is, a switch and its Reset), then its rows. `from`: the page it's on when it's a
-- search result (its heading says where, and Open page takes the place of Reset)
local function draw_card(sec, items, x, y, w, from, hl)
    local dl = imgui.GetWindowDrawList()
    local pad = S(CARD_PAD)
    local h = card_height(sec, items, from ~= nil)
    local off = from == nil and sec.switch ~= nil and not v2.v[sec.switch]
    local a, en = off and 0.38 or 1, not off
    -- DLSS 5 runs on DLSS: its switch is locked (dimmed) while that is off, unless it's on and needs turning off
    local locked = sec.switch == 'dlss5_on' and not v2.v.dlss_on and not v2.v.dlss5_on
    M.fill(dl, x, y, w, h, '#FFFFFF', S(6), 0, 0.04)
    edge(dl, x, y, w, h, S(6), 0.16)

    local cy = y + S(HEAD_H) / 2
    local tw = M.text_spaced(dl, x + pad, cy, sec.title:upper(), G.ink, display_font(), 12.5, 1, S(2.4))
    local tag = from ~= nil and (TOPOF[from] .. ' › ' .. from) or sec.tag
    if tag ~= nil then
        M.text_mid(dl, x + pad + tw + S(12), cy, tag, G.faint, fonts.text, 11)
    end
    local rx = x + w - pad
    if from == nil and sec.switch ~= nil then
        local on, changed = switch('##v2.sw.' .. sec.switch, rx - S(38), cy, v2.v[sec.switch], true, locked and 0.4 or 1, not locked)
        if changed then
            v2.v[sec.switch] = on
            -- as the real page: DLSS and FSR are one or the other, and switching either turns DLSS 5 off
            if sec.switch == 'dlss_on' or sec.switch == 'fsr_on' then
                v2.v.dlss5_on = false
                if on then
                    v2.v[sec.switch == 'dlss_on' and 'fsr_on' or 'dlss_on'] = false
                end
            end
        end
        rx = rx - S(38) - S(10)
    end
    if from ~= nil then
        if ghost('##v2.open.' .. from .. sec.title, rx, cy, 'Open page ›', nil) then
            v2.top, v2.page, v2.search[1] = TOPOF[from], from, ''
        end
    elseif ghost('##v2.reset.' .. sec.title, rx, cy, 'Reset', 'reset') then
        for _, item in ipairs(items) do
            v2.v[item.id] = item.def
        end
        if sec.extra then
            for _, item in ipairs(sec.extra.items) do
                v2.v[item.id] = item.def
            end
        end
    end
    M.hline(dl, x + pad, x + w - pad, y + S(HEAD_H), G.line, 0.16)

    local at = y + S(HEAD_H)
    at = at + draw_rows(items, sec.grid, x, at, w, a, en, hl)
    if from == nil and sec.status ~= nil then
        local bx, bw = x + pad, w - pad * 2
        local bh = S(14) + #sec.status * S(18)
        at = at + S(8)
        M.fill(dl, bx, at, bw, bh, '#000000', S(6), 0, 0.28)
        edge(dl, bx, at, bw, bh, S(6), 0.16)
        for i, line in ipairs(sec.status) do
            local ly = at + S(7) + (i - 0.5) * S(18)
            M.text_mid(dl, bx + S(12), ly, line[1], G.faint, fonts.text, 12)
            M.text_right_mid(dl, bx + bw - S(12), ly, line[2](), G.dim, fonts.text, 12)
        end
        at = at + bh + S(4)
    end
    if from == nil and sec.extra ~= nil then
        local sy = at + S(17)
        local sw = M.text_spaced(dl, x + pad, sy, sec.extra.title:upper(), G.crystal, fonts.semi_italic or fonts.semi, 11, 1, S(2))
        M.fill(dl, x + pad + sw + S(12), sy, w - pad * 2 - sw - S(12), 1, G.crystal, 0, 0, 0.3)
        at = at + S(34)
        draw_rows(sec.extra.items, false, x, at, w, a, en, nil)
    end
    return h
end

-- the nameplates over made-up heads, as the Nameplates page's settings would draw them
local function draw_preview(x, y, w)
    local dl = imgui.GetWindowDrawList()
    local h = S(104)
    local v = v2.v
    M.fill(dl, x, y, w, h, '#141E37', S(6), 0, 0.9)
    edge(dl, x, y, w, h, S(6), 0.16)
    M.text_spaced(dl, x + S(14), y + S(14), 'LIVE PREVIEW', G.faint, fonts.semi, 10, 1, S(1.6))
    local shown = {}
    for _, plate in ipairs(PLATES) do
        if not v[plate[3]] then
            shown[#shown + 1] = plate
        end
    end
    if #shown == 0 then
        M.text_centred(dl, x, y, w, h, 'All names hidden', G.faint, fonts.text, 12)
        return h
    end
    local size = math.max(8, math.min(30, v.np_fontsize * v.np_size * 0.78))
    local font = (v.np_bold and (fonts.bold or fonts.semi)) or (v.np_italic and fonts.italic) or fonts.text
    if v.np_bold and v.np_italic then
        font = fonts.bold_italic or font
    elseif v.np_italic then
        font = fonts.italic or font
    end
    local cell = (w - S(40)) / #shown
    local ny = y + S(54)
    for i, plate in ipairs(shown) do
        local cx = x + S(20) + cell * (i - 0.5)
        local name = v.np_upper and plate[1]:upper() or plate[1]
        local nw = M.measure(name, font, size)
        if v.np_mastery then
            local gap = S(9 * v.np_starsize + 3)
            local n = v.np_starstyle == 'one' and 1 or 3
            for s = 1, n do
                M.icon_centred(dl, cx + (s - (n + 1) / 2) * gap, ny - S(16), 'star', '#FFD86B', 7 * math.max(0.6, v.np_starsize + 0.3))
            end
        end
        if plate[4] and v.np_linkshell then
            local r = S(2 + 14 * v.np_pearlsize)
            M.circle(dl, cx - nw / 2 - S(8) - r, ny, r, '#B9A4FF')
            M.circle(dl, cx - nw / 2 - S(8) - r * 1.3, ny - r * 0.3, r * 0.4, '#FFFFFF', 0.7)
        end
        M.text_mid(dl, cx - nw / 2 + 1, ny + 1, name, '#000000', font, size, 0.85)
        M.text_mid(dl, cx - nw / 2, ny, name, v['np_c_' .. plate[2]], font, size)
        M.fill(dl, cx - S(4), ny + S(13), S(8), S(24), '#2A2640', S(3), 0, 0.9)
    end
    return h
end

-------------------------------------------------------------------------------
-- The page
-------------------------------------------------------------------------------

local function draw_title(x, y, title, sub)
    local dl = imgui.GetWindowDrawList()
    local cy = y + S(18)
    local tw = M.text_spaced(dl, x, cy, title:upper(), G.ink, display_font(), 22, 1, S(2.8))
    if sub ~= nil then
        M.text_mid(dl, x + tw + S(14), cy + S(4), sub, G.faint, fonts.text, 12)
    end
    return S(40)
end

local function draw_foot(x, y)
    local dl = imgui.GetWindowDrawList()
    M.circle(dl, x + S(3), y + S(8), S(3), G.faint)
    M.text_mid(dl, x + S(14), y + S(8), 'Advanced settings are hidden. Turn them on in Client › VanaCore › Shaders.', G.faint, fonts.text, 12)
    return S(20)
end

local function draw_search_page(x, y, w, q)
    local lower = q:lower()
    local groups, n = {}, 0
    for _, name in ipairs(PAGE_ORDER) do
        for _, sec in ipairs(PAGES[name].secs) do
            local parts = { { sec, sec.items } }
            if sec.extra then
                parts[2] = { sec.extra, sec.extra.items }
            end
            for _, part in ipairs(parts) do
                local found = {}
                for _, item in ipairs(part[2]) do
                    if item.label:lower():find(lower, 1, true) or part[1].title:lower():find(lower, 1, true) then
                        found[#found + 1] = item
                    end
                end
                if #found > 0 then
                    groups[#groups + 1] = { name, part[1], found }
                    n = n + #found
                end
            end
        end
    end
    local at = y
    for _, group in ipairs(groups) do
        if group[1] == 'Nameplates' then
            at = at + draw_preview(x, at, w) + S(GAP)
            break
        end
    end
    local sub = (n == 0 and 'Nothing found' or n .. (n == 1 and ' option' or ' options')) .. ' for “' .. q .. '”'
    at = at + draw_title(x, at, 'Search', sub)
    if n == 0 then
        local dl = imgui.GetWindowDrawList()
        M.fill(dl, x, at, w, S(54), '#FFFFFF', S(6), 0, 0.04)
        edge(dl, x, at, w, S(54), S(6), 0.16)
        M.text_centred(dl, x, at, w, S(54), 'No option matches. Try a shorter word.', G.dim, fonts.text, 14)
        at = at + S(64)
    end
    for _, group in ipairs(groups) do
        local sec = { title = group[2].title, grid = group[2].grid }
        at = at + draw_card(sec, group[3], x, at, w, group[1], lower) + S(GAP)
    end
    at = at + draw_foot(x, at)
    return at - y, 'Only the DLSS / FSR and Nameplates pages are built here, so only their options are searched.'
end

local function draw_page_body(x, y, w)
    local dl = imgui.GetWindowDrawList()
    local q = v2.search[1]:match('^%s*(.-)%s*$')
    if q ~= '' then
        local h, note = draw_search_page(x, y, w, q)
        local at = y + h
        M.text_mid(dl, x + S(14), at + S(2), note, G.faint, fonts.text, 12)
        return h + S(24)
    end
    local page = PAGES[v2.page]
    local at = y
    if page == nil then
        at = at + draw_title(x, at, v2.page, 'Not built in this prototype')
        M.fill(dl, x, at, w, S(70), '#FFFFFF', S(6), 0, 0.04)
        edge(dl, x, at, w, S(70), S(6), 0.16)
        M.text_centred(dl, x, at, w, S(70), v2.page .. ' would sit here.', G.dim, fonts.text, 14)
        return at + S(86) - y
    end
    if page.preview then
        at = at + draw_preview(x, at, w) + S(GAP)
    end
    at = at + draw_title(x, at, v2.page, page.sub)
    for _, sec in ipairs(page.secs) do
        at = at + draw_card(sec, sec.items, x, at, w, nil, nil) + S(GAP)
    end
    at = at + draw_foot(x, at)
    return at - y
end

-- `fn` in a child window (ImGui's End and EndChild always run, even when `fn` fails)
local function in_child(id, w, h, fn)
    local err
    if imgui.BeginChild(id, { w, h }, 0, M.NO_NAV) then
        local ok, e = pcall(fn)
        if not ok then
            err = e
        end
    end
    imgui.EndChild()
    if err ~= nil then
        error(err, 0)
    end
end

-------------------------------------------------------------------------------
-- The window
-------------------------------------------------------------------------------

local function draw_top(dl, x, y, pw, r)
    local th = S(TOP_H)
    M.fill(dl, x, y, pw, th, G.side, r, M.CORNERS.top, 0.55)
    M.hline(dl, x, x + pw, y + th, G.line, 0.16)
    local cy = y + th / 2
    local at = x + S(8)
    for _, top in ipairs(TOPS) do
        local label = top[1]:upper()
        local lw = M.text_spaced(nil, 0, 0, label, G.ink, fonts.semi, 11, 1, S(1.5))
        local tw = S(14) * 2 + S(14) + S(8) + lw
        local hov, clicked = M.hit('##v2.top.' .. top[1], at, y, tw, th)
        local on = v2.top == top[1]
        local c = on and G.crystal or (hov and G.ink or G.dim)
        M.icon_centred(dl, at + S(14) + S(6), cy, top[2], c, 11)
        M.text_spaced(dl, at + S(14) + S(14) + S(8), cy, label, c, fonts.semi, 11, 1, S(1.5))
        if on then
            M.fill(dl, at + S(12), y + th - S(6), tw - S(24), S(6), G.crystal, S(3), 0, 0.18) -- (inside the bar: ends where it does)
            M.fill(dl, at + S(12), y + th - S(2), tw - S(24), S(2), G.crystal, S(1))
        end
        if clicked and not on then
            v2.top, v2.page, v2.search[1] = top[1], DEFAULT_PAGE[top[1]] or NAV[top[1]][1], ''
        end
        at = at + tw + S(2)
    end

    -- the close, the preset, then the Tools as three icons beside it
    local rx = x + pw - S(18)
    if M.icon_button('##v2.close', rx, cy, 'close', 11, '#EE6F78', G.dim) then
        v2.open = false
    end
    rx = rx - S(22)
    local cw = M.measure('GOOD', fonts.semi, 11) + S(24)
    M.fill(dl, rx - cw, cy - S(11), cw, S(22), '#FFFFFF', S(11), 0, 0.05)
    edge(dl, rx - cw, cy - S(11), cw, S(22), S(11), 0.2)
    M.text_centred(dl, rx - cw, cy - S(11), cw, S(22), 'GOOD', G.ink, fonts.semi, 11)
    if M.over(rx - cw, cy - S(11), cw, S(22)) then
        M.tooltip('Preset')
    end
    rx = rx - cw - S(8)
    local tools = {
        { 'stats', 'Stats', '/vana stats' }, { 'chart', 'Graphs', '/vana graph' }, { 'bug', 'Debug', '/vana debug' },
    }
    for i = #tools, 1, -1 do
        local clicked, hovered = M.icon_button('##v2.tool.' .. i, rx - S(9), cy, tools[i][1], 11, G.crystal, G.dim)
        if hovered then
            M.tooltip(tools[i][2])
        end
        if clicked then
            vana.chat.run(tools[i][3])
        end
        rx = rx - S(28)
    end
end

-- a glow that fades out to the right, its left corners round: the open page's, as the first mock-up drew it. One gradient
-- rectangle for the middle and a pixel-tall strip each for the corners (a rounded cap beside the gradient showed a seam)
local function fade_fill(dl, x, y, w, h, hex, alpha, radius)
    local to = M.u32(hex, 0)
    local function strip(top, bottom, inset)
        local from = M.u32(hex, alpha * (1 - inset / w))
        dl:AddRectFilledMultiColor({ x + inset, top }, { x + w, bottom }, from, to, to, from)
    end
    local r, top, rows = math.max(1, math.floor(radius + 0.5)), math.floor(y + 0.5), math.floor(h + 0.5)
    for k = 0, r - 1 do
        local inset = r - math.sqrt(r * r - (r - k - 0.5) ^ 2)
        strip(top + k, top + k + 1, inset)
        strip(top + rows - k - 1, top + rows - k, inset)
    end
    strip(top + r, top + rows - r, 0)
end

local function draw_side(dl, x, y, w, h, r)
    M.fill(dl, x, y, w, h, G.side, r, 64, 0.5) -- (only its bottom left corner rounds)
    M.vline(dl, x + w, y, y + h, G.line, 0.16)

    -- the search box: what's typed finds options on the pages built here, shown in place of the page
    local sx, sy, sw, sh = x + S(10), y + S(10), w - S(20), S(30)
    M.fill(dl, sx, sy, sw, sh, '#FFFFFF', S(7), 0, 0.045)
    edge(dl, sx, sy, sw, sh, S(7), 0.16)
    M.icon_centred(dl, sx + S(14), sy + sh / 2, 'search', G.faint, 9)
    local searching = v2.search[1] ~= ''
    imgui.SetCursorScreenPos({ sx + S(24), sy + 1 })
    imgui.SetNextItemWidth(sw - S(26) - (searching and S(24) or S(4)))
    imgui.PushStyleColor(COL.FrameBg, M.rgba(P.well, 0))
    imgui.PushStyleColor(COL.FrameBgHovered, M.rgba(P.well, 0))
    imgui.PushStyleColor(COL.FrameBgActive, M.rgba(P.well, 0))
    imgui.PushStyleVar(VAR.FramePadding, { S(4), math.max(0, (sh - 2 - S(13.5)) / 2) })
    imgui.PushStyleVar(VAR.FrameRounding, S(5))
    imgui.PushStyleVar(VAR.FrameBorderSize, 0)
    M.push(fonts.text, 13.5)
    imgui.InputTextWithHint('##v2.search', 'Search settings', v2.search, 64)
    M.pop()
    imgui.PopStyleVar(3)
    imgui.PopStyleColor(3)
    if searching and M.icon_button('##v2.search.clear', sx + sw - S(13), sy + sh / 2, 'close', 11, '#FFA7AE', P.rose) then
        v2.search[1] = ''
    end

    -- the pages of the open category
    local ny = sy + sh + S(6)
    imgui.SetCursorScreenPos({ x, ny })
    in_child('##v2.nav', w, y + h - ny - S(8), function ()
        local cdl = imgui.GetWindowDrawList()
        local cx, cy0 = imgui.GetCursorScreenPos()
        local cw = (imgui.GetContentRegionAvail())
        local at = cy0 + S(2)
        for _, name in ipairs(NAV[v2.top]) do
            local rh = S(28)
            local hov, clicked = M.hit('##v2.nav.' .. name, cx + S(10), at, cw - S(20), rh)
            local on = not searching and name == v2.page
            if on then
                fade_fill(cdl, cx + S(10), at, cw - S(20), rh, G.crystal, 0.2, S(7))
            elseif hov then
                M.fill(cdl, cx + S(10), at, cw - S(20), rh, '#FFFFFF', S(7), 0, 0.05)
            end
            local tx = cx + S(22)
            if v2.top == 'HUD' and name == 'Settings' then
                M.circle(cdl, tx + S(3), at + rh / 2, S(3), G.amber)
                tx = tx + S(14)
            end
            M.text_mid(cdl, tx, at + rh / 2, name, on and G.crystal or (hov and G.ink or G.dim), fonts.text, 13.5)
            if clicked then
                v2.page, v2.search[1] = name, ''
            end
            at = at + rh + S(1)
        end
        imgui.SetCursorScreenPos({ cx, at })
        imgui.Dummy({ 1, S(6) })
    end)
end

-- The resize corner, bottom right: three short lines across it, lit under the mouse; dragged, the window's corner follows the
-- mouse (no smaller than MIN_W by MIN_H, no bigger than the screen), kept as it's let go.
local function resize_corner(dl, x, y, pw, ph, screen_w, screen_h)
    local g = S(GRIP)
    local hovered, _, active = M.hit('##v2.resize', x + pw - g, y + ph - g, g, g)
    if active then
        local mx, my = imgui.GetMousePos()
        v2.grab = v2.grab or { x + pw - mx, y + ph - my }
        local w = (mx + v2.grab[1] - x) / M.scale
        local h = (my + v2.grab[2] - y) / M.scale
        v2.size = { math.floor(math.max(MIN_W, math.min(w, (screen_w - S(MARGIN) * 2) / M.scale)) + 0.5),
                    math.floor(math.max(MIN_H, math.min(h, (screen_h - S(MARGIN) * 2) / M.scale)) + 0.5) }
    elseif v2.grab ~= nil then
        v2.grab = nil
        save_size()
    end
    if hovered or active then
        imgui.SetMouseCursor(6) -- ImGuiMouseCursor_ResizeNWSE
    end
    local colour = M.u32((hovered or active) and G.crystal or G.faint)
    local right, bottom = x + pw - S(6), y + ph - S(6)
    for _, k in ipairs({ 4, 8, 12 }) do
        dl:AddLine({ right - S(k), bottom }, { right, bottom - S(k) }, colour, math.max(1, S(1.25)))
    end
    if hovered and not active then
        M.tooltip('Drag to resize')
    end
end

local function draw_panel(x, y, pw, ph, screen_w, screen_h)
    local dl = imgui.GetWindowDrawList()
    local r = S(6)
    M.fill(dl, x - 1, y, pw + 2, ph + 2, '#000000', r + 1, 0, 0.35)
    -- frosted glass: the renderer blurs the game behind the panel (vana.ui.backdrop, DirectX 12) and the fill lets it show;
    -- without that (an older VanaCore.Core.dll, another renderer) the fill is nearly solid
    -- ([vana] vana2.blur=0 leaves the blur out, to compare what it costs: /vana abtest vana/vana2.blur=1,0)
    local frosted = vana.config.get('vana', 'vana2.blur') ~= '0' and vana.ui.backdrop ~= nil
        and vana.ui.backdrop(x, y, pw, ph, r, tonumber(vana.config.get('vana', 'vana2.blur.strength')) or BLUR)
    M.fill(dl, x, y, pw, ph, G.bg, r, 0, frosted and G.bg_glass or G.bg_a)
    edge(dl, x, y, pw, ph, r, 0.28)
    draw_top(dl, x, y, pw, r)

    local th, sw = S(TOP_H), S(SIDE_W)
    draw_side(dl, x, y + th + 1, sw, ph - th - 1, r)

    imgui.SetCursorScreenPos({ x + sw + 1, y + th + 1 })
    -- the page stops above the resize corner (a child over it would take its mouse)
    in_child('##v2.page.' .. (v2.search[1] ~= '' and 'search' or v2.page), pw - sw - 1, ph - th - 1 - S(GRIP), function ()
        local cx, cy0 = imgui.GetCursorScreenPos()
        local cw = (imgui.GetContentRegionAvail())
        local px = cx + S(PAD)
        local h = draw_page_body(px, cy0 + S(PAD - 2), cw - S(PAD) * 2)
        imgui.SetCursorScreenPos({ cx, cy0 + S(PAD - 2) + h })
        imgui.Dummy({ 1, S(PAD) })
    end)
    resize_corner(dl, x, y, pw, ph, screen_w, screen_h)
end

function M.draw_vana2(now)
    if not fonts.ready then
        fonts.ready = true
        pcall(M.load_fonts)
        return
    end
    local screen_w, screen_h = vana.ui.screen()
    screen_w, screen_h = (screen_w and screen_w > 0) and screen_w or 1920, (screen_h and screen_h > 0) and screen_h or 1080
    -- with the /vana window and the Debug window shut their frames don't run: the scale and the animations' clock kept here
    if not st.open and not M.debug.open and not (M.gptest ~= nil and M.gptest.open) then
        st.dt = math.max(0, math.min(0.1, now - st.last))
        st.last = now
        M.compute_scale(now, screen_h, M.user_scale() * 100)
    end

    local m = S(MARGIN)
    local size = v2.size or { WIN_W, WIN_H }
    local ww = math.min(S(math.max(MIN_W, size[1])) + m * 2, screen_w)
    local wh = math.min(S(math.max(MIN_H, size[2])) + m * 2, screen_h)
    local pw, ph = ww - m * 2, wh - m * 2
    -- placed in ImGui's pixels, where it's drawn ww * k by wh * k (M.display)
    local dw, dh, k = M.display(screen_w, screen_h)
    imgui.SetNextWindowPos({ (dw - ww * k) / 2, (dh - wh * k) / 2 }, M.FIRST_USE_EVER)
    imgui.SetNextWindowSize({ ww, wh }, 0)

    local colours = {
        { COL.Text, P.text }, { COL.TextDisabled, G.faint }, { COL.WindowBg, G.bg }, { COL.ChildBg, G.bg },
        { COL.PopupBg, '#0F1118' }, { COL.Border, P.border }, { COL.FrameBg, P.well }, { COL.ScrollbarBg, G.bg },
        { COL.ScrollbarGrab, '#FFFFFF' }, { COL.ScrollbarGrabHovered, '#FFFFFF' }, { COL.ScrollbarGrabActive, '#D6DEF0' }, -- (white)
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

    local err
    if imgui.Begin('VanaCore 2###vana_v2', nil, M.WINDOW_FLAGS) then
        local wx, wy = imgui.GetWindowPos()
        local ok, e = pcall(draw_panel, wx + m, wy + m, pw, ph, screen_w, screen_h)
        if not ok then
            err = e
        end
    end
    imgui.End()
    imgui.PopStyleVar(12)
    imgui.PopStyleColor(#colours)
    if err ~= nil then
        error(err, 0)
    end
end

-------------------------------------------------------------------------------
-- /vana2
-------------------------------------------------------------------------------

local failed_at = -10

vana.events.on('present', function ()
    if not v2.open then
        return
    end
    local now = vana.time.clock()
    local ok, err = pcall(M.draw_vana2, now)
    if not ok and now - failed_at > 2 then
        failed_at = now
        print('VanaCore: vana2: ' .. tostring(err))
    end
end, 'vana2')

vana.events.on('command', function (e)
    local word, rest = e.command:match('^%s*(%S+)%s*(.-)%s*$')
    if word == nil or word:lower():gsub('^/+', '') ~= 'vana2' then
        return false
    end
    rest = rest:lower()
    if rest == 'open' then
        v2.open = true
    elseif rest == 'close' then
        v2.open = false
    elseif rest == 'reset' then
        reset_values()
    else
        v2.open = not v2.open
    end
    return true
end)
