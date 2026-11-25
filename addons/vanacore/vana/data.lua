--[[
vana\data.lua: the settings of the /vana window: the row constructors, the helpers the rows use, the tooltips (TIPS),
the tabs (TABS, each its rows), the write path to vanacore.ini (read, write, flush, reset_rows) and the per-controller
gamepad profiles (vana.gamepad.profile and the rest). A new setting is a row here (and its tooltip in TIPS). Loaded by
vana.lua with `local M = ...`.

It began as the old window's code (since removed).
--]]

local M = ...

local function toggle(label, section, key, default, restart) return { kind = 'toggle', label = label, section = section, key = key, default = default, restart = restart } end
-- Who has one of the game's windows hidden through vana.gameui.hide (fancychat, for the chat), as one string of names, or
-- nil; asked of the game four times a second at most, however many rows and boxes read it (vana.gameui.hidden: a DLL
-- without it answers nobody). A Hide switch of such a window (`hides`, its gameui element) shows that as its own: on, locked,
-- its tooltip naming who (vana.lua's tip_of). Its box in the HUD Manager stays (hud.hidden). The player's own switch
-- is the [client] key as ever, and untouched.
local hide_asked = {}
local function hidden_by(element)
    local now = vana.time.clock()
    local entry = hide_asked[element]
    if entry == nil or now - entry.at >= 0.25 then
        local ok, names = false, nil
        local gameui = vana.gameui
        if gameui ~= nil and gameui.hidden ~= nil then
            ok, names = pcall(gameui.hidden, element)
        end
        entry = { at = now, names = ok and type(names) == 'string' and names or nil }
        hide_asked[element] = entry
    end
    return entry.names
end
M.hidden_by = hidden_by
local function hides(row, element)
    row.hides = element
    row.lock_if = function () return hidden_by(element) ~= nil end
    return row
end
-- [client] hide.windows (hudwin_hide.h): the game's windows left out by their layouts' names, `word` one of them (persona
-- the Status window, money@equip the gil beside the Equipment window, ...): a Hide switch that puts it in the list or takes
-- it out, the others kept
local function hide_window(label, word)
    local row = toggle(label, 'client', 'hide.windows', false)
    row.source = word -- (its own id: every one's key is hide.windows, and two in one element were one control, vana.lua draw_row)
    local function others()
        local list, had = {}, false
        for w in (vana.config.get('client', 'hide.windows') or ''):gmatch('[^%s,;]+') do
            if w:lower() == word then
                had = true
            else
                list[#list + 1] = w
            end
        end
        return list, had
    end
    row.get = function ()
        local _, had = others()
        return had
    end
    row.set = function (on)
        local list = others()
        if on then
            list[#list + 1] = word
        end
        vana.config.set('client', 'hide.windows', #list > 0 and table.concat(list, ' ') or nil)
    end
    return row
end
local function choice(label, section, key, default, options, restart) return { kind = 'choice', label = label, section = section, key = key, default = default, options = options, restart = restart } end
-- A colour, #RRGGBB in vanacore.ini ('' the game's own; `game` the colour it is), picked in ImGui's colour
-- picker beside the window.
local function colour_pick(label, section, key, game) return { kind = 'colour', label = label, section = section, key = key, default = '', game = game } end
-- Colours on one line (vana.lua kind 'colours', row.line: M.draw_colours_line): the name, a reset, each colour's swatch
-- in its order, then `switch`, the toggle that turns them on (nil: none), at the line's end; the colours lock while it's
-- off. A section's reset takes the colours out and leaves the switch, as it leaves a toggle row's. (The switch is
-- row.toggle: a row.switch is a panel's switch to reset_rows, which would leave the colours too.)
local function colours_line(label, switch, ...)
    local row = { kind = 'colours', line = true, label = label, toggle = switch, parts = { switch } }
    for _, part in ipairs({ ... }) do
        if switch ~= nil then
            part.lock_if = function () return M.read(switch) ~= true end
        end
        row.parts[#row.parts + 1] = part
    end
    row.source = row.parts[1].key -- (its id: draw_row's, by its first part)
    row.reset = function ()
        for _, part in ipairs(row.parts) do
            if part ~= switch then
                vana.config.set(part.section, part.key, nil)
            end
        end
    end
    return row
end
local function dropdown(label, section, key, default, options, restart) local row = choice(label, section, key, default, options, restart); row.dropdown = true; return row end
-- A choice row drawn as a dropdown however few its options (the aspect ratios: no < and > either side, 12 November)
local function as_dropdown(row) row.dropdown = true; return row end
-- The aspect-ratio dropdowns' options: `first` (the non-shape ones), then the shapes, narrowest first, under the kind of
-- screen each is
local function aspects(first)
    for _, group in ipairs({ { 'Classic', '5:4', '4:3' }, { 'Widescreen', '3:2', '16:10', '5:3', '16:9' },
                             { 'Ultrawide', '2:1', '21:9', '24:10' }, { 'Super ultrawide', '32:10', '32:9', '48:9' } }) do
        first[#first + 1] = { header = group[1] }
        for i = 2, #group do
            first[#first + 1] = { group[i], group[i] }
        end
    end
    return first
end
local function slider(label, section, key, default, min, max, step, fmt, restart) return { kind = 'slider', label = label, section = section, key = key, default = default, min = min, max = max, step = step, fmt = fmt, restart = restart } end
local function header(text) return { kind = 'header', text = text } end
-- A slider that takes any number typed into its value box, beyond the slider's ends (the slider then shows its end); `low` and
-- `high`, when given, are as far as the setting itself goes (what VanaCore reads from vanacore.ini)
local function free(row, low, high) row.free = true; row.low = low; row.high = high; return row end
-- A slider shown in other units than vanacore.ini keeps it in: shown = kept * a + b (Mass Grass's Range in tens of yalms, its
-- Height as a percent). Its default, ends, step and typed limits are the shown ones; the key keeps its own units, for VanaCore
-- and the presets. A follower's nil (put_back) takes the key out
local function shown_as(row, a, b)
    b = b or 0
    local kept = { kind = 'slider', section = row.section, key = row.key, step = 0.0001 } -- (what M.write formats and writes)
    row.get = function ()
        local value = tonumber(vana.config.get(row.section, row.key) or '')
        if value == nil then
            return row.default
        end
        value = value * a + b
        return row.free and math.max(row.low or value, math.min(row.high or value, value)) or math.max(row.min, math.min(row.max, value))
    end
    row.set = function (value, now)
        if value == nil then
            vana.config.set(row.section, row.key, nil)
            vana.graphics.reload()
            return
        end
        M.write(kept, (value - b) / a, now)
    end
    return row
end

-- The fonts a True Font row offers, each once: Windows' fonts, then VanaCore's fonts folder's (vana.ui.fonts: Browse puts a
-- picked font there), then the one set now (section.key), if it's none of those
local function font_list(section, key)
    local list, seen = {}, {}
    -- `face`: the font's file in Windows' Fonts, so the list shows its name in it (draw.lua M.face); an added font has none
    local function add(name, face)
        if name ~= nil and name ~= '' and not seen[name] then
            seen[name] = true
            list[#list + 1] = { name, name, face = face }
        end
    end
    for _, font in ipairs({ { 'Segoe UI', 'segoeui.ttf' }, { 'Segoe UI Semibold', 'seguisb.ttf' }, { 'Segoe UI Black', 'seguibl.ttf' },
                            { 'Arial', 'arial.ttf' }, { 'Arial Black', 'ariblk.ttf' }, { 'Bahnschrift', 'bahnschrift.ttf' },
                            { 'Calibri', 'calibri.ttf' }, { 'Cambria', 'cambria.ttc' }, { 'Candara', 'candara.ttf' },
                            { 'Comic Sans MS', 'comic.ttf' }, { 'Consolas', 'consola.ttf' }, { 'Constantia', 'constan.ttf' },
                            { 'Corbel', 'corbel.ttf' }, { 'Courier New', 'cour.ttf' }, { 'Franklin Gothic Medium', 'framd.ttf' },
                            { 'Georgia', 'georgia.ttf' }, { 'Impact', 'impact.ttf' }, { 'Lucida Console', 'lucon.ttf' },
                            { 'Palatino Linotype', 'pala.ttf' }, { 'Tahoma', 'tahoma.ttf' }, { 'Times New Roman', 'times.ttf' },
                            { 'Trebuchet MS', 'trebuc.ttf' }, { 'Verdana', 'verdana.ttf' } }) do
        add(font[1], font[2])
    end
    for _, name in ipairs(vana.ui ~= nil and vana.ui.fonts ~= nil and vana.ui.fonts() or {}) do
        add(name)
    end
    add(vana.config.get(section, key))
    return list
end

-- The fonts the Japanese Font row offers: each area's own font (''), Windows' Japanese fonts this PC has (looked for once),
-- then the ones Browse added and the one set now
local japanese_fonts = nil -- (data.lua's main chunk is near LuaJIT's 200 locals: this and the list's function alone)
local function japanese_font_list(section, key)
    if japanese_fonts == nil then
        japanese_fonts = {}
        local dir = ((os and os.getenv and os.getenv('WINDIR')) or 'C:\\Windows') .. '\\Fonts\\'
        for _, font in ipairs({ { 'Yu Gothic UI', 'YuGothR.ttc' }, { 'Yu Gothic', 'YuGothR.ttc' }, { 'Yu Gothic Medium', 'YuGothM.ttc' },
                                { 'Yu Gothic Light', 'YuGothL.ttc' }, { 'Meiryo', 'meiryo.ttc' }, { 'Meiryo UI', 'meiryo.ttc' },
                                { 'MS Gothic', 'msgothic.ttc' }, { 'MS PGothic', 'msgothic.ttc' }, { 'BIZ UDGothic', 'BIZ-UDGothicR.ttc' },
                                { 'BIZ UDPGothic', 'BIZ-UDGothicR.ttc' }, { 'Yu Mincho', 'yumin.ttf' }, { 'MS Mincho', 'msmincho.ttc' },
                                { 'BIZ UDMincho', 'BIZ-UDMinchoM.ttc' }, { 'UD Digi Kyokasho N-R', 'UDDigiKyokashoN-R.ttc' } }) do
            local file = io.open(dir .. font[2], 'rb')
            if file ~= nil then
                file:close()
                japanese_fonts[#japanese_fonts + 1] = font[1]
            end
        end
    end
    local list, seen = { { '', 'Each area\'s font' } }, { [''] = true }
    local function add(name)
        if name ~= nil and not seen[name] then
            seen[name] = true
            list[#list + 1] = { name, name }
        end
    end
    for _, name in ipairs(japanese_fonts) do
        add(name)
    end
    for _, name in ipairs(vana.ui ~= nil and vana.ui.fonts ~= nil and vana.ui.fonts() or {}) do
        add(name)
    end
    add(vana.config.get(section, key))
    return list
end

-- Font file: Browse, Windows' Open dialog for a .ttf or .otf from anywhere on the PC: copied into VanaCore's fonts folder (it
-- loads at every start, and every font list has it) and set as section.key (no key: only added, True Font's Custom Fonts).
-- The dialog opens once the click is let go (release).
local function font_browse(section, key)
    return { kind = 'action', label = key ~= nil and 'Font file' or 'Add a font', text = 'Browse', icon = 'folder', release = true, rebuild = true,
             source = key ~= nil and (section .. '.' .. key) or 'fonts.add', -- (its ImGui id's: one page has more than one Font file row)
             tip = 'A .ttf or .otf font from anywhere on your PC. It is copied into VanaCore\'s fonts folder, so every font list has it.',
             run = function ()
                 if vana.ui == nil or vana.ui.add_font == nil then
                     return
                 end
                 local window = vana.window
                 local block = window ~= nil and window.block_mouse or nil
                 if block ~= nil then
                     block(true, 600000) -- the dialog holds the game's thread: no frame renews it
                 end
                 local family, why = vana.ui.add_font()
                 if block ~= nil then
                     block(true, 400)
                 end
                 if family ~= nil and key ~= nil then
                     vana.config.set(section, key, family)
                 elseif family ~= nil then
                     print('VanaCore: ' .. family .. ' is in every font list now.')
                 elseif why ~= nil then
                     print('VanaCore: that font can\'t be used: ' .. why)
                 end
             end }
end
-- A picture for section.key ([client] window.image): Browse, Windows' Open dialog for a .png or .jpg from anywhere on the PC,
-- copied into VanaCore's images folder (vana.ui.add_image) and set as images\<name>; the name shown beside it, an x clears it.
-- The setting is its part (parts, reset), so the section's reset, a HUD preset and Vanilla UI take it as any other's
local function image_browse(label, section, key, tip)
    local function clear() vana.config.set(section, key, nil) end
    return { kind = 'action', label = label, text = 'Browse', icon = 'folder', release = true, rebuild = true, source = section .. '.' .. key,
             tip = tip, clear_tip = 'Back to the game\'s background',
             parts = { { section = section, key = key } }, reset = clear,
             value = function () return (vana.config.get(section, key) or ''):match('([^\\/]*)$') or '' end,
             clear = clear,
             run = function ()
                 if vana.ui == nil or vana.ui.add_image == nil then
                     return
                 end
                 local window = vana.window
                 local block = window ~= nil and window.block_mouse or nil
                 if block ~= nil then
                     block(true, 600000) -- the dialog holds the game's thread: no frame renews it
                 end
                 local path, why = vana.ui.add_image()
                 if block ~= nil then
                     block(true, 400)
                 end
                 if path ~= nil then
                     vana.config.set(section, key, path)
                 elseif why ~= nil then
                     print('VanaCore: that picture can\'t be used: ' .. why)
                 end
             end }
end
-- A True Font line: its font (a dropdown row), Bold, Italic and (a True Font area's) Uppercase (toggle rows), then its size
-- and (an area's) outline (slider rows, drawn as small drop-downs: vana.lua M.number_drop) on one row (vana.lua's kind
-- 'font'). Each part is its own setting (presets take them: parts); its one reset puts them all back.
local function font_line(font, size, italic, bold, upper, outline)
    local row = { kind = 'font', label = 'Font', parts = { font }, source = font.key, -- (source: each line its own id)
                  font = font, size = size, italic = italic, bold = bold, upper = upper, outline = outline }
    for _, part in ipairs({ { size }, { italic }, { bold }, { upper }, { outline } }) do -- (a line with no size: Zone Name's)
        row.parts[#row.parts + 1] = part[1]
    end
    if outline ~= nil then
        outline.short, outline.icon = 'Outline', 'outline' -- (its box a hollow A and "1.5" (draw.lua DRAWN.outline); the size's "16 px")
    end
    row.reset = function ()
        for _, part in ipairs(row.parts) do
            vana.config.set(part.section, part.key, nil)
        end
    end
    return row
end

-- An interface area's letters are True Font's ([client] `prefix`: chat.font, items.font, tips.font, ...): set, as it is;
-- unset, the menus' (menu.font) for an area that `follows` them (true), or a key's area's (a key: the party list's
-- status.font, which follows the menus' in turn), as VanaCore reads it (chatfont.cpp key_for). An area that follows
-- another that follows a third (the Target window's title the titles', those the buttons'): FOLLOWS, the second's
local FOLLOWS = { ['titles.font'] = 'buttons.font' }
local function area_sharp(prefix, follows)
    local value = vana.config.get('client', prefix) or ''
    if value == '' and type(follows) == 'string' then
        return area_sharp(follows, FOLLOWS[follows] or true)
    end
    if value == '' and follows then
        value = vana.config.get('client', 'menu.font') or ''
    end
    return value == 'sharp'
end
-- An area's True Font switch, at the end of its font line (row.switch: vana.lua draw_font_row; each was a Letters row of
-- Game's and True Font above it till the switch was asked for, 2026-11-08): on, `prefix` sharp, off game, read as
-- area_sharp has it; `pinned`: a key following this one while unset (the titles the buttons'), set to what it is first so it
-- stays as it was (as True Font's page's switches do). `section`: a [graphics] key's own (the damage numbers', the
-- nameplates': no following), the renderer's settings read again. A line's reset leaves it as it is, as a sub-panel's switch;
-- Vanilla UI takes it out (its `section` and `key`: vana.lua vanilla_ui).
local function area_switch(prefix, follows, pinned, section)
    return {
        section = section or 'client',
        key = prefix,
        get = function ()
            if section ~= nil then
                return (vana.config.get(section, prefix) or '') == 'sharp'
            end
            return area_sharp(prefix, follows)
        end,
        set = function (on)
            if pinned ~= nil and (vana.config.get('client', pinned) or '') == '' then
                vana.config.set('client', pinned, area_sharp(prefix, follows) and 'sharp' or 'game')
            end
            vana.config.set(section or 'client', prefix, on and 'sharp' or 'game')
            if section == 'graphics' then
                vana.graphics.reload()
            end
        end,
    }
end
-- A part of an area's line that reads `from`'s key while its own isn't set (the titles' the buttons', the party list's the
-- Status area's: chatfont.cpp key_for). `follows`: a row like it, reading another setting while its own key is unset, is
-- its default while unset, and its reset takes the key out so it follows again (vana.lua M.differs, M.put_back)
local function follow_key(part, prefix, from)
    local suffix = part.key:sub(#prefix + 1)
    local own, theirs, plain = part.key, from .. suffix, {}
    for k, v in pairs(part) do
        plain[k] = v
    end
    local function set(key) return (vana.config.get('client', key) or '') ~= '' end
    part.get = function ()
        -- (`from`'s own following another while unset: FOLLOWS)
        local followed = FOLLOWS[from] and not set(theirs) and FOLLOWS[from] .. suffix or theirs
        plain.key = set(own) and own or followed
        return M.read(plain)
    end
    part.follows = true
    return part
end
-- a free row's typed value held to the setting's own ends, or a slider's to its own
local function within(row, value)
    if not row.free then
        return math.max(row.min, math.min(row.max, value))
    end
    return math.max(row.low or value, math.min(row.high or value, value))
end

-- An area's own outline (<prefix>.outline), the last part of its font line (a tooltip part's: a slider): unset, what
-- VanaCore draws then (chatfont.cpp read_outline): `from`'s (the titles' the buttons'), else the Default Font's (truefont.outline)
local function outline_row(prefix, from)
    local key = prefix .. '.outline'
    local row = free(slider('Outline', 'client', key, 0, 0, 3, 0.5, '%.2f px'), 0, 4)
    row.tip = 'A black outline round these letters, this many pixels wide. 0 is none. Until it\'s moved, the Default Font\'s Outline (True Font page).'
    row.get = function ()
        local value = tonumber(vana.config.get('client', key) or '')
        if value == nil and from ~= nil then
            value = tonumber(vana.config.get('client', from .. '.outline') or '')
                or (FOLLOWS[from] and tonumber(vana.config.get('client', FOLLOWS[from] .. '.outline') or ''))
        end
        return within(row, value or tonumber(vana.config.get('client', 'truefont.outline') or '') or 0)
    end
    row.follows = true
    return row
end

-- That area's True Font on one line: its font (its own or the default font), size (0: its letters hidden), italic, bold,
-- uppercase (the tooltips': every part's until a part's own is set) and outline, its True Font switch at the end
-- (area_switch, `pinned` its; off, the line greyed, still settable); `follows` a key: each part that area's until set
-- (follow_key; the outline's outline_row's own)
local function area_font(prefix, size, follows, pinned)
    local fonts = font_list('client', prefix .. '.family')
    table.insert(fonts, 1, { '', 'Default font' })
    local theirs = type(follows) == 'string' and vana.config.get('client', follows .. '.family') or '' -- (followed, so in its list)
    for _, font in ipairs(fonts) do
        theirs = font[1] == theirs and '' or theirs
    end
    if theirs ~= '' then
        fonts[#fonts + 1] = { theirs, theirs }
    end
    local parts = { dropdown('Font', 'client', prefix .. '.family', '', fonts),
                    free(slider('Font size', 'client', prefix .. '.size', size, 0, 32, 1, '%d px'), 0, 48),
                    toggle('Italic', 'client', prefix .. '.italic', false),
                    toggle('Bold', 'client', prefix .. '.bold', false),
                    toggle('Uppercase', 'client', prefix .. '.uppercase', false) }
    parts[2].zero = 'Off' -- (size 0: its letters hidden)
    for _, part in ipairs(type(follows) == 'string' and parts or {}) do
        follow_key(part, prefix, follows)
    end
    local row = font_line(parts[1], parts[2], parts[3], parts[4], parts[5], outline_row(prefix, type(follows) == 'string' and follows or nil))
    row.switch = area_switch(prefix, follows, pinned)
    return row
end
-- A list's numbers' own True Font size (<list>.font.numbers.size: chatfont.cpp read_numbers), under its font line: unset,
-- the names' size (the font line's, Status & Numbers' until set); locked with it while its letters are the game's
local function numbers_size(list)
    local prefix = list .. '.font'
    local row = free(slider('Numbers size', 'client', prefix .. '.numbers.size', 11, 0, 32, 1, '%d px'), 0, 48)
    row.get = function ()
        local value = tonumber(vana.config.get('client', prefix .. '.numbers.size') or '')
            or tonumber(vana.config.get('client', prefix .. '.size') or '')
            or tonumber(vana.config.get('client', 'status.font.size') or '')
        return within(row, value or 11)
    end
    row.follows = true
    row.lock_if = function () return not area_sharp(prefix, 'status.font') end
    return row
end
-- effect: a heading with an On/Off toggle for a [graphics.effects] effect, `key` its strength: off
-- writes <key>.enabled=0 (the renderer gets 0, the strength stays); on takes that out, and when the
-- strength is 0 (off by default: the cel look, depth of field) sets it to `on`
local function effect(text, key, default, on) return { kind = 'header', text = text, effect = true, section = 'graphics.effects', key = key, default = default, on = on or default } end
-- `glyph`: a Material Symbols icon name drawn before the text, in `glyph_color` (a colour of C)
local function note(text, glyph, glyph_color) return { kind = 'note', text = text, glyph = glyph, glyph_color = glyph_color } end
-- a heading in yellow: a setting that is still being worked on
local function warn(row) row.warn = true; return row end
-- alert: a red bar with its text across a panel
local function alert(text) return { kind = 'alert', text = text } end
-- hint: the same bar in grey, a line saying what the rows under it do
local function hint(text) return { kind = 'hint', text = text } end
-- A row Reset to defaults leaves as it is (the renderer: a player on Direct3D 12 stays on it)
local function kept(row) row.keep = true; return row end
--- A switch Reset to defaults puts back all the same (it fine-tunes a panel, it doesn't turn a feature on or off: Plant
--- shadows hold still, on by default); the other switches a reset leaves as they are (switch_row)
local function resets(row) row.resets = true; return row end
-- info: a card, its label above its value; two to a line, a wide one on its own
-- value() may return a second string: an aside drawn dimmer after the value ("2560 x 1600" and "[3D: 3072 x 3072]")
local function info(label, value, wide) return { kind = 'info', label = label, value = value, wide = wide } end
-- The banner, as the README's: banner.png (the logo and the name, beside this file) and a line under it
local function banner(tagline) return { kind = 'banner', tagline = tagline } end

local FX = 'graphics.effects'

-- DLSS: its panel's switch ([graphics] dlss on or off) and its slider, 0 to 100% in steps of 5 (dlss.quality, with
-- dlss=on): performance at 0, balanced at 35, quality at 65, native (DLAA) at 100 - the scene drawn at 50%, 58%, 67% and 100%
-- each way, straight between them (graphics.cpp's dlss_quality_scale). A mode vanacore.ini names (dlss=quality,
-- dlaa, ...), or an older dlss.scale (the scene's percent), shows at its place until the slider moves.
local DLSS_PLACES = { dlaa = 100, quality = 65, balanced = 35, performance = 0, ultra = 0 }
local DLSS_STOPS = { { 0, 50 }, { 35, 58 }, { 65, 66.7 }, { 100, 100 } } -- { slider, the scene's percent }
local function dlss_mode()
    return ((vana.config.get('graphics', 'dlss') or 'off'):lower():gsub('%s', ''))
end
-- the slider's place for the scene's percent
local function dlss_place(percent)
    for i = 1, #DLSS_STOPS - 1 do
        local a, b = DLSS_STOPS[i], DLSS_STOPS[i + 1]
        if percent <= b[2] then
            return a[1] + (b[1] - a[1]) * math.max(0, percent - a[2]) / (b[2] - a[2])
        end
    end
    return 100
end
-- DLSS or FSR on in vanacore.ini but stopped (its helper crashed) or not there after all (vana.graphics.info().stopped,
-- asked once a second): its switch shows off, and turning it on tries it again (vana.graphics.retry)
M.stopped = { at = -1, which = '' }
function M.stopped.is(which)
    local now = vana.time.clock()
    if now - M.stopped.at >= 1 then
        M.stopped.at, M.stopped.which = now, vana.graphics.info().stopped or ''
    end
    return M.stopped.which == which
end
function M.stopped.retry(which)
    if M.stopped.is(which) and vana.graphics.retry ~= nil then
        vana.graphics.retry()
        M.stopped.at, M.stopped.which = -1, ''
    end
end
-- Its switch doesn't turn on on a card DLSS doesn't run on (vana.graphics.info().dlss_card: an NVIDIA RTX 20 or later;
-- nil from a VanaCore before it said); the Resolution line says why. Only the switch, not lock_if.
-- DLSS and FSR are one or the other: turning DLSS on turns FSR off. DLSS 5 runs on DLSS: turning DLSS off turns it off.
function M.dlss_enabled()
    local mode = dlss_mode()
    return mode ~= 'off' and mode ~= '' and not M.stopped.is('dlss')
end
local function dlss_header()
    return { kind = 'header', text = 'DLSS / FSR', section = 'graphics', key = 'dlss',
             -- the tab is DLSS / FSR but this heading says DLSS, green while it runs (vana.lua draw_heading's parts)
             parts = { { 'DLSS', function () return M.dlss_enabled() end } },
             switch = { get = M.dlss_enabled,
                        set = function (on)
                            if on and vana.graphics.info().dlss_card == false then
                                return
                            end
                            if on then
                                vana.config.set('graphics', 'fsr', 'off')
                                M.stopped.retry('dlss')
                            else
                                vana.config.set('graphics', 'dlss5', '0')
                            end
                            vana.config.set('graphics', 'dlss', on and 'on' or 'off')
                        end } }
end
local function dlss_quality()
    local row = slider('Quality', 'graphics', 'dlss.quality', 35, 0, 100, 5, '%d%%')
    row.get = function ()
        local value = DLSS_PLACES[dlss_mode()] or tonumber(vana.config.get('graphics', 'dlss.quality') or '')
        if value == nil then
            local percent = tonumber(vana.config.get('graphics', 'dlss.scale') or '')
            value = percent and dlss_place(percent) or row.default
        end
        return math.max(row.min, math.min(row.max, value))
    end
    row.set = function (value, now)
        if not now then
            return -- while it's dragged: written once it's let go (DLSS made again for each size)
        end
        vana.config.set('graphics', 'dlss.quality', tostring(math.floor(value + 0.5)))
        vana.config.set('graphics', 'dlss.scale', nil)
        if DLSS_PLACES[dlss_mode()] then
            vana.config.set('graphics', 'dlss', 'on')
        end
        vana.graphics.reload()
    end
    return row
end
-- AMD FSR: its panel's switch ([graphics] fsr on or off) and its slider, as DLSS's (fsr.quality, with fsr=on): performance
-- at 0, balanced at 35, quality at 65, native at 100 - the scene drawn at 50%, 59%, 67% and 100% each way (AMD's 2x, 1.7x,
-- 1.5x and 1x), straight between them (graphics.cpp's fsr_quality_scale). A mode vanacore.ini names (fsr=quality,
-- native, ...) shows at its place until the slider moves. Its switch doesn't turn on where FSR can't run
-- (vana.graphics.info().fsr_card: DirectX 11 or 12, FSR 1 where AMD's FSR doesn't run; nil from a VanaCore before it said), and
-- turning it on turns DLSS off. One table: the main chunk is near Lua's 200 locals.
local FSR = { places = { native = 100, quality = 65, balanced = 35, performance = 0, ultra = 0 } }
function FSR.mode()
    return ((vana.config.get('graphics', 'fsr') or 'off'):lower():gsub('%s', ''))
end
function FSR.header()
    -- a sub-panel of the DLSS / FSR tab, drawn as a section heading is (headline: vana.lua draw_headline)
    local row = { kind = 'subheader', text = 'FSR', section = 'graphics', key = 'fsr' } -- (subheader() is defined below)
    row.headline, row.green_on, row.own_panel = true, true, true
    -- its Reset: every FSR setting on the page, its own switch left as it is
    row.reset = function ()
        local list = {}
        for _, other in ipairs(M.st.rows and M.st.rows.graphics or {}) do
            if other.section == 'graphics' and type(other.key) == 'string' and other.key:find('^fsr%.') then
                list[#list + 1] = other
            end
        end
        M.reset_rows(list)
    end
    row.switch = { get = function ()
                            local mode = FSR.mode()
                            return mode ~= 'off' and mode ~= '' and not M.stopped.is('fsr')
                        end,
                        set = function (on)
                            if on and vana.graphics.info().fsr_card == false then
                                return
                            end
                            if on then
                                vana.config.set('graphics', 'dlss', 'off')
                                vana.config.set('graphics', 'dlss5', '0') -- (it runs on DLSS)
                                M.stopped.retry('fsr')
                            end
                            vana.config.set('graphics', 'fsr', on and 'on' or 'off')
                        end }
    return row
end
function FSR.quality()
    local row = slider('Quality', 'graphics', 'fsr.quality', 35, 0, 100, 5, '%d%%')
    row.get = function ()
        local value = FSR.places[FSR.mode()] or tonumber(vana.config.get('graphics', 'fsr.quality') or '') or row.default
        return math.max(row.min, math.min(row.max, value))
    end
    row.set = function (value, now)
        if not now then
            return -- while it's dragged: written once it's let go (FSR made again for each size)
        end
        vana.config.set('graphics', 'fsr.quality', tostring(math.floor(value + 0.5)))
        if FSR.places[FSR.mode()] then
            vana.config.set('graphics', 'fsr', 'on')
        end
        vana.graphics.reload()
    end
    return row
end

-- A sub-panel inside a panel: its heading, a line across the panel with its title (and a tag after it) on it, and an
-- on/off switch (switch = { get, set }) that greys the rows after it to the next sub-panel or the panel's end
local function subheader(text, tag, switch, section, key)
    return { kind = 'subheader', text = text, tag = tag, switch = switch, section = section, key = key }
end
-- a sub-panel inside another: nested, greyed with the sub-panel it's in (DLSS 5's); group 'group', its rows and the
-- sub-panels after it marked 'in' drawn in a box (Shadows' Advanced Settings; vana.lua's draw_group)
local function inner_sub(text, nested, group)
    local row = subheader(text)
    row.nested = nested
    row.group, row.in_group = group == 'group', group == 'in'
    return row
end
-- The Advanced Settings groups (DLSS's, Shadows', Mass Grass's: a 'group' heading and the rows under it, drawn in a box):
-- hidden unless Client > VanaCore > Shaders' Enable Advanced Graphics Settings ([vana] advanced) is on. `advanced` marks
-- the heading for M.reset_advanced.
local function advanced_on() return (tonumber(vana.config.get('vana', 'advanced') or '') or 0) ~= 0 end
local function advanced_group(tip)
    local sub = inner_sub('Advanced Settings', nil, 'group')
    sub.advanced = true
    sub.show_if = advanced_on
    sub.tip = tip
    return sub
end
-- a sub-panel switched as an effect panel's heading is (effect): off writes <key>.enabled=0 and keeps the strength; on takes
-- that out and sets the strength to `on` where it's 0. Marked an effect, so a reset keeps an effect that's on, on
-- A strength slider that was an effect's switch and heading's (effect_sub) with no switch now: above 0 is on, 0 is off.
-- The effect switched off before (<key>.enabled=0) with a strength is switched on, as the number says; moving the slider
-- clears it too (row.changed).
function M.unswitched(row, key)
    if tonumber(vana.config.get(FX, key .. '.enabled') or '') == 0 and (tonumber(vana.config.get(FX, key) or '') or 0) > 0 then
        vana.config.set(FX, key .. '.enabled', nil)
    end
    row.changed = function () vana.config.set(FX, key .. '.enabled', nil) end
    return row
end

local function effect_sub(text, key, default, on, tag)
    local row = subheader(text, tag, {
        get = function ()
            local enabled = tonumber(vana.config.get(FX, key .. '.enabled') or '') ~= 0
            return enabled and (tonumber(vana.config.get(FX, key) or '') or default) > 0
        end,
        set = function (value)
            if not value then
                vana.config.set(FX, key .. '.enabled', '0')
                return
            end
            vana.config.set(FX, key .. '.enabled', nil)
            if (tonumber(vana.config.get(FX, key) or '') or default) <= 0 then
                vana.config.set(FX, key, tostring(on))
            end
        end,
    }, FX, key)
    row.effect, row.default, row.on = true, default, on
    return row
end
-- warning: a line of red text across a panel, a warning triangle before it (after a sub-panel's heading marked danger);
-- tone 'grey' draws the same box in grey
local function warning(text, tone) return { kind = 'warning', text = text, tone = tone } end
--- callout: THE banner (vana.lua's banner standard): a title in capitals over its text, in a tone - 'warning' orange (the
--- default), 'success' green, 'basic' grey, 'notice' blue, 'danger' red. Every notice on a page is one of these
local function callout(title, text, tone) return { kind = 'callout', title = title, text = text, tone = tone } end
-- status: a line of cyan text across a panel, "<label>: <value>", the value (a function) read again twice a second
local function status(label, value) return { kind = 'status', label = label, value = value } end
-- a status row's text as it reads now
local function status_text(row)
    local ok, value = pcall(row.value)
    return row.label .. ': ' .. (ok and tostring(value) or '?')
end
-- DLSS 5 (neural rendering), in the DLSS panel: [graphics] dlss5 its switch. Where it can't run (vana.graphics.info().dlss5:
-- an NVIDIA RTX 40 or 50, NVIDIA's DLL, NGX's say) it's greyed and locked while it's off, dlss5_why() says why under it,
-- and its own rows, each dlss5_why(row), are left out (VanaCore doesn't ship nvngx_dlssnr.dll: a player adds it). It's
-- locked off while DLSS is off too (it runs on DLSS), and switching DLSS (or FSR) on or off sets it off.
local function dlss5_on() return (tonumber(vana.config.get('graphics', 'dlss5') or '') or 0) ~= 0 end
-- The rows are built from what vana.graphics.info() says then: the page is built again when that changes (vana.lua's
-- tick). A DLSS helper just started (DLSS switched on, from FSR) reads as able to till NGX answers, seconds later.
function M.dlss5_seen()
    local info = vana.graphics.info()
    return tostring(info.dlss5) .. '|' .. tostring(info.dlss5_why)
end
-- The Client page's Resolution: in a windowed mode, the window's inside while it's maximized (not kept: restored, it's
-- its size again), else the size its edges were dragged to ([window] lastsize, with Resizable edges on: window.cpp's
-- resized_client); else [ffxi] resolution.game. Then 'maximized' or 'resized' when it's one of those, else nil. The page
-- is built again when it changes (vana.lua's tick).
function M.resolution_shown()
    local kept = (vana.config.get('window', 'lastsize') or ''):lower():gsub('%s', '')
    local mode = (vana.config.get('ffxi', 'mode') or ''):lower():gsub('[%s_-]', '')
    if mode == '' then -- unset: the game's own (0034), borderless full screen unless that says otherwise
        mode = ({ ['1'] = 'windowed', ['2'] = 'borderless' })[vana.config.game('0034') or ''] or 'full'
    end
    local windowed = mode == 'windowed' or mode == 'borderless' or mode == 'borderlesswindowed' or mode == '1' or mode == '2'
    local place = windowed and vana.window and vana.window.info and vana.window.info()
    if place and place.maximized and not place.minimized and (place.client_width or 0) > 0 and (place.client_height or 0) > 0 then
        return place.client_width .. 'x' .. place.client_height, 'maximized'
    end
    if kept ~= '' and windowed and (tonumber(vana.config.get('window', 'resizable') or '') or 0) ~= 0 then
        return kept, 'resized'
    end
    -- unset is the screen's resolution (auto)
    local set = (vana.config.get('ffxi', 'resolution.game') or ''):lower():gsub('%s', '')
    return set ~= '' and set or 'auto', nil
end
-- The Client page's monitor: which one the window is on and its size, and how many there are. The page is built again when
-- it changes (vana.lua's tick), so the resolutions' "(screen)" and Screen (auto) sizes are the monitor's it's on now.
function M.monitor_seen()
    local place = vana.window and vana.window.info and vana.window.info()
    local g = vana.graphics.info()
    return string.format('%s|%s|%sx%s', place and place.monitor or 0, place and #place.monitors or 0, g.monitor_width or 0,
                         g.monitor_height or 0)
end
local function dlss5_header()
    M.dlss5_built = M.dlss5_seen()
    -- drawn as the DLSS panel's own heading is (headline: vana.lua draw_headline): the title, a Reset button and the switch
    local row = subheader('DLSS 5', nil, {
        get = dlss5_on,
        set = function (on) vana.config.set('graphics', 'dlss5', on and '1' or '0') end,
    }, 'graphics', 'dlss5')
    row.headline = true
    -- its Reset: every DLSS 5 setting on the page (the rows built with the page), its own switch left as it is
    row.reset = function ()
        local list = {}
        for _, other in ipairs(M.st.rows and M.st.rows.graphics or {}) do
            if other.section == FX and type(other.key) == 'string' and other.key:find('^dlss5_') then
                list[#list + 1] = other
            end
        end
        M.reset_rows(list)
    end
    local can = vana.graphics.info().dlss5 ~= false -- (nil: a VanaCore before it said)
    -- (on, it's never locked: that's how a DLSS 5 left on is turned off)
    row.lock_if = function () return not dlss5_on() and (not can or not M.dlss_enabled()) end
    return row
end
local function dlss5_why(row)
    local info = vana.graphics.info()
    if row then -- one of DLSS 5's own rows: shown where it can run, or while it's on (to turn it off)
        return (info.dlss5 ~= false or dlss5_on()) and row or { kind = 'none' }
    end
    if info.dlss5 ~= false then
        return { kind = 'none' }
    end
    return note('Not available here: ' .. (info.dlss5_why ~= '' and info.dlss5_why or 'unknown') .. '.', 'block')
end
-- marks: names over the slider after it, each at its value (min..max, the slider's)
local function marks(min, max, list) return { kind = 'marks', min = min, max = max, marks = list } end

-- The screen sizes a resolution row offers (the game's resolution, and the background resolution after its squares)
local SCREEN_SIZES = { '1280x720', '1280x800', '1366x768', '1440x900', '1600x900', '1680x1050', '1920x1080', '1920x1200',
                       '2400x1350', '2560x1080', '2560x1440', '2560x1600', '3440x1440', '3840x2160' }

-- An option that needs DirectX 11 or 12: greyed and locked until the game draws with one, a "DX11+" pill after its name
-- (vana.lua's dx_pill) saying why
local function needs_dx(row)
    row.needs_dx = true
    return row
end

-- DLSS is Direct3D 12's alone (DirectX 11 has the effects above, not this): drawing with any other renderer, the rows of
-- its panel (needs_dx12) are drawn half there and do nothing, and a red bar (dx12_alert) at the top of the panel says so; a
-- row that shows nothing under DirectX 12.
local FADED = 0.5
-- vana.graphics.info() builds a table; the renderer doesn't change while the window is up, so it's asked once a second
local dx12_at, dx12_is = -1, false
local function dx12()
    local now = vana.time.clock()
    if now - dx12_at >= 1 then
        dx12_at, dx12_is = now, vana.graphics.info().running == 'd3d12'
    end
    return dx12_is
end

local function needs_dx12(row)
    row.needs_dx12 = true
    return row
end

-- One of the ink lines' grass rows (the Effects tab): locked while Mass Grass is off; a slider (`zero` given: shown as `zero` at
-- 0, `fmt` else) also while the grass is outlined with the scenery (outline_nograss 0), where its own lines aren't drawn
local function grass_lines(row, zero, fmt)
    local function grass_off() return tonumber(vana.config.get('graphics', 'grass') or '') ~= 1 end
    if zero == nil then
        row.lock_if = grass_off
    else
        row.lock_if = function () return grass_off() or tonumber(vana.config.get(FX, 'outline_nograss') or '') == 0 end
        row.format = function (value) return value < 0.0001 and zero or string.format(fmt, value) end
    end
    return row
end

-- A short note after a setting's name, dim and smaller (vana.lua's row.hint): Max FPS's "before frame gen"
local function hinted(row, hint)
    row.hint = hint
    return row
end

-- While building the content of such a row, `dim` holds how opaque it's drawn: its text and icons (dim.text) and the
-- rectangles of its controls (dim.rects, set by control(); not the panel's own lines behind them). Its clicks and hovers do
-- nothing (hit). dim.of(row) is how opaque a row's content is.
local dim = { text = 1, rects = 1 }
function dim.of(row)
    if row.needs_dx12 ~= true then
        return 1
    end
    if dim.has_dx12 == nil then -- asked once a build
        dim.has_dx12 = dx12()
    end
    return dim.has_dx12 and 1 or FADED
end

local function dx12_alert(what)
    if dx12() then
        return { kind = 'none' }
    end
    return alert(what .. ' is disabled: it needs DirectX 12')
end

-- A value from vanacore.ini without an inline comment ("pivot    ; relative to ..."), or nil.
local function ini_value(section, key)
    local text = vana.config.get(section, key)
    if text == nil then
        return nil
    end
    text = text:gsub('%s+;.*$', ''):gsub('^%s+', ''):gsub('%s+$', '')
    return text
end

-- Under the renderer, while vanacore.ini names another than the game started with (it changes at the next
-- start); a row that shows nothing otherwise.
local function renderer_alert()
    local chosen = (ini_value('graphics', 'renderer') or ''):lower()
    if chosen ~= 'd3d9' and chosen ~= 'd3d11' and chosen ~= 'd3d12' then
        chosen = 'd3d8' -- as VanaCore takes anything else
    end
    if chosen == vana.graphics.info().renderer then
        return { kind = 'none' }
    end
    return alert('Game must restart for changes to take effect!!')
end

-- The pivot folder ([pivot] rootfolder, from VanaCore's folder).
local function pivot_root()
    local root = ini_value('pivot', 'rootfolder') or ''
    if root == '' then
        root = 'pivot'
    end
    if not root:match('^%a:[\\/]') and not root:match('^[\\/][\\/]') then
        root = vana.paths.install .. '\\' .. (root:gsub('^%.[\\/]', ''))
    end
    return root
end

-- [pivot.order]'s overlays, lowest number first, without soulspirexi: the server's own (the DATs for its custom
-- content), which the client uses first whenever the pivot folder has it, listed or not (true as the second value).
local function pivot_order()
    local order = {}
    for n = 0, 99 do
        local name = ini_value('pivot.order', tostring(n))
        if name ~= nil and name ~= '' and name:lower():gsub('[\\/]+$', '') ~= 'soulspirexi' then
            order[#order + 1] = name
        end
    end
    return order, vana.fs.is_dir(pivot_root() .. '\\soulspirexi') == true
end

-- A folder laid out like the FFXI folder: a ROM folder (ROM, ROM2 ...), a sound folder (sound,
-- sound2 ...: a music pack has only those), FTABLE.DAT or VTABLE.DAT in it.
local function is_overlay(path)
    local found = vana.fs.list(path, '^(rom[0-9]*|sound[0-9]*|[fv]table\\.dat)$')
    return found ~= nil and #found > 0
end

-- The overlays under the pivot folder `root`: walking down from it, each folder that's the first laid
-- out like the FFXI folder (ffxi-maps\1024x1024), looked for down to 4 folders deep; an
-- overlay's own folders aren't looked in.
local function find_overlays(root, relative, depth, found)
    local here = relative == '' and root or root .. '\\' .. relative
    for _, folder in ipairs(vana.fs.list(here, '', 'folders') or {}) do
        local name = relative == '' and folder or relative .. '\\' .. folder
        if is_overlay(root .. '\\' .. name) then
            found[#found + 1] = name
        elseif depth < 4 then
            find_overlays(root, name, depth + 1, found)
        end
    end
    return found
end

-- The overlays: those in [pivot.order], in its order, and the pivot folder's others (a folder of
-- them, as ffxi-maps' sizes, gives each); and whether soulspirexi is in use, before them. The folder is
-- looked at afresh each time: a new overlay shows on Refresh.
local function pivot_overlays()
    local root = pivot_root()
    local on, server = pivot_order()
    local listed, off = { soulspirexi = true }, {}
    for _, name in ipairs(on) do
        listed[name:lower():gsub('/', '\\')] = true
    end
    for _, name in ipairs(find_overlays(root, '', 1, {})) do
        if not listed[name:lower()] then
            off[#off + 1] = name
        end
    end
    return on, off, root, server
end

-- [pivot.order] written afresh from `order` (0 first), soulspirexi first when the pivot folder has it.
local function write_order(order)
    local _, server = pivot_order()
    if server then
        table.insert(order, 1, 'soulspirexi')
    end
    for n = 0, 99 do
        if vana.config.get('pivot.order', tostring(n)) ~= nil then
            vana.config.set('pivot.order', tostring(n), nil)
        end
    end
    for i, name in ipairs(order) do
        vana.config.set('pivot.order', tostring(i - 1), name)
    end
end

-- Overlay `name` put in [pivot.order], last, or taken out; the others keep their order.
local function set_overlay(name, on)
    local order = {}
    for _, other in ipairs(pivot_order()) do
        if other:lower() ~= name:lower() then
            order[#order + 1] = other
        end
    end
    if on then
        order[#order + 1] = name
    end
    write_order(order)
end

-- The overlay at `index` in [pivot.order] moved up (-1) or down (1) a place.
local function move_overlay(index, by)
    local order = pivot_order()
    local other = index + by
    if order[index] == nil or order[other] == nil then
        return
    end
    order[index], order[other] = order[other], order[index]
    write_order(order)
end

-- An overlay's row: a folder, its name; in use, arrows to move it and a switch to take it out; not in
-- use, a switch to put it in (last). The server's own (soulspirexi): a padlock, in its colour, locked on.
local function overlay(name, on, index, count, server)
    if server then
        return { kind = 'overlay', label = name, name = name, on = true, server = true, icon = 'lock', colour = '#E5C07BFF',
                 tip = "SoulspireXI's own files: the DATs for the server's custom content.\nAlways on, and always first." }
    end
    return { kind = 'overlay', label = name, name = name, on = on, index = index, count = count, icon = 'folder' }
end

-- Addons and plugins: what VanaCore's folders hold, and which of them are loaded now.
local KIND_NAMES = { vanacore = 'VanaCore', ashita = 'Ashita', windower = 'Windower' }

-- 'source\0name' (the name lower case) of each one loaded, or nil when this VanaCore can't say.
local function loaded_extensions(what)
    local api = vana[what == 'addon' and 'addons' or 'plugins']
    if api == nil or api.loaded == nil then
        return nil
    end
    local set = {}
    for _, entry in ipairs(api.loaded()) do
        set[entry.source .. '\0' .. entry.name:lower()] = true
    end
    return set
end

-- Every addon (a folder with <name>\<name>.lua in it, under addons\vanacore, addons\ashita or addons\windower) or plugin
-- (a .dll in plugins\ashita or plugins\windower): { name, source }.
-- Plugins VanaCore refuses because they only work inside Ashita, and provides itself: Addons (its own addon host) and
-- Sequencer (patches Ashita's packet code; VanaCore reports it present so LuAshitacast uses resends). Not listed, as
-- there's nothing to load. (src/core/plugins.cpp kRefused.)
local HIDDEN_PLUGINS = { addons = true, sequencer = true }

local function folder_extensions(what)
    local found = {}
    local root = vana.paths.install .. '\\' .. (what == 'addon' and 'addons' or 'plugins') .. '\\'
    for _, source in ipairs(what == 'addon' and { 'vanacore', 'ashita', 'windower' } or { 'ashita', 'windower' }) do
        local folder = root .. source
        if what == 'addon' then
            for _, name in ipairs(vana.fs.list(folder, '', 'folders') or {}) do
                if vana.fs.exists(folder .. '\\' .. name .. '\\' .. name .. '.lua') then
                    found[#found + 1] = { name = name, source = source }
                end
            end
        else
            for _, file in ipairs(vana.fs.list(folder, '.+\\.dll', 'files') or {}) do
                local name = file:sub(1, -5)
                if not HIDDEN_PLUGINS[name:lower()] then
                    found[#found + 1] = { name = name, source = source }
                end
            end
        end
    end
    return found
end

-- What's typed in each tab's search box, and which box has the keyboard (nil: none).
local query = { addon = '', plugin = '', graphics = '', effects = '', options = '' }
local search_focus = nil
local search_blink = true -- the caret shown (it blinks twice a second)

-- The tab's rows: a search box, then the ones loaded first and the rest, each by name; only what the box's text is in
-- the name or the kind of.
local function extension_rows(what)
    local plural = what == 'addon' and 'Addons' or 'Plugins'
    local loaded = loaded_extensions(what)
    if loaded == nil then
        return { header(plural), note("This VanaCore can't say what is loaded: update it.") }
    end
    -- first in their list whatever the name; keybinds (always on) has no switch
    local first = { addon = { vana = 1, keybinds = 2 }, plugin = {} }
    local fixed = { addon = { keybinds = true }, plugin = {} }
    local wanted = query[what]:lower()
    local on, off = {}, {}
    for _, entry in ipairs(folder_extensions(what)) do
        local lower = entry.name:lower()
        if wanted == '' or lower:find(wanted, 1, true) or KIND_NAMES[entry.source]:lower():find(wanted, 1, true) then
            local row = { kind = 'extension', label = entry.name, name = entry.name, source = entry.source, what = what,
                          on = loaded[entry.source .. '\0' .. lower] == true,
                          icon = what == 'addon' and 'extension' or 'power',
                          rank = first[what][lower] or 99,
                          fixed = fixed[what] ~= nil and fixed[what][lower] == true }
            table.insert(row.on and on or off, row)
        end
    end
    local function by_name(a, b)
        if a.rank ~= b.rank then
            return a.rank < b.rank
        end
        if a.name:lower() ~= b.name:lower() then
            return a.name:lower() < b.name:lower()
        end
        return a.source < b.source
    end
    table.sort(on, by_name)
    table.sort(off, by_name)
    local rows = { { kind = 'search', what = what, query = query[what] } }
    if wanted ~= '' and #on == 0 and #off == 0 then
        rows[#rows + 1] = note('Nothing matches "' .. query[what] .. '".')
        return rows
    end
    if wanted == '' or #on > 0 then
        rows[#rows + 1] = header('Enabled')
        for _, row in ipairs(on) do rows[#rows + 1] = row end
        if #on == 0 then
            rows[#rows + 1] = note('None loaded: switch one on below.')
        end
    end
    if wanted == '' or #off > 0 then
        rows[#rows + 1] = header('Available')
        for _, row in ipairs(off) do rows[#rows + 1] = row end
        if #off == 0 then
            rows[#rows + 1] = note(#on == 0 and 'None in the ' .. plural:lower() .. ' folder.' or 'Every one is loaded.')
        end
    end
    return rows
end

-- The Addons tab's pages (List, the addons to load, and Layout, their windows: the list of pages at the left picks), and Layout:
-- the windows addons and plugins draw with ImGui, each where it is and its scale, to change (vana.layout: VanaCore moves and
-- scales them at their Begin, and keeps what's set in config\layout.json). One table: the window's chunk has all the locals
-- Lua allows.
local layout = {
    page = 'list',
    cache = { tick = -1, list = {}, by_name = {} }, -- the windows, asked for once a frame
    tick = 0,         -- frames drawn ('present')
    signature = nil,  -- the windows the page was built with: built again when they change
    next = 0,         -- when the page looks at the windows again
    lives = {},       -- functions that read a live row's value again (as build_control made them)
    KINDS = { ashita = 'Ashita', windower = 'Windower', vanacore = 'VanaCore', plugin = 'Plugin' },
}

function layout.windows()
    local cache = layout.cache
    if cache.tick ~= layout.tick then
        cache.tick = layout.tick
        cache.list, cache.by_name = {}, {}
        for _, w in ipairs(vana.layout.windows()) do
            if w.owner ~= 'vanacore/vana' then -- this window's own (its colour and number boxes)
                cache.list[#cache.list + 1] = w
                cache.by_name[w.name] = w
            end
        end
    end
    return cache.list, cache.by_name
end

-- What the page shows of the windows: who draws each, and whether it's drawn and placed by its addon
function layout.signature_of(list)
    local parts = {}
    for _, w in ipairs(list) do
        parts[#parts + 1] = w.owner .. '\0' .. w.name .. '\0' .. tostring(w.shown) .. tostring(w.pinned)
    end
    table.sort(parts)
    return table.concat(parts, '\1')
end

-- ImGui's name without its id: "Party Finder", "YunaGearOpt" (YunaGearOpt##main), "pf_mini" (##pf_mini); and the id
function layout.title(name)
    local title, id = name:match('^(.-)###(.*)$')
    if title == nil then
        title, id = name:match('^(.-)##(.*)$')
    end
    if title == nil then
        return name, nil
    end
    if title == '' then
        return id ~= '' and id or name, nil
    end
    return title, id
end

-- "partyfinder (Ashita)" for "ashita/partyfinder", spelt as its folder is when it's loaded
function layout.owner_title(owner, names)
    local kind, name = owner:match('^(%w+)/(.+)$')
    if kind == nil then
        return owner ~= '' and owner or 'Other'
    end
    return (names[kind .. '/' .. name:lower()] or name) .. ' (' .. (layout.KINDS[kind] or kind) .. ')'
end

-- A window's X, Y or Scale: what it is now (as the last frame drew it), followed while the page is open
function layout.slider(label, w, what, min, max, step, fmt)
    local row = slider(label, 'layout', w.name .. '\0' .. what, what == 'scale' and 1 or w[what], min, max, step, fmt)
    row.window = w.name
    row.live = true
    row.no_reset = what ~= 'scale' -- (a place has no default: the panel's reset forgets what's set)
    row.get = function ()
        local _, by_name = layout.windows()
        local now = by_name[w.name]
        return now ~= nil and now[what] or w[what]
    end
    row.set = function (value)
        vana.layout.set(w.name, what == 'x' and value or nil, what == 'y' and value or nil, what == 'scale' and value or nil)
    end
    row.reset = function () vana.layout.reset(w.name) end
    return what == 'scale' and free(row, 0.25, 4) or free(row)
end

-- A window's Lock and No Pointer Events (vana.layout.flags), icon switches at its row's end (draw.lua M.icon_switches):
-- each as the last frame had it. None for text and shapes (not an ImGui window), nor with a VanaCore.Core.dll that hasn't
-- vana.layout.flags. Asked for 2026-11-07 (statustimers' icons dragged and their tooltips in the way).
function layout.flags_of(w)
    if vana.layout.flags == nil or w.name:find('##objects/', 1, true) ~= nil then
        return nil
    end
    local function now(what)
        local _, by_name = layout.windows()
        return (by_name[w.name] or w)[what] == true
    end
    return {
        { key = 'locked', label = 'Lock', icon = 'lock_open', icon_on = 'lock',
          tip = 'The mouse can drag and resize it.',
          tip_on = 'The mouse can\'t drag or resize it. Its X, Y and Scale here still move it.',
          get = function () return now('locked') end,
          set = function (on) vana.layout.flags(w.name, on, nil) end },
        { key = 'no_pointer', label = 'No Pointer Events', icon = 'pointer', icon_on = 'pointer_off',
          tip = 'It takes the mouse: clicks, the wheel, its tooltips.',
          tip_on = 'The mouse goes through it to what\'s under it (the game, another window): no clicks, wheel or tooltips on it.',
          get = function () return now('no_pointer') end,
          set = function (on) vana.layout.flags(w.name, nil, on) end },
    }
end

-- The addons and plugins loaded now: 'ashita/partyfinder' (the owner's kind and name, lower case) -> the name as its folder
-- spells it; and those keys in one string, for the page to be built again when one is loaded or unloaded
function layout.loaded()
    local names, keys = {}, {}
    for _, entry in ipairs(vana.addons.loaded and vana.addons.loaded() or {}) do
        local key = entry.source .. '/' .. entry.name:lower()
        names[key], keys[#keys + 1] = entry.name, key
    end
    for _, entry in ipairs(vana.plugins.loaded and vana.plugins.loaded() or {}) do
        local key = 'plugin/' .. entry.name:lower()
        names[key], keys[#keys + 1] = entry.name, key
    end
    return names, table.concat(keys, '\1')
end

-- What the page is built from (vana.lua builds it again when this changes): the windows and the addons loaded now
function layout.state(list)
    local _, loaded = layout.loaded()
    return layout.signature_of(list) .. '\2' .. loaded
end

-- An addon's (or plugin's) windows, those drawn now first: a row each, its X, Y and Scale along it
function layout.window_rows(rows, group, sw, sh)
    table.sort(group.windows, function (a, b)
        if a.shown ~= b.shown then
            return a.shown
        end
        return a.name:lower() < b.name:lower()
    end)
    local titles = {}
    for _, w in ipairs(group.windows) do
        local title = layout.title(w.name)
        titles[title] = (titles[title] or 0) + 1
    end
    for _, w in ipairs(group.windows) do
        local title, id = layout.title(w.name)
        if titles[title] > 1 and id ~= nil then
            title = title .. ' (' .. id .. ')' -- two of its windows go by one name
        end
        -- one row: the window's name, then its X, Y and Scale along it (vana.lua draws the kind); the panel's reset
        -- goes through row.reset (no section or key here, so it isn't a setting of the panel's)
        rows[#rows + 1] = { kind = 'layout', label = title, window = w.name,
                            tag = not w.shown and 'Not shown' or w.pinned and 'Placed by its addon' or nil,
                            reset = function () vana.layout.reset(w.name) end, flags = layout.flags_of(w),
                            sliders = { layout.slider('X', w, 'x', 0, math.max(1, sw), 1, '%d'),
                                        layout.slider('Y', w, 'y', 0, math.max(1, sh), 1, '%d'),
                                        layout.slider('Scale', w, 'scale', 0.5, 2, 0.05, '%.2fx') } }
    end
end

-- The Layout page: a panel for each addon (or plugin) loaded now with a window, those drawing now first, by name (each a
-- sub-tab under Addons). The windows of those not loaded now (drawn earlier this session, or saved places none has drawn
-- since the game started) are in one panel, Other, last, a sub-panel for each. Asked for (2026-11-10): the list
-- under Addons only the addons that are active.
function layout.rows()
    local rows = {}
    if vana.layout == nil then
        layout.signature = nil
        rows[#rows + 1] = note("This VanaCore can't move addons' windows: update it.")
        return rows
    end
    local list = layout.windows()
    local names, loaded = layout.loaded() -- (the names as their folders spell them)
    layout.signature = layout.signature_of(list) .. '\2' .. loaded -- (layout.state)
    local can_tell = vana.addons.loaded ~= nil and vana.plugins.loaded ~= nil -- (an old VanaCore: every addon its panel)
    local groups, order, others = {}, {}, {}
    for _, w in ipairs(list) do
        local group = groups[w.owner]
        if group == nil then
            local kind, name = w.owner:match('^(%w+)/(.+)$')
            group = { title = layout.owner_title(w.owner, names), owner = w.owner, windows = {}, shown = false }
            groups[w.owner] = group
            if kind ~= nil and (not can_tell or names[kind .. '/' .. name:lower()] ~= nil) then
                order[#order + 1] = group
            else
                others[#others + 1] = group
            end
        end
        group.windows[#group.windows + 1] = w
        group.shown = group.shown or w.shown
    end
    table.sort(order, function (a, b)
        if a.shown ~= b.shown then
            return a.shown
        end
        return a.title:lower() < b.title:lower()
    end)
    if #order == 0 and #others == 0 then
        rows[#rows + 1] = header('Windows')
        rows[#rows + 1] = note('No addon or plugin is drawing a window. Open one and it shows here.')
        return rows
    end
    local sw, sh = vana.ui.screen()
    for _, group in ipairs(order) do
        local head = header(group.title)
        -- the addon (or plugin) the panel is, for its heading's Unload (vana.lua): its kind and name as its folder spells it
        local kind, name = group.owner:match('^(%w+)/(.+)$')
        head.extension = { what = kind == 'plugin' and 'plugin' or 'addon', source = kind ~= 'plugin' and kind or nil,
                           name = names[kind .. '/' .. name:lower()] or name }
        rows[#rows + 1] = head
        layout.window_rows(rows, group, sw, sh)
    end
    if #others > 0 then
        local head = header('Other')
        head.icon, head.tag = 'eye_off', 'Not loaded now'
        rows[#rows + 1] = head
        -- each addon by name, then the saved places whose addon isn't known yet
        table.sort(others, function (a, b)
            if (a.owner == '') ~= (b.owner == '') then
                return b.owner == ''
            end
            return a.title:lower() < b.title:lower()
        end)
        for _, group in ipairs(others) do
            local sub = group.owner == '' and subheader('Saved places', 'Not drawn since the game started') or subheader(group.title)
            sub.tip = group.owner == '' and 'Places kept for windows no addon has drawn since the game started: whose they are is known once one draws'
                      or 'Not loaded now: its windows\' places are kept for when it is'
            rows[#rows + 1] = sub
            layout.window_rows(rows, group, sw, sh)
        end
    end
    return rows
end

-- /vana reset ui: every window on the Layout page (addons' and plugins' ImGui windows, and their text and shapes) moved to
-- the screen's top-left corner, 0,0, to find one gone off the screen (put there on a bigger screen or resolution, or moved by
-- its addon). Those not shown now go there when they next show; scales stay. config\layout.json is copied first, to
-- config\layout-before-reset-<date>-<time>.json, for a layout to have back (with the game closed, rename it layout.json).
function layout.reset_command(what)
    if what == nil or what:lower() ~= 'ui' then
        print('VanaCore: /vana reset ui moves every addon\'s and plugin\'s window to the top-left corner of the screen (0,0).')
        return
    end
    if vana.layout == nil then
        print("VanaCore: this VanaCore can't move addons' windows: update it.")
        return
    end
    local dir = vana.paths.install .. '\\config\\'
    local kept = nil
    local from = io.open(dir .. 'layout.json', 'rb')
    if from ~= nil then
        local text = from:read('*a')
        from:close()
        local name = 'layout-before-reset-' .. os.date('%Y-%m-%d-%H%M%S') .. '.json'
        local to = io.open(dir .. name, 'wb')
        local ok = to ~= nil and to:write(text) ~= nil
        if to ~= nil then
            ok = to:close() and ok
        end
        kept = ok and name or nil
        if kept == nil then
            print('VanaCore: config\\layout.json couldn\'t be copied, so nothing was moved.')
            return
        end
    end
    local count = 0
    for _, w in ipairs(vana.layout.windows()) do
        if w.owner ~= 'vanacore/vana' then -- this window's own (its colour and number boxes)
            vana.layout.set(w.name, 0, 0)
            count = count + 1
        end
    end
    if count == 0 then
        print('VanaCore: no addon or plugin has a window to move.')
        return
    end
    print('VanaCore: ' .. count .. (count == 1 and ' window' or ' windows') .. ' moved to the top-left corner (0,0). Drag '
          .. (count == 1 and 'it' or 'them') .. ' back, or place ' .. (count == 1 and 'it' or 'them') .. ' on /vana layout.'
          .. (kept ~= nil and ' The layout before is in config\\' .. kept .. '.' or ''))
end

-- A Chat window row that takes the Windows panel's values while "Match Game Windows" is on: greyed and locked then
local function matched(row)
    row.lock_if = function ()
        local text = vana.config.get('client', 'chat.match')
        return text ~= nil and (tonumber(text) or 0) ~= 0
    end
    return row
end

-- A Chat input row, greyed and locked while the line matches the game's windows or the chat's ([client] input.match)
local function input_own(row)
    row.lock_if = function () return (vana.config.get('client', 'input.match') or 'windows'):lower() ~= 'off' end
    return row
end

-- A Chat position row, greyed and locked while its window's Move switch ([client] chat.move, or `switch`) is off
local function moved(row, switch)
    row.lock_if = function ()
        return (tonumber(vana.config.get('client', switch or 'chat.move') or '') or 0) == 0
    end
    -- the window follows the slider as it's dragged: only for the places chat.preview knows (the others, items.count and
    -- status, have no preview, so they're written to vanacore.ini as it's dragged and the game reloads them)
    local key = row.key or ''
    local previewable = key:match('^chat2?%.') or key:match('^compass%.') or key:match('^clock%.') or key:match('^menus%.') or key:match('^input%.')
    if previewable and vana.chat ~= nil and vana.chat.preview ~= nil then
        row.preview = function (value) vana.chat.preview(row.key, value) end
    end
    return row
end

-- A chat window's place on one row, drawn as the Layout page's windows are (vana.lua, kind 'layout'): its name, its edges'
-- sliders along the row, then its Move switch ([client] `key`), which locks the sliders while it's off. The panel's reset
-- puts the edges back and the switch off, so a reset pins the window back where the game puts it (row.reset, not
-- row.switch; asked 2026-10-23); a preset keeps the switch and the edges (row.parts).
-- `outline` (vana.layout.outline's name): an eye after the label shows where the window goes, as a box in the game
local function mover(label, key, tip, sliders, outline)
    -- (slots and slot_label: the columns line up from row to row, the input line's two sliders over the windows' X and Y)
    -- (place: a window's place)
    local row = { kind = 'layout', label = label, tip = tip, sliders = sliders, title_w = 84, slots = 3, slot_label = 'Width', label_gap = 9, parts = {}, place = true,
                  toggle = { get = function () return (tonumber(vana.config.get('client', key) or '') or 0) ~= 0 end,
                             set = function (on) vana.config.set('client', key, on and '1' or '0') end } }
    row.outline = outline
    if outline ~= nil then
        row.title_w = 106 -- (room for the eye after the longest name, Second chat)
    end
    for _, sub in ipairs(sliders) do
        moved(sub, key).value_w_base = 34
        sub.format = function (value) return string.format('%g%%', math.floor(value * 2 + 0.5) / 2) end -- 100%, 0.5%
        sub.no_reset = true -- (the panel's reset puts them back: no room for one each)
        row.parts[#row.parts + 1] = sub
    end
    row.parts[#row.parts + 1] = { section = 'client', key = key }
    row.reset = function ()
        for _, sub in ipairs(sliders) do
            vana.config.set(sub.section, sub.key, nil)
        end
        vana.config.set('client', key, nil)
    end
    return row
end

-- Sliders on one row (a nudge's X and Y), drawn as a mover is but with no switch; each keeps its own reset
local function pair(label, tip, sliders)
    local row = { kind = 'layout', label = label, tip = tip, sliders = sliders, title_w = 84, slots = #sliders, label_gap = 7, parts = sliders }
    row.reset = function ()
        for _, sub in ipairs(sliders) do
            vana.config.set(sub.section, sub.key, nil)
        end
    end
    return row
end

-- Game Options > Buttons: tabs across it (vana.lua's kind 'tabs'), each the menus' buttons' rows of one set of [client] keys
-- (buttons.cpp): Global, buttons.*; Main Menu, mainmenu.buttons.* (the main menu and the menus it opens in its place);
-- Action Menu, action.buttons.* (the Action Menu and its Magic and Abilities lists). Their labels' True Font: buttons.font
-- (the titles' pinned to it), mainmenu.font and action.font. Main Menu and Action Menu each have an Enable ([client]
-- mainmenu.buttons, action.buttons, as buttons.cpp and chatfont.cpp key_for read it): on, their own keys alone, Global's
-- changes never reaching them (switched on, the keys they haven't set take Global's as they are then, so they start as
-- Global looks); off, Global's, their rows locked and showing Global's. Never switched, each key Global's until its own is
-- set (as before the switch: what was set for the main menu's font stays). A tab's rows carry row.tab_of (its bar, its
-- place, and its name as a sub-heading for a search: vana.lua search_results)
local function button_tabs()
    -- every key of a set (after its prefix), and of a True Font area (after its prefix): what Enable copies from Global's
    local BUTTON_KEYS = { 'style', 'color', 'selected.color', 'text', 'selected.text', 'border', 'selected.border', 'opacity',
                          'border.size', 'padding.x', 'padding.y', 'spacing', 'text.x', 'text.y', 'selected.nudge', 'selected.flash' }
    local FONT_KEYS = { '', '.family', '.size', '.bold', '.italic', '.uppercase', '.outline', '.outline.opacity' }
    local bar = { kind = 'tabs', id = 'buttons', source = 'tabs', labels = {} }
    local rows = { bar }
    for index, tab in ipairs({ { 'Global', 'buttons', 'buttons.font' }, { 'Main Menu', 'mainmenu.buttons', 'mainmenu.font' },
                               { 'Action Menu', 'action.buttons', 'action.font' } }) do
        local name, prefix, font = tab[1], tab[2], tab[3]
        local own = prefix ~= 'buttons'
        local function key(k) return prefix .. '.' .. k end
        local function enabled() return not own or (vana.config.get('client', prefix) or '') == '1' end
        -- a row of an own tab: its own key while on (unset, its default), Global's (`from`'s) shown while off
        local function mirror(part, mine, from)
            if not own then
                return part
            end
            local own_key, theirs, plain = part.key, from .. part.key:sub(#mine + 1), {}
            for k, v in pairs(part) do
                plain[k] = v
            end
            plain.get = nil
            part.get = function ()
                plain.key = enabled() and own_key or theirs
                return M.read(plain)
            end
            part.follows = nil -- (its reset puts its default back)
            return part
        end
        local function button(row) return mirror(row, prefix, 'buttons') end
        -- locked while off (Global's in use), and by its own lock as well
        local function lock(row)
            local before = row.lock_if
            if own then
                row.lock_if = function () return not enabled() or (before ~= nil and before()) end
            end
            return row
        end
        -- the flat style's rows (and their sliders) greyed and locked with the game's, as VanaCore reads the style
        local function game()
            return (vana.config.get('client', enabled() and key('style') or 'buttons.style') or ''):lower() ~= 'flat'
        end
        local function flat_only(row)
            row.lock_if = game
            for _, sub in ipairs(row.sliders or {}) do
                sub.lock_if = game
            end
            return lock(row)
        end
        local group = {}
        if own then
            local enable = toggle('Enable', 'client', prefix, false)
            enable.get = enabled
            enable.set = function (on)
                if on then
                    local function copy(mine, theirs)
                        local value = vana.config.get('client', theirs) or ''
                        if (vana.config.get('client', mine) or '') == '' and value ~= '' then
                            vana.config.set('client', mine, value)
                        end
                    end
                    for _, k in ipairs(BUTTON_KEYS) do
                        copy(key(k), 'buttons.' .. k)
                    end
                    for _, k in ipairs(FONT_KEYS) do
                        copy(font .. k, 'buttons.font' .. k)
                    end
                end
                vana.config.set('client', prefix, on and '1' or '0')
            end
            enable.rebuild = true -- the rows below lock and unlock by it
            enable.colour = M.P.green -- (light green, name and switch: plain to see, asked for 2026-11-07)
            group[#group + 1] = enable
        end
        local style = lock(button(choice('Style', 'client', key('style'), 'game', { { 'game', 'Game' }, { 'flat', 'Flat' } })))
        style.rebuild = true -- the flat style's rows lock and unlock by it
        group[#group + 1] = style
        -- its labels' letters and True Font: Global's buttons.font (the titles pinned to it); an own tab's area its own keys
        -- while on, buttons.font's shown while off
        if not own then
            group[#group + 1] = area_font(font, 11, true, 'titles.font')
        else
            local function area() return enabled() and font or 'buttons.font' end
            local line = area_font(font, 11, 'buttons.font')
            for _, part in ipairs(line.parts) do
                mirror(part, font, 'buttons.font')
            end
            -- (its switch shows Global's while the tab's off, and the line's locked then; on, its own, Global's while unset, as
            -- chatfont.cpp read_want has it: not General's, which left the main menu the game's letters, reported 2026-11-10)
            line.switch.get = function ()
                local a = area()
                return area_sharp(a, a == font and 'buttons.font' or true)
            end
            line.lock_if = function () return not enabled() end
            local outline = line.outline -- (the Default Font's while the area's isn't set)
            outline.get = function ()
                local value = tonumber(vana.config.get('client', area() .. '.outline') or '')
                return within(outline, value or tonumber(vana.config.get('client', 'truefont.outline') or '') or 0)
            end
            outline.follows = nil
            group[#group + 1] = line
        end
        for _, row in ipairs({
            lock(pair('Colour', nil, { button(colour_pick('Buttons', 'client', key('color'), '#4B65A0')),
                                       button(colour_pick('Selected', 'client', key('selected.color'), '#4B4845')) })),
            lock(pair('Text', nil, { button(colour_pick('Buttons', 'client', key('text'), '#FFFFFF')),
                                     button(colour_pick('Selected', 'client', key('selected.text'), '#FFA040')) })),
            flat_only(pair('Border', nil, { button(colour_pick('Buttons', 'client', key('border'), '#8A8FA6')),
                                            button(colour_pick('Selected', 'client', key('selected.border'), '#8A8FA6')) })),
            flat_only(pair('Box', nil, { button(slider('Opacity', 'client', key('opacity'), 100, 0, 100, 1, '%d%%')),
                                         button(slider('Border size', 'client', key('border.size'), 1, 0, 4, 1, '%d px')) })),
            flat_only(pair('Padding', nil, { button(slider('Across', 'client', key('padding.x'), 0, -16, 16, 1, '%d px')),
                                             button(slider('Down', 'client', key('padding.y'), 0, -8, 8, 1, '%d px')) })),
            flat_only(button(slider('Spacing', 'client', key('spacing'), 0, 0, 16, 1, '%d px'))),
            flat_only(pair('Text position', nil, { button(slider('Across', 'client', key('text.x'), 0, -16, 16, 1, '%d px')),
                                                   button(slider('Down', 'client', key('text.y'), 0, -8, 8, 1, '%d px')) })),
            lock(button(toggle('Selected button moves', 'client', key('selected.nudge'), true))),
            lock(button(toggle('Flash when picked', 'client', key('selected.flash'), true))),
        }) do
            group[#group + 1] = row
        end
        if prefix == 'mainmenu.buttons' then -- (the main menu's labels alone, whether the tab's Enable is on or not)
            group[#group + 1] = toggle('Long labels full size', 'client', 'mainmenu.font.fixed', true)
        end
        bar.labels[index] = name
        local tab_of = { id = bar.id, index = index, head = subheader(name .. ' Buttons') }
        for _, row in ipairs(group) do
            row.tab_of = tab_of
            rows[#rows + 1] = row
        end
    end
    return rows
end
-- Game Options > Sliders: the Config windows' sliders ([client] sliders.*, sliders.cpp); the flat style's rows locked with the game's
local function slider_rows()
    local function game() return (vana.config.get('client', 'sliders.style') or ''):lower() ~= 'flat' end
    local function flat_only(row)
        row.lock_if = game
        for _, part in ipairs(row.parts or {}) do
            part.lock_if = game
        end
        return row
    end
    local style = choice('Style', 'client', 'sliders.style', 'game', { { 'game', 'Game' }, { 'flat', 'Flat' } })
    style.rebuild = true -- the flat style's rows lock and unlock by it
    local marks = { { 'none', 'None' }, { 'round', 'Round' }, { 'square', 'Square' }, { 'bar', 'Line' } }
    return {
        style,
        flat_only(colours_line('Colours', nil, colour_pick('Bar', 'client', 'sliders.track.color', '#3A4266'),
                               colour_pick('Fill', 'client', 'sliders.fill.color', '#8C9EFF'),
                               colour_pick('Selected', 'client', 'sliders.selected.color', '#8C9EFF'))),
        flat_only(pair('Bar', nil, { slider('Height', 'client', 'sliders.height', 4, 1, 16, 1, '%d px'),
                                     slider('Opacity', 'client', 'sliders.track.opacity', 100, 0, 100, 1, '%d%%') })),
        flat_only(toggle('Round bar ends', 'client', 'sliders.round', true)),
        flat_only(choice('Handle', 'client', 'sliders.knob', 'round', marks)),
        flat_only(slider('Handle size', 'client', 'sliders.knob.size', 10, 2, 24, 1, '%d px')),
        flat_only(colour_pick('Handle colour', 'client', 'sliders.knob.color', '#FFFFFF')),
        flat_only(choice('Ends', 'client', 'sliders.ends', 'none',
                         { { 'none', 'None' }, { 'game', 'Game' }, { 'round', 'Round' }, { 'square', 'Square' }, { 'bar', 'Line' } })),
        flat_only(slider('Ends size', 'client', 'sliders.ends.size', 6, 2, 16, 1, '%d px')),
        flat_only(colour_pick('Ends colour', 'client', 'sliders.ends.color', '#8A8FA6')),
    }
end
-- An Equipment Squares row (and its sliders), greyed and locked while Own squares ([client] items.equip) is off
local function equip_own(row)
    local function off() return (tonumber(vana.config.get('client', 'items.equip') or '') or 0) == 0 end
    row.lock_if = off
    for _, sub in ipairs(row.sliders or {}) do
        sub.lock_if = off
    end
    return row
end

-- Where the compass or menus go on one row, as a mover (name, eye, X and Y): one setting over two switches at the row's end,
-- Game (the game's place), Chat (on the top chat window, [client] `follow`) or Custom (X and Y, [client] `move`, which wins
-- over following the chat); X and Y lock unless it's Custom
local function placed(label, follow, move, tip, sliders, outline)
    local row = mover(label, move, tip, sliders, outline)
    row.toggle = nil
    row.slots = 2 -- (no Width)
    local function on(key) return (tonumber(vana.config.get('client', key) or '') or 0) ~= 0 end
    row.modes = { labels = { 'Game', 'Chat', 'Custom' },
                  tips = { 'Pinned to the chat, where the game puts it.',
                           'Pinned to the chat, going with its left edge.',
                           'Moved: at its X and Y, apart from the chat. Dragging its box in the game picks this; '
                               .. 'Game or Chat pins it back.' },
                  get = function () return on(move) and 3 or on(follow) and 2 or 1 end,
                  set = function (index)
                      vana.config.set('client', move, index == 3 and '1' or '0')
                      vana.config.set('client', follow, index == 2 and '1' or '0')
                  end }
    row.parts[#row.parts + 1] = { section = 'client', key = follow }
    local reset = row.reset -- (and Game: pinned where the game puts it)
    row.reset = function ()
        reset()
        vana.config.set('client', follow, nil)
    end
    return row
end

-- [camera] zoom: on as this window writes it (1), or as vanacore.ini documents it (distance)
local function true_zoom()
    local text = (vana.config.get('camera', 'zoom') or ''):lower()
    return text == '1' or text == 'on' or text == 'distance'
end

-- A Camera row for True Zoom alone (without it the game's zoom is the field of view): greyed and locked while it's off
local function zoomed(row)
    row.lock_if = function () return not true_zoom() end
    if vana.camera ~= nil and vana.camera.preview ~= nil then -- a slider: shown as it's dragged, without vanacore.ini
        row.preview = function (value) vana.camera.preview(row.key, value) end
    end
    return row
end

-- A vanacore.ini switch as VanaCore reads it: a number, 0 off; `default` when it isn't set
local function ini_on(section, key, default)
    local number = tonumber(vana.config.get(section, key) or '')
    if number == nil then
        return default
    end
    return number ~= 0
end

-- A Developer > Logging row for what writes to VanaCore's log: greyed and locked while Enable logs is off ([logging]
-- enabled, on unless it says 0). `costs`: it costs the game something even with logs off (hooks, Windows' DRED), so it
-- stays free to switch off; `d3d12`: it only does anything with renderer=d3d12, and is locked off without it
local function logged(row, costs, d3d12)
    row.lock_if = function ()
        if costs and ini_on(row.section, row.key, false) then
            return false
        end
        return not ini_on('logging', 'enabled', true)
            or d3d12 == true and (vana.config.get('graphics', 'renderer') or ''):lower() ~= 'd3d12'
    end
    return row
end

-- What each setting does, in a line or two, shown when the mouse rests on its name: 'section.key' -> text. A row that
-- sets its own `tip` keeps it. Panel titles (TIPS.panels) are keyed by their text. (One table: M.rebuild() is at LuaJIT's upvalue limit.)
local TIPS = {}
TIPS.rows = {
    -- Client: Party Lists, Macro Bar
    ['client.alliance.macrobar'] = 'The alliance menus no longer hide while the macro bar is visible.',
    ['client.party.tpbar'] = 'On a job with no spells, the party list shows a blue TP bar where the MP bar would be.',
    ['client.party.tp'] = 'Every member gets a blue TP bar and TP number under their name, beside the MP bar. Fills at 3000 TP, brighter from 1000.',
    ['client.blink.disable'] = 'Your character no longer vanishes for a moment when its gear changes. Its look stays as it was until you turn this off or zone. Changing a main or sub weapon still blinks it unless Disable Weapon Blink is on too.',
    ['client.blink.weapons'] = 'With Disable Character Blink on, a change of your main or sub weapon no longer blinks you either.',
    ['client.party.hide'] = 'Hides the party list. Your party is still there; only the list is not drawn. The alliance lists have their own switches.',
    ['client.alliance1.hide'] = 'Hides the first alliance list (the second party of an alliance).',
    ['client.alliance2.hide'] = 'Hides the second alliance list (the third party of an alliance).',
    ['client.party.background'] = 'Off: no frame or title behind the party list; the names and bars stay.',
    ['client.alliance1.background'] = 'Off: no frame behind the first alliance list; the names and numbers stay.',
    ['client.alliance2.background'] = 'Off: no frame behind the second alliance list; the names and numbers stay.',
    ['client.macrobar.hide'] = 'The macro bar is never drawn, but Ctrl / Alt + a number still runs the macro.',
    ['client.macrobar.mode'] = 'Game: what the game does (a tap toggles the bar, a hold shows it after a moment).\n'
        .. 'Toggle: pressing Ctrl, Alt or a trigger shows the bar, and it stays up until the key is pressed again, Esc or Alt+Tab.\n'
        .. 'Hold: the bar shows while the key is held, at once, and goes when it is let go. (Credit: macrofix)',
    ['client.zonename'] = 'When you zone in, the zone\'s name shows in the middle of the screen, fades in, stays a moment and fades away.\n'
        .. 'It waits for the zone to load. Going into your Mog House shows nothing; leaving it does.',
    ['client.zonename.login'] = 'Shows the zone\'s name and the region\'s when you log in too.',
    ['client.zonename.region'] = 'When you zone in, the region the zone is in (Ronfaure, Zulkheim, ...) shows above the zone\'s name, with the same animation.\n'
        .. 'It has its own font, size, colours and place. A zone in no region shows none.',
    ['client.zonename.region.family'] = 'The font the region\'s name is drawn in. One Windows has, or one in the fonts folder.',
    ['client.zonename.region.italic'] = 'Slants the letters.',
    ['client.zonename.region.bold'] = 'Makes the letters heavy.',
    ['client.zonename.region.uppercase'] = 'Writes the region\'s name in capitals.',
    ['client.zonename.region.outline'] = 'How thick the outline round the letters is. 0 has none.',
    ['client.zonename.region.color'] = 'The colour of the letters.',
    ['client.zonename.region.gradient'] = 'The name shades from the first colour to the second. Gradient direction says which way.',
    ['client.zonename.region.gradient.direction'] = 'Which way the gradient runs. Left to right: each letter its own colour, from the first colour at the first letter to the second at the last.\n'
        .. 'Top to bottom: the whole name shades from the first colour at the top of the letters to the second at their bottom (needs the latest VanaCore.Core.dll; an older one runs left to right).',
    ['client.zonename.region.color2'] = 'The colour the last letter ends on, with Gradient on.',
    ['client.zonename.region.outline.color'] = 'The colour of the outline round the letters.',
    ['client.zonename.region.fontsize'] = 'How big the letters are, in pixels at the interface\'s size.',
    ['client.zonename.region.x'] = 'How far across the screen the middle of the region\'s name sits: 0% is the left edge, 100% the right.',
    ['client.zonename.region.y'] = 'How far down the screen the middle of the region\'s name sits: 0% is the top, 100% the bottom.',
    ['client.zonename.family'] = 'The font the name is drawn in. One Windows has, or one in the fonts folder.',
    ['client.zonename.italic'] = 'Slants the letters.',
    ['client.zonename.bold'] = 'Makes the letters heavy.',
    ['client.zonename.uppercase'] = 'Writes the name in capitals.',
    ['client.zonename.animation'] = 'How the name comes and goes.\nFade: it fades in and out.\nScale: it grows from half its size and shrinks away.\n'
        .. 'Slide: it comes in from a side and leaves toward another.\nWave in-out: a ripple runs through the whole name as it comes and goes, and it is still while it stays.\n'
        .. 'Wave - Continuous: the ripple carries on, gently, while it stays.\n'
        .. 'Dissolve: the letters are eaten away in a noise, with a burning edge, and come back the same way.',
    ['client.zonename.wavespeed'] = 'How fast the ripple runs along the name with either Wave. 1 is the usual; 2 is twice as fast.',
    ['client.zonename.dissolve.burn'] = 'Lights the front of the dissolve: the letters about to go glow in the edge colour, white-hot at the very front.',
    ['client.zonename.dissolve.color'] = 'The colour of the burning edge with Dissolve.',
    ['client.zonename.slide'] = 'Which side the name comes in from with Slide.',
    ['client.zonename.slide.to'] = 'Which side the name leaves toward with Slide. Opposite is the side across from where it came in.',
    ['client.zonename.outline'] = 'How thick the outline round the letters is. 0 has none.',
    ['client.zonename.color'] = 'The colour of the letters.',
    ['client.zonename.gradient'] = 'The name shades from the first colour to the second. Gradient direction says which way.',
    ['client.zonename.gradient.direction'] = 'Which way the gradient runs. Left to right: each letter its own colour, from the first colour at the first letter to the second at the last.\n'
        .. 'Top to bottom: the whole name shades from the first colour at the top of the letters to the second at their bottom (needs the latest VanaCore.Core.dll; an older one runs left to right).',
    ['client.zonename.color2'] = 'The colour the last letter ends on, with Gradient on.',
    ['client.zonename.outline.color'] = 'The colour of the outline round the letters.',
    ['client.zonename.fontsize'] = 'How big the letters are, in pixels at the interface\'s size.',
    ['client.zonename.x'] = 'How far across the screen the middle of the name sits: 0% is the left edge, 100% the right.',
    ['client.zonename.y'] = 'How far down the screen the middle of the name sits: 0% is the top, 100% the bottom.',
    ['client.zonename.seconds'] = 'How long the name stays at full strength, between its fade in and its fade out.',
    ['client.macronames.extended'] ='Macro book and equip set names take spaces and all symbols, not only letters and digits,\n'
        .. 'and their lists show more of each name: the row\'s whole width.\n'
        .. 'A name is still 15 characters at most; the game keeps no room for more.\n'
        .. 'With True Font off for menus, the lists draw only a name\'s letters and digits.',
    ['client.macrobar.nodelay'] = 'Game: Ctrl and Alt show the bar at once instead of after the game\'s wait.\n'
        .. 'The controller\'s triggers stay as the game has them. Toggle and Hold have no wait already.',
    ['client.macrobar.x'] = 'Where the left edge of the macro bar is, as a percent of the screen.',
    ['client.macrobar.y'] = 'Where the top of the macro bar is, as a percent of the screen.',
    -- Client: Game
    ['ffxi.mode'] = 'How the game window fits your screen.\n'
        .. 'Full screen: the game takes over the screen. Slower to Alt+Tab out of.\n'
        .. 'Windowed: a normal window with a title bar, that you can move.\n'
        .. 'Borderless windowed: a window with no border or title bar.\n'
        .. 'Borderless fullscreen: fills the screen, and Alt+Tab is instant.',
    ['ffxi.resolution.game'] = 'How big the game draws. In a window it changes at once.',
    ['ffxi.resolution.ui'] = 'How big the menus and text are drawn: smaller makes them bigger. Changes at once.\n'
        .. 'Fit the game window: the game window\'s inside (no title bar or borders), at the UI aspect ratio.\n'
        .. 'It follows the window as it changes. 75% and 50% make the menus 1.33x and 2x bigger.\n'
        .. '% of the screen: a share of your monitor, whatever the window.',
    ['ui.aspect'] = 'The shape the menus and text keep when the window is another shape. Changes at once.\n'
        .. 'Game\'s: the game resolution\'s shape, centred.\n'
        .. 'Fill the window: stretched to the window\'s shape.\n'
        .. '5:4 to 48:9: always that shape, centred.\n'
        .. 'Works with UI resolution on Fit the game window (or a size of that shape).',
    ['window.resizable'] = 'Lets you drag the window edges to resize it.',
    ['window.position'] = 'Where the game window sits on your screen.\nDefault: where the game puts it.\nRemembered: where you last left it.\nCentered: the middle of the screen.',
    ['window.monitor'] = 'Moves the game window to another monitor.\nScreen-sized resolutions change to fit it.',
    ['ffxi.gamepad.guid'] = 'Which controller the game uses.\n'
        .. 'Disable: no controller works, and the game stops looking for them.\n'
        .. 'Automatic: the game picks one itself.\n'
        .. 'Or pick one of your connected controllers by name.',
    ['addons.gamepad'] = 'Lets an addon\'s window (Party Finder, the Auction House) be driven with the pad while it is open:\n'
        .. 'the D-pad moves between things, Confirm presses, Cancel backs out, and the game does not see the pad meanwhile.\n'
        .. 'Made for Xbox-style XInput controllers; a DirectInput pad may not line up. One switch for every addon.',
    ['gamepads.camera'] = 'Which stick turns the camera. Match it to your controller.\n'
        .. 'Xbox: Xbox and other XInput controllers.\n'
        .. 'DualSense / DS4: PlayStation controllers.\n'
        .. 'Custom: something else is set, such as in the game\'s gamepad config tool.',
    ['input.fishing.dpad'] = 'With a fish on the line, the D-pad\'s left and right fight it as the stick does.\n'
        .. 'Off: only the movement controls do, as in the game.',
    ['client.fps.max'] = 'The most frames per second the game will draw.\n'
        .. 'Unlocked: no limit (V-Sync holds it to your screen). Above 60 FPS the game\'s animations and effects run fast.\n'
        .. 'It counts the frames before frame generation: 60 FPS with 2x shows 120.',
    ['client.fps.cutscene'] = 'The most frames per second during a cutscene, in place of Max FPS (back to it when the cutscene ends).\n'
        .. 'Unlocked: no limit, so cutscenes play faster. Also applies while you talk to an NPC. /vana fps cutscene sets any rate.',
    ['client.fps.background'] = 'The frame rate while another window is in front.\n'
        .. 'Reduced FPS: 30 frames a second, so the game takes less of your PC while you do something else.',
    ['client.framepacing'] = 'At 60 FPS, keeps characters\' motion and weapon skills, spells and effects at real speed when the frame rate drops, instead of slowing down.',
    ['client.packetflow'] = 'Sends to the server as soon as it has answered, instead of waiting out the game\'s own delay. Zoning is much faster. What the PacketFlow plugin did.',
    ['client.simplelog.fix'] = 'Stops SimpleLog throwing away spells, weapon skills and abilities it wrongly takes for repeats, which left them with no animation and no chat line. Packets the server really sends twice are still caught.',
    ['client.packets.keepactions'] = 'When an addon blocks a spell, weapon skill or ability from the server, the game still gets it, so its animation always plays. Its chat line stays hidden, as the addon wanted.',
    ['client.sound.follow'] = 'Moves the game\'s sound to the new output when Windows\' default output changes, or a headset resets, instead of staying silent until the game is restarted.',
    ['client.packets.watch'] = 'Writes what the server\'s packets did on their way in to vanacore.log (Logs must be on): buffers the server sent that never came, your own weapon skills and spells as they arrive, and packets an addon or plugin blocked. For a skill that shows nothing.',
    ['graphics.vsync'] = 'Waits for the screen to refresh. Stops tearing, can add a little delay.',
    ['graphics.vsync.cutscene'] = 'Off: V-Sync turns off during a cutscene and back on when it ends. With Cutscene FPS Unlocked, cutscenes play as fast as your PC draws them.\n'
        .. 'Also applies while you talk to an NPC.',
    ['window.topmost'] = 'Keeps the game window above every other window.',
    ['window.keepawake'] = 'Stops the screen saver, display sleep and the lock screen while the game runs.',
    ['window.close.shutdown'] = 'The window\'s X (and Alt+F4) runs /shutdown while you are in the game. Close it again, close it three times in a row, or lose the server, to close the game at once.',
    ['window.mouse.scale'] = 'Keeps the mouse lined up when the window is a different size from the game.',
    ['window.dpiaware'] = 'Draws at your screen\'s real pixels so a scaled display (125%) is not blurry. Windows are smaller. Needs a restart.',
    ['graphics.aspect'] = 'The shape of the 3D world.\n'
        .. 'Game\'s: as the game draws it, so it can look stretched.\n'
        .. 'Fit the window: matches the window\'s shape, with no stretching.\n'
        .. '5:4 to 48:9: always that shape, whatever the window.',
    -- Client: Game > Frame rate, the FPS counter
    ['client.fps.display'] = 'Shows the frames per second on screen.',
    ['client.fps.position'] = 'Which corner of the screen the counter sits in.',
    ['client.fps.padding'] = 'The gap between the counter and the screen edge.',
    ['client.font.family'] = 'The FPS counter\'s font, and the VanaCore label\'s.',
    ['client.font.size'] = 'How big the counter\'s and the VanaCore label\'s text is.',
    ['client.font.outline'] = 'A dark edge round the text so it is easier to read.',
    -- Client: VanaCore
    ['screenshots.overlays'] = 'Includes VanaCore\'s text and windows in screenshots. Needs a restart.',
    ['screenshots.clipboard'] = 'Each screenshot is also put on the clipboard, ready to paste into Discord or an image editor. '
        .. 'The chat log says so.',
    ['client.attached'] = "VanaCore's console window (the one that starts the game) stays open while you play, showing VanaCore's log.\n"
        .. 'Closing it closes the game, and the other way round. Off, it closes once the game is up. Needs a restart.',
    ['client.ini.upstream'] = "Off: the game reads its settings file as it starts and only writes it after that. On: edits made to "
        .. 'the file by anything else (a hand edit, the launcher) apply while the game runs.',
    ['client.autoclose'] = 'With the console window kept open, it closes by itself when the game ends instead of waiting for a key.\n'
        .. 'After a crash or an error it still waits, so it can be read. Needs a restart.',
    ['client.dep'] = 'Turns Data Execution Prevention on as the game starts. Needs a restart.\n'
        .. 'On a Mac under CrossOver this stops the first three minutes of every start running at 1-2 FPS (the logos and\n'
        .. 'the agreement screen); it is what Auto does there. On Windows it changes nothing (DEP is already on).\n'
        .. 'Off elsewhere: on a Steam Deck it stopped PlayOnline from starting at all. Auto: on only when the game runs\n'
        .. 'under Wine on macOS.',
    ['input.keyboard.silentbinds'] = 'No "Bound <key> to ..." line in the chat log for each /bind and /unbind.',
    ['input.keyboard.hookthread'] = 'Which thread the game\'s low-level keyboard hook runs on. Needs a restart.\n'
        .. 'Own thread: typing in other programs does not wait on the game\'s frames.\n'
        .. 'Game\'s thread: as the game sets it.\n'
        .. 'Auto: its own thread, except under Wine / Proton, where it is the game\'s.',
    ['client.imgui.font'] = "The font the text in addons' and plugins' ImGui windows draws in. Needs a restart.\n"
        .. "Agave is Ashita's own. The others are the font files in VanaCore's fonts folder and in Windows' fonts.\n"
        .. 'A full path to a .ttf, .otf or .ttc file works too, set in vanacore.ini ([client] imgui.font).',
    ['addons.silent'] = 'No "Loaded:" line in the chat log as an addon loads or unloads. Errors still show.',
    ['addons.plugins.silent'] = 'No "Loaded:" line in the chat log as a plugin loads or unloads. Errors still show.',
    ['discord.enabled'] = 'Shows your character, jobs and zone on your Discord profile. Needs the Discord app running.',
    ['discord.name'] = 'Puts your character\'s name in the Discord status. Off: the jobs and the zone only.',
    -- Client: Developer
    ['logging.enabled'] = 'Writes VanaCore\'s log file. Useful when reporting a bug.',
    ['logging.crashdumps'] = 'What a crash leaves behind for debugging.\n'
        .. 'Off: one line in the log.\n'
        .. 'Normal: a full report in the log and a 15-40 MB .dmp file. Enough for a bug report.\n'
        .. 'Full memory: as Normal, plus all of the game\'s memory (hundreds of MB).',
    ['logging.frametimes'] = 'Logs where each frame\'s time goes. For finding lag.',
    ['logging.gpu'] = 'DirectX 12: logs what the graphics card was doing if it crashes. A little slower.',
    ['logging.mouse'] = 'Logs the mouse and cursor, for mouse bugs. Stops after 3000 lines.',
    ['pivot.log'] = 'Logs every file Pivot serves from an overlay.',
    ['mcp.enabled'] = 'Lets an AI app on this PC look inside the game. Leave off unless you use one.',
    -- Graphics: General
    ['graphics.renderer'] = 'What draws the game. Needs a restart.\n'
        .. 'DirectX 8 (game\'s): the game\'s own. No VanaCore effects.\n'
        .. 'DirectX 9: the game as it is, through DirectX 9. Works with ReShade or DXVK.\n'
        .. 'DirectX 11: VanaCore\'s effects, without DLSS. Try it if DirectX 12 runs badly on your card.\n'
        .. 'DirectX 12: VanaCore\'s effects, DLSS and the fastest frames.',
    ['graphics.antialias'] = 'Smooths jagged edges in the 3D world. Menus and text stay sharp.\n'
        .. 'Off: no smoothing.\n'
        .. 'FXAA: smooths edges, costs very little.',
    ['graphics.background'] = 'The size the 3D world is drawn at, then fitted to the screen. Bigger is sharper and smoother, but slower.',
    -- DLSS
    ['graphics.dlss.quality'] = 'Left: faster, drawn smaller then upscaled. Right: sharper, slower. 100% is native.',
    -- FSR
    ['graphics.fsr.quality'] = 'Left: faster, drawn smaller then upscaled. Right: sharper, slower. 100% is native.',
    ['graphics.fsr.sharpness'] = 'FSR\'s own sharpening of the upscaled picture, 1 to 10. Off is none.',
    ['graphics.fsr.framegen'] = 'Makes an extra frame between the real ones, for smoother motion. Needs FSR on.\n'
        .. 'Off: real frames only.\n'
        .. '2x: one extra frame for each real one. Any DirectX 12 card; AMD\'s AI version on Radeon RX 9000 with Windows 11.',
    ['graphics.fsr.version'] = 'Which FSR runs.\n'
        .. 'Automatic: AMD picks. FSR 4 where your card has it, else FSR 3.1. FSR 1 where neither runs.\n'
        .. 'FSR 4: AMD\'s AI upscaler. Radeon RX 7000 and 9000 only.\n'
        .. 'FSR 3.1: runs on any DirectX 12 card.\n'
        .. 'FSR 1: the lightest. Any card, DirectX 11 too. Softer, no frame generation.',
    ['graphics.dlss.framegen'] = 'Makes extra frames between the real ones, for smoother motion. Needs DLSS on.\n'
        .. 'Off: real frames only.\n'
        .. '2x: one extra frame for each real one. RTX 40 or newer.\n'
        .. '3x: two extra frames for each real one. RTX 50.\n'
        .. '4x: three extra frames for each real one. RTX 50.\n'
        .. 'AMD FSR 2x: AMD\'s, one extra frame for each real one. Any DirectX 12 card, so RTX 20 and 30 too.',
    ['graphics.effects.dlss_preset'] = 'Which DLSS model brings the picture up to full size.\n'
        .. 'Automatic: NVIDIA picks for the quality you set. Fine for most.\n'
        .. 'K: the newest, best quality. Costs the most.\n'
        .. 'M: the newest, made for Performance.\n'
        .. 'J: a newer model.\n'
        .. 'F: an old model. Softer, but the fastest.\n'
        .. 'E: an old model. Softer.',
    ['graphics.effects.dlss_compare'] = 'Shows the picture without DLSS (or FSR) at the same size, to compare.',
    ['graphics.effects.dlss5_style'] = 'The look DLSS 5 gives the picture.\n'
        .. 'Natural: a subtle, true-to-life look.\n'
        .. 'Cinematic: a stronger, film-like look.\n'
        .. 'Default: NVIDIA\'s standard look.',
    ['graphics.dlss5'] = 'NVIDIA\'s AI look for the picture, on top of DLSS. Needs an RTX 40 or 50 and NVIDIA\'s DLSS 5 file '
        .. 'in the dlss folder.',
    ['graphics.effects.dlss5_preset'] = 'Which DLSS 5 model runs.\n'
        .. 'Default: the normal one.\n'
        .. 'Model 1 to 4: other models to try.',
    ['graphics.effects.dlss5_scale'] = 'The size DLSS 5 runs at. Lower costs less GPU time; its light and colour still '
        .. 'reach the whole picture, its fine detail comes out softer. DLSS 5 restarts when this changes.\n'
        .. 'Low: 30%. Medium: 50%. High: 75%. Max: full size.',
    ['graphics.effects.dlss5_intensity'] = 'How strong DLSS 5\'s effect is.',
    ['graphics.effects.dlss5_tone'] = 'How much DLSS 5 changes the colour and brightness.',
    ['graphics.effects.dlss5_structure'] = 'How much DLSS 5 changes shapes and detail.',
    ['graphics.effects.dlss5_skin'] = 'Detail strength on skin only. Auto leaves it to DLSS 5.',
    ['graphics.effects.dlss5_mask'] = 'Lets DLSS 5 work out for itself what to change.',
    ['graphics.effects.dlss5_fx_out'] = 'Spells, weapon skills, flames and glows are left out of DLSS 5 and laid back over its picture '
        .. 'as the game drew them, so it doesn\'t flatten them. The rest of the scene still goes through DLSS 5.',
    ['graphics.effects.dlss5_fx_light'] = 'A glow from spells, flames and glows is laid over what DLSS 5 sees, brighter than white, '
        .. 'as light sources, so it lights the scene around them. The sky is left out.',
    ['graphics.effects.dlss5_fx_gain'] = 'How much brighter than the game draws the effects the glow DLSS 5 sees is.',
    ['graphics.effects.dlss5_fx_actor'] = 'How much of that strength spells, abilities and weapon skills get. Their sprites are big '
        .. 'and bright, so at the flames\' strength they washed the ground out. 0 leaves them out.',
    ['graphics.effects.dlss5_fx_cap'] = 'The brightest the glow lifts a pixel to, times white. A flame\'s glow stays well under it; '
        .. 'a beam of light that fills the screen can\'t be handed at six times white, which DLSS 5 paints as coloured blocks. '
        .. 'Under 1, only what is darker than that is lit: pale places (Sky) are left as they are.',
    ['graphics.effects.dlss5_fx_norm'] = 'How much of the picture the lights may fill before their strength eases down to match: '
        .. 'a beam that fills a hall gives about the light a crystal does, and a crystal keeps its strength. Off: no easing.',
    ['graphics.effects.dlss5_elight_post'] = 'Glow Effects are added after DLSS 5, so DLSS 5 doesn\'t change them.',
    ['graphics.effects.dlss_motion_chars'] = 'Tells DLSS, FSR and frame generation how players, NPCs and monsters move, so a running '
        .. 'character stays sharp. Off, they move with the camera: blurry when running, or flickering with Stability: moving things up.',
    ['graphics.effects.dlss_motion_batch'] = 'Draws the characters\' motion for DLSS and FSR all at once after the scene, not one by one: '
        .. 'less CPU time in a crowd, the same picture. Off: one by one, as before.',
    ['graphics.effects.dlss_reactive'] = 'How much DLSS and FSR trust this frame over earlier ones for spells, glows, maws and other '
        .. 'effects, which can\'t tell them how they move. Higher: no ghosting or smearing, a little softer. 0: off, as before.',
    ['graphics.effects.dlss_reactive_moving'] = 'The same for characters, monsters, trees, grass and boats, which move on their own. '
        .. 'Higher: no smearing or warping as they move, a little softer than the scenery. 0: off, as before.',
    ['graphics.effects.fg_fx_motion'] = 'With frame generation, how faint an effect\'s model can be and still move as itself '
        .. '(a home point crystal), not with the wall behind it. Lower keeps more of it; 0 moves every effect with the scene.',
    ['graphics.effects.dlss_motion_plants'] = 'Also tells DLSS, FSR and frame generation how the zone\'s trees and grass sway. '
        .. 'Off, they move with the scene, which frame generation handles better. On, frame generation tears them.',
    -- DLSS 5's Advanced Settings
    ['graphics.effects.dlss5_rebuild'] = 'DLSS 5 may only take up Look, Intensity, Tone, Structure, Skin structure and Find its own mask '
        .. 'when it starts. On: it restarts a second after you stop changing them, so the change shows. DLSS is off for the few '
        .. 'seconds that takes.',
    ['graphics.effects.dlss5_passes'] = 'How many times the picture goes through DLSS 5 (1x, 2x or 3x), each pass on the one before. More passes '
        .. 'give a stronger look and cost about as much GPU time again each. DLSS 5 restarts when this changes.',
    ['graphics.effects.dlss5_taper'] = 'Passes 2 and 3 run Tone and Structure at this share of the first pass\'s, so dark detail '
        .. 'isn\'t crushed to black. DLSS 5 restarts when this changes.',
    ['graphics.effects.dlss5_mix'] = 'How much of DLSS 5\'s picture you see. 0: the game\'s own picture. 1: DLSS 5\'s. '
        .. 'Above 1 goes further the same way.',
    ['graphics.effects.dlss5_colour'] = 'How much of DLSS 5\'s own colour comes with it. 0 keeps the game\'s colours and takes '
        .. 'only DLSS 5\'s light and shade. 1: DLSS 5\'s colours.',
    ['graphics.effects.dlss5_brighten'] = 'The most DLSS 5 may brighten a pixel, as times its brightness before DLSS 5. Stops bright '
        .. 'lights turning into blocks of colour and pale places (Tu\'Lia) going white. At 1 it lights only from the effects\' '
        .. 'glow, never on its own. Darkening isn\'t limited. Off: no limit.',
    -- Draw distance
    ['graphics.drawdistance.enabled'] = 'On: the distances below apply. Off: the game\'s own.',
    ['graphics.drawdistance'] = 'How far away scenery (buildings, trees, ground) is drawn. 1x is the game\'s.',
    ['graphics.drawdistance.mobs'] = 'How far away players, NPCs and monsters are drawn. 1x is the game\'s.',
    ['graphics.drawdistance.ship'] = 'How far away scenery is drawn on the ferries and the open sea routes, in place of Scenery. 1x is the game\'s.',
    ['graphics.drawdistance.target'] = 'How far away you can target them. As far as drawn reaches NPCs and doors behind walls.',
    ['graphics.drawdistance.full'] = 'Draws every piece of the zone in view out to the Scenery distance, instead of the game\'s '
        .. 'own lists of what can be seen from where you are.\n'
        .. 'Fills in far ground that can be missing in a few places (trees standing on nothing).\n'
        .. 'Costs a lot of frames, most in big zones and cities. Needs Scenery above 1x.',
    ['graphics.drawdistance.fill'] = 'Draws the game\'s own lists of what can be seen from where you are, and the big pieces '
        .. 'they leave out (ground, buildings).\n'
        .. 'Fills in far ground that can be missing in a few places, for far fewer frames than the whole zone.\n'
        .. 'Needs Scenery above 1x; Draw the whole zone in view takes over when on.',
    ['graphics.drawdistance.large_off'] = 'Al\'Taieu keeps the game\'s own Scenery distance. The fog and characters keep yours.\n'
        .. 'The game draws all of it in view at once there, so 5x draws over twice as much: about half the frames.',
    ['graphics.effects.batch'] = 'Draws the zone\'s repeated pieces (the same tree again and again) together, many in one draw.\n'
        .. 'Far fewer draws a frame at long Scenery distances, where the game waits on them. Experimental.',
    ['graphics.lod'] = 'Which meshes buildings and trees use as they get farther away.\n'
        .. 'Low: the game\'s, which swaps to simpler ones with distance.\n'
        .. 'Medium: keeps the detailed ones 4 times farther out.\n'
        .. 'High: keeps the detailed ones 8 times farther out.\n'
        .. 'Max: the detailed ones at any distance.',
    ['graphics.lod.range'] = 'How many times farther than the game the detailed meshes are kept.',
    ['graphics.hide.players'] = 'Players outside your party and alliance are never loaded or drawn: faster in busy places.\n'
        .. 'Someone who asks you to trade shows up.',
    ['graphics.hide.pets'] = 'Pets of players outside your party and alliance are never loaded or drawn.',
    ['graphics.hide.trusts'] = 'Trusts of players outside your party are never loaded or drawn.',
    ['graphics.fog'] = 'Where the distance fog starts.\n'
        .. 'Match distance: moves farther out as you raise the draw distance.\n'
        .. 'Vanilla: stays where the game puts it, even when you see farther.\n'
        .. 'Custom: starts and ends where you set below.',
    ['graphics.bump'] = 'The game\'s own Bump Mapping option. Turning it on shows from the next zone you enter; turning it off is at once.',
    ['graphics.untextured'] = 'Draws the world\'s solid surfaces plain white, so only the light and shadows show. Leaves, fences, effects and water keep their textures.',
    ['graphics.fog.start'] = 'How far away the fog starts.',
    ['graphics.fog.end'] = 'How far away the fog is solid.',
    -- Shadows
    ['graphics.effects.sun'] = 'How dark the shadows are by day. 0 turns them off, night time too.',
    ['graphics.effects.sun_casters'] = 'What throws shadows by day.\n'
        .. 'All: characters and scenery (buildings, trees, cliffs).\n'
        .. 'Characters: players, NPCs and monsters. Scenery already has shading painted on.\n'
        .. 'Environment: scenery only. Characters keep the game\'s own shadows.',
    ['graphics.effects.sun_moon_strength'] = 'How dark the shadows are at night, by the moon\'s light.',
    ['graphics.effects.sun_moon_casters'] = 'What throws shadows at night.\n'
        .. 'All: characters and scenery (buildings, trees, cliffs).\n'
        .. 'Characters: players, NPCs and monsters.\n'
        .. 'Environment: scenery only. Characters keep the game\'s own shadows (Config > Shadows on).',
    ['graphics.effects.sun_plants'] = 'Trees, bushes and grass throw sun shadows.\n'
        .. 'Off saves frames in zones full of plants, such as Kazham. Buildings, cliffs and characters still throw them.',
    ['graphics.effects.sun_plants_still'] = 'A plant\'s shadow keeps the pose it was first seen in, so swaying trees don\'t make it flicker.\n'
        .. 'Off: the shadow sways with the plant.',
    ['graphics.effects.sun_distance'] = 'How far from you shadows are drawn.',
    ['graphics.effects.sun_near'] = 'How far from you shadows stay extra sharp.',
    ['graphics.effects.sun_handoff'] = 'How gradually the sharp shadows near you blend into the softer ones farther out. Higher is smoother as you walk.',
    ['graphics.effects.sun_mid'] = 'Sharper shadows a little way off, out this far from you. 0 is off. Costs about as much as the far shadows.',
    ['graphics.effects.sun_detail'] = 'How sharp nearby shadows are: Low is 512, Medium 1024, High 2048 and Max 4096 pixels across.\n'
        .. 'Higher is sharper and uses more video memory. For sharper still, set sun_detail=8192 in vanacore.ini (256 MB).',
    ['graphics.effects.sun_far_hold'] = 'Distant shadows of things that don\'t walk are redrawn every this many frames. Faster.\n'
        .. '0 redraws them every frame.',
    ['graphics.effects.sun_near_hold'] = 'Nearby shadows of things that don\'t walk are kept while the camera stays put, up to this many frames. Much faster.\n'
        .. '0 redraws them every frame.',
    ['graphics.effects.sun_roll'] = 'Redraws the shadows of things that don\'t walk a bit each frame, not all in one. Smoother. Uses more video memory. Can make shadows strobe on some cards.',
    ['graphics.effects.sun_rings'] = 'Takes fewer samples for shadows farther from the camera, where they cover less of the screen. Saves frames.\n'
        .. 'Past each ring fewer are kept (all 16, then 12, 8, 6). Off takes them all everywhere.',
    ['graphics.effects.sun_ring_spacing'] = 'How far apart the shadow rings are, out from the camera. Wider is smoother in the distance and slower.',
    ['graphics.effects.sun_soft'] = 'Softer shadow edges the farther a shadow falls. Sharp (0) is hard-edged; Low is 0.05, Medium 0.1 and High 0.2.\n'
        .. 'For a value in between, set sun_soft in vanacore.ini.',
    ['graphics.effects.sun_soft_zone'] = 'Blurs the shadows on rock, ground and characters, this far across them, so the hard edges of low-polygon shapes come out smooth. 0 leaves them as sharp as the map draws them.',
    ['graphics.effects.sun_min'] = 'Ignores things this close to a surface, which stops rocks shading themselves.',
    ['graphics.effects.sun_face'] = 'Shades the sides of things that face away from the sun: the underside of a roof or a ledge, the far side of a rock or a wall. 1 shades them fully, 0 leaves them lit.',
    ['graphics.effects.sun_curved'] = 'Curved rock and ground take the sun\'s shadow as the smooth shape the game shades them as, so a low-polygon dome or hillside fades into its shadow along a curve. Off, each polygon turned from the sun goes dark whole: squares and stair-steps on rock. Buildings with flat walls are the same either way.',
    ['graphics.effects.sun_game_shading'] = 'Lets the game\'s own shading take shadows off surfaces it already lights as turned away from its sun. Off, the shadow map alone decides, so no shadow goes missing when the game\'s light and the shadows\' light point different ways (a placed sun, dusk).',
    ['graphics.effects.sun_direct'] = 'How much of the light must be sunlight for shadows to show. Higher fades them under cloud.',
    ['graphics.effects.sun_moon'] = 'Shadows all through the night, by the moon\'s light, with no fading at dusk or dawn. Off, they go at 18:00 and come back with the sun.',
    ['graphics.effects.sun_dusk'] ='Keeps long shadows at dusk and dawn instead of fading them out, brings them in from 05:30 to 06:30, and eases them through 17:30 to 18:30.',
    ['graphics.effects.moghouse'] = 'How much of the sun\'s shadows stay in your Mog House and in zones with no sky.',
    ['graphics.effects.sun_seek'] = 'The zone\'s shadows are drawn from everything round your character every frame, the same whatever the camera sees or how it turns.',
    ['graphics.effects.sun_sharp'] = 'The sharp close-up map round your character. Off, the wide map alone shades everything.',
    ['graphics.effects.sun_free'] = 'Puts the shadows\' light where the two sliders below say, whatever the game\'s sun, the hour or the cloud do. Shadows show at night too. For testing.',
    ['graphics.effects.sun_free_az'] = 'Which way round the horizon the light comes from, in degrees.',
    ['graphics.effects.sun_free_el'] = 'How high over the horizon the light is, in degrees. 90 is straight down.',
    -- Ambient occlusion
    ['graphics.effects.ao'] = 'How dark corners and creases get. Adds depth.',
    ['graphics.effects.radius'] = 'How far round each point it looks for things to shade it.',
    ['graphics.effects.ao_chars'] = 'How much of that shading characters take, against the world. 1 is the same, 0 none, 2 twice as much.',
    ['graphics.effects.ao_soft_chars'] = 'Smooths that shading across a character\'s own body, so its polygons don\'t show as flat patches. 0 is off.',
    ['graphics.effects.ao_quality'] = 'How clean the shading in corners and creases is.\n'
        .. 'Each step up is cleaner and slower. Max is the cleanest here; set ao_quality=4 in vanacore.ini for one step more (32 taps).',
    ['graphics.effects.ao_rings'] = 'Takes fewer samples for that shading farther from the camera, where it covers less of the screen. Saves frames.\n'
        .. 'Past each ring fewer are kept (all, then 75%, 50%, 30%). Off takes them all everywhere.',
    ['graphics.effects.ao_ring_spacing'] = 'How far apart the rings are, out from the camera. Wider is cleaner in the distance and slower.',
    ['graphics.effects.temporal'] = 'How much of the last frame the shading in corners and the sun\'s and small shadows keep.\n'
        .. 'Higher is smoother and steadier but trails behind things that move.\n'
        .. 'Lower is crisper and grainier. 0 shades each frame on its own.',
    -- Reflections
    ['graphics.effects.refl'] = 'How strong reflections are on floors and flat ground.',
    ['graphics.effects.refl_wet'] = 'How wet and shiny the ground looks.',
    ['graphics.effects.refl_rough'] = 'How blurry the reflections are.',
    ['graphics.effects.refl_dist'] = 'How far away reflections are drawn.',
    ['graphics.effects.refl_sun'] = 'The bright sparkle of the sun on the ground.',
    ['graphics.effects.refl_bump'] = 'How much the stone\'s own texture bends the reflections: grooves and tile edges give them depth.',
    ['graphics.effects.refl_lift'] = 'Tilts the reflections up where the camera is low, so the floor shows more than the bottom of the walls. 0 is a true mirror.',
    ['graphics.effects.refl_floors'] = 'Which ground reflects, by the zone\'s collision (the ground the footsteps know).\n'
        .. 'Stone and metal: not dirt, grass, sand or snow.\n'
        .. 'Anything built: wood and props too.\n'
        .. 'Any: everywhere not green.',
    ['graphics.effects.refl_quality'] = 'How clean the reflections are.\n'
        .. 'Low to Max: each step is smoother and slower. For one step more, set refl_quality=4 in vanacore.ini.',
    -- Glow effects (the light fire, glows and spells give what's near them)
    ['graphics.effects.elight'] = 'How brightly fire, glows and spells light what is near them.',
    ['graphics.effects.elight_radius'] = 'How far their light reaches.',
    ['graphics.effects.elight_lantern'] = 'How much light lantern flames give, as a share of the rest.',
    ['graphics.effects.elight_actor'] = 'How brightly spells and abilities light what is near them. Strength above doesn\'t change it.',
    -- Bounce light
    ['graphics.effects.gi'] = 'How much light bounces off surfaces onto their neighbours.',
    ['graphics.effects.gi_radius'] = 'How far the bounced light reaches.',
    ['graphics.effects.gi_quality'] = 'Less grain costs more.',
    -- Height fog
    ['graphics.effects.fog'] = 'How thick the fog is.',
    ['graphics.effects.fog_falloff'] = 'How quickly the fog thins as you go higher.',
    ['graphics.effects.fog_height'] = 'How far below the camera the fog is thickest.',
    ['graphics.effects.fog_floor'] = 'How far below the camera the fog keeps thickening; lower down it stays that thick, so a harbour seen from above is not drowned.',
    ['graphics.effects.fog_max'] = 'The most fog that can ever cover something.',
    ['graphics.effects.fog_sun'] = 'How much the sun lights up the fog.',
    ['graphics.effects.fog_g'] = 'How bright the fog glows when you look toward the sun.',
    ['graphics.effects.fog_color'] = 'The height fog\'s colour.\n'
        .. 'Zone\'s own: follows the zone\'s weather and time of day.\n'
        .. 'Match the sky: the colour of the sky you can see. DirectX 12.\n'
        .. 'Custom: the colour you pick below.',
    ['graphics.effects.fog_custom'] = 'Your own fog colour.',
    -- Bloom, god rays
    ['graphics.effects.bloom'] = 'How strongly bright lights glow.',
    ['graphics.effects.threshold'] = 'How bright something must be to glow. Lower makes more things glow.',
    ['graphics.effects.rays'] = 'How strong the beams of light from the sun are.',
    ['graphics.effects.rays_decay'] = 'How quickly the beams fade away.',
    ['graphics.effects.rays_length'] = 'How long the beams are.',
    -- Lighting (was Day & Night)
    ['graphics.effects.ambient_force'] = 'The least light anything gets, day and night, indoors and out, so nothing is too dark to see. 0 is off. '
        .. 'Higher lifts the darkest places; 1 lights everything fully, with no shading. As Windower\'s game_forceambientlight.',
    ['graphics.effects.night'] = 'How dark the night gets outdoors. 1 is the darkest.',
    ['graphics.effects.night_moon'] = 'How far the night picture turns a cool moonlit blue. Lamps, fires and spells keep their colour.',
    ['graphics.effects.night_bright'] = 'How strongly the moon lights what faces it, as the sun does by day. Its shadows come with it.',
    ['graphics.effects.night_bright_ambient'] = 'Raises the night\'s light everywhere, in the shade too. Too much washes the picture out.',
    ['graphics.effects.day_bright'] = 'How much stronger the sun is, outdoors by day: what faces it is lit brighter and the shade stays as it is. 0 is off. '
        .. '1 doubles it; higher blows the sunlit places out to full brightness. Lower Bloom\'s threshold as well for a blown-out look.',
    ['graphics.effects.indoor_bright'] = 'How much brighter indoor zones are: Mog Houses, caves, dungeons and other zones with no sun in the sky. '
        .. 'Outdoor zones are left as they are. 0 is off. Only works in zones the game counts as indoors.',
    ['graphics.effects.player_light'] = 'How bright the light round you is at night. Needs Dark night on.',
    ['graphics.effects.player_light_radius'] = 'How wide the pool of light round you is.',
    ['graphics.effects.player_light_warm'] = 'The light\'s colour: 0 is white, 1 a torch\'s orange.',
    ['graphics.effects.player_light_day'] = 'By day, how much darker the world round the pool is, so the light still shows. 0 is no pool by day.',
    ['graphics.effects.day_night_warmth'] = 'Tints the picture warmer by day and cooler at night, outdoors, on top of Warmth. 0 is off.\n'
        .. 'The higher, the stronger the tint. It works through the Colour grade, so that must be on too.',
    ['graphics.effects.moon_size'] = 'How big the moon is in the sky.',
    ['graphics.effects.moon_brightness'] = 'How bright the moon is in the sky.',
    -- Texture filtering
    ['graphics.effects.filter'] = 'Smooths textures in the distance so they shimmer less.',
    ['graphics.effects.aniso'] = 'Keeps textures sharp at a slant, such as the ground far ahead.',
    -- Water
    ['graphics.water.tint'] = 'Your own water colour. Game\'s means none picked.',
    ['graphics.water.detect'] = 'What counts as water: flat tiles, the game\'s water models, or all of it, with the water in the zone\'s own meshes told by where your footsteps splash.',
    ['graphics.water.pass'] = 'Draw the water after the world, so the bed shows through where it is shallow. Off, the water is a solid sheet.',
    ['graphics.water.horizon'] = 'Experimental: draw the sea on past the edge of the zone to the horizon, where the game paints its sea on the sky. Off by default: it is a different sea from the game\'s, darker, with a step where its rings meet. Needs the water pass.\n'
        .. 'With VanaCore sea on, the sea goes past your draw distance, out to the horizon.',
    ['graphics.water.sea'] = "Replace the game's sea with one VanaCore sea that runs across the zone and on to the horizon. Ponds, lakes and streams stay as they are. Needs the water pass.",
    ['graphics.water.shadows'] = "Let the sun's shadows fall on VanaCore's water. Off, the water takes no shadow. Needs the water pass.",
    ['graphics.water.depth'] = 'How far into the water you can see, in yalms. Needs the water pass.',
    ['graphics.water.refraction'] = 'How much the waves bend what shows through the water. Needs the water pass.',
    ['graphics.water.shore'] = 'How wide the soft edge and the foam are where the water meets the shore, in yalms. Needs the water pass.',
    ['graphics.water.foam'] = 'How strong the foam line at the shore is. Needs the water pass.',
    ['graphics.water.falls'] = 'How strongly a waterfall churns and foams the water it lands in. 0 is off. Needs the water pass.',
    ['graphics.water.caustics'] = 'The rippling net of light on the bed in shallow water by day. 0 is off. Needs the water pass.',
    ['graphics.water.relief'] = 'How much the wavelets are lit as slopes, crests bright and troughs dark, and bend what shows through. 0 is a flat sheet.',
    ['graphics.rain'] = "VanaCore's own rain while it rains: streaks the world and the characters hide, splashes where the drops land, a wet sheen on the ground.",
    ['graphics.rain.density'] = 'How much rain falls.',
    ['graphics.rain.splash'] = 'How strongly the drops splash where they land, on the ground and on the characters.',
    ['graphics.rain.range'] = 'How far round you the drops splash, in yalms: each 100 yalms out a little less. Further costs more.',
    ['graphics.rain.far'] = 'How far round you the rain falls, in yalms: each 100 yalms out a fifth fewer drops. Further costs more.',
    ['graphics.rain.water'] = "The rings the drops make where they land on VanaCore's water. 0 is none. Needs the water pass.",
    ['graphics.rain.spatter'] = 'How big the spatter is that the drops throw off the characters where they hit. 0 is none.',
    ['graphics.rain.shelter'] = 'No rain under roofs, bridges and in caves, from the zone\'s collision.',
    ['graphics.rain.lens.on'] = 'Droplets on the lens while the rain reaches the camera. Off keeps the amount below for when they are back.',
    ['graphics.rain.reflect'] = 'How strong the reflection is on the wet patches in the rain. Needs Reflections on. DirectX 12.',
    ['graphics.rain.puddles'] = 'How much of the ground is wet: patches, more of them the higher, and the dips. DirectX 12.',
    ['graphics.rain.ground'] = 'Wet patches on the ground in the rain, reflecting like a wet floor. Needs Reflections on.',
    ['graphics.rain.lens'] = 'Droplets on the lens while the rain reaches the camera: each lands as a hit and sits as a bead showing the scene upside down in it; the big ones slide off down a rivulet, the small ones dry. How many there are: 0 is none, 1 a wet windscreen.',
    ['graphics.rain.game'] = "Keep the game's own faint rain drawn under VanaCore's. Off, VanaCore's rain is the only rain.",
    ['graphics.rain.thunder'] = 'Rain during thunder weather too, in zones that have rain, as a look: the weather itself stays thunder.',
    ['graphics.rain.wet'] = 'Players, NPCs and monsters reflect like the wet ground while it rains, less in their creases. Needs Reflections on.',
    ['graphics.rain.sheen'] = 'How strong the reflection on them is. 0 is none.',
    ['graphics.rain.dry'] = 'How many seconds they take to dry once the rain stops.',
    ['graphics.rain.force'] = 'Shows the rain and the wet look whatever the weather.',
    ['graphics.water.ripples'] = 'How strong the fine ripples over the waves are.',
    ['graphics.water.ripple.size'] = 'How large the fine ripples are.',
    ['graphics.water.colour'] = 'Slide the water from blue to green.',
    ['graphics.water.brightness'] = 'How bright the water is.',
    ['graphics.water.shallowness'] = 'How see-through the water is.',
    ['graphics.water.turbulence'] = 'How choppy the surface is.',
    ['graphics.water.waves'] = 'How tall the waves are.',
    ['graphics.water.speed'] = 'How fast the waves move.',
    ['graphics.water.size'] = 'How big each wave is.',
    ['graphics.water.direction'] = 'Which way the waves travel.',
    ['graphics.water.reflection'] = 'How much of the sky shows in the water.',
    ['graphics.water.glint'] = 'The sparkle of the sun and moon on the water.',
    ['graphics.water.glint.size'] = 'How big the sparkles are.',
    ['graphics.water.mirror'] = 'How strongly the world around the water is mirrored in it: the shore, trees, walls and cliffs. 0 is off. Needs the water pass.',
    ['graphics.water.mirror.scale'] = 'How sharp the mirrored world is: its size as a share of the picture. Lower is softer and faster.',
    ['graphics.water.mirror.reach'] = 'How far from the camera things are still mirrored, in yalms. 0 mirrors everything drawn. Lower is faster.',
    ['graphics.water.mirror.waves'] = 'How much the waves break up the mirrored world. 0 is a still mirror.',
    -- Mass grass
    ['graphics.grass.radius'] = 'How far from you grass is drawn, in tens of yalms: 7 is 70. Click the number to type up to 50.',
    ['graphics.grass.density'] = 'How thickly the grass grows: 100% is the most. More costs frame rate and video memory.',
    ['graphics.grass.ring.spacing'] = 'How dense the grass stays further away. Three rings round you, at this share of the Range, twice it and three times it: full density inside the first, then medium, then lower past the third. Higher keeps denser grass further out, and costs more frame rate.',
    ['graphics.grass.rings.show'] = 'Draws the grass rings round you: 1 green, 2 yellow, 3 orange, where tufts shrink blue, the range red.',
    ['graphics.grass.freecam'] = 'While /freecam is on, grass grows round the camera instead of your character.',
    ['graphics.grass.log'] = 'Writes why ground gets grass or not to logs\\vanacore.log, and the textures it finds to logs\\grass.',
    ['graphics.grass.slope'] = 'The steepest slope grass grows on, in degrees. Steeper ground stays bare.',
    ['graphics.grass.dry'] = "Grows grass on yellow, dry grass ground too (East Sarutabaruta's), not only green. Outdoors only.",
    ['graphics.grass.mega'] = "Fills all the ground the game calls grass, not only its green spots. More grass to draw.",
    ['graphics.grass.shade'] = "Grows grass on the shaded and olive parts of grass ground too, not only the green parts (Bhaflau Thickets' forest floors).",
    ['graphics.grass.walkable'] = "Grass grows only on ground you could stand on, by the zone's collision: not on carts, crates, trees or roofs.",
    ['graphics.grass.nowater'] = "No grass on ground the game calls shallow water, where your footsteps splash (Dragon's Aery's meadow, Pashhow's marsh).",
    ['graphics.grass.moss'] = 'Low cushions of moss on that shallow water instead of bare ground. Needs No grass in water.',
    ['graphics.grass.cover.show'] = 'Grass under a floor (left out) drawn red; grass with no floor under it to check, yellow.',
    ['graphics.grass.overgrown'] = 'Nyzul Isle?',
    ['graphics.grass.build'] = 'Milliseconds a frame spent growing grass as you come up to it; the rest waits for the next frame. Lower is smoother, higher fills in faster.',
    ['graphics.grass.cull'] = "Skips drawing grass that's behind or beside the camera. It's kept ready, so it shows at once when you turn. Faster; off draws it all.",
    ['graphics.grass.pathpad'] = "Keeps grass this many yalms back from paths, so they read wider. It's the default for every zone; a zone set up with its own (Wajaom: none) keeps it, and this is locked there.",
    ['graphics.grass.wind.speed'] = 'How fast the grass sways in the wind. 0 stills it; 1 is the usual pace.',
    ['graphics.grass.wind.turbulence'] = 'How much the sway differs from place to place: low, the whole field moves together in long waves; high, it ripples. 0 moves it as one.',
    ['graphics.grass.edges'] = 'Near a ledge or the top of a rock, the grass grows smaller so it does not hang out over the edge.',
    ['graphics.grass.edge.fade'] = 'Grass this near where it ends (a cliff, a drop, rock, a path) grows shorter, and shortens as it climbs toward the max slope angle. 0 turns it off.',
    ['graphics.grass.edge.height'] = 'How tall the grass is right at its edge, as a share of its height.',
    ['graphics.grass.height'] = 'How tall the grass is: 1% is the shortest, 100% the tallest. Click the number to type more.',
    ['graphics.grass.width'] = 'How wide each tuft is.',
    ['graphics.grass.variation'] = 'How much the grass heights differ from each other.',
    ['graphics.grass.brightness'] = 'How bright the grass is.',
    ['graphics.grass.match'] = 'Makes the grass and plants the colour of the ground under them, lit as the ground is: 0 their own shade, 1 the ground\'s.',
    ['graphics.grass.brightness.auto'] = "Dims the grass and flowers by Vana'diel time, a little every minute: full at noon, halfway at 6:00 and 18:00, Night brightness at midnight.",
    ['graphics.grass.brightness.night'] = 'How bright the grass is at midnight with Dim at night on, as a share of Brightness: 0.5 is half.',
    ['graphics.grass.flowers'] = 'How many of the tufts have a flower: 1 is one in ten, 0.1 one in a hundred.',
    ['graphics.grass.flower_brightness'] = 'How bright the flowers are.',
    ['graphics.grass.flowers_only'] = 'Draws only flowers, no grass.',
    ['graphics.effects.debug'] = 'Shows one effect on its own, for testing. Off again whenever the game starts.\n'
        .. 'Off: the normal picture.\n'
        .. 'Any other: only that effect, so you can see what it adds.',
    ['graphics.effects.sun_gate'] = 'Turns one part of the sun\'s shadows off, to find which one changes them when the camera moves. '
        .. 'Off again whenever the game starts. DirectX 12.',
    -- Effects: colour grade
    ['graphics.effects.grade'] = 'How much of the colour adjustments below are applied.',
    ['graphics.effects.sat'] = 'How vivid the colours are. 0 is black and white.',
    ['graphics.effects.contrast'] = 'The gap between light and dark.',
    ['graphics.effects.brightness'] = 'Makes the whole picture lighter or darker.',
    ['graphics.effects.shadows'] = 'Lightens or darkens the dark parts only.',
    ['graphics.effects.highlights'] = 'Lightens or darkens the bright parts only.',
    ['graphics.effects.midtones'] = 'Lightens or darkens the middle tones only.',
    ['graphics.effects.whites'] = 'Moves the brightest white up or down.',
    ['graphics.effects.rolloff'] = 'Eases very bright areas into white instead of cutting them off.',
    ['graphics.effects.light_tint'] = 'Colours the light itself, cooler or warmer.',
    ['graphics.effects.warmth'] = 'Cooler (blue) to warmer (orange).',
    ['graphics.effects.hue'] = 'Turns every colour round the colour wheel: reds toward yellow and green one way, toward purple and blue the other. Greys stay as they are.',
    ['graphics.effects.clarity'] = 'Makes mid-sized detail pop. Gives a crunchier look.',
    ['graphics.effects.sharpen'] = 'Makes edges crisper.',
    ['graphics.effects.smooth'] = 'Softens the picture slightly.',
    ['graphics.effects.outline'] = 'How dark the cartoon-style outlines are. 0 is off.',
    ['graphics.effects.outline_width'] = 'How thick the outlines are.',
    ['graphics.effects.outline_width_chars'] = 'How thick the outlines are on characters. 0 is the same as Line width.',
    ['graphics.effects.outline_depth'] = 'How big a depth step must be to get a line. Lower draws more lines.',
    ['graphics.effects.outline_crease'] = 'How sharp a fold must be to get a line. Lower draws more lines.',
    ['graphics.effects.outline_fade'] = 'How far away lines fade out.',
    ['graphics.effects.outline_nograss'] = 'Mass Grass gets its own lines (the grass settings below), and the scenery\'s lines ignore it. Off: the grass is outlined like the scenery. Needs Mass Grass on.',
    ['graphics.effects.outline_grass'] = 'How dark the lines on Mass Grass are, as a share of Darkness. Off draws none. Needs Mass Grass on and Separate from scenery.',
    ['graphics.effects.outline_width_grass'] = 'How thick the lines on Mass Grass are. Same is Line width.',
    ['graphics.effects.outline_depth_grass'] = 'How big a depth step on Mass Grass must be to get a line. Lower draws more lines. Same is Edge sensitivity.',
    ['graphics.effects.outline_fade_grass'] = 'How far away the lines on Mass Grass fade out. Same is Lines fade by.',
    ['graphics.effects.bands'] = 'Splits the light into this many flat steps, for a cartoon look.',
    ['graphics.effects.band_soft'] = 'How soft the edges between steps are.',
    ['graphics.effects.dof'] = 'How strong the blur is on things out of focus.',
    ['graphics.effects.dof_range'] = 'How far either side of the focus stays sharp.',
    ['graphics.effects.dof_far'] = 'How quickly the blur grows with distance.',
    ['graphics.effects.dof_near'] = 'Blurs things closer than the focus too.',
    ['graphics.effects.dof_focus'] = 'A fixed focus distance. 0 focuses on you.',
    ['graphics.effects.dof_sky'] = 'No blur on the sky, its clouds, the sun and the moon.',
    ['graphics.effects.normalmap_strength'] = 'How strongly the texture pack\'s bump detail shows in the light.',
    ['graphics.effects.normalmap_view'] = 'How the texture pack\'s bump detail is shown.\n'
        .. 'Lit: normal play.\n'
        .. 'Normals as colour: the bump maps as colours, for checking them.\n'
        .. 'Lit, Y flipped: try this if bumps look pushed in instead of out.',
    ['graphics.effects.normalmap_refl'] = 'Reflections use the bump detail too.',
    ['graphics.effects.normalmap_fx'] = 'Ambient occlusion and sun shadows use the bump detail too.',
    -- Options: Camera
    ['camera.zoom'] = 'Zoom moves the camera in and out, instead of narrowing the view.',
    ['camera.fov'] = 'How wide the view is. Higher shows more.',
    ['camera.distance.max'] = 'The farthest the camera can pull back.',
    ['camera.distance'] = 'How far back the camera starts each game.',
    ['camera.distance.remember'] = 'Each game starts at the zoom the last one ended on.',
    ['camera.zoom.speed'] = 'How fast the mouse wheel and zoom keys zoom.',
    ['camera.mouse.edgeturn'] = 'The mouse at the edge of the window turns the camera, as in the game.',
    ['camera.behind'] = 'The camera swings round behind your character as you move.',
    ['camera.behind.speed'] = 'How fast the camera swings behind you.',
    ['camera.behind.delay'] = 'How long after you start moving or turn the camera before it swings behind you.',
    ['camera.turn'] = 'How fast the camera turns left and right.',
    ['camera.turn.vertical'] = 'How fast the camera turns up and down.',
    ['camera.battle.distance'] = 'How far back the camera sits when locked on.',
    ['camera.battle.range'] = 'How far round the target can get before the locked-on camera turns.',
    ['camera.mouse.noleftdrag'] = 'Dragging with the left mouse button no longer turns the camera; the right button still does.',
    ['camera.mouse.hidecursor'] = 'Hides the mouse cursor while you hold the right button to turn the camera.',
    ['camera.home.reset'] = 'Home turns the camera round behind your character, as in the game.',
    ['camera.view.reset'] = 'The controller\'s Change View button turns the camera round behind your character,\n'
        .. 'instead of switching to first person. In first person it still switches back.',
    ['camera.walls'] = 'Walls, floors and ceilings between your character and the camera pull it in front of them.',
    ['camera.lock'] = 'The camera moves with your character at once, without the game\'s lag.',
    ['camera.offset.x'] = 'Moves your character left or right of the middle of the screen.',
    ['camera.offset.y'] = 'Moves your character up or down from the middle of the screen.',
    -- Characters, glow, zone lines
    ['graphics.characters.ambient'] = 'The least light a player, NPC or monster gets. Higher lifts them out of the dark; the zone around them stays as it is.',
    ['graphics.characters.brightness'] ='Lights up players, NPCs and monsters so they do not sit in the dark.',
    ['graphics.glow'] = 'The glow round Trusts and avatars.\n'
        .. 'Off: no glow. The character is drawn crisp.\n'
        .. '256 (game): the game\'s own glow. Blurry at high resolutions.\n'
        .. '512, 1024: the game\'s glow drawn bigger. Sharper, costs more.\n'
        .. 'Custom: VanaCore\'s own glow. Sharp and quick; set it with the sliders below.',
    ['graphics.glow.strength'] = 'How bright the glow is.',
    ['graphics.glow.size'] = 'How far the glow spreads.',
    ['graphics.glow.quality'] = 'How smooth a big glow is.\n'
        .. 'Low: the fastest. A wide glow can look stepped.\n'
        .. 'Medium: the usual.\n'
        .. 'High, Max: smoother, and cost more.',
    ['graphics.trusts.spawn'] = 'Never: Do not play any Trust summoning effects.\n'
        .. 'When summoned: Only when you actually see the Trust being summoned.\n'
        .. 'Always: Always play the Trust summon animation, even when Trusts pop in as you run around.',
    ['graphics.zonelines'] = 'Marks the ground where you walk into the next zone.',
    ['graphics.zonelines.style'] = 'What the zone line markers look like.\n'
        .. 'Fill: a glowing curtain.\n'
        .. 'Line: a line floating across it.\n'
        .. 'Dots: a dotted line.\n'
        .. 'Stars: sparkles drifting upward.\n'
        .. 'Floor dots: dots lying on the ground.',
    ['graphics.zonelines.glow'] = 'Makes the markers glow.',
    ['graphics.zonelines.height'] = 'How high above the ground the markers float.',
    ['graphics.zonelines.size'] = 'How big the markers are.',
    ['graphics.zonelines.fade'] = 'How soft the edges of the markers are.',
    ['graphics.zonelines.color'] = 'The markers\' colour.',
    ['graphics.zonelines.strength'] = 'How solid the markers are.',
    ['graphics.sparkles'] = 'Twinkling sparkles rising from portals.',
    ['graphics.sparkles.strength'] = 'How bright the sparkles are.',
    ['graphics.sparkles.size'] = 'How big the sparkles are.',
    ['graphics.sparkles.density'] = 'How many sparkles there are.',
    ['graphics.sparkles.speed'] = 'How fast the sparkles rise.',
    ['graphics.sparkles.match'] = 'Each sparkle takes the colour of the spot it rises from.',
    -- Windows
    ['client.window.opacity'] = 'How see-through menu and help windows are. Chat has its own.',
    ['client.window.border.opacity'] = 'How see-through window borders are.',
    ['client.window.color'] = 'Tints window backgrounds.',
    ['client.window.image.fit'] = 'Fill: covers each window, cropped to its shape. Stretch: squeezed to each window. Tile: repeated at its own size. Screen: one picture across the screen, each window showing its part.',
    ['client.window.border.color'] = 'Tints window borders.',
    ['client.buttons.color'] = 'The colour of every menu button.',
    ['client.buttons.selected.color'] = 'The colour of the selected menu button.',
    ['client.buttons.style'] = 'Game: the game\'s glossy bars. Flat: a plain box with a border, in the colours below.',
    ['client.buttons.text'] = 'The colour of the buttons\' labels. Unset is the game\'s white.',
    ['client.buttons.selected.text'] = 'The colour of the selected button\'s label. Unset is the game\'s gold.',
    ['client.buttons.border'] = 'The colour of a flat button\'s border.',
    ['client.buttons.selected.border'] = 'The colour of the selected flat button\'s border. Unset is the same as the others.',
    ['client.buttons.opacity'] = 'How solid a flat button\'s fill is. At 0 only its border and label show.',
    ['client.buttons.border.size'] = 'How thick a flat button\'s border is. 0 for none.',
    ['client.buttons.padding.x'] = 'How far a flat button reaches past its own width on each side. Less than 0 is narrower.',
    ['client.buttons.padding.y'] = 'How far a flat button reaches past its own height on each side. Less than 0 leaves a gap between buttons.',
    ['client.buttons.spacing'] = 'Room between the rows of flat buttons; the menu grows to fit.',
    ['client.buttons.text.x'] = 'Moves a flat button\'s label right, or left below 0.',
    ['client.buttons.text.y'] = 'Moves a flat button\'s label down, or up below 0.',
    ['client.buttons.selected.nudge'] = 'The selected button sits a pixel up and left, and presses in when picked. Off, it stays put.',
    ['client.buttons.selected.flash'] = 'A button flashes when picked.',
    ['client.sliders.style'] = 'The Config windows\' sliders. Game: the game\'s bars. Flat: a plain bar, fill and handle in the colours below.',
    ['client.sliders.track.color'] = 'The colour of a slider\'s bar.',
    ['client.sliders.fill.color'] = 'The colour of a slider\'s fill, up to its value.',
    ['client.sliders.selected.color'] = 'The fill of the slider the cursor is on.',
    ['client.sliders.height'] = 'How thick a slider\'s bar and fill are.',
    ['client.sliders.track.opacity'] = 'How solid a slider\'s bar is. At 0 only the fill and handle show.',
    ['client.sliders.round'] = 'Rounds the ends of a slider\'s bar and fill.',
    ['client.sliders.knob'] = 'The mark at a slider\'s value.',
    ['client.sliders.knob.size'] = 'How big the mark at a slider\'s value is.',
    ['client.sliders.knob.color'] = 'The colour of the mark at a slider\'s value.',
    ['client.sliders.ends'] = 'The marks at each end of a slider. Game: the game\'s half dots.',
    ['client.sliders.ends.size'] = 'How big the marks at each end of a slider are.',
    ['client.sliders.ends.color'] = 'The colour of the marks at each end of a slider.',
    ['client.mainmenu.buttons'] = 'On: the main menu and the menus it opens use the settings below alone, whatever Global\'s are. Off: they look as Global says.',
    ['client.action.buttons'] = 'On: the Action Menu and its Magic and Abilities lists use the settings below alone, whatever Global\'s are. Off: they look as Global says.',
    -- Buttons > Main Menu: the main menu's and the menus it opens in its place (Magic, Party, Search, Config...), while its Enable is on
    ['client.mainmenu.buttons.style'] = 'The main menu\'s buttons. Game: the game\'s glossy bars. Flat: a plain box with a border.',
    ['client.mainmenu.buttons.color'] = 'The colour of the main menu\'s buttons.',
    ['client.mainmenu.buttons.selected.color'] = 'The colour of the main menu\'s selected button.',
    ['client.mainmenu.buttons.text'] = 'The colour of the main menu\'s button labels.',
    ['client.mainmenu.buttons.selected.text'] = 'The colour of the main menu\'s selected button label.',
    ['client.mainmenu.buttons.border'] = 'The colour of the main menu\'s flat buttons\' border.',
    ['client.mainmenu.buttons.selected.border'] = 'The colour of the main menu\'s selected flat button\'s border.',
    ['client.mainmenu.buttons.opacity'] = 'How solid the main menu\'s flat buttons\' fill is.',
    ['client.mainmenu.buttons.border.size'] = 'How thick the main menu\'s flat buttons\' border is.',
    ['client.mainmenu.buttons.padding.x'] = 'How far the main menu\'s flat buttons reach past their own width on each side.',
    ['client.mainmenu.buttons.padding.y'] = 'How far the main menu\'s flat buttons reach past their own height on each side.',
    ['client.mainmenu.buttons.spacing'] = 'Room between the main menu\'s rows of flat buttons.',
    ['client.mainmenu.buttons.text.x'] = 'Moves the main menu\'s flat button labels right, or left below 0.',
    ['client.mainmenu.buttons.text.y'] = 'Moves the main menu\'s flat button labels down, or up below 0.',
    ['client.mainmenu.buttons.selected.nudge'] = 'The main menu\'s selected button sits a pixel up and left, and presses in when picked.',
    ['client.mainmenu.buttons.selected.flash'] = 'The main menu\'s buttons flash when picked.',
    -- Buttons > Action Menu: the Action Menu's and its Magic and Abilities lists, while its Enable is on
    ['client.action.buttons.style'] = 'The Action Menu\'s buttons. Game: the game\'s glossy bars. Flat: a plain box with a border.',
    ['client.action.buttons.color'] = 'The colour of the Action Menu\'s buttons.',
    ['client.action.buttons.selected.color'] = 'The colour of the Action Menu\'s selected button.',
    ['client.action.buttons.text'] = 'The colour of the Action Menu\'s button labels.',
    ['client.action.buttons.selected.text'] = 'The colour of the Action Menu\'s selected button label.',
    ['client.action.buttons.border'] = 'The colour of the Action Menu\'s flat buttons\' border.',
    ['client.action.buttons.selected.border'] = 'The colour of the Action Menu\'s selected flat button\'s border.',
    ['client.action.buttons.opacity'] = 'How solid the Action Menu\'s flat buttons\' fill is.',
    ['client.action.buttons.border.size'] = 'How thick the Action Menu\'s flat buttons\' border is.',
    ['client.action.buttons.padding.x'] = 'How far the Action Menu\'s flat buttons reach past their own width on each side.',
    ['client.action.buttons.padding.y'] = 'How far the Action Menu\'s flat buttons reach past their own height on each side.',
    ['client.action.buttons.spacing'] = 'Room between the Action Menu\'s rows of flat buttons.',
    ['client.action.buttons.text.x'] = 'Moves the Action Menu\'s flat button labels right, or left below 0.',
    ['client.action.buttons.text.y'] = 'Moves the Action Menu\'s flat button labels down, or up below 0.',
    ['client.action.buttons.selected.nudge'] = 'The Action Menu\'s selected button sits a pixel up and left, and presses in when picked.',
    ['client.action.buttons.selected.flash'] = 'The Action Menu\'s buttons flash when picked.',
    ['client.titles.hide'] = 'Hides the title on top of the menus, like Commands over the main menu.',
    -- Chat
    ['input.keyboard.blockbindsduringinput'] = 'Stops key binds firing while the chat line is open. Addons and plugins get no key presses either until you close it.',
    ['client.chat.prefix'] = 'Adds the kind of message in front of each chat line, like [Party] or [Tell].',
    ['client.chat.prefix.short'] = 'Short names: [LS 2], [P], [T].',
    ['client.chat.noindent'] = 'Wrapped chat lines start at the left edge instead of being pushed in.',
    ['client.chat.lineheight'] = 'The height of each chat line. Higher spaces them out.',
    ['client.chat.scrollback'] = 'How many lines the chat window scrolls back through, since the game started. The game kept 1,000 a window; VanaCore keeps up to 12,500.',
    ['client.chat.history'] = 'How many lines you have sent that Up and Down bring back in the chat line. The game keeps 9.',
    ['client.chat.shadow'] = 'A dark shadow or outline behind the game font\'s chat text, so it reads over bright scenery. True Font has its own Outline.',
    ['client.chat.shadow.opacity'] = 'How dark the chat text shadow is.',
    ['client.truefont.disable'] = 'Every True Font area of the interface off at once: the game\'s own letters in the menus, buttons, lists, windows and chat, wherever each area\'s switch is (Buttons and Equipment are on Game Options). The nameplates and damage numbers keep their own switches. Each area\'s settings are kept for when this is turned off.',
    ['client.truefont.family'] = 'The font every True Font area uses unless it has its own. Custom Fonts adds any .ttf or .otf to the lists.',
    ['client.truefont.japanese'] = 'The font for kana, kanji and other full-width letters in every True Font area. Each area\'s font uses Windows\' Japanese font where it has none.',
    ['client.truefont.outline'] = 'A black outline round True Font\'s letters, this many pixels wide. 0 is none. Each area\'s own Outline (under its font) once that is moved.',
    ['client.truefont.outline.opacity'] = 'How dark the outline is (every area\'s).',
    ['client.truefont.gdi'] = 'Every True Font letter drawn with Windows\' GDI, none with DirectWrite: a font installed in Windows is drawn as one in VanaCore\'s fonts folder always is. Letters sit on whole pixels, so their spacing is a little uneven at small sizes. Takes effect at the next start.',
    ['client.chat.font'] = 'What draws the chat\'s letters: the game\'s own font, or True Font, a font on your PC drawn at the screen\'s own size so it stays sharp.',
    ['client.chat.font.size'] = 'The font\'s size, in pixels at the interface\'s size. The chat\'s rows are 16 apart.',
    ['client.chat.font.spacing'] = 'More room between the chat\'s letters, or less below 0.',
    ['client.menu.font'] = 'What draws the letters in menus and lists: the game\'s own font, or True Font. Each letter keeps the game\'s spacing, so no window\'s layout moves. The chat input has its own (Game Options > Chat).',
    ['client.items.font'] = 'What draws the item lists\' letters (Items, storage, Equipment, trades, shops, the auction house).',
    ['client.lists.font'] = 'What draws the Merit Points menu\'s lists. Each setting follows General until it\'s set here.',
    ['client.tips.font'] = 'What draws an item\'s details: the box that comes up for the item the cursor is on.',
    ['client.tips.font.lineheight'] = 'How far apart the details\' lines are. The game\'s are 16; the box grows to hold them. True Font only.',
    ['client.tips.font.padding.top'] = 'Moves the details\' text down from the top of the box. The box grows to hold it. True Font only.',
    ['client.tips.font.padding.bottom'] = 'More room below the details\' last line: the box\'s bottom that much lower. True Font only.',
    ['client.tips.font.title.size'] = 'The size of the item\'s name, the details\' top line, in pixels at the interface\'s size. True Font only.',
    ['client.tips.font.title.color'] = 'The item\'s name\'s colour. Unset is the game\'s.',
    ['client.tips.font.type.size'] = 'The size of the type line after the name, such as [Body]All Races.',
    ['client.tips.font.type.color'] = 'The type line\'s colour. Unset is the game\'s.',
    ['client.tips.font.type.space'] = 'More room between the name and the type line.',
    ['client.tips.font.stats.size'] = 'The size of the stats and description lines.',
    ['client.tips.font.stats.color'] = 'The stats\' colour. Unset is the game\'s.',
    ['client.tips.font.stats.numbers.color'] = 'The stats\' numbers\' colour, with the + or - before one and the % after. Unset: the stats\' colour.',
    ['client.tips.font.stats.space'] = 'More room above the stats.',
    ['client.tips.font.jobs.size'] = 'The size of the job list: the line starting Lv. and the job names after it.',
    ['client.tips.font.jobs.color'] = 'The job list\'s colour. Unset is the game\'s.',
    ['client.tips.font.jobs.space'] = 'More room above the job list.',
    ['client.tips.font.signature.size'] = 'The size of the crafter\'s signature, the last line in brackets such as [Voltiz].',
    ['client.tips.font.signature.color'] = 'The signature\'s colour. Unset is the game\'s.',
    ['client.tips.font.signature.space'] = 'More room above the signature.',
    ['client.tips.font.opacity'] = 'How see-through all the details\' text is. 0 hides it. True Font only.',
    ['client.tips.font.title.opacity'] = 'How see-through the item\'s name is. 0 hides it.',
    ['client.tips.font.type.opacity'] = 'How see-through the type line is. 0 hides it.',
    ['client.tips.font.stats.opacity'] = 'How see-through the stats are. 0 hides them.',
    ['client.tips.font.jobs.opacity'] = 'How see-through the job list is. 0 hides it.',
    ['client.tips.font.signature.opacity'] = 'How see-through the signature is. 0 hides it.',
    ['client.tips.font.timer.size'] = 'The size of a line in angle brackets, such as a usable item\'s <1/1 0:30/[20:00:00, 0:30]>.',
    ['client.tips.font.timer.color'] = 'The timer line\'s colour. Unset is the game\'s.',
    ['client.tips.font.timer.numbers.color'] = 'The timer\'s numbers\' colour. Unset: the timer\'s colour, not the stats\' numbers colour.',
    ['client.tips.font.timer.opacity'] = 'How see-through the timer line is. 0 hides it.',
    ['client.tips.font.timer.space'] = 'More room above the timer line.',
    ['client.tips.icon.size'] = 'The item\'s picture\'s size, round its middle. Works with either letters.',
    ['client.tips.badges.size'] = 'The Rare and Ex badges\' size, round their middle. Works with either letters.',
    ['client.tips.extended'] = 'Shows everything on the first page: the augments and the rest of a long description too, with no arrow to turn the page. The box grows to hold them.',
    ['client.tips.empty.hide'] = 'Hides the details box when the cursor is on an empty slot, and the comment box under /check for a player with no bazaar message, where the game shows them empty.',
    ['client.tips.type'] = 'How the line under the name is written. Default: [Body]All Races. Simple: Body - All Races. Type only: Body.',
    ['client.tips.rarity.name'] = 'Draws a +1 to +4 item\'s name in its rarity colour, the one set under Item Rarity.',
    ['client.tips.rarity.gradient'] = 'A soft band of the item\'s rarity colour across the top of its details, fading down. +1 to +4 items only.',
    ['client.tips.rarity.gradient.strength'] = 'How strong the colour is at the very top.',
    ['client.tips.rarity.gradient.height'] = 'How far down the colour fades out, in pixels at the interface\'s size.',
    ['client.help.font'] = 'What draws the help bar, spell details and comments.',
    ['client.input.font'] = 'What draws the chat input line and its Say / Shout / Linkshell tab. Follows General until it\'s set here.',
    ['client.input.font.family'] = 'This area\'s own font, or the default font.',
    ['client.input.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each word starts where the game\'s does.',
    ['client.phrases.font'] = 'What draws the auto-translate list (Tab while typing). Follows General until it\'s set here.',
    ['client.phrases.font.family'] = 'This area\'s own font, or the default font.',
    ['client.phrases.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each word starts where the game\'s does.',
    ['client.party.font'] = 'What draws the party list\'s names and numbers. Each setting follows Labels & Stats until it\'s set here.',
    ['client.party.font.family'] = 'This area\'s own font, or the default font. Unset, the Labels & Stats font.',
    ['client.party.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Unset, the Labels & Stats size.',
    ['client.alliance.font'] = 'What draws the alliance lists\' names and numbers. Each setting follows Labels & Stats until it\'s set here.',
    ['client.alliance.font.family'] = 'This area\'s own font, or the default font. Unset, the Labels & Stats font.',
    ['client.alliance.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Unset, the Labels & Stats size.',
    ['client.party.font.numbers.size'] = 'The HP, MP and TP numbers\' size, in pixels at the interface\'s size. Unset, the font size above (the names\').',
    ['client.alliance.font.numbers.size'] = 'The HP, MP and TP numbers\' size, in pixels at the interface\'s size. Unset, the font size above (the names\').',
    ['client.party.names.upper'] = 'Prints the members\' names in capitals, your pet\'s too. With the game\'s letters or True Font.',
    ['client.party.spacing'] = 'Room between the members\' rows, in pixels at the interface\'s size. The list grows up by as much, its bottom where it was. 0 is the game\'s.',
    ['client.alliance.spacing'] = 'Room between the members\' rows, in pixels at the interface\'s size. Each list grows up by as much; left where the game puts them, they also go up by as much as the lists under them grew. 0 is the game\'s.',
    ['client.alliance.names.upper'] = 'Prints the members\' names in capitals. With the game\'s letters or True Font.',
    ['client.party.hp.color'] = 'The HP number\'s colour. Under 75% HP the game\'s yellow, orange and red still show.',
    ['client.alliance.hp.color'] = 'The HP number\'s colour. Under 75% HP the game\'s yellow, orange and red still show.',
    ['client.party.mp.color'] = 'The MP number\'s colour.',
    ['client.alliance.mp.color'] = 'The MP number\'s colour.',
    ['client.party.tp.color'] = 'The TP number\'s colour from 1000 TP. Under 1000 it\'s a little darker, as the game\'s grey is.',
    ['client.alliance.tp.color'] = 'The TP number\'s colour from 1000 TP. Under 1000 it\'s a little darker, as the game\'s grey is.',
    ['client.buttons.font'] = 'What draws the labels on the menus\' buttons (Status, Equipment).',
    ['client.titles.font'] = 'What draws the windows\' titles and the menus\' other labels, the ones not on a button. Each setting follows Buttons until it\'s set here.',
    ['client.mainmenu.font'] = 'What draws the main menu\'s commands (Status, Equipment ... Log Out), apart from every other menu\'s buttons.',
    ['client.mainmenu.font.fixed'] = 'A label the game squeezes to fit its button (Communication, Edit Equip. Set) drawn at the font\'s full size, running past its button like a long label does. Off: its letters narrowed as the game narrows them, to 85% at most.',
    ['client.mainmenu.font.family'] = 'This area\'s own font, or the default font. Unset, the buttons\' font.',
    ['client.mainmenu.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Unset, the buttons\' size.',
    ['client.action.font'] = 'What draws the Action Menu\'s commands (Attack, Magic, Abilities...) and its Magic and Abilities lists, apart from every other menu\'s buttons.',
    ['client.action.font.family'] = 'This area\'s own font, or the default font. Unset, the buttons\' font.',
    ['client.action.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Unset, the buttons\' size.',
    ['client.status.font'] = 'What draws the Status window\'s numbers and words. The party and alliance lists follow it until theirs are set.',
    ['client.clock.font'] = 'What draws the clock under the compass. Only the settings here change it.',
    ['client.clock.font.family'] = 'This area\'s own font, or the default font.',
    ['client.clock.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.trade.font'] = 'What draws the trade windows\' letters: a trade\'s and the delivery box (sent and received) with its Recipient box; their amounts, text and buttons. Only the settings here change it.',
    ['client.trade.font.family'] = 'This area\'s own font, or the default font.',
    ['client.trade.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.gil.font'] = 'What draws the Current Gil box, wherever it shows (Equipment, Items, a trade, the delivery box): its title and amount. Only the settings here change it.',
    ['client.gil.font.family'] = 'This area\'s own font, or the default font.',
    ['client.gil.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.profile.font'] = 'What draws the Profile panel (Status > Profile): its labels and values in one font. Only the settings here change it.',
    ['client.profile.title.font'] = 'What draws the Profile panel\'s titles (Profile, Monstrosity), and Job Levels\', Combat Skills\', Magic Skills\' and Craft Skills\'. The text\'s until set here.',
    ['client.profile.title.font.family'] = 'This area\'s own font, or the default font.',
    ['client.profile.title.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.profile.font.family'] = 'This area\'s own font, or the default font.',
    ['client.profile.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.craft.brackets'] = 'Craft Skills\' levels in brackets, as the game has them: (60). Off, the number alone.',
    ['client.profile.clean'] ='Lines the Profile panel up: labels at the left, their values in one column, a row each.',
    ['client.profile.title.color'] = 'The colour of the Profile panel\'s title bars (Profile, Monstrosity), and Job Levels\', Combat Skills\', Magic Skills\' and Craft Skills\'.',
    ['client.profile.title.opacity'] = 'How solid those title bars are: the Profile panel\'s, Job Levels\', Combat Skills\', Magic Skills\' and Craft Skills\'.',
    ['client.equip.font'] = 'What draws the Equipment window\'s slot names (Main, Sub, Range...): the game\'s font, True Font, or Hidden to leave them out. Under an item they stay under its picture, as the game draws them. Only the settings here change it.',
    ['client.equip.font.family'] = 'This area\'s own font, or the default font.',
    ['client.equip.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.equip.stats.font'] = 'What draws the Equipment window\'s stats: its element numbers, Attack and Defense, and the stats under it; and the /check stats box. Only the settings here change it.',
    ['client.equip.stats.font.family'] = 'This area\'s own font, or the default font.',
    ['client.equip.stats.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.macrobar.font'] = 'What draws the macro bar\'s key labels: Ctrl or Alt and the number under each macro. The macros\' names have their own, Name Font. Only the settings here change it.',
    ['client.macrobar.name.font'] = 'What draws the macros\' names under the keys. True Font writes each whole name in its key, in the font below. Each setting follows General until it\'s set here.',
    ['client.macrobar.name.font.family'] = 'This area\'s own font, or the default font.',
    ['client.macrobar.name.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.macrobar.font.family'] = 'This area\'s own font, or the default font.',
    ['client.macrobar.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.macrobar.font.color'] = 'The colour of the key labels\' True Font letters. Until it\'s set, the game\'s colour.',
    ['client.macrobar.prefix'] = 'Off: each key shows its number alone, without Ctrl or Alt.',
    ['client.chat.font.family'] = 'This area\'s own font, or the default font.',
    ['client.menu.font.family'] = 'This area\'s own font, or the default font.',
    ['client.items.font.family'] = 'This area\'s own font, or the default font.',
    ['client.help.font.family'] = 'This area\'s own font, or the default font.',
    ['client.tips.font.family'] = 'This area\'s own font, or the default font.',
    ['client.buttons.font.family'] = 'This area\'s own font, or the default font.',
    ['client.titles.font.family'] = 'This area\'s own font, or the default font. Unset, the buttons\' font.',
    ['client.status.font.family'] = 'This area\'s own font, or the default font.',
    ['client.menu.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each word starts where the game\'s does.',
    ['client.items.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each word starts where the game\'s does.',
    ['client.help.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each word starts where the game\'s does.',
    ['client.tips.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each letter sits in the game\'s own space for it, so a big size can crowd.',
    ['client.buttons.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each letter is centred where the game\'s stands.',
    ['client.titles.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Unset, the buttons\' size.',
    ['client.status.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each word starts and ends where the game\'s does.',
    ['client.chatlog'] = 'Saves everything in chat to a text file in the logs folder, a new one each day.',
    ['client.chat.match'] = 'Chat looks like the game\'s other windows. Off, it uses its own settings below.',
    ['client.chat.opacity'] = 'How see-through the chat background is.',
    ['client.chat.border.opacity'] = 'How see-through the chat border is.',
    ['client.chat.color'] = 'Chat background colour.',
    ['client.chat.border.color'] = 'Chat border colour.',
    ['client.input.match'] = 'The line you type in looks like the game\'s windows or the chat window. Custom, it uses its own settings below.',
    ['client.input.opacity'] = 'How see-through the chat input background is.',
    ['client.input.border.opacity'] = 'How see-through the chat input border is.',
    ['client.input.color'] = 'Chat input background colour.',
    ['client.input.border.color'] = 'Chat input border colour.',
    ['client.window.border.solid'] = 'Draws the window borders as a plain line in the border colour. Off, the windows\' corners stay square: cut or rounded corners need this line, or the border at 0%.',
    ['client.window.corners'] = 'Cuts or rounds the corners of the windows. What\'s inside a window (text, rows, item squares) is cut with them. Needs Solid border line, or the border at 0%.',
    ['client.window.corners.size'] = 'How big the cut or rounded corners are.',
    ['client.chat.border.solid'] = 'Draws the chat border as a plain line in the border colour. Off, the chat\'s corners stay square: cut or rounded corners need this line, or the border at 0%.',
    ['client.input.border.solid'] = 'Draws the chat input border as a plain line in the border colour. Off, its corners stay square: cut or rounded corners need this line, or the border at 0%.',
    ['client.input.hidetab'] = 'Hides the Say / Shout / Linkshell tab above the chat input.',
    ['client.chattabs.font'] = 'The tabs over the chat log when you open it to scroll (Window, Say, Tell...). True Font writes each tab\'s whole name in its tab, in the font below. Until set, the Titles\' letters.',
    ['client.chat.tabs.names'] = 'Full names (Linkshell 2, Party) or short ones (LS2, PT), which fit the narrow tabs.',
    ['client.chat.tabs.align'] = 'Each name from its tab\'s left, or in the middle of the part of its tab you can see.',
    ['client.chat.tabs.color'] = 'The colour of the names on the tabs.',
    ['client.chat.tabs.selected.color'] = 'The colour of the name on the tab that\'s picked.',
    ['client.chat.tabs.spacing'] = 'How far apart the tabs are. The game\'s is 30: each tab covers the end of the one before. The picked tab is always its whole width.',
    ['client.chat.tabs.hide'] = 'No tabs over the chat log at all.',
    ['client.chat.tabs.background'] = 'What\'s behind each name: the game\'s tab picture, a flat box, or nothing.',
    ['client.chat.tabs.tint'] = 'Tints the game\'s tab picture. White leaves it as it is.',
    ['client.chat.tabs.selected.tint'] = 'Tints the game\'s picture of the tab that\'s picked. White leaves it as it is.',
    ['client.chat.tabs.background.color'] = 'The flat boxes\' colour.',
    ['client.chat.tabs.background.selected.color'] = 'The flat box of the tab that\'s picked.',
    ['client.chat.tabs.background.opacity'] = 'How see-through the tabs\' backgrounds are.',
    ['client.chat.tabs.height'] = 'How tall the tabs are. The game\'s is 12. Taller tabs grow upward.',
    ['client.chat.tabs.width'] = 'The game\'s widths, or each tab as wide as its name (and the padding on both sides).',
    ['client.chat.tabs.padding'] = 'How far each name sits in from its tab\'s left edge.',
    ['client.chat.tabs.gap'] = 'The space between tabs, with Fit names or flat boxes.',
    ['client.chat.hide'] = 'The main chat window is never shown, even when you open the chat line. The line you type in stays.',
    ['client.chat2.hide'] = 'The second chat window (Config: Log Window 2) is never shown.',
    ['client.input.hide'] = 'The chat input line is kept off the screen. Typing still works.',
    ['client.chat.timestamps'] = 'Puts the time in front of each chat line.',
    ['client.chat.timestamps.system'] = 'Also stamps the game\'s own system lines.',
    ['client.chat.timestamps.style'] = 'How the time is written.',
    ['client.chat.timestamps.seconds'] = 'Includes the seconds.',
    ['client.chat.timestamps.hour12'] = 'Uses 12-hour time with am/pm.',
    ['client.chat.timestamps.ampm'] = 'How am/pm is written.',
    ['client.chat.timestamps.color'] = 'The time\'s colour.',
    -- Chat layout (percent of the screen)
    ['client.chat.x'] = 'Where the left side of the chat window is, as a percent of the screen.',
    ['client.chat.y'] = 'Where the bottom of the chat window is, as a percent of the screen.',
    ['client.chat.width'] = 'How wide the chat window is, as a percent of the screen.',
    ['client.chat2.x'] = 'Where the left side of the second chat window is.',
    ['client.chat2.y'] = 'Where the bottom of the second chat window is.',
    ['client.chat2.width'] = 'How wide the second chat window is.',
    ['client.input.x'] = 'Where the left side of the chat input line is.',
    ['client.input.y'] = 'Where the bottom of the chat input line is.',
    ['client.compass.move'] = 'Where the compass and clock go.\n'
        .. 'Game: where the game puts them.\n'
        .. 'Follow the chat: on top of the chat window, wherever you move it.\n'
        .. 'Custom: where you set Left edge and Bottom edge below.',
    ['client.compass.x'] = 'Where the middle of the compass is.',
    ['client.compass.hide'] = 'Takes the compass away. Unload FancyCompass first: it uses the same switch.',
    ['client.compass.y'] = 'Where the bottom of the compass is.',
    ['client.clock.hide'] = "Takes away the line under the compass: the Vana'diel time, your position, the day's icon and the weather's.",
    ['client.clock.always'] = "Keeps the clock on screen while a menu is open (Status, Equipment, Items and the like take it away). The HUD's own hide key still hides it.",
    ['client.clock.move'] = "Puts the clock's line where X and Y say instead of under the compass. Shift + drag its box (the eye) to move it.",
    ['client.clock.x'] = 'Where the clock goes across: its left edge, middle or right edge, by Alignment.',
    ['client.clock.y'] = 'Where the top of the clock is.',
    ['client.clock.align'] = 'Which part of the clock X places: its left edge, its middle or its right edge. Right keeps its right edge still as the text changes length.',
    ['client.menus.move'] = 'Where the windows over the chat go, such as an NPC\'s choices.\n'
        .. 'Game: where the game puts them.\n'
        .. 'Follow the chat: on top of the chat window, wherever you move it.\n'
        .. 'Custom: where you set Left edge and Bottom edge below.',
    ['client.menus.x'] = 'Where the left side of the chat-area menus is.',
    ['client.menus.y'] = 'Where the bottom of the chat-area menus is.',
    ['client.menu.rows'] = 'How many choices a selection menu (such as an NPC\'s) shows before it scrolls.',
    ['client.menu.hovermove'] = 'A mouse pointer resting on a menu no longer picks the row under it. The game does, pulling the cursor back after a key or the pad moves it (a menu that seems stuck until you move the mouse off it), and opening a menu on the row under the pointer instead of the one it remembers. With this on the mouse picks a row only while it moves or a button is down.\n'
        .. 'Off: the game\'s own.',
    ['client.menu.nomouse'] = 'The mouse pointer no longer moves the cursor in a menu or hands the keys to a window by being over it (the Items list\'s sort box, a storage\'s window), moving or not. For playing with the keys or a pad. A click still acts on the row under it.\n'
        .. 'Off: the game\'s own.',
    ['client.menus.ontop'] = 'The game\'s windows (menus, the action menu, the macro bar, item details, shops) are drawn over fancychat\'s chat, and over the windows of any addon that asks. With fancychat\'s "Prevent obstructing FFXI UI" on, the chat also stays where it is.\n'
        .. 'Off: fancychat draws over them, or slides out of their way, as it does in Ashita.',
    ['client.items.rows'] = 'How many rows the Items list, the auction house\'s Sell list and the Mog House lists (View House) show before they scroll.',
    ['client.auction.rows'] = 'How many rows the auction house\'s item list shows before it scrolls.',
    ['client.shop.rows'] = 'How many rows a shop\'s list (buying or selling) shows before it scrolls.',
    ['client.equip.rows'] = 'How many rows the Equipment menu\'s list of what fits a slot shows before it scrolls.',
    ['client.lists.rows'] = 'How many rows the lists over the chat (Magic, Abilities, Key Items...) show before they scroll.',
    ['client.currencies.rows'] = 'How many rows the Currencies lists (Status > Currencies, Currencies 2) show before they scroll.',
    ['client.shop.total'] = 'When you pick how many to buy in a shop, the chat says what it will cost in total.',
    ['client.items.autosort'] = 'Sorts the Items list and the Mog House lists (safe, storage, wardrobes...) every time they open, as the Sort option does.',
    ['client.items.sort'] = 'The order the Sort option and auto-sort put items in.\nGame: by kind, as the game sorts. A-Z: by name.',
    ['client.spells.autosort'] = 'Puts the Magic lists in A-Z order every time they open. Rearrange orders are replaced.',
    -- Inventory count
    ['client.items.count'] = 'Replaces the small "17/80" on bag windows with one number in True Font, bigger and sharper.',
    ['client.items.count.font.family'] = 'This area\'s own font, or the default font.',
    ['client.items.count.font.size'] = 'The font\'s size, in pixels at the interface\'s size.',
    ['client.items.count.x'] = 'Moves the number left or right.',
    ['client.items.count.y'] = 'Moves the number up or down.',
    ['client.items.count.color'] = 'The number\'s colour.',
    ['client.items.count.hidetotal'] = 'Shows only how many you have, not "/80".',
    ['client.items.count.separator'] = 'How the count and total are written.',
    ['client.items.count.autototal'] = 'Draws the total as big as the count.',
    -- Item square, rarity
    ['client.items.slot.color'] = 'The square\'s colour.',
    ['client.items.slot.brightness'] = 'Lighter or darker square.',
    ['client.items.slot.contrast'] = 'How strong the square\'s light and dark edges are.',
    ['client.items.slot.opacity'] = 'How see-through the square is.',
    ['client.items.equip.background.opacity'] = 'How solid the Equipment squares\' background is.',
    ['client.items.equip.border.opacity'] = 'How solid the Equipment squares\' border is.',
    ['client.items.equip.background'] = 'The Equipment squares\' background colour.',
    ['client.items.equip.border'] = 'The Equipment squares\' border colour.',
    ['client.items.equip.border.size'] = 'How thick the border is. 0: no border.',
    ['client.items.equip.wardrobe'] = 'The number on an equipped item that says which wardrobe it\'s in.',
    ['client.items.rarity.preset'] = 'Sets the four +1 to +4 colours below. You can still change each one after.\n'
        .. 'SoulspireXI Default: the game\'s usual grey, blue, gold, green.\n'
        .. 'Uncommon to Mythic: green, blue, gold, purple.\n'
        .. 'Classic MMO: green, blue, purple, orange.\n'
        .. 'Action RPG: blue, yellow, tan, green.\n'
        .. 'Guild Wars style: green, yellow, orange, pink.\n'
        .. 'Destiny style: green, blue, purple, gold.',
    ['client.items.rarity.color1'] = 'The box colour for +1 items.',
    ['client.items.rarity.color2'] = 'The box colour for +2 items.',
    ['client.items.rarity.color3'] = 'The box colour for +3 items.',
    ['client.items.rarity.color4'] = 'The box colour for +4 items.',
    ['client.items.rarity.names'] = 'Colours an item\'s name to match its rarity box.',
    ['client.items.equipped'] = 'The colour of an equipped item\'s name in item lists and its details, over its rarity colour. Off: plain, like the rest.',
    ['client.items.equipped.color'] = 'The equipped items\' name colour.',
    -- Network window
    ['client.network.icon'] = 'The little symbol beside the connection numbers.\n'
        .. 'Arrows (game): the game\'s own arrows.\n'
        .. 'Dot: one dot, green, yellow or red by how good the connection is.\n'
        .. 'None: no symbol.',
    ['client.network.layout'] = 'Game: the game\'s usual stacked layout.\n'
        .. 'One line: everything on one row.',
    ['client.network.percent'] = 'Shows the connection quality as a percent.',
    ['client.network.sent'] = 'Shows how much you are sending.',
    ['client.network.received'] = 'Shows how much you are receiving.',
    ['client.network.simple'] = 'Shows just the word Connected, Failing or Disconnected.',
    ['client.network.simple.green'] = 'Makes the word "Connected" green.',
    ['client.network.hide'] = 'Hides it all while no menu is up, as the window\'s frame is.',
    ['client.network.panel'] = 'Takes the whole window away, its frame and its numbers.',
    ['client.network.background'] = 'Off takes away the window\'s frame and its "Network" label; the numbers stay.',
    ['client.helpbar.background'] = 'The bar across the top while the menu is open. Off: no frame behind the description.',
    ['client.helptitle.hide'] = 'Hides the title box, its frame and the page\'s name.',
    ['client.helptitle.background'] = 'Off: no frame behind the title box; the page\'s name stays.',
    ['client.helpbar.text'] = 'Off takes away the description of the command under the cursor.',
    ['client.helpbar.label'] = 'Off takes away the small "Help" caption at the bar\'s right end.',
    ['client.helpbar.width'] = 'How wide the help line is, a percent of the screen. Reset: the game\'s width.',
    ['client.network.x'] = 'Moves the window left or right.',
    ['client.network.y'] = 'Moves the window up or down.',
    ['client.buffs.move'] = 'Puts your status icons (the buffs under the help bar) where the sliders say. Game puts them back where the game has them.',
    ['client.buffs.x'] = 'Where the left edge of the status icons is, as a percent of the screen.',
    ['client.buffs.y'] = 'Where the bottom of the first row of status icons is, as a percent of the screen. More rows go below it.',
    ['client.buffs.scale'] = 'How big the status icons and their timers are, as a percent of the game\'s. The icons grow from the box\'s top left, with the square that picks one, and a click lands on the icon it is on.',
    ['client.buffs.columns'] = 'How many status icons go in a row before the next row starts. The game\'s is 9; 32 puts every status on one line.',
    ['client.buffs.font'] = 'What draws the status icons\' timers. Follows General until it\'s set here.',
    ['client.buffs.font.family'] = 'This area\'s own font, or the default font.',
    ['client.buffs.font.size'] = 'The font\'s size, in pixels at the interface\'s size. Each timer stays centred under its icon.',
    ['client.map.fullscreen'] = 'The map (Main menu > Map) over the whole screen, as the game has it. Off, it\'s a square box you place.',
    ['client.map.scale'] = 'The box\'s size, as a percent of the screen\'s height.',
    ['client.map.opacity'] = 'How see-through the map\'s picture is. Its markers stay solid.',
    ['client.map.border'] = 'A 2 px black border with rounded corners and a drop shadow round the box.',
    ['client.map.background'] = 'Fills the box behind the map with a colour, where its torn edges show the game through.',
    ['client.map.wheel'] = 'The mouse wheel over the map zooms it, not the camera.',
    ['client.map.drag'] = 'Click and drag the map to scroll it.',
    ['client.map.track'] = 'Your marker on the map moves as you do. Off, it stays where you were when you opened the map, as in the game.',
    ['client.map.walk'] = 'In the box, you keep walking and turning the camera while the map is open. Off, its keys scroll the map, as in the game.',
    ['client.map.center'] = 'The map opens with you in the middle of it.',
    ['client.map.pointzoom'] = 'How far the map zooms in on a home point picked at a crystal. 100% doesn\'t zoom.',
    ['client.map.instantclose'] = 'The map goes the moment you close it, instead of fading out.',
    ['client.regionmap.fullscreen'] = 'The region maps (Region Info > Conquest, Besieged, Campaign, Colonization) over the whole screen, as the game has them. Off, they\'re a square box you place.',
    ['client.regionmap.scale'] = 'The box\'s size, as a percent of the screen\'s height.',
    ['client.regionmap.opacity'] = 'How see-through the map\'s picture is. Its markers and zone names stay solid.',
    ['client.regionmap.border'] = 'A 2 px black border with rounded corners and a drop shadow round the box.',
    ['client.regionmap.background'] = 'Fills the box behind the map with a colour, where its edges show the game through.',
    ['client.regionmap.wheel'] = 'The mouse wheel over the map zooms it, not the camera.',
    ['client.regionmap.drag'] = 'Click and drag the map to scroll it.',
    ['client.regionmap.walk'] = 'In the box, you keep walking and turning the camera while the map is open. Off, its keys scroll the map, as in the game.',
    ['client.regionmap.center'] = 'The map opens with your zone in the middle of it.',
    ['client.regionmap.instantclose'] = 'The map goes the moment you close it, instead of fading out.',
    ['client.target.hide'] = 'Hides the Target window (the target\'s name and HP bar). The arrow over the target stays, and targeting is unchanged.',
    ['client.castbar.hide'] = 'Hides the bar that fills while you cast a spell or use an item. Casting is unchanged.',
    ['client.castbar.background'] = 'Off: no frame behind the cast bar; the bar and its percent stay.',
    ['client.castbar.move'] = 'Puts the cast bar where the sliders say. Off, the game places it.',
    ['client.castbar.x'] = 'Where the middle of the cast bar is, as a percent of the screen.',
    ['client.castbar.y'] = 'Where the top of the cast bar is, as a percent of the screen.',
    ['client.target.move'] = 'Puts the Target window (the target\'s name and HP bar) where the sliders say: the top middle by default.',
    ['client.target.x'] = 'Where the middle of the Target window is, as a percent of the screen.',
    ['client.target.y'] = 'Where the top of the Target window is, as a percent of the screen.',
    ['client.target.width'] = 'How wide the Target window is, in the interface units (the game has 112). The HP bar grows with it.',
    ['client.target.background'] = 'The frame\'s backdrop. Off, the name and HP bar stand alone.',
    ['client.target.label'] = 'The small "Target" title on the window\'s top edge.',
    ['client.target.label.x'] = 'Moves the "Target" title left or right. It stays in the middle as the window gets wider.',
    ['client.target.label.y'] = 'Moves the "Target" title up or down.',
    ['client.target.title.font'] = 'What draws the "Target" title. Each setting follows True Font\'s Titles until it\'s set here.',
    ['client.target.title.font.family'] = 'The title\'s own font, or the default font. Unset, the titles\' font.',
    ['client.target.title.font.size'] = 'The title\'s size, in pixels at the interface\'s size. Unset, the titles\' size. 0 hides it.',
    ['client.target.font'] = 'What draws the target\'s name. Each setting follows Labels & Stats until it\'s set here.',
    ['client.target.font.family'] = 'The name\'s own font, or the default font. Unset, Labels & Stats\' font.',
    ['client.target.font.size'] = 'The name\'s size, in pixels at the interface\'s size. Unset, Labels & Stats\' size. 0 hides it.',
    ['client.target.lock.label'] = 'The "Locked" under the window when you\'re locked on (the game\'s lock-on picture). Off, its corner glows and side bars stay.',
    ['client.target.padding.x'] = 'Room added at the window\'s left and right. The title, name and bar move in with it; Width stays the room inside.',
    ['client.target.padding.y'] = 'Room added at the window\'s top and bottom. The title, name and bar move down with it.',
    ['client.target.spacing'] = 'More room between the title and the name, and between the name and the bar. The window grows to fit.',
    ['client.target.name.align'] = 'Where the target\'s name sits across the window: the game\'s left, the middle, or the right. A name too long for one line stays at the left.',
    ['client.target.name.x'] = 'Moves the target\'s name left or right.',
    ['client.target.name.y'] = 'Moves the target\'s name up or down.',
    ['client.target.lock.label.x'] = 'Moves the "Locked" of the game\'s lock-on picture left or right. It stays in the middle as the window gets wider.',
    ['client.target.lock.label.y'] = 'Moves the "Locked" of the game\'s lock-on picture up or down.',
    ['client.target.bar.x'] = 'Moves the HP bar left or right inside the window.',
    ['client.target.bar.y'] = 'Moves the HP bar up or down inside the window.',
    ['client.target.bar.mask'] = 'The white outline with blue ends behind the HP bar. Off, only the colour bar shows.',
    ['client.target.bar.outline'] = 'When the window is another width the game outline is left out and VanaCore draws a thin white one round the bar.',
    ['client.target.lock'] = 'What shows on a locked-on target: the game\'s picture, a glowing border, or nothing. Both follow the window\'s width and its cut or rounded corners.',
    ['client.target.lock.color'] = 'The glowing border\'s colour.',
    ['client.target.lock.pulse'] = 'The glow slowly brightens and dims.',
    ['client.target.arrow'] = 'The arrow the game draws over the target. Off, only the Target window marks it; the sub-target cursor stays. Addons can draw their own picture in its place.',
    ['client.network.dot.size'] = 'How big the dot icon is.',
    -- Status window
    ['client.status'] = 'Redraws the Status window (main menu > Status) with the options below.',
    ['client.status.name.font'] = 'Your name at the top of the window in its own True Font. Off: the bigger menu-title letters.',
    ['client.status.name.font.family'] = 'The name\'s own font, or the default font.',
    ['client.status.name.font.size'] = 'The name\'s size, in pixels at the interface\'s size. A name too wide for the window is drawn smaller. 0 hides it.',
    ['client.status.jobs'] = 'How your job and level are written.',
    ['client.status.itemlevel'] = 'Shows your item level.',
    ['client.status.taller'] = 'Spaces the rows out and makes the window taller.',
    ['client.status.separator'] = 'A line between the name block and the stats.',
    ['client.status.separator.color'] = 'The line\'s colour.',
    ['client.status.colors'] = 'Colours the HP, MP and TP numbers.',
    ['client.status.hp.color'] = 'The HP colour.',
    ['client.status.mp.color'] = 'The MP colour.',
    ['client.status.tp.color'] = 'The TP colour.',
    ['client.status.exp.level'] = 'Shows your level in the EXP window.',
    ['client.status.exp.shortnext'] = 'Writes NEXT instead of NEXT Level.',
    ['client.equip.stats'] = 'Makes the Equipment window taller, with your accuracy, attack, defense, evasion and everything your gear adds (haste, skills, latent effects that are on, ...), in place of its Attack and Defense row.',
    ['client.equip.stats.checkparam'] = 'Asks the server for your accuracy, attack, defense and evasion (a /checkparam kept out of the chat). Off: what the gear adds.',
    ['client.equip.stats.move'] = 'Puts the stats in a box of their own, which the HUD Manager moves. Off, they sit under the Equipment window.',
    ['client.equip.stats.check'] = "Shows what the gear of someone you /check adds (haste, store TP, damage taken, ...) in a box under the Check window. Latent effects aren't counted.",
    ['client.equip.stats.check.move'] = 'Puts the /check stats in a box of their own, which the HUD Manager moves. Off, they sit under the Check window.',
    ['client.equip.stats.check.x'] = "Where the left edge of the /check stats' box is, as a percent of the screen.",
    ['client.equip.stats.check.y'] = "Where the top of the /check stats' box is, as a percent of the screen.",
    ['client.equip.stats.x'] = "Where the left edge of the stats' box is, as a percent of the screen.",
    ['client.equip.stats.y'] = "Where the top of the stats' box is, as a percent of the screen.",
    -- Nameplates
    ['graphics.nameplates.font'] = 'The letters names are drawn with.\n'
        .. 'Game\'s: the game\'s own letters. Soft when made bigger.\n'
        .. 'True Font: a real font, sharp at any size. Pick it below.',
    ['graphics.nameplates.family'] = 'Which font True Font uses. Default font is the one on the True Font page.',
    ['graphics.nameplates.bold'] = 'Makes True Font bold.',
    ['graphics.nameplates.italic'] = 'Makes True Font italic.',
    ['graphics.nameplates.uppercase'] = 'Writes names in capital letters.',
    ['graphics.nameplates.textspacing'] = 'The space between the letters of True Font.',
    ['graphics.nameplates.fontsize'] = 'How big names are up close, in pixels, the same as the other True Font sizes.\n'
        .. 'Farther names shrink as Distance scaling says, and Size makes the whole nameplate bigger or smaller.',
    ['graphics.nameplates.gap'] = 'The space between the letters of names over heads.',
    ['graphics.nameplates.size'] = 'How big names over heads are.',
    ['graphics.nameplates.distance'] = 'How much names shrink with distance. 0% keeps them all one size.',
    ['graphics.nameplates.order'] = 'Draws nearer names over farther ones.',
    ['client.target.name.oneline'] = 'Keeps the Target window\'s name on one line with the game\'s letters: a door\'s "Door:Chamber of\n'
        .. 'Commerce", which the game splits at the colon, and a name too long for the window, which runs past its edge.\n'
        .. 'With the name\'s True Font on, it\'s always one line, every letter in that font.',
    ['graphics.nameplates.blink'] ='The name of what you target fades in and out, as the game has it. Off keeps it steady.',
    ['graphics.nameplates.framegen'] = 'With frame generation on, draws names in the frames it makes where they are at that moment,\n'
        .. 'so they keep up with the camera. Off: names step at the game\'s own frame rate.',
    ['graphics.nameplates.hideown'] = 'Hides the name over your own head.',
    ['graphics.nameplates.hideparty'] = 'Hides the names over your party\'s other members, trusts too.',
    ['graphics.nameplates.hidealliance'] = 'Hides the names over the members of the alliance\'s other two parties.',
    ['graphics.nameplates.hideplayers'] = 'Hides the names over other players.',
    ['graphics.nameplates.hidenpcs'] = 'Hides the names over NPCs.',
    ['graphics.nameplates.hidemonsters'] = 'Hides the names over monsters.',
    ['graphics.nameplates.hidetrusts'] = 'Hides the names over trusts.',
    ['graphics.nameplates.hidepets'] = 'Hides the names over players\' pets.',
    ['graphics.nameplates.bazaar'] = 'Shows the bag icon by players with a bazaar.',
    ['graphics.nameplates.linkshell'] = 'Shows the linkshell pearl by names.',
    ['graphics.nameplates.gm'] = "Server staff icons in place of the game's GM icon, over names and in the Target window.",
    ['graphics.nameplates.pearl.x'] = 'Moves the linkshell pearl right (closer to the name) or left.',
    ['graphics.nameplates.pearl.y'] = 'Moves the linkshell pearl down or up.',
    ['graphics.nameplates.pearl.shape'] = 'How wide the icons by names (linkshell pearl, bazaar bag, the game\'s GM icon) are against\n'
        .. 'their height, in any window. The staff GM icons are always square.',
    ['graphics.nameplates.pearl.size'] = 'How big the linkshell pearl is.',
    ['graphics.nameplates.bazaar.x'] = 'Moves the bazaar bag right or left, from where the pearl settings put it.',
    ['graphics.nameplates.bazaar.y'] = 'Moves the bazaar bag down or up, from where the pearl settings put it.',
    ['graphics.nameplates.bazaar.size'] = 'How big the bazaar bag is, times the pearl size.',
    ['graphics.nameplates.gm.x'] = 'Moves the staff GM icon right (closer to the name) or left.',
    ['graphics.nameplates.gm.size'] = 'How big the staff GM icon is.',
    ['graphics.nameplates.characteronly'] = 'Applies the positioning below to other players and you only, not NPCs or monsters.',
    ['graphics.nameplates.center'] = 'Centres the name\'s letters over the head; the linkshell pearl hangs left of them.',
    ['graphics.nameplates.plate.x'] = 'Moves the whole nameplate (pearl, name and stars) right or left.',
    ['graphics.nameplates.plate.y'] = 'Moves the whole nameplate down or up.',
    ['graphics.nameplates.name.x'] = 'Moves the name\'s letters right or left, apart from the pearl and stars.',
    ['graphics.nameplates.name.y'] = 'Moves the name\'s letters down or up.',
    ['graphics.nameplates.stars.x'] = 'Moves the mastery stars right or left.',
    ['graphics.nameplates.stars.y'] = 'Moves the mastery stars down or up.',
    ['graphics.nameplates.stars.spacing'] = 'How far apart the mastery stars are, as a share of the game\'s spacing. Lower draws them closer\n'
        .. 'together, 0% all in one place; the line stays centred where it was.',
    ['graphics.nameplates.stars.size'] = 'How big the mastery stars are; the line grows about its middle.',
    ['graphics.nameplates.mastery'] = 'Shows the mastery stars by names.',
    ['graphics.nameplates.stars.style'] = 'The mastery stars as the game draws them, only the big star, or three small stars.',
    ['graphics.nameplates.color.players'] = 'Other players\' name colour.',
    ['graphics.nameplates.color.npcs'] = 'NPC name colour.',
    ['graphics.nameplates.color.monsters'] = 'Name colour of monsters nobody has claimed.',
    ['graphics.nameplates.color.claimed'] = 'Name colour of monsters someone else has claimed.',
    ['graphics.nameplates.color.mine'] = 'Name colour of monsters you have claimed.',
    ['graphics.nameplates.color.trusts'] = 'Trust name colour.',
    ['graphics.nameplates.color.pets'] = 'Pet name colour.',
    -- Damage numbers
    ['graphics.damage.font'] = 'The letters damage numbers and Miss! are drawn with.\n'
        .. 'Game\'s: the game\'s own numbers. Soft, as they\'re blown up.\n'
        .. 'True Font: a real font, sharp at any size. Pick it below.',
    ['graphics.damage.ontop'] = 'Draws damage numbers over everything, so characters in front and names don\'t hide them.',
    ['graphics.damage.family'] = 'Which font damage numbers use.',
    ['graphics.damage.bold'] = 'Makes damage numbers bold.',
    ['graphics.damage.italic'] = 'Makes damage numbers italic.',
    ['graphics.damage.textsize'] = 'How big damage numbers are next to the game\'s.',
    ['graphics.damage.outline'] = 'How thick the dark outline round damage numbers is.',
    ['graphics.damage.outline.color'] = 'The colour of the outline round damage numbers. Clear it for black.',
    ['graphics.damage.gradient'] = 'Hits fade from their colour to their second colour. Off, they are one colour.',
    ['graphics.damage.color2'] = 'Where hits fade to with Gradient on.',
    ['graphics.damage.miss.gradient'] = 'Miss! fades from its colour to its second colour. Off, it is one colour.',
    ['graphics.damage.miss.color2'] = 'Where Miss! fades to with Gradient on.',
    ['graphics.damage.taken.gradient'] = 'Hits on you fade from their colour to their second colour. Off, they are one colour.',
    ['graphics.damage.taken.color2'] = 'Where hits on you fade to with Gradient on.',
    ['graphics.damage.heal.gradient'] = 'Heals fade from their colour to their second colour. Off, they are one colour.',
    ['graphics.damage.heal.color2'] = 'Where heals fade to with Gradient on.',
    ['graphics.damage.crit.gradient'] = 'Critical hits fade from their colour to their second colour. Off, they are one colour.',
    ['graphics.damage.crit.color2'] = 'Where critical hits fade to with Gradient on.',
    ['graphics.damage.gradient.direction'] = 'Which way every gradient runs: from the top of the number to its bottom, or from its left to its right.',
    ['graphics.damage.min'] = 'Far-off damage numbers are never smaller than this. 0 for no limit.',
    ['graphics.damage.max'] = 'Close-up damage numbers are never bigger than this. 0 for no limit.',
    ['graphics.damage.commas'] = 'Writes 1,337 instead of 1337.',
    ['graphics.damage.misscaps'] = 'Writes MISS! instead of Miss!.',
    ['graphics.damage.crit'] = 'Shows critical hits in their own colour.',
    ['graphics.damage.crit.color'] = 'The colour of critical hits.',
    ['graphics.damage.crit.size'] = 'How much bigger critical hits are.',
    ['graphics.damage.crit.exclaim'] = 'Writes critical hits as 1200!!.',
    ['graphics.damage.preview'] = 'Shows sample damage numbers on screen while you change these settings.',
    ['graphics.damage.duration'] = 'How long a damage number stays on screen. The game\'s is 2.5 seconds.',
    ['graphics.damage.spawn'] = 'Where a damage number first appears.\n'
        .. 'Game\'s: where the game puts it.\n'
        .. 'Stacked: numbers for the same target that come together stack one above the next.\n'
        .. 'Random: anywhere within Spread of where the game puts it, so they don\'t sit on top of each other.',
    ['graphics.damage.spread'] = 'Random: how far from the game\'s spot a number can appear. Stacked: the gap between numbers. In number heights.',
    ['graphics.damage.offset.x'] = 'Moves every damage number left (-) or right (+), in number heights.',
    ['graphics.damage.offset.y'] = 'Moves every damage number down (-) or up (+), in number heights.',
    ['graphics.damage.color'] = 'The colour of damage numbers, instead of the game\'s. Critical hits keep their own colour. Clear it for the game\'s.',
    ['graphics.damage.miss.color'] = 'The colour of Miss!, instead of the game\'s. Clear it for the game\'s.',
    ['graphics.damage.taken.color'] = 'The colour of damage numbers on you, instead of the Hit colours. Clear it to use those.',
    ['graphics.damage.heal.plus'] = 'Writes HP recovered with a plus in front: +530.',
    ['graphics.damage.heal.color'] = 'The colour of numbers for HP recovered (a Cure, a Curing Waltz), instead of the game\'s. The Hit and Hit taken colours never apply to a heal. Clear it for the game\'s.',
    ['graphics.damage.multi'] = 'Puts an icon beside each number from a round of several hits on one target, such as a double attack or a weapon skill with several hits.',
    ['graphics.damage.multi.icon'] = 'Crossed swords, or how many hits the round had (x2).',
    ['graphics.damage.multi.size'] = 'How big the icon is next to the number.',
    ['graphics.damage.label'] = 'Writes the name of the weapon skill, spell or ability above the number it did.',
    ['graphics.damage.label.size'] = 'How big the skill name is next to the number.',
    -- Other
    ['addons.delay.login'] = 'Waits until a character is in the world before loading addons. Plugins always load at start.',
    ['client.autologin'] = 'Accepts the terms, picks Select Character, then the character below and Yes. Once per game start. Hold Shift at the title screen to stop it.',
    ['client.autologin.character'] = 'Which character in the list Auto Login plays: 1 is the first.',
    ['vana.scale'] = 'How big this window is. /vana scale 2 in the chat does it too.',
    ['vana.hud.grid'] = 'While this page (not its Presets) or the HUD Manager is open, a grid over the whole game screen to line the windows up by, laid out from the screen\'s centre, which two 3-pixel lines mark.',
    ['vana.hud.grid.size'] = 'How far apart the grid lines are, in screen pixels.',
    ['vana.hud.grid.color'] = 'The colour of the grid lines.',
    ['vana.hud.grid.opacity'] = 'How solid the grid lines are.',
    ['vana.hud.grid.center.opacity'] = 'How solid the two 3-pixel lines through the screen\'s centre are.',
    ['vana.hud.grid.background'] = 'Fills the screen under the grid lines with a colour, so the game behind is dimmed.',
    ['vana.hud.grid.background.color'] = 'The colour under the grid lines.',
    ['vana.hud.grid.background.opacity'] = 'How solid the colour under the grid lines is.',
    ['client.mainmenu.background'] = 'Off: no frame behind the main menu or the menus it opens in its place; the commands stay.',
    ['client.statusmenu.background'] = 'Off: no frame behind the list Status opens (and Unity\'s and Merit Points\'); the commands stay.',
    ['client.statusmenu.solid'] = 'With the frame off: the commands drawn solid, as they look on the frame.',
    ['client.action.background'] = 'Off: no frame behind the action menu (Attack, Magic, ...); the commands stay.',
    ['client.action.solid'] = 'With the frame off: the commands drawn solid, as they look on the frame.',
    ['client.mainmenu.solid'] = 'With the frame off: the commands drawn solid, as they look on the frame (the menus it opens too).',
    ['client.sortconfirm.background'] = 'Off: no frame behind the Yes/No an Options box\'s Auto asks; Yes and No stay.',
    ['client.sortconfirm.solid'] = 'With the frame off: Yes and No drawn solid, as they look on the frame.',
    ['client.macrooptions.background'] = 'Off: no frame behind the box an equip set or a macro book opens; its commands stay.',
    ['client.macrooptions.solid'] = 'With the frame off: its commands drawn solid, as they look on the frame.',
    ['client.macroeditoptions.background'] = 'Off: no frame behind the box at the right while macros are edited; its commands stay.',
    ['client.macroeditoptions.solid'] = 'With the frame off: its commands drawn solid, as they look on the frame.',
    ['client.macroedit.background'] = 'Off: no frame behind the macro bar being edited and the macro editor; the macros and lines stay.',
    ['client.spellinfo.hide'] = 'Hides the box beside the Magic and Abilities lists (MP cost, recast).',
    ['client.spellinfo.background'] = 'Off: no frame behind that box; the MP and recast stay.',
    ['client.action.hide'] = 'Hides the action menu (Attack, Magic, ...). Its keys still work.',
    ['client.actionmagic.hide'] = 'Hides the list the action menu\'s Magic opens (Black Magic, Songs, Ninjutsu...). Its keys still work.',
    ['client.actionmagic.background'] = 'Off: no frame behind that list; the choices stay.',
    ['client.actionability.hide'] = 'Hides the list the action menu\'s Abilities opens (Job Abil., Weapon Skl., Mount...). Its keys still work.',
    ['client.actionability.background'] = 'Off: no frame behind that list; the choices stay.',
    ['client.actionlist.hide'] = 'Hides the Magic and Abilities lists (the spells, the abilities). Their keys still work.',
    ['client.actionlist.background'] = 'Off: no frame behind those lists; the choices stay.',
    ['client.actionmount.hide'] = 'Hides the list the action menu\'s Mounts opens (Raptor, Tiger, Coeurl...). Its keys still work.',
    ['client.actionmount.background'] = 'Off: no frame behind that list; the choices stay.',
    ['client.jobchange.hide'] = 'Hides the Change Jobs list. Its keys still work.',
    ['client.jobchange.background'] = 'Off: no frame behind the Change Jobs list; the jobs stay.',
    ['client.auction.hide'] = 'Hides the auction house\'s menus. Its keys still work.',
    ['client.auction.background'] = 'Off: no frame behind the auction house\'s menus; the choices stay.',
    ['client.checkwin.hide'] = 'Hides the Check window (another\'s gear).',
    ['client.checkwin.background'] = 'Off: no frame behind the Check window; the gear stays.',
    ['client.commentwin.hide'] = 'Hides the comment box (a checked player\'s bazaar message, a search comment).',
    ['client.commentwin.background'] = 'Off: no frame behind the comment box; the text stays.',
    ['client.commentwin.keep'] = 'Keeps a checked player\'s bazaar message up while the cursor is on their gear (the game closes it for the item\'s details).',
    ['client.helpbar.hide'] = 'Hides the help line. The title box has its own Hide.',
    ['client.hpgauge.background'] = 'Off: no frame behind the HP gauge; the bars stay.',
    ['client.battletimer.hide'] = 'Hides the time left in a battlefield or an instance.',
    ['client.battletimer.background'] = 'Off: no frame behind the battle timer; the time stays.',
    ['client.shopconfirm.background'] = 'Off: no frame behind a shop\'s Yes/No; the choices stay.',
    ['client.inventory.hide'] = 'Hides the Items list. Its keys still work.',
    ['client.inventory.background'] = 'Off: no frame behind the Items list; the items stay.',
    ['client.itemdetails.hide'] = 'Hides every item\'s description.',
    ['client.itemdetails.background'] = 'Off: no frame behind an item\'s description; the text stays.',
    ['client.macrobar.background'] = 'Off: no frame behind the macro bar; the macros stay.',
    ['client.menucontent.background'] = 'Off: no frames behind Status, Equipment, Quests and the other menus at the top left.',
    ['client.storagemenu.hide'] = 'Hides the list of bags beside Equipment and Items.',
    ['client.storagemenu.background'] = 'Off: no frame behind the list of bags; the bags stay.',
    ['client.target.background'] = 'Off: no frame behind the Target window; the name and HP bar stay.',
    ['client.menus.hide'] = 'Hides the menus over the chat (an NPC\'s choices, the Magic and Abilities lists). Their keys still work.',
    ['client.menus.background'] = 'Off: no frame behind the menus over the chat; the choices stay.',
    ['client.singlemenu'] = "The main menu's second page (Missions to Log Out) under the first, one list with no flipping. Applies once the menu is closed.",
    ['client.gamesettings.extended'] = "Config > Gameplay gets World Sounds Volume and Interface Sounds Volume under Music Volume. English menus only. Applies once the window is closed.",
    ['client.macrooptions.equipfirst'] = "An equip set's options (Edit, Copy ... Style Lock) with Equip at the top, the cursor starting on it. Applies the next time the box opens.",
    ['vana.hud.names'] = "Writes each window's name in its box's top left corner.",
    ['vana.hud.autosave'] = 'The HUD preset you loaded is kept up to date: every change to the HUD and Game Options settings is saved back into it.',
    ['vana.hud.snap'] = 'A window moved with Shift + drag snaps to the grid: the edge nearer a line goes to it. Works with the grid hidden too.',
}
-- Every True Font size goes down to 0, which leaves its letters out (chatfont.cpp read_want): said on each one's tooltip
for key, text in pairs(TIPS.rows) do
    if key:match('^client%..+%.font.*%.size$') then
        TIPS.rows[key] = text .. ' 0 hides the letters.'
    end
end

-- The panel titles the same way: header or sub-panel text -> text.
TIPS.panels = {
    ['Game'] = 'The game window: its mode, size and place.',
    ['Toggles'] = 'Every effect as one switch: green is on, red is off. Turn them on one at a time to show what each adds.',
    ['DLSS / FSR'] = 'DLSS (NVIDIA, an RTX card) or FSR (AMD, any DirectX 12 card): draws smaller, then scales up for more FPS. One or the other.',
    ['DLSS'] = 'NVIDIA\'s upscaler: draws smaller, then scales up for more FPS. Needs an RTX card and DirectX 12.',
    ['DLSS 5'] = 'NVIDIA\'s AI look for the picture, on top of DLSS. Needs an RTX 40 or 50 and NVIDIA\'s DLSS 5 file in the dlss folder.',
    ['Advanced Settings'] = 'More DLSS 5 controls to try: extra passes, and blending its picture with the game\'s.',
    ['FSR'] = 'AMD\'s upscaler: draws smaller, then scales up for more FPS. Any DirectX 12 card. FSR 4 on Radeon RX 7000 and 9000.',
    ['Camera'] = 'How the game camera zooms and turns.',
    ['Chat'] = 'How chat looks and what it shows.',
    ['Chat window'] = 'The chat window\'s background and border.',
    ['Compass'] = 'The compass near the chat.',
    ['Menus'] = 'Menus the game opens over the chat.',
    ['Logging'] = 'What VanaCore writes to its log file.',
    ['Draw distance'] = 'How far away the game draws things.',
    ['Shadows'] = 'Shadows cast by the sun. Turns the game\'s own shadows off.',
    ['Ambient occlusion'] = 'Darkens corners and creases to add depth.',
    ['Reflections'] = 'Reflections on floors and flat ground.',
    ['True Font'] = 'Fonts from your PC, drawn sharp at the screen\'s own size. Chat, Damage Numbers, Item Tooltips, Windows, Inventory and Nameplates have their own.',
    ['Inventory'] = 'The item lists\' letters and rows, the bag count, the item squares and the rarity box.',
    ['Equipment'] = 'The Equipment window\'s item squares, its list of what fits a slot and the stats under it.',
    ['Slot Names'] = 'The names in the Equipment window\'s empty squares and under its items: the game\'s, True Font, or hidden.',
    ['Stats Text'] = 'The Equipment window\'s numbers and the stats under it, in their own True Font.',
    ['Equipment Squares'] = 'The squares behind the Equipment window\'s items.',
    ['Extended Stats'] = 'Your accuracy, attack, defense, evasion and what your gear adds, under the Equipment window.',
    ['Glow Effects'] = 'Fire, glows and spells light up what is near them.',
    ['Bounce light'] = 'Light bouncing off surfaces onto the ones next to them.',
    ['Height fog'] = 'Fog that is thicker near the ground.',
    ['Bloom'] = 'A soft glow round bright lights.',
    ['God rays'] = 'Beams of light streaming from the sun.',
    ['Lighting'] = 'How the world is lit through the Vana\'diel day and night, and indoors.',
    ['Force ambient light'] = 'Everything gets at least this much light, day and night, indoors and out, so nothing is too dark to see. '
        .. 'As Windower\'s game_forceambientlight.',
    ['Night'] = "How the night looks outdoors: the game's own (Off), darker (Dark) or brighter (Bright). Each keeps its own settings.",
    ['Dark night'] = 'A darker night outdoors, from dusk to dawn: the world\'s light dims and the picture turns a cool moonlit blue. '
        .. 'Lamps, fires and spells keep their light.',
    ['Bright night'] = 'A brighter night outdoors, from dusk to dawn: the moon lights the world as the sun does by day, with shadows.',
    ['Sun strength'] = 'A stronger sun outdoors by day: what faces it is lit brighter and the shade stays as it is. '
        .. 'Lower Bloom\'s threshold as well for a blown-out look.',
    ['Indoor brightness'] = 'Brightens Mog Houses, caves, dungeons and other zones with no sun in the sky. Outdoor zones are left as they are.',
    ['Player light'] = 'A light over your character. At night it lights the ground round you, with Dark night on only. '
        .. 'By day the world round you darkens a little instead, so the pool still shows.',
    ['Temperature'] = 'Tints the picture warmer by day and cooler at night, outdoors. It works through the Colour grade, so that must be on too.',
    ['Moon'] = 'The moon in the sky.',
    ['Texture filtering'] = 'How sharp textures stay in the distance and at a slant.',
    ['Water Shader (Experimental)'] = 'VanaCore\'s own moving water, with the bed showing through where it is shallow.',
    ['Mass Grass'] = 'Adds lots of grass and flowers on the ground. West Ronfaure so far.',
    ['Snow Trails (Experimental)'] = 'Footprints in the snow where characters walk. Shading only: the ground is not dug.',
    ['Colour grade'] = 'Changes the picture\'s colours and light, like a photo filter. On DirectX 12 it is applied last, over every other effect and after DLSS or FSR.',
    ['Ink lines'] = 'Cartoon-style outlines round things.',
    ['Brightness bands'] = 'Flattens the light into steps, for a cartoon look.',
    ['Depth of field'] = 'Blurs things that are not in focus.',
    ['Normal maps (Experimental)'] = 'Bump detail from a texture pack. Needs maps made with xi-tools.',
    ['Characters'] = 'Light on players, NPCs and monsters.',
    ['NPC effects'] = 'Glows and arrival effects on Trusts and avatars.',
    ['Zones'] = "Markers where you walk into the next zone, and the zone's name when you get there.",
    ['Windows'] = 'How the game\'s menu windows look.',
    ['Timestamps'] = 'The time shown in front of chat lines.',
    ['Inventory Count'] = 'The "17/80" on a bag window\'s title bar.',
    ['Item Squares'] ='The grey square behind an item\'s icon.',
    ['Item Rarity'] = 'The coloured box round a +1 to +4 item\'s icon.',
    ['Network Window'] = 'The connection window with the S and R numbers.',
    ['Status Window'] = 'The Status window in the main menu.',
    ['EXP window'] = 'The level and EXP window under the Status window.',
    ['Nameplates'] = 'The names over characters\' heads.',
    ['Developer'] = 'Logs and tools for fixing problems. Most players can leave these alone.',
    ['Commands'] = "VanaCore's own commands. Switch one off and your own addon or plugin with that name gets it instead.",
    ['Boot.txt'] = 'scripts\\boot.txt: the commands VanaCore runs when the game starts.',
    ['AI MCP'] = 'Lets an AI app on this PC look inside the game.',
    ['Startup'] = 'What happens when the game starts.',
}

-- Mass Grass's catalogue (src\core\grass_assets.cpp): the drawn blades, VanaCore's own assets and the player's library (//vana grass
-- capture), each switched on or off ([graphics] grass.asset.<id>) with its share of the mix (grass.asset.<id>.amount), in place of the
-- marker row in the Graphics tab

local function grass_asset_rows(rows)
    local out = {}
    for _, row in ipairs(rows) do
        if row.grass_assets then
            out[#out + 1] = note("What grows: the drawn blades, VanaCore's own plants and your own (//vana grass capture beside some grass or "
                                 .. 'shrubs). Switch each on and set its share: 70 and 30 grow seven of the one for three of the other.')
            local ok, assets = pcall(vana.graphics.grass_assets)
            for _, a in ipairs(ok and assets or {}) do
                local key = 'grass.asset.' .. a.id
                -- its share until set: the catalogue's (grass_assets.cpp Info.amount: a file's "default", the drawn blades' own),
                -- on when over 0; a DLL from before it says none, and Type 2's blades alone grew
                local default = a.default or (a.id == 'blades' and 100 or 0)
                local t = needs_dx(toggle(a.name, 'graphics', key, default > 0))
                t.tip = (a.shipped and "VanaCore's own" or 'From your library (config\\grass\\' .. a.id .. '.json)') .. (a.kind ~= '' and (': ' .. a.kind) or '')
                out[#out + 1] = t
                local share = needs_dx(slider('Share', 'graphics', key .. '.amount', default, 0, 100, 5, '%d'))
                share.tip = 'Its share of the mix, against the others switched on.'
                out[#out + 1] = share
            end
        else
            out[#out + 1] = row
        end
    end
    return out
end

-- Client > Commands: every command VanaCore answers itself, what it does, and a switch for each it can do without.
-- [commands] <name>=0 leaves /name and //name to the player's own addons and plugins, and the game: VanaCore.Core.dll's
-- (src/core/commands.cpp, read by addons.cpp's vanacore_command) and this window's own (vana.lua's OWN_COMMANDS). A
-- command added to either goes here too. { name, label, tooltip }
local COMMANDS = {
    { 'screenshot', 'Save a screenshot', "Saves a screenshot, as Print Screen does (its bind runs /screenshot).\n"
                                        .. "/screenshot hide leaves VanaCore's own drawing out, show keeps it, open opens the folder." },
    { 'copy', 'Copy the chat line', "Copies what's typed in the chat line to the clipboard (Ashita's /copy)." },
    { 'paste', 'Paste into the chat line', "Types the clipboard into the chat line where the caret is (Ashita's /paste)." },
    { 'cancel', 'Take an effect off you', '/cancel <status name or id> takes a status effect off you: /cancel invisible, /cancel 69.' },
    { 'freecam', 'Fly the camera', '/freecam [on|off]: the camera leaves your character and flies with W A S D and the mouse.\n'
                                   .. '/freecam float makes it glide; /freecam weather rain and /freecam time dusk show a weather '
                                   .. 'or an hour on your screen only.' },
    { 'freelook', 'Turn the camera freely', '/freelook [on|off]: the camera turns without turning your character.' },
    { 'fillmode', 'Wireframe or point world', '/fillmode solid, wireframe or point: how the world is drawn (DirectX 11 and 12). The interface stays solid.' },
    { 'exit', 'Close the game', "Closes the game as /shutdown does (Ashita's /exit)." },
    { 'terminate', 'End the game at once', "Ends the game on the spot, without /shutdown (Ashita's /terminate)." },
    { 'execstr', 'Run a line as a script', "/execstr <commands> [args]: runs the line as a script, its commands split at ; (Ashita's /execstr)." },
    { 'sleep', 'Wait in a script', 'In a script or a key bind, waits as /wait does (/sleep 2).\nOff, it runs as any other command there too.' },
    { 'pause', 'Wait in a script', 'In a script or a key bind, waits as /wait does (/pause 2).\nOff, it runs as any other command there too.' },
    { 'abtest', "Compare two settings' FPS", '/abtest <setting>=<a>,<b>: flips a graphics setting back and forth and compares the frame rate of each.\n'
                                            .. '/vana abtest does the same and stays on.' },
    { 'camtrace', 'Camera trace to the log', "Writes the camera's recent walls and lock to vanacore.log, for camera bug reports." },
    { sub = 'The /vana window' },
    { 'hud', 'Open the HUD Manager', "/hud [element] opens or closes the HUD Manager, at that window's settings.\n"
                                     .. "/vana hud manager stays, and the game's Config > HUD runs it." },
    { 'gamepad', 'Client > Gamepads', 'Opens this window at Gamepads. /gamepad test opens the Gamepad Test window.' },
    { 'gamepads', 'Client > Gamepads', 'As /gamepad.' },
    { 'addons', 'The Addons page', 'Opens this window at the Addons page.' },
    { 'plugins', 'The Plugins page', 'Opens this window at the Plugins page.' },
    { 'pivot', 'The Pivot page', "Opens this window at the Pivot page: the DAT overlays and their order." },
    { 'api', 'The API Explorer', 'Loads the API Explorer addon and shows its window, when it isn\'t loaded.\n'
                                 .. 'Once loaded, /api shows or hides it (the addon\'s own /api).' },
    { 'apiexplorer', 'The API Explorer', 'As /api.' },
    { sub = 'Always on' },
    { 'vana', 'The VanaCore window', 'Opens and closes this window. /vana help lists what else it takes.', kept = true },
    { 'addon', 'Load and unload addons', '/addon load, unload, reload and list.', kept = true },
    { 'plugin', 'Load and unload plugins', '/plugin load, unload and list.', kept = true },
    { 'exec', 'Run a script', '/exec <script> runs scripts\\<script>.txt.', kept = true },
    { 'wait', 'Wait in a script', '/wait <seconds>, in a script or a key bind.', kept = true },
    { 'bind', 'Bind a key', '/bind <key> <command>. The keybinds window (/keybinds) does it with the mouse.', kept = true },
    { 'unbind', 'Unbind a key', '/unbind <key>.', kept = true },
    { 'unbindall', 'Unbind every key', 'Takes every key bind off.', kept = true },
    { 'alias', 'Name a command yourself', '/alias <name> <command>: a name of your own for a command.\n'
                                          .. 'For /ss: /alias /ss /screenshot (a line in scripts\\boot.txt keeps it).', kept = true },
    { 'raw', 'Send a line past addons', "/raw <command> sends the command to the game as it is, past every plugin and addon (Ashita's /raw).", kept = true },
}

local function commands_rows()
    local rows = {
        header('Commands'),
        note('Switch a command off and VanaCore leaves it to your own addons and plugins. /name and //name go together.', 'terminal'),
        subheader('VanaCore'),
    }
    for _, c in ipairs(COMMANDS) do
        if c.sub ~= nil then
            rows[#rows + 1] = subheader(c.sub)
        else
            local row = toggle('/' .. c[1] .. ' - ' .. c[2], 'commands', c[1], true)
            row.tip = c[3]
            if c.kept then
                -- VanaCore needs it (commands.cpp's kKept ignores a key for it): shown on, locked, and no key to reset
                row.get, row.lock_if, row.keep = function () return true end, function () return true end, true
                row.tip = row.tip .. '\nVanaCore needs it, so it stays on.'
            else
                row.resets = true -- the section's reset turns every command back on
                row.tip = row.tip .. '\nOff: /' .. c[1] .. ' and //' .. c[1] .. ' go to your own addons and plugins, and the game.'
            end
            rows[#rows + 1] = row
        end
    end
    return rows
end

local TABS = {
    {
        -- Client's first sub-tab, VanaCore, not a page of its own (put there after TABS: /vana about or /vana vanacore
        -- opens it): the heading, the banner and Info, then Client's VanaCore settings (About and VanaCore were two
        -- sub-tabs till 2026-11-22)
        name = 'About',
        rows = function ()
            return {
                -- (its heading not drawn: the banner says it; a search still shows it. vana.lua's draw_section)
                (function (row) row.bare = true return row end)(header('VanaCore')),
                banner('SoulspireXI experimental client (based on VanaCore).'),
                subheader('Info'),
                -- the version first, its value larger (big: vana.lua's info row); Window scale after it
                (function (row) row.big = true return row end)(info('Version', function () return 'v' .. tostring(vana.version) end)),
                info('Renderer', function ()
                    local g = vana.graphics.info()
                    if g.active then
                        local shown = (g.running == 'd3d11' and 'DirectX 11 on ' or 'DirectX 12 on ') .. (g.gpu ~= '' and g.gpu or 'this graphics card')
                        if g.d3d12_unsupported then
                            -- DirectX 12 can't draw here (its shaders don't compile: Wine, Proton; binding tier 1: a Mac under CrossOver): switched to DirectX 11 ([graphics] d3d12.unsupported)
                            shown = shown .. " (DirectX 12 can't run here)"
                        end
                        return shown
                    elseif g.running == 'd3d9' then
                        return 'DirectX 9 on ' .. (g.gpu ~= '' and g.gpu or 'this graphics card')
                    elseif g.renderer == 'd3d12' then
                        return "DirectX 12 didn't start: the game's DirectX 8"
                    elseif g.renderer == 'd3d9' then
                        return "DirectX 9 didn't start: the game's DirectX 8"
                    elseif g.renderer == 'd3d11' then
                        return "DirectX 11 didn't start: the game's DirectX 8"
                    end
                    return "The game's own DirectX 8"
                end, true),
                -- the card's driver (graphics.cpp's find_driver: Device Manager's, found on a thread of its own)
                info('Driver', function ()
                    local g = vana.graphics.info()
                    if g.gpu == '' or g.driver == nil then
                        return '-'
                    end
                    return g.driver ~= '' and g.driver or '...'
                end),
                info('Driver updated', function ()
                    local g = vana.graphics.info()
                    local y, m, d = (g.driver_installed or ''):match('^(%d+)-(%d+)-(%d+)$')
                    if y == nil then
                        return g.gpu ~= '' and g.driver == '' and '...' or '-'
                    end
                    local month = ({ 'Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec' })[tonumber(m)]
                    local days = g.driver_days or -1
                    local ago = days < 0 and '' or days == 0 and ' (today)' or days == 1 and ' (1 day ago)'
                                or string.format(' (%d days ago)', days)
                    return string.format('%d %s %s%s', tonumber(d), month or m, y, ago)
                end),
                info('Frame rate', function () return string.format('%d FPS', M.fps or 0) end),
                -- the screen, and in brackets the size the 3D world is drawn at (Client > Game > Background resolution, or
                -- DLSS's; the launcher's with DirectX 8 or 9)
                info('Screen', function ()
                    local w, h = vana.ui.screen()
                    local g = vana.graphics.info()
                    if (g.scene_width or 0) == 0 then
                        return string.format('%d x %d', w, h)
                    end
                    return string.format('%d x %d', w, h), string.format('[3D: %d x %d]', g.scene_width, g.scene_height)
                end),
                info('Settings', function ()
                    -- the settings file: the account's, as the launcher names it (--ini), else vanacore.ini
                    local now = vana.config.info and vana.config.info()
                    local path = now and now.file ~= nil and now.file ~= '' and now.file or vana.paths.install .. '\\vanacore.ini'
                    if #path > 60 then
                        path = '...' .. path:sub(-57)
                    end
                    return path
                end, true),
            }
        end,
    },
    {
        name = 'Client',
        aliases = { 'window', 'input', 'game', 'display', 'developer', 'mcp', 'gamepad', 'gamepads', 'commands', 'boot' },
        buttons = { { 'Frame stats', function () vana.chat.run('/vana stats') end, 'account_tree', 'Frame stats: where a frame goes (/vana stats)' },
                    { 'Performance graph', function () vana.chat.run('/vana graph') end, 'show_chart', 'Performance graph: FPS and more, 1 minute (/vana graph)' } },
        rows = function ()
            -- A resolution key's choices: unset (`default` names what that is), the screen's resolution
            -- as Windows has it (auto) and shares of it (auto-75: three quarters each way), then sizes.
            -- The UI's has shares of the game window first (window-75): of the game's resolution, the
            -- window's inside in a window, fitted to the UI aspect ratio, shown at the size they come to now
            local graphics_info = vana.graphics.info()
            local mw, mh = graphics_info.monitor_width, graphics_info.monitor_height
            local fw, fh = graphics_info.ui_fit_width, graphics_info.ui_fit_height
            local screen = mw and mh and mw > 0 and mh > 0
            local function resolutions(key, default)
                -- the game's has no unset choice: unset is Screen (auto)
                local sizes = default and { { '', default } } or {}
                if key == 'resolution.ui' then
                    for _, share in ipairs({ 100, 75, 50 }) do
                        local label = share == 100 and 'Fit the game window' or share .. '% of the game window'
                        if fw and fh and fw > 0 and fh > 0 then
                            -- as VanaCore works it out: the height, then the width from it
                            local h = math.max(64, math.floor(fh * share / 100 + 0.5))
                            label = label .. string.format(' - %d x %d', math.max(64, math.floor(h * fw / fh + 0.5)), h)
                        end
                        sizes[#sizes + 1] = { share == 100 and 'window' or 'window-' .. share, label }
                    end
                end
                for _, share in ipairs({ 100, 75, 50 }) do
                    local label = share == 100 and 'Screen (auto)' or share .. '% of the screen'
                    if screen then
                        label = label .. string.format(' - %d x %d', math.floor(mw * share / 100 + 0.5), math.floor(mh * share / 100 + 0.5))
                    end
                    sizes[#sizes + 1] = { share == 100 and 'auto' or 'auto-' .. share, label }
                    -- the screen less the taskbar (its work area), after Screen (auto): the game's only
                    if share == 100 and key == 'resolution.game' then
                        local ww, wh = graphics_info.work_width, graphics_info.work_height
                        sizes[#sizes + 1] = { 'auto-taskbar', 'Screen without the taskbar'
                            .. ((ww or 0) > 0 and (wh or 0) > 0 and string.format(' - %d x %d', ww, wh) or '') }
                    end
                end
                -- a line between the screen's own choices and the sizes
                sizes[#sizes + 1] = { sep = true }
                local first_size = #sizes + 1
                for _, size in ipairs(SCREEN_SIZES) do
                    sizes[#sizes + 1] = { size, (size:gsub('x', ' x ')) }
                end
                -- the screen's own size (the monitor's): marked, and added when the list doesn't have it
                if screen then
                    local own, found = mw .. 'x' .. mh, false
                    for _, size in ipairs(sizes) do
                        if size[1] == own then
                            size[2], found = size[2] .. ' (screen)', true
                        end
                    end
                    if not found then
                        sizes[#sizes + 1] = { own, mw .. ' x ' .. mh .. ' (screen)' }
                    end
                end
                -- a size vanacore.ini already has that isn't in the list shows too. The game's Resolution shows the size
                -- the window's edges were dragged to, or its inside while maximized (M.resolution_shown), built again
                -- when that changes (vana.lua)
                local current, resized = (vana.config.get('ffxi', key) or ''):lower():gsub('%s', ''), nil
                if key == 'resolution.game' then
                    current, resized = M.resolution_shown()
                    M.resolution_built = current
                end
                local listed = false
                for _, size in ipairs(sizes) do
                    if size[1] == current then
                        listed = true
                        if resized then
                            size[2] = size[2] .. ' (' .. resized .. ')'
                        end
                    end
                end
                if not listed then
                    local w, h = current:match('^(%d+)x(%d+)$')
                    sizes[#sizes + 1] = { current, resized and w and (w .. ' x ' .. h .. ' (' .. resized .. ')') or current }
                end
                -- the sizes under the line biggest first (wider, then taller); anything that isn't a size last
                local fixed = {}
                for i = first_size, #sizes do
                    fixed[#fixed + 1] = sizes[i]
                    sizes[i] = nil
                end
                local function side(size)
                    local w, h = tostring(size[1]):match('^(%d+)x(%d+)$')
                    return tonumber(w) or -1, tonumber(h) or -1
                end
                table.sort(fixed, function (a, b)
                    local aw, ah = side(a)
                    local bw, bh = side(b)
                    if aw ~= bw then
                        return aw > bw
                    end
                    if ah ~= bh then
                        return ah > bh
                    end
                    return tostring(a[1]) < tostring(b[1])
                end)
                for _, size in ipairs(fixed) do
                    sizes[#sizes + 1] = size
                end
                return sizes
            end
            -- The background resolution's choices ([graphics] background, DirectX 11 and 12 only; with DirectX 8 or 9
            -- the launcher's [ffxi] resolution.background is used and this row is locked): the screen sizes, a line,
            -- then the squares, 8192 down to 1024 in steps of 1024 and 512; each part highest first. No launcher
            -- option: on DirectX 11 and 12 the launcher's resolution doesn't come into it. A size vanacore.ini has that
            -- isn't listed shows too.
            local function background_options()
                local options = {}
                for _, size in ipairs(SCREEN_SIZES) do
                    options[#options + 1] = { size, (size:gsub('x', ' x ')) }
                end
                -- the screen's own size (the monitor's): marked, and added when the list doesn't have it
                if screen then
                    local own, found = mw .. 'x' .. mh, false
                    for _, option in ipairs(options) do
                        if option[1] == own then
                            option[2], found = option[2] .. ' (screen)', true
                        end
                    end
                    if not found then
                        options[#options + 1] = { own, mw .. ' x ' .. mh .. ' (screen)' }
                    end
                end
                -- widest first, then tallest (as the Resolution list)
                table.sort(options, function (a, b)
                    local aw, ah = a[1]:match('^(%d+)x(%d+)$')
                    local bw, bh = b[1]:match('^(%d+)x(%d+)$')
                    aw, ah, bw, bh = tonumber(aw) or -1, tonumber(ah) or -1, tonumber(bw) or -1, tonumber(bh) or -1
                    if aw ~= bw then
                        return aw > bw
                    end
                    return ah > bh
                end)
                options[#options + 1] = { sep = true }
                for side = 8192, 1024, -1024 do
                    options[#options + 1] = { side .. 'x' .. side, side .. ' x ' .. side }
                end
                options[#options + 1] = { '512x512', '512 x 512' }
                local current = (vana.config.get('graphics', 'background') or ''):lower():gsub('%s', '')
                local listed = current == '' or current == 'launcher'
                for _, option in ipairs(options) do
                    listed = listed or option[1] == current
                end
                if not listed then
                    local side = current:match('^(%d+)$') -- one number is a square
                    options[#options + 1] = { current, side and side .. ' x ' .. side or (current:gsub('x', ' x ')) }
                end
                return options
            end
            -- full screen (the mode, else the game's own setting, 0034): the window is the screen, so its
            -- edges can't be dragged (the frame is drawn at the screen's size, or the game's)
            -- unset, the game's setting as VanaCore hands it over (Borderless Fullscreen unless
            -- [ffxi.registry] 0034 says otherwise), which the Mode list shows
            local mode = (vana.config.get('ffxi', 'mode') or ''):lower():gsub('[%s_-]', '')
            local unset_mode = ({ ['0'] = 'fullscreen', ['1'] = 'windowed', ['2'] = 'borderless' })[vana.config.game('0034') or '']
                               or 'borderlessfullscreen'
            if mode == '' then
                mode = unset_mode
            end
            local full = mode == 'fullscreen' or mode == 'borderlessfullscreen' or mode == '0' or mode == '3'
            -- the old exclusive Full Screen isn't offered (Borderless Fullscreen is it, done right); an
            -- ini that still has it shows it
            local modes = { { 'windowed', 'Windowed' }, { 'borderless', 'Borderless windowed' },
                            { 'borderlessfullscreen', 'Borderless fullscreen' } }
            if mode == 'fullscreen' or mode == '0' then
                table.insert(modes, 1, { vana.config.get('ffxi', 'mode') or 'fullscreen', 'Full screen' })
            end
            -- the game's resolution: a window's size at once, the game's own at the next start; the background
            -- resolution under it: DirectX 11 and 12's, locked while DLSS sizes the scene, with a line under it and its
            -- tooltip saying so; the interface's resolution after the aspect ratio, at once (uiscale.cpp makes the
            -- game's menus again at its size)
            -- the renderer first: the game starts with it, so an ini naming another gets an alert under it
            local function dlss_sized() return vana.graphics.info().dlss_sized == true end
            local rows = { header('Game'),
                           subheader('Display'),
                           kept(dropdown('Renderer', 'graphics', 'renderer', 'd3d8', { { 'd3d8', "DirectX 8 (game's)" }, { 'd3d9', 'DirectX 9' }, { 'd3d11', 'DirectX 11' }, { 'd3d12', 'DirectX 12' } }, true)),
                           renderer_alert(),
                           dropdown('Mode', 'ffxi', 'mode', unset_mode, modes),
                           subheader('Resolution'),
                           -- shows the dragged window's size ([window] lastsize); a pick takes that out, so the window is
                           -- the size picked (window.cpp)
                           (function (row)
                               row.get = function () return (M.resolution_shown()) end
                               row.set = function (value)
                                   -- maximized, a pick (the size it was set to as well) takes it out of that
                                   local place = vana.window and vana.window.info and vana.window.info()
                                   if place and place.maximized then
                                       vana.chat.run('/vana window restore')
                                   end
                                   vana.config.set('window', 'lastsize', nil)
                                   vana.config.set('ffxi', 'resolution.game', value ~= '' and value or nil)
                               end
                               return row
                           end)(dropdown('Resolution', 'ffxi', 'resolution.game', 'auto', resolutions('resolution.game'))),
                           -- the 3D world's size right under the game's (below UI aspect ratio till asked for here, 2026-11-10)
                           needs_dx((function (row)
                               row.lock_if = dlss_sized
                               row.tip = function ()
                                   return dlss_sized() and TIPS.rows['graphics.background'] .. '\nLocked: DLSS or FSR sets it while on.' or nil
                               end
                               return row end)(
                               dropdown('Background resolution', 'graphics', 'background', '3072x3072', background_options()))),
                           (function (row) row.show_if = dlss_sized return row end)(note('Set by DLSS or FSR while on.', 'lock')),
                           -- the picture's shape and smoothing (under Picture till asked for here, 2026-11-10)
                           as_dropdown(choice('Aspect ratio', 'graphics', 'aspect', 'auto', aspects({ { 'off', "Game's" }, { 'auto', 'Fit the window' } }))),
                           dropdown('UI resolution', 'ffxi', 'resolution.ui', '', resolutions('resolution.ui', 'Same as Resolution')),
                           needs_dx(as_dropdown(choice('UI aspect ratio', 'ui', 'aspect', 'game', aspects({ { 'game', "Game's" }, { 'off', 'Fill the window' } })))),
                           needs_dx(choice('Anti-aliasing', 'graphics', 'antialias', 'off', { { 'off', 'Off' }, { 'fxaa', 'FXAA' } })),
                           subheader(full and 'Monitor' or 'Window') }
            if not full then
                rows[#rows + 1] = toggle('Resizable edges', 'window', 'resizable', false)
            end
            -- where the window is ([window] position): it moves there at once, and the game starts it there. A window:
            -- Default (VanaCore leaves it where the game puts it), Remembered (last: where it was left) or Centered.
            -- Full screen covers a monitor: which one, by its top-left (the list is made again each time it opens).
            if full then
                local place = vana.window and vana.window.info and vana.window.info()
                local position = (vana.config.get('window', 'position') or ''):lower():gsub('%s', '')
                local places = { { '', 'Default' }, { 'last', 'Remembered' } }
                for i, m in ipairs(place and place.monitors or {}) do
                    places[#places + 1] = { m.x .. ',' .. m.y, string.format('Monitor %d%s - %d x %d', i, m.main and ' (main)' or '', m.width, m.height) }
                end
                local listed = false
                for _, p in ipairs(places) do
                    listed = listed or p[1] == position
                end
                if not listed then
                    local x, y = position:match('^(-?%d+),(-?%d+)$')
                    places[#places + 1] = { position, x and string.format('Saved - %s, %s', x, y) or position }
                end
                rows[#rows + 1] = dropdown('Monitor', 'window', 'position', '', places)
            else
                rows[#rows + 1] = choice('Position', 'window', 'position', '', { { '', 'Default' }, { 'last', 'Remembered' }, { 'center', 'Centered' } })
                -- a window: the monitor it's on now, a pick moving it to the middle of another (vana.window.monitor; the
                -- resolutions go with it). Kept with Position on Remembered. Not a setting: no reset, no preset.
                local place = vana.window and vana.window.info and vana.window.info()
                if place ~= nil and #place.monitors > 1 and vana.window.monitor ~= nil then
                    local options = {}
                    for i, m in ipairs(place.monitors) do
                        options[#options + 1] = { tostring(i), string.format('Monitor %d%s - %d x %d', i, m.main and ' (main)' or '', m.width, m.height) }
                    end
                    local row = kept(dropdown('Monitor', 'window', 'monitor', tostring(place.monitor), options))
                    row.get = function ()
                        local now = vana.window.info()
                        return tostring(now and now.monitor or place.monitor)
                    end
                    row.set = function (value)
                        local number = tonumber(value)
                        if number ~= nil then
                            vana.window.monitor(number)
                        end
                    end
                    rows[#rows + 1] = row
                end
            end
            M.monitor_built = M.monitor_seen()
            -- Characters: the two games running on this PC (vana.clients, src/core/clients.cpp). The Main at the top, the Alt
            -- under it, and between them the three copies of the Main's to the Alt (copy.lua): its Graphics settings, its Game
            -- Options, its addons and plugins. The Main is the game vanacore.ini names, else this one.
            local function characters_rows()
                local list = vana.clients.list()
                local main, alt
                for _, game in ipairs(list) do
                    if game.main then
                        main = game
                    end
                end
                for _, game in ipairs(list) do
                    if main == nil and game.self then
                        main = game
                    end
                end
                for _, game in ipairs(list) do
                    if game ~= main and alt == nil then
                        alt = game
                    end
                end
                -- a card's value, read again twice a second: the character (as the game is when it is built), and
                -- true when it's this game (the name drawn light yellow)
                local function card(title, game)
                    return {
                        label = title,
                        value = function ()
                            if game == nil then
                                return '-'
                            end
                            for _, now in ipairs(vana.clients.list()) do
                                if now.pid == game.pid then
                                    return now.name ~= '' and now.name or 'Not in the game yet', nil, now.self
                                end
                            end
                            return 'Closed'
                        end,
                    }
                end
                local copies = {
                    { kind = 'graphics', action = 'characters_copy_graphics', label = 'Copy Graphics Settings', icon = 'display',
                      tip = "The Main's Graphics and Experimental settings, made the Alt's: every one, as a Graphics preset holds them\n"
                            .. "With them Client > Game's renderer, background resolution, frame rate, V-Sync, anti-aliasing and aspect ratio\n"
                            .. "Not the window: its mode, resolution and place stay the Alt's\n"
                            .. "The renderer and background resolution apply at the Alt's next start" },
                    { kind = 'options', action = 'characters_copy_options', label = 'Copy Game Options', icon = 'sliders',
                      tip = "The Main's Game Options and HUD settings, made the Alt's: the interface, True Font, chat, menus, items,\n"
                            .. "nameplates, the world, camera and damage numbers, and the game's windows' places, sizes and Hide switches" },
                    { kind = 'addons', action = 'characters_copy_addons', label = 'Copy Addons/Plugins', icon = 'puzzle',
                      tip = "The addons and plugins the Main has loaded, loaded on the Alt too (none of the Alt's is unloaded)\n"
                            .. "Each addon's settings for the Main's character made the Alt's, and the addon reloaded to take them up\n"
                            .. "(the files the Alt had are kept in config\\before-copy)\n"
                            .. "Their windows put where the Main has them: in the same place, or in proportion on a screen of another size\n"
                            .. "Not Windower addons' own settings, nor files an addon writes itself (LuAshitacast's job files)" },
                }
                -- Auto Login at the top, its switch on the bar (greys Character while it's off): the character this game
                -- logs in with, by its place in the list (autologin.cpp, 1 to kMostCharacters)
                local places = {}
                for place = 1, 16 do
                    places[place] = { tostring(place), tostring(place) }
                end
                local rows = { header('Characters'),
                    subheader('Auto Login', nil, {
                        get = function () return (tonumber(vana.config.get('client', 'autologin') or '') or 0) ~= 0 end,
                        set = function (on) vana.config.set('client', 'autologin', on and '1' or '0') end,
                    }, 'client', 'autologin'),
                    dropdown('Character', 'client', 'autologin.character', '1', places),
                    subheader('Dual-Box Alts'),
                    { kind = 'transfer', main = card('Main', main), alt = card('Alt', alt), copies = copies,
                      refresh = alt == nil, -- one game running: the button looks for the other
                      tip = 'Look for the other game',
                      -- a copy, the Main's `kind` to the Alt: true once it's sent (or the Main is asked for it)
                      run = function (kind)
                          local what = M.copy ~= nil and M.copy.NAMES[kind] or kind
                          if alt == nil then
                              print("VanaCore: couldn't copy the Main's " .. what .. ': no other game is running.')
                              return false
                          end
                          if main.name == '' then
                              print("VanaCore: couldn't copy the Main's " .. what .. ": the Main isn't in the game yet.")
                              return false
                          end
                          if not main.main then
                              vana.clients.set_main(main.name) -- the one this game is, until another is picked
                          end
                          if M.copy == nil then
                              print("VanaCore: couldn't copy the Main's " .. what .. ': the copies (copy.lua) didn\'t load.')
                              return false
                          end
                          local sent, why = M.copy.send(kind)
                          if sent == nil then
                              print("VanaCore: couldn't copy the Main's " .. what .. ': ' .. why .. '.')
                              return false
                          elseif sent == 0 then
                              print('VanaCore: asked ' .. main.name .. ' (the Main) for its ' .. what .. '.')
                          else
                              print('VanaCore: ' .. main.name .. "'s " .. what .. ' sent to ' .. (sent == 1 and (alt.name ~= '' and alt.name or 'the Alt') or sent .. ' games') .. '.')
                          end
                          return true
                      end } }
                if alt ~= nil and alt.name ~= '' then
                    rows[#rows + 1] = { kind = 'action', label = 'Switch Main', text = 'Switch Main', action = 'characters_switch',
                                        rebuild = true, tip = alt.name .. ' becomes the Main, ' .. (main.name ~= '' and main.name or 'the other game') .. ' the Alt',
                                        run = function () vana.clients.set_main(alt.name) end }
                end
                return rows
            end
            -- ImGui's font ([client] imgui.font, gui.cpp): the text addons' and plugins' ImGui windows draw in, Agave unless a
            -- font file is picked. Listed: VanaCore's fonts\imgui\ and fonts\, then the fonts Windows installs for one user and
            -- Windows' own, by file name (the order gui.cpp looks in). The atlas is made once, so a pick is taken at the next start.
            local function imgui_font_row()
                local options, seen = { { '', 'Agave (default)' } }, { [''] = true }
                local windows = os.getenv('WINDIR') or os.getenv('SystemRoot') or 'C:\\Windows'
                local own = os.getenv('LOCALAPPDATA')
                for _, group in ipairs({
                    { "VanaCore's fonts", { vana.paths.install .. '\\fonts\\imgui', vana.paths.install .. '\\fonts' } },
                    { 'Windows fonts', { own and own .. '\\Microsoft\\Windows\\Fonts' or '', windows .. '\\Fonts' } },
                }) do
                    local names = {}
                    for _, folder in ipairs(group[2]) do
                        for _, name in ipairs(folder ~= '' and vana.fs.list(folder, '.+\\.(ttf|otf|ttc)', 'files') or {}) do
                            local lower = name:lower()
                            -- the icon fonts are merged in whatever is picked; Agave is the default above
                            if not seen[lower] and not lower:match('^fa%-') and lower ~= 'agave-regular.ttf' then
                                seen[lower] = true
                                names[#names + 1] = name
                            end
                        end
                    end
                    if #names > 0 then
                        table.sort(names, function (a, b) return a:lower() < b:lower() end)
                        options[#options + 1] = { header = group[1] }
                        for _, name in ipairs(names) do
                            options[#options + 1] = { name, name }
                        end
                    end
                end
                -- a font set by hand in the ini by its full path (or one that's since gone) still shows as what it is
                local saved = vana.config.get('client', 'imgui.font') or ''
                if saved ~= '' and not seen[saved:lower()] then
                    options[#options + 1] = { saved, saved }
                end
                return hinted(dropdown('Font', 'client', 'imgui.font', '', options, true), 'ImGui')
            end
            -- the input settings ([input]: controllers, the Windows keys, the cursor) are the launcher's: it writes them at
            -- each start, and they only take effect then
            -- Gamepads: the controllers Windows lists to the game (vana.gamepad.list, the last search: Rescan searches
            -- again), the one it uses ([ffxi] gamepad.guid: the game's padguid000, as its own config tool saves it), and
            -- the XInput slots with a pad connected
            local function gamepad_rows()
                local none = '{00000000-0000-0000-0000-000000000000}'
                local saved = (vana.config.get('ffxi', 'gamepad.guid') or ''):upper()
                local auto = saved == '' and '' or none
                local pads, slots = vana.gamepad.list(false)
                -- Disable: no controller works ([input] gamepad.disable), the game's search for them included; the rest
                -- of the section is locked meanwhile
                local DISABLE = 'disable'
                local off = function () return (tonumber(vana.config.get('input', 'gamepad.disable') or '') or 0) ~= 0 end
                local options, listed = { { DISABLE, 'Disable' }, { auto, 'Automatic' } }, {}
                for _, pad in ipairs(pads) do
                    options[#options + 1] = { pad.guid, pad.name }
                    listed[pad.guid] = true
                end
                -- picked: kept in vanacore.ini for the next start, and used by the game now (its own change-the-pad
                -- path, vana.gamepad.select); a client it doesn't know leaves it for the next start
                local pick = dropdown('Gamepad', 'ffxi', 'gamepad.guid', auto, options)
                -- a saved controller that isn't connected shows as Automatic (kept in the ini for when it's back)
                pick.get = function ()
                    if off() then
                        return DISABLE
                    end
                    return listed[saved] and saved or auto
                end
                pick.set = function (value)
                    pick.rebuild = true
                    if value == DISABLE then
                        vana.gamepad.set_disabled(true, saved)
                        return
                    end
                    if off() then
                        vana.gamepad.set_disabled(false)
                    end
                    vana.config.set('ffxi', 'gamepad.guid', value)
                    -- its own profile if it has one, else [ffxi]'s: mode, button map and the controller together
                    local ok, why = vana.gamepad.apply_profile(value)
                    if not ok then
                        print('VanaCore: the gamepad is saved for the next start (' .. tostring(why) .. ').')
                    end
                end
                pick.rebuild = true -- the switches below are the picked controller's
                local rows = { header('Gamepads'), pick }
                -- the picked controller's switches, as the game's gamepad config tool has them, used by the game at once
                -- (not its first, the gamepad itself: a profile always has it on)
                local target = saved
                for at, label in ipairs({ 'Force feedback', 'Sliders', 'Hat switches', 'Works while the game is inactive',
                                          'XInput' }) do
                    local index = at + 1
                    local switch = toggle(label, 'gamepads', 'switch' .. index, false)
                    switch.get = function () return vana.gamepad.switch(target, index) end
                    switch.set = function (value) vana.gamepad.set_switch(target, index, value) end
                    switch.lock_if = off
                    rows[#rows + 1] = switch
                end
                local camera = choice('Camera stick', 'gamepads', 'camera', 'xbox',
                                      { { 'xbox', 'Xbox (axes 35, 36)' }, { 'playstation', 'DualSense / DS4 (axes 34, 37)' },
                                        { 'custom', 'Custom' } })
                camera.get = function () return vana.gamepad.camera(target) end
                camera.set = function (value) vana.gamepad.set_camera(target, value) end
                camera.lock_if = off
                rows[#rows + 1] = camera
                -- the fish fight takes the menus' left and right (the D-pad) when the movement's give none: [input] fishing.dpad
                local fishing = toggle('Fish with the D-pad', 'input', 'fishing.dpad', true)
                fishing.lock_if = off
                rows[#rows + 1] = fishing
                -- the one switch for the addons' windows (Party Finder, the Auction House): [addons] gamepad, which
                -- they read; off, a window is the mouse's and the keyboard's and the pad stays the game's
                local addons = toggle('XInput gamepad support in VanaCore addons (experimental)', 'addons', 'gamepad', false)
                addons.get = function () return (tonumber(vana.config.get('addons', 'gamepad') or '') or 0) ~= 0 end
                addons.set = function (on) vana.config.set('addons', 'gamepad', on and '1' or '0') end
                addons.lock_if = off
                rows[#rows + 1] = addons
                -- Switch Game: its button shows the switch's buttons, or what to do (read each frame)
                if vana.gamepad.switch_capture ~= nil then
                    rows[#rows + 1] = setmetatable({ kind = 'action', label = 'Switch Game', lock_if = off,
                                                     run = function () vana.gamepad.switch_capture() end,
                                                     tip = 'Brings your other game in front.\nClick, then press a button or two together.\nClick again while it waits to clear it.' },
                                                   { __index = function (_, key)
                                                       if key ~= 'text' then
                                                           return nil
                                                       end
                                                       if vana.gamepad.switch_capturing() then
                                                           return 'Press buttons...'
                                                       end
                                                       return vana.gamepad.switch_text() or 'Set buttons'
                                                   end })
                end
                -- what DirectInput calls a device: its type's low byte (DI8DEVTYPE_*), and its product's name when it isn't the
                -- name shown (a dongle's name is often the product's)
                local KINDS = { [0x14] = 'Joystick', [0x15] = 'Gamepad', [0x16] = 'Driving controller', [0x17] = 'Flight stick',
                                [0x18] = 'First person controller' }
                local function pad_kind(pad)
                    local kind = KINDS[(pad.type or 0) % 256] or 'Controller'
                    if pad.product ~= nil and pad.product ~= '' and pad.product ~= pad.name then
                        kind = kind .. ' (' .. pad.product .. ')'
                    end
                    return kind
                end
                -- The game's gamepad as it is now (vana.gamepad.game), not as the settings file started it: a controller's
                -- profile or a pick above turns it on while it runs. The file's (an older VanaCore, another client) else.
                local function game_now()
                    local game = vana.gamepad.game ~= nil and vana.gamepad.game() or nil
                    return game ~= nil and game.known and game or nil
                end
                local function file_mode()
                    return vana.config.get('ffxi', 'gamepad.mode') or ''
                end
                -- why the game reads no controller ('disabled', 'search': the launcher's controller search off, 'off': its
                -- gamepad switch), or nil
                local function why_none()
                    if off() then
                        return 'disabled'
                    end
                    if (tonumber(vana.config.get('input', 'gamepad.disableenumeration') or '') or 0) ~= 0 then
                        return 'search'
                    end
                    local game = game_now()
                    if game ~= nil then
                        return not game.on and 'off' or nil
                    end
                    return file_mode():match('^%s*(%d)') == '0' and 'off' or nil
                end
                -- the line's value is "<guid>\t<what it is>\t<use|idle|gone|off|disabled>" (vana.lua's draw_pad takes it
                -- apart): in use while the game reads it now (DirectInput's reads, or XInput's for an XInput pad with XInput
                -- on); off and disabled while the game reads no controller at all
                local function pad_state(now, reading_x)
                    local none = why_none()
                    if none ~= nil then
                        return none == 'disabled' and 'disabled' or 'off'
                    end
                    if now.reading == nil then
                        return now.opened and 'use' or 'idle' -- an older VanaCore: opened this session
                    end
                    local game = game_now()
                    local by_xinput = now.xinput and game ~= nil and game.xinput
                                      and (reading_x[1] or reading_x[2] or reading_x[3] or reading_x[4])
                    return (now.reading or by_xinput) and 'use' or 'idle'
                end
                for _, pad in ipairs(pads) do
                    local kind = pad_kind(pad)
                    rows[#rows + 1] = status(pad.name, function ()
                        local list, _, reading_x = vana.gamepad.list(false)
                        for _, now in ipairs(list) do
                            if now.guid == pad.guid then
                                return pad.guid .. '\t' .. kind .. '\t' .. pad_state(now, reading_x or {})
                            end
                        end
                        return pad.guid .. '\t' .. kind .. '\tgone'
                    end)
                end
                if #pads == 0 then
                    rows[#rows + 1] = status('DirectInput', function () return 'no gamepad found' end)
                end
                rows[#rows + 1] = status('Game uses', function ()
                    local none = why_none()
                    if none ~= nil then
                        return ({ disabled = 'Nothing (disabled)', search = 'Nothing (controller search off)',
                                  off = 'Nothing (gamepad off)' })[none]
                    end
                    local game = game_now()
                    if game ~= nil then
                        return game.xinput and 'XInput' or 'DirectInput'
                    end
                    local mode = file_mode()
                    if mode == '' then
                        return "the game's own setting"
                    end
                    return mode:match('(%d)%s*$') == '1' and 'XInput' or 'DirectInput'
                end)
                rows[#rows + 1] = status('XInput', function ()
                    local _, now = vana.gamepad.list(false)
                    local connected = {}
                    for slot = 1, 4 do
                        if now[slot] then connected[#connected + 1] = tostring(slot) end
                    end
                    return #connected > 0 and 'slot ' .. table.concat(connected, ', ') or 'none connected'
                end)
                rows[#rows + 1] = { kind = 'action', label = 'Rescan gamepads', text = 'Rescan gamepads', action = 'gamepad_rescan',
                                    tip = 'Looks for gamepads again, as plugging one in does', lock_if = off }
                -- the test window (gamepadtest.lua): the buttons pressed, the sticks and triggers, each controller's stats
                if M.gamepadtest_toggle ~= nil then
                    rows[#rows + 1] = { kind = 'action', label = 'Gamepad Test', text = 'Gamepad Test', action = 'gamepadtest_toggle',
                                        icon = 'gamepad',
                                        tip = "Shows which buttons are pressed on each controller, with the sticks, the triggers and its stats.\nThe game ignores the controller while it's open (a switch in it lets the game have it).\nIts Record button logs presses and the game's timing to find input lag (/gamepad record).\n/gamepad test opens it too" }
                end
                return rows
            end
            for _, row in ipairs({
                subheader('Frame rate'),
                hinted(choice('Max FPS', 'client', 'fps.max', '60', { { '30', '30 FPS' }, { '45', '45 FPS' }, { '60', '60 FPS' }, { '0', 'Unlocked', tip = 'Use at your own risk - Game was designed for 30/60 FPS' } }), 'before frame gen'),
                choice('Cutscene FPS', 'client', 'fps.cutscene', '', { { '', 'Same' }, { '30', '30 FPS' }, { '60', '60 FPS' }, { '0', 'Unlocked', tip = 'Cutscenes play faster. Use at your own risk - Game was designed for 30/60 FPS' } }),
                dropdown('Background FPS', 'client', 'fps.background', '0', { { '0', 'Same as Max FPS' }, { '30', 'Reduced FPS' } }),
                toggle('V-Sync', 'graphics', 'vsync', false),
                needs_dx(choice('Cutscene V-Sync', 'graphics', 'vsync.cutscene', '', { { '', 'Same' }, { '0', 'Off' } })),
                subheader('Behaviour'),
                toggle('Keep FFXI always on top', 'window', 'topmost', false),
                toggle('Keep-Awake (Disable Screen Saver)', 'window', 'keepawake', true),
                toggle('Trigger /shutdown on window close button', 'window', 'close.shutdown', true),
                toggle('Follow audio output changes', 'client', 'sound.follow', true, true),
                toggle('Mouse scaled to the game', 'window', 'mouse.scale', true),
                toggle('Enable Scaled Display (DPI) Support', 'window', 'dpiaware', true, true),
                toggle('Load addons after character login', 'addons', 'delay.login', true, true),
                toggle('Fix Auto Run/Walk', 'client', 'autorun.walk', false),
                toggle('Frame pacing', 'client', 'framepacing', true),
                toggle('Packet flow', 'client', 'packetflow', true),
                toggle('Simple Log Fix', 'client', 'simplelog.fix', true),
                toggle('Prevent Addon Packet Block', 'client', 'packets.keepactions', true),
                toggle('Packet watch', 'client', 'packets.watch', false),
                choice('Keyboard hook thread (Linux / Proton)', 'input', 'keyboard.hookthread', 'auto', { { 'auto', 'Auto' }, { '1', 'Own thread' }, { '0', "Game's thread" } }, true),
                choice('DEP at start (Mac / CrossOver)', 'client', 'dep', 'auto', { { 'auto', 'Auto' }, { '1', 'On' }, { '0', 'Off' } }, true),
                -- the FPS counter ([client] fps.*, overlay.cpp); its font is the VanaCore label's too ([client] font.*). A
                -- sub-panel with its rows in a box (a group, to the next heading), as Shadows' Advanced Settings is; the
                -- Game page's last (asked for, 2026-11-10)
                (function ()
                    local sub = inner_sub('FPS Counter Options', nil, 'group')
                    sub.tip = 'The FPS counter in the corner of the screen: whether it shows, where, and its font.'
                    return sub
                end)(),
                toggle('Show the FPS counter', 'client', 'fps.display', true),
                choice('Corner', 'client', 'fps.position', '1', { { '0', 'Top left' }, { '1', 'Top right' }, { '2', 'Bottom right' }, { '3', 'Bottom left' } }),
                free(slider('Padding', 'client', 'fps.padding', 0, 0, 32, 1, '%d px'), 0, 64),
                font_line(dropdown('Font', 'client', 'font.family', 'Consolas', font_list('client', 'font.family')),
                          free(slider('Font size', 'client', 'font.size', 20, 6, 40, 1, '%d px'), 6, 128),
                          toggle('Italic', 'client', 'font.italic', false),
                          toggle('Bold', 'client', 'font.bold', true)),
                toggle('Text outline', 'client', 'font.outline', true),
            }) do
                rows[#rows + 1] = row
            end
            if vana.clients ~= nil then
                for _, extra in ipairs(characters_rows()) do
                    rows[#rows + 1] = extra
                end
            end
            if vana.gamepad ~= nil and vana.gamepad.apply_profile ~= nil then
                for _, extra in ipairs(gamepad_rows()) do
                    rows[#rows + 1] = extra
                end
            end
            -- VanaCore's own settings, before Game: with no heading of their own, under the VanaCore heading, the banner
            -- and Info that About brings (the merge after TABS). (Addon windows first, and the console window's switches
            -- on Developer: asked for, 2026-11-10)
            for i, row in ipairs({
                subheader('Addon windows'),
                imgui_font_row(),
                subheader('Screenshots'),
                toggle('Overlay text in screenshots', 'screenshots', 'overlays', true, true),
                toggle('Copy screenshots to clipboard', 'screenshots', 'clipboard', true),
                subheader('Chat log'),
                toggle('Hide key bind lines', 'input', 'keyboard.silentbinds', false),
                toggle('Hide addon load lines', 'addons', 'silent', false),
                toggle('Hide plugin load lines', 'addons', 'plugins.silent', false),
                subheader('Discord'),
                (function (row) row.rebuild = true return row end)( -- Show character name locks and unlocks by it
                    toggle('Discord status', 'discord', 'enabled', true)),
                (function (row) row.lock_if = function () return (tonumber(vana.config.get('discord', 'enabled') or '') or 1) == 0 end return row end)(
                    toggle('Show character name', 'discord', 'name', true)),
                subheader('Shaders'),
                needs_dx({ kind = 'action', label = 'Recompile shaders', text = 'Recompile shaders', action = 'recompile_shaders',
                           tip = "Compiles the scene effects' shaders again from scratch; the effects are off till they're done",
                           -- next to it: every setting in the Advanced Settings groups back to its default (M.reset_advanced)
                           more = { { kind = 'action', label = 'Reset ALL Advanced Settings', text = 'Reset ALL Advanced Settings',
                                      action = 'reset_advanced', confirm = 'Click again to reset',
                                      tip = 'Puts every setting in the Advanced Settings boxes (DLSS, Shadows, Mass Grass) back to its '
                                            .. 'default. The switch beside this stays as it is.' } } }),
                (function (row) -- (its name and its switch in the danger red: it's the switch for settings that can break things)
                    row.colour = M.P.rose
                    return row
                end)(toggle('Enable Advanced Graphics Settings', 'vana', 'advanced', false)),
                callout('Not recommended', 'It is not recommended to change any of the advanced settings. If anything breaks you '
                        .. 'will be asked to reset them to default.', 'warning'),
            }) do
                table.insert(rows, i, row)
            end
            -- Commands, after Gamepads (/vana commands): each of VanaCore's commands, and a switch for those it can do without
            for _, row in ipairs(commands_rows()) do
                rows[#rows + 1] = row
            end
            -- Boot.txt, after Commands (/vana boot): scripts\boot.txt, what VanaCore runs at each start, in a box to edit and
            -- save (vana.lua's draw_textfile)
            rows[#rows + 1] = header('Boot.txt')
            rows[#rows + 1] = { kind = 'textfile', label = 'boot.txt', path = vana.paths.install .. '\\scripts\\boot.txt' }
            -- Developer: VanaCore's log and each kind of logging in it ([logging]: dllmain.cpp, crash.cpp, frametime.cpp,
            -- graphics.cpp's DRED, mousediag.cpp), and the MCP server in the game ([mcp], src/core/mcp.cpp) for AI tools on
            -- this PC (it starts and stops within 2 seconds of the switch)
            local mcp_port = vana.config.get('mcp', 'port') or '47800'
            for _, row in ipairs({
                warn(header('Developer')),
                subheader('Tools'),
                { kind = 'action', label = 'API Explorer', text = 'Open', action = 'open_apiexplorer',
                  tip = "Every addon API call to run and copy (/api). Loads the addon if it isn't loaded" },
                subheader('Settings file'),
                toggle('Enable .ini upstream edits', 'client', 'ini.upstream', false),
                note('The game plays from a copy of its settings file, made as it starts. Off: changes made to the file while you '
                     .. 'play (by hand, or by the launcher) wait for a restart or Reload settings. On: the game looks at the file '
                     .. 'every second and takes them up, so whatever rewrites it changes your settings in the game too. What you '
                     .. 'change in this window is saved to the file either way.', 'info'), -- (an icon: shown, not folded into a tooltip)
                -- the file the settings are kept in (vana.config.info: src/core/settings.hpp) and whether it changed on disk
                status('Settings', function ()
                    local info = vana.config.info and vana.config.info()
                    if info == nil then
                        return 'vanacore.ini, up to date'
                    end
                    local name = (info.file or ''):match('[^\\/]+$') or 'vanacore.ini'
                    if name:lower() ~= 'vanacore.ini' then
                        -- the launcher names an account's file by a hash of the username: the character says whose it is
                        local place = vana.window and vana.window.info and vana.window.info()
                        if place ~= nil and (place.character or '') ~= '' then
                            name = name .. ' (' .. place.character .. "'s account)"
                        end
                    end
                    if info.changed then
                        return name .. (info.upstream and ', changed, taking it up' or ', changed on disk, not taken up')
                    end
                    return name .. (info.upstream and ', up to date, edits taken up' or ', up to date')
                end),
                { kind = 'action', label = 'Reload settings', text = 'Reload settings', action = 'reload_settings',
                  tip = 'Takes the settings file up now: what was changed in it since by something else' },
                -- VanaCore.exe's console ([client] attached / autoclose, src/launcher/main.cpp): kept open with the game's
                -- log, and whether it then waits for a key when the game ends (it closes by itself once the game is up
                -- otherwise, so autoclose locks while attached is off)
                subheader('Console window'),
                (function (row) row.rebuild = true return row end)(
                    toggle('Keep the console window open', 'client', 'attached', false, true)),
                (function (row)
                    row.sub = true
                    row.lock_if = function () return (tonumber(vana.config.get('client', 'attached') or '') or 0) == 0 end
                    return row
                end)(toggle('Close it when the game ends', 'client', 'autoclose', false, true)),
                -- (the shadows' redraw speed rows are Graphics > Shadows > GPU Redraw Options now)
                subheader('Logging'),
                toggle('Enable logs', 'logging', 'enabled', true),
                logged(choice('Crash dumps', 'logging', 'crashdumps', '1', { { '0', 'Off' }, { '1', 'Normal' }, { '2', 'Full memory' } }, true)),
                note('What a crash leaves. Off: one line in the log. Normal: a full report in the log and a .dmp file (15-40 MB). '
                     .. "Full memory: as Normal, with all of the game's memory in the .dmp (hundreds of MB)."),
                logged(toggle('Frame times', 'logging', 'frametimes', false, true)),
                note("A line every 10 seconds on where the game's frames go: its work, its sleeps, VanaCore and Present. "
                     .. 'Adds a little overhead.'),
                logged(toggle('GPU crash data', 'logging', 'gpu', false, true), true, true),
                note('DirectX 12. If the graphics card stops (AMD\'s "driver timeout"), the log says which pass and operation it '
                     .. 'was on. A little slower: on to catch a crash, then off again.'),
                logged(needs_dx(toggle('Mass Grass', 'graphics', 'grass.log', false))),
                logged(toggle('World widgets', 'graphics', 'widgets.log', false)),
                logged(toggle('Mouse trace', 'logging', 'mouse', false, true), true),
                note("The game's cursor and window calls and its mouse messages, for mouse bugs. Stops after 3000 lines."),
                logged(toggle('Pivot files served', 'pivot', 'log', false)),
                note('Every file Pivot serves from an overlay, in the log. Off: the first 20 only. Needs Pivot on ([pivot] enabled in vanacore.ini).'),
                logged(toggle('Zone loading', 'logging', 'zoneload', false)),
                note('A timeline of each zone change in its own file (logs\\zoneload-<time>.log): the packets in and out while it '
                     .. 'waits for the server, the files the zone reads, the frames. From the next zone change.'),
                note('Chat logs are saved from Game Options > Chat > Save chat logs.'),
            }) do
                rows[#rows + 1] = row
            end
            -- [mcp] available=0 (a server that doesn't want AI tools in its players' games): no AI MCP rows, and
            -- mcp.cpp never starts the server
            for _, row in ipairs((tonumber(vana.config.get('mcp', 'available') or '') or 1) == 0 and {} or {
                subheader('AI MCP'),
                note('Lets an AI app on this PC, such as Claude Code, look inside the game while you play and work in it: '
                     .. 'it can read the game\'s memory and code, watch packets and the chat log, take screenshots, '
                     .. 'and run commands and Lua. For modding and reverse engineering.', 'smart_toy'),
                note('Only programs on this PC can reach it, and your chat log says when one connects. '
                     .. 'Leave it off when no AI is using it.', 'lock'),
                toggle('MCP server', 'mcp', 'enabled', false),
                status('Server', function ()
                    local on = (tonumber(vana.config.get('mcp', 'enabled') or '') or 0) ~= 0
                    local info = vana.mcp and vana.mcp.info and vana.mcp.info()
                    if info == nil then
                        return on and 'on at the next start' or 'off'
                    end
                    if info.running then
                        local text = 'on, http://127.0.0.1:' .. info.port .. '/mcp'
                        if info.client ~= '' and info.idle >= 0 then
                            text = text .. ' - ' .. info.client .. ', last used ' ..
                                   (info.idle < 60 and math.floor(info.idle) .. 's ago' or math.floor(info.idle / 60) .. 'm ago')
                        end
                        return text
                    end
                    if info.failed then
                        return "couldn't start: its port is taken (switch it off and on to try again)"
                    end
                    return on and 'starting...' or 'off'
                end),
                -- every chat that connected since the game started (an MCP session each), the 4 used most recently
                status('Chats', function ()
                    local info = vana.mcp and vana.mcp.info and vana.mcp.info()
                    local list = info and info.sessions or {}
                    if #list == 0 then
                        return 'none yet'
                    end
                    table.sort(list, function (a, b) return a.idle < b.idle end)
                    local parts = {}
                    for i = 1, math.min(#list, 4) do
                        local s = list[i]
                        local ago = s.idle < 60 and math.floor(s.idle) .. 's' or math.floor(s.idle / 60) .. 'm'
                        parts[#parts + 1] = '#' .. s.number .. ' ' .. (s.name ~= '' and s.name or s.client) .. ' (' .. ago .. ')'
                    end
                    if #list > 4 then
                        parts[#parts + 1] = '+' .. (#list - 4) .. ' more'
                    end
                    return table.concat(parts, ', ')
                end),
                note('To connect Claude Code (or the Claude desktop app\'s Code tab), run this once in a terminal: '
                     .. 'claude mcp add --scope user --transport http vanacore http://127.0.0.1:' .. mcp_port .. '/mcp'),
            }) do
                rows[#rows + 1] = row
            end
            for _, row in ipairs({
                -- every tab's settings back to VanaCore's defaults (win.reset_all), on a second press
                (function ()
                    local sub = subheader('Reset')
                    sub.danger = true
                    return sub
                end)(),
                { kind = 'action', label = 'Reset all settings', text = 'Reset all settings', action = 'reset_all',
                  confirm = 'Click again to reset every setting',
                  tip = "Every tab's settings back to VanaCore's defaults\nThe on/off switches, the renderer, the addons and plugins loaded, "
                        .. "Pivot's overlays and addon windows' places stay as they are" },
            }) do
                rows[#rows + 1] = row
            end
            -- every heading on Developer in red (asked for, 2026-11-10): back from the end to its header, the last
            for i = #rows, 1, -1 do
                if rows[i].kind == 'header' then
                    break
                end
                if rows[i].kind == 'subheader' then
                    rows[i].danger = true
                end
            end
            return rows
        end,
    },
    {
        name = 'Graphics',
        search = true,
        aliases = { 'lighting', 'shadows', 'toggles', 'layers' },
        buttons = { { 'Frame stats', function () vana.chat.run('/vana stats') end, 'account_tree', 'Frame stats: where a frame goes (/vana stats)' },
                    { 'Performance graph', function () vana.chat.run('/vana graph') end, 'show_chart', 'Performance graph: FPS and more, 1 minute (/vana graph)' } },
        rows = function ()
            local distance_fog = choice('Distance fog', 'graphics', 'fog', 'distance',
                { { 'distance', 'Match distance' }, { 'game', 'Vanilla' }, { 'custom', 'Custom' } })
            distance_fog.rebuild = true -- the custom fog rows lock and unlock by it
            -- A custom fog row: greyed and locked while Distance fog isn't Custom
            local function custom_fog(row)
                row.lock_if = function () return M.read(distance_fog) ~= 'custom' end
                return row
            end
            -- Low (the game's), Medium (lod=range at 4x), High (range at 8x), Max (always high). [graphics] lod and lod.range stay
            -- the settings (a Medium or High writes both); lod.range itself has no row now, only the ini
            local scenery_detail = choice('Level of Detail', 'graphics', 'lod', 'low',
                { { 'low', 'Low' }, { 'medium', 'Medium' }, { 'high', 'High' }, { 'max', 'Max' } })
            scenery_detail.get = function ()
                local mode = (vana.config.get('graphics', 'lod') or 'game'):lower()
                if mode == 'high' then
                    return 'max'
                elseif mode == 'range' then
                    return (tonumber(vana.config.get('graphics', 'lod.range') or '') or 2) >= 6 and 'high' or 'medium'
                end
                return 'low'
            end
            scenery_detail.set = function (value)
                if value == nil or value == 'low' then
                    vana.config.set('graphics', 'lod', value == nil and nil or 'game')
                elseif value == 'max' then
                    vana.config.set('graphics', 'lod', 'high')
                else
                    vana.config.set('graphics', 'lod.range', value == 'high' and '8' or '4')
                    vana.config.set('graphics', 'lod', 'range')
                end
                vana.graphics.reload()
            end
            -- Lighting's Night (2026-11-07: Dark night and Bright night "kinda conflict"): Off (the game's own night), Dark
            -- or Bright picked on one heading (vana.lua: a sub-panel's modes), their sliders side by side under it, the one not
            -- picked at 60% (vana.lua: columns). Dark night (gfx/d3d8.c night_light, gfx_d3d12.c moon_grade): the world's ambient
            -- and moon light dimmed outdoors through the night, the picture drawn to a cool blue-grey. Bright night
            -- (gfx/d3d8_sun.c d3d8_brighten): the moon's light raised as strong as a sun's, its shadows with it, the ambient
            -- (General lighting) only if asked. Both fade in from 17:00 and out by 07:00; lamps and effects left. Their effect
            -- switches stay rows of their own, never drawn: a preset keeps night.enabled and night_bright.enabled by them (before
            -- the sliders, so those keys are an effect's), and a reset keeps the night that's on, on
            local night = { dark = effect_sub('Dark night', 'night', 0, 0.5), bright = effect_sub('Bright night', 'night_bright', 0, 1) }
            night.dark.show_if = function () return false end
            night.bright.show_if = night.dark.show_if
            night.sliders = { slider('Darkness', FX, 'night', 0, 0, 1, 0.05), slider('Moonlight colour', FX, 'night_moon', 0, 0, 1, 0.05),
                              slider('Moon strength', FX, 'night_bright', 0, 0, 3, 0.05),
                              slider('General lighting', FX, 'night_bright_ambient', 0, 0, 3, 0.05) }
            night.pick = function () -- 1 Off, 2 Dark, 3 Bright (both on, from a hand-edited ini: Dark)
                return night.dark.switch.get() and 2 or night.bright.switch.get() and 3 or 1
            end
            night.head = subheader('Night')
            night.head.modes = {
                labels = { 'Off', 'Dark', 'Bright' },
                tips = { "The game's own night.", TIPS.panels['Dark night'], TIPS.panels['Bright night'] },
                get = night.pick,
                set = function (pick)
                    if night.dark.switch.get() ~= (pick == 2) then
                        night.dark.switch.set(pick == 2)
                    end
                    if night.bright.switch.get() ~= (pick == 3) then
                        night.bright.switch.set(pick == 3)
                    end
                end,
            }
            night.columns = {
                kind = 'columns', label = 'Darkness, Moonlight colour, Moon strength, General lighting', parts = night.sliders,
                columns = { { title = 'Dark night', tip = TIPS.panels['Dark night'], rows = { night.sliders[1], night.sliders[2] },
                              dim_if = function () return night.pick() ~= 2 end },
                            { title = 'Bright night', tip = TIPS.panels['Bright night'], rows = { night.sliders[3], night.sliders[4] },
                              dim_if = function () return night.pick() ~= 3 end } },
                -- (the sub-panel's and the section's reset: the sliders, with the switches beside them so a night that's on keeps a
                -- strength above 0, as two effect sub-panels' did)
                reset = function ()
                    M.reset_rows({ night.dark, night.bright, night.sliders[1], night.sliders[2], night.sliders[3], night.sliders[4] })
                end,
            }
            local rows = {
                -- all of DLSS's panel is Direct3D 12's: needs_dx12 draws it half there, doing nothing, under a red bar
                needs_dx12(dlss_header()),
                dx12_alert('DLSS'),
                needs_dx12(marks(0, 100, { { 0, 'Performance' }, { 35, 'Balanced' }, { 65, 'Quality' }, { 100, 'Native' } })),
                needs_dx12(dlss_quality()),
                -- AMD's with DLSS: frame generation on an RTX 20 or 30, which NVIDIA's doesn't run on
                needs_dx12(dropdown('Frame generation', 'graphics', 'dlss.framegen', 'off', { { 'off', 'Off' }, { '2x', '2x (RTX 40)' },
                                                                                              { '3x', '3x (RTX 50)' }, { '4x', '4x (RTX 50)' },
                                                                                              { 'fsr', "AMD FSR 2x (any card)" } })),
                -- DLSS 5 (neural rendering): a switch like Quality's and Frame generation's; needs NVIDIA's nvngx_dlssnr.dll in
                -- the dlss folder; the Debug window's DLSS tab says when it runs, or why not. Its rows are left out where it can't run
                needs_dx12(dlss5_header()),
                needs_dx12(dlss5_why()),
                needs_dx12(dlss5_why(choice('DLSS 5 Style', FX, 'dlss5_style', '1', { { '1', 'Natural' }, { '2', 'Cinematic' }, { '0', 'Default' } }))),
                -- the size DLSS 5 runs at (dlss5_scale: its change laid back onto the full-size scene, the helper's nrscale), by name
                needs_dx12(dlss5_why(choice('DLSS 5 Resolution', FX, 'dlss5_scale', '1', { { '0.3', 'Low' }, { '0.5', 'Medium' },
                                                                                          { '0.75', 'High' }, { '1', 'Max' } }))),
                -- passes: the picture through DLSS 5 again, each pass its own (made again when changed), the later ones
                -- with the local tone and structure at a share of the first's
                needs_dx12(dlss5_why((function (row) row.rebuild = true return row end)(
                    choice('DLSS 5 Overdrive', FX, 'dlss5_passes', '1', { { '1', '1x' }, { '2', '2x' }, { '3', '3x' } })))),
                -- (the Resolution and Frame generation lines are on the Debug window's DLSS tab: debug.lua)
                -- Advanced Settings, as Shadows' and Mass Grass's are: hidden unless Client > VanaCore > Shaders' Enable Advanced
                -- Graphics Settings is on. A group runs to the next heading (FSR's) or status row, so the DLSS panel's are placed last
                advanced_group('Fine-tuning for DLSS: the upscaler model, how the picture is kept steady, and what gets its own '
                               .. 'motion. Most players never need these.'),
                -- DLSS's render preset: the transformer models cost more than the older CNN ones (E and F), which are softer
                -- and cheaper; "Automatic" is NVIDIA's choice for the quality chosen above
                needs_dx12(dropdown('Upscaler model', FX, 'dlss_preset', '0', { { '0', 'Automatic' }, { '11', 'K: transformer, best quality' },
                                                                                 { '13', 'M: transformer, for Performance' }, { '10', 'J: transformer' },
                                                                                 { '6', 'F: older CNN, fastest' }, { '5', 'E: older CNN' } })),
                -- how far DLSS (and FSR) lean on this frame over their history for the game's effects, and for what moves on its own:
                -- the camera's motion is all they're told, and it's wrong for those
                needs_dx12(slider('Stability: effects', FX, 'dlss_reactive', 0.8, 0, 1, 0.05, '%.2f')),
                needs_dx12(slider('Stability: moving things', FX, 'dlss_reactive_moving', 0.5, 0, 1, 0.05, '%.2f')),
                -- the characters' own motion (each part matched to its nearest the frame before's), on by default
                needs_dx12(toggle('Motion for characters', FX, 'dlss_motion_chars', true)),
                -- (the effects' and placed objects' own motion, a guess that broke more than it mended, is
                -- [graphics.effects] dlss_motion_all in vanacore.ini alone)
                -- with frame generation, the least alpha at which an effect's model takes its own motion (0: the scene's)
                needs_dx12(slider('Frame gen effect motion', FX, 'fg_fx_motion', 0.03, 0, 0.3, 0.01, '%.2f')),
                -- the zone's plants' own motion too (off: the depth's; frame generation tore them when given their own)
                needs_dx12(toggle('Motion for plants', FX, 'dlss_motion_plants', false)),
                inner_sub('Advanced DLSS 5', nil, 'in'),
                -- DLSS 5's own model number: not named by NVIDIA yet. 0 is its default; the others are for trying, with the
                -- Resolution line's timing (/vana stats) saying what each costs
                needs_dx12(dlss5_why(choice('Model (to try)', FX, 'dlss5_preset', '0', { { '0', 'Default' }, { '1', 'Model 1' }, { '2', 'Model 2' },
                                                                                          { '3', 'Model 3' }, { '4', 'Model 4' } }))),
                needs_dx12(dlss5_why(free(slider('Tone', FX, 'dlss5_tone', 1, 0, 4, 0.05)))),
                needs_dx12(dlss5_why(free(slider('Structure', FX, 'dlss5_structure', 1, 0, 4, 0.05)))),
                -- the skin's own structure strength: -1 leaves it to DLSS 5 ("Auto")
                (function ()
                    local row = free(slider('Skin structure', FX, 'dlss5_skin', -1, -1, 4, 0.05))
                    row.format = function (value) return value < -0.001 and 'Auto' or string.format('%.2f', value) end
                    return needs_dx12(dlss5_why(row))
                end)(),
                needs_dx12(dlss5_why(toggle('Find its own mask', FX, 'dlss5_mask', true))),
                needs_dx12(dlss5_why((function (row)
                    row.lock_if = function () return (tonumber(vana.config.get(FX, 'dlss5_passes') or '') or 1) < 2 end
                    return row
                end)(slider('Overdrive Strength', FX, 'dlss5_taper', 0.5, 0, 1, 0.05)))),
                -- DLSS 5's picture blended back with the scene it was handed (the helper's own pass, after DLSS 5)
                needs_dx12(dlss5_why(free(slider('DLSS 5 amount', FX, 'dlss5_mix', 1, 0, 2, 0.05), 0, 4))),
                needs_dx12(dlss5_why(slider('DLSS 5 colour', FX, 'dlss5_colour', 1, 0, 1, 0.05))),
                (function ()
                    local row = slider('Brightening limit', FX, 'dlss5_brighten', 1, 0, 8, 0.25)
                    row.format = function (value) return value < 1 and 'Off' or string.format('%.2fx', value) end
                    return needs_dx12(dlss5_why(row))
                end)(),
                -- the game's effects (spells, flames, glows) and DLSS 5: left out of it and laid back over its picture, or a glow
                -- from them laid over what it sees, past white, as its lights (with both, it lights from the glow and they're
                -- laid back as drawn)
                needs_dx12(dlss5_why(inner_sub('Effects', nil, 'in'))),
                needs_dx12(dlss5_why(toggle('Keep effects out of DLSS 5', FX, 'dlss5_fx_out', false))),
                needs_dx12(dlss5_why((function (row) row.rebuild = true return row end)(
                    toggle('Effects as light sources', FX, 'dlss5_fx_light', true)))),
                needs_dx12(dlss5_why((function (row)
                    row.lock_if = function () return (tonumber(vana.config.get(FX, 'dlss5_fx_light') or '') or 1) == 0 end
                    return row
                end)(free(slider('Effect light strength', FX, 'dlss5_fx_gain', 5, 1, 16, 0.5, '%.1fx'), 1, 64)))),
                needs_dx12(dlss5_why((function (row)
                    row.lock_if = function () return (tonumber(vana.config.get(FX, 'dlss5_fx_light') or '') or 1) == 0 end
                    return row
                end)(slider('Spells and abilities as lights', FX, 'dlss5_fx_actor', 0.25, 0, 1, 0.05)))),
                needs_dx12(dlss5_why((function (row)
                    row.lock_if = function () return (tonumber(vana.config.get(FX, 'dlss5_fx_light') or '') or 1) == 0 end
                    return row
                end)(free(slider('Effect light ceiling', FX, 'dlss5_fx_cap', 1.25, 0.05, 1.5, 0.05, '%.2fx'), 0.05, 64)))),
                needs_dx12(dlss5_why((function (row)
                    row.lock_if = function () return (tonumber(vana.config.get(FX, 'dlss5_fx_light') or '') or 1) == 0 end
                    row.format = function (value) return value < 0.0001 and 'Off' or string.format('%.1f%%', value * 100) end
                    return row
                end)(slider('Light size for full strength', FX, 'dlss5_fx_norm', 0.02, 0, 0.2, 0.005)))),
                -- Glow Effects (elight) laid onto DLSS's output, not into what DLSS 5 is handed
                needs_dx12(dlss5_why(toggle('Glow effects after DLSS 5', FX, 'dlss5_elight_post', true))),
                -- AMD FSR, DLSS's place on any card: FSR 4 on a Radeon RX 7000 or 9000, FSR 3.1 elsewhere. It upscales
                -- under DirectX 11 or 12 (needs_dx); its frame generation and Compare are Direct3D 12's (needs_dx12)
                needs_dx(FSR.header()),
                needs_dx(marks(0, 100, { { 0, 'Performance' }, { 35, 'Balanced' }, { 65, 'Quality' }, { 100, 'Native' } })),
                needs_dx(FSR.quality()),
                -- shown 1-10 (the setting 0.1-1.0, 10 = the old 1.00); 0 stays off
                needs_dx((function (row)
                    row.format = function (value)
                        local n = math.floor(value * 10 + 0.5)
                        return n < 1 and 'Off' or tostring(n)
                    end
                    return row
                end)(slider('Sharpening', 'graphics', 'fsr.sharpness', 0, 0, 1, 0.1))),
                needs_dx12(dropdown('Frame generation', 'graphics', 'fsr.framegen', '2x', { { 'off', 'Off' }, { '2x', '2x' } })),
                -- (the Resolution and Frame generation lines are on the Debug window's DLSS / FSR tab: debug.lua)
                -- Advanced Settings (as DLSS's): the version and DLSS's Extras rows, the same keys: set on one page, it's set on both
                advanced_group('Fine-tuning for FSR: which FSR runs, how the picture is kept steady, and what gets its own motion. '
                               .. 'Most players never need these.'),
                needs_dx(dropdown('Version', 'graphics', 'fsr.version', 'auto', { { 'auto', 'Automatic' }, { '4', 'FSR 4 (Radeon RX 7000, 9000)' },
                                                                                  { '3', 'FSR 3.1' }, { '1', 'FSR 1 (any card)' } })),
                needs_dx12(toggle('Compare (FSR off, same size)', FX, 'dlss_compare', false)),
                needs_dx12(slider('Stability: effects', FX, 'dlss_reactive', 0.8, 0, 1, 0.05, '%.2f')),
                needs_dx12(slider('Stability: moving things', FX, 'dlss_reactive_moving', 0.5, 0, 1, 0.05, '%.2f')),
                needs_dx12(toggle('Motion for characters', FX, 'dlss_motion_chars', true)),
                -- the characters' motion (and the moving things' mask) drawn together once the scene is drawn, on by default
                needs_dx12(toggle('Batch character motion', FX, 'dlss_motion_batch', true)),
                needs_dx12(slider('Frame gen effect motion', FX, 'fg_fx_motion', 0.03, 0, 0.3, 0.01, '%.2f')),
                -- the panel's own switch in its heading, as the others': off, the game's own distances
                (function (row)
                    row.section, row.key, row.default = 'graphics', 'drawdistance.enabled', true
                    row.switch = { get = function () return (tonumber(vana.config.get('graphics', 'drawdistance.enabled') or '') or 1) ~= 0 end,
                                   set = function (on) vana.config.set('graphics', 'drawdistance.enabled', on and '1' or '0') end }
                    return row
                end)(header('Draw distance')),
                free(slider('Scenery', 'graphics', 'drawdistance', 5, 1, 10, 1, '%.0fx'), 0.1, 100),
                free(slider('Characters', 'graphics', 'drawdistance.mobs', 5, 1, 10, 1, '%.0fx'), 0.1, 100),
                free(slider('Boat rides', 'graphics', 'drawdistance.ship', 1, 1, 10, 1, '%.0fx'), 0.1, 100),
                scenery_detail,
                -- (what was the Hiding sub-panel: with the distances, above the Advanced Settings group)
                toggle('Hide other players', 'graphics', 'hide.players', false),
                toggle('Hide other players\' pets', 'graphics', 'hide.pets', false),
                toggle('Hide other players\' trusts', 'graphics', 'hide.trusts', false),
                -- (Fog is above the Advanced Settings group, which ends the panel: its rows run to the next heading)
                subheader('Fog'),
                distance_fog,
                -- shown in hundreds (7 is 700; fog.start keeps its own units)
                custom_fog(shown_as(free(slider('Custom fog starts at', 'graphics', 'fog.start', 1, 1, 20, 1, '%d'), 0, 50), 0.01)),
                custom_fog(shown_as(free(slider('Custom fog ends at', 'graphics', 'fog.end', 4, 1, 100, 1, '%d'), 0, 200), 0.01)),
                advanced_group('Fine-tuning for the draw distance: filling in far ground that can be missing, the whole zone in view, '
                               .. 'and how the far scenery is drawn. Most players never need these.'),
                toggle('Fill in missing far ground', 'graphics', 'drawdistance.fill', false),
                toggle('Draw the whole zone in view', 'graphics', 'drawdistance.full', false),
                toggle('Off in large open zones', 'graphics', 'drawdistance.large_off', true),
                -- the zone's draws of one mesh sent together (gfx_d3d12_batch.h): on by default (2026-11-12, Zi'Tah)
                needs_dx12(toggle('Batch repeated scenery', FX, 'batch', true)),
                -- far scenery hidden behind what's drawn left undrawn (occlusion.cpp, gfx_d3d12_occl.h); Count only logs it
                needs_dx12(choice('Skip hidden scenery', 'graphics', 'occl', '1', { { '0', 'Off' }, { '1', 'On' }, { '2', 'Count only' } })),
                effect('Shadows', 'sun', 0.6),
                note("The game's own shadows are turned off while these are on, on purpose: they would draw twice. "
                     .. "Turning these off leaves the game's off: Config > Shadows puts them back."),
                -- day and night alike, at the top of the panel under its heading (what was the Day and night sub-panel)
                slider('Distance', FX, 'sun_distance', 150, 10, 300, 5, '%d'),
                -- (8192 is an ini edit: the box shows the number for a value it doesn't list)
                choice('Shadow Quality', FX, 'sun_detail', '4096', { { '512', 'Low' }, { '1024', 'Medium' }, { '2048', 'High' }, { '4096', 'Max' } }),
                -- (a value between these, or past them, is an ini edit: the box lights none)
                choice('Softness', FX, 'sun_soft', '0', { { '0', 'Sharp' }, { '0.05', 'Low' }, { '0.1', 'Medium' }, { '0.2', 'High' } }),
                slider('Underside Shadows', FX, 'sun_face', 1, 0, 1, 0.05),
                -- what most players set (2026-11-05: "shadows is getting mad complicated"); the rest under Advanced Settings.
                -- By day and by night apart (a player, 2026-11-06): the night's pair go to the renderer in the day's place from
                -- 18:00 to 06:00 outdoors (gfx_dispatch.c gfx_sun_hours); the day's Strength at 0 is no shadows at night either
                subheader('Daytime', '06:00 to 18:00'),
                slider('Strength', FX, 'sun', 0.6, 0, 1, 0.05),
                choice('Cast by', FX, 'sun_casters', '0', { { '0', 'All' }, { '1', 'Characters' }, { '2', 'Environment' } }),
                -- its switch is Shadows at night (sun_moon, on by default)
                subheader('Night time', '18:00 to 06:00', {
                    get = function () return tonumber(vana.config.get(FX, 'sun_moon') or '') ~= 0 end,
                    set = function (on) vana.config.set(FX, 'sun_moon', on and '1' or '0') end,
                }, FX, 'sun_moon'),
                slider('Strength', FX, 'sun_moon_strength', 0.6, 0, 1, 0.05),
                choice('Cast by', FX, 'sun_moon_casters', '0', { { '0', 'All' }, { '1', 'Characters' }, { '2', 'Environment' } }),
                -- Advanced Settings: its rows and its sub-panels (inner_sub ... 'in') in a box, hidden unless Client > VanaCore >
                -- Shaders' Enable Advanced Graphics Settings is on (till 2026-11-23 a button over it showed it)
                advanced_group('Fine-tuning for the shadows: plants, the sharp shadows near you, smoothing, the sun and weather, '
                               .. 'and how often they\'re redrawn. Most players never need these.'),
                -- two sub-panels only (2026-11-12: "a lot of subcategories"): the shadows' own rows under the heading
                -- itself (Close-up and Look till then), Sun and weather (Conditions and Place the sun), GPU Redraw Options
                needs_dx12(toggle('Plants cast shadows', FX, 'sun_plants', true)),
                needs_dx12(resets(toggle('Keep plant shadows still', FX, 'sun_plants_still', true))),
                -- the sharp close-up map round the player (gfx_d3d12.c sun_map) and how far it reaches, the middle map's reach; the zone's pieces the
                -- camera's view leaves out drawn for the maps all the same (casters.cpp). The rest of the shadows' parts are ini
                -- keys alone (sun_centre, sun_cache, sun_late, sun_cull, sun_fade: README), the rings in the Debug window
                needs_dx12(toggle('Sharp shadows near you', FX, 'sun_sharp', true)),
                slider('Sharp shadow range', FX, 'sun_near', 10, 3, 80, 1, '%d'),
                -- how much of the sharp map's reach it spends fading into the maps beyond (sun_look's edge): thin shadows soften
                -- across it as you walk instead of popping
                slider('Sharp shadow fade', FX, 'sun_handoff', 0.5, 0.05, 0.9, 0.05, '%.2f'),
                -- a third map between the sharp one and the far one (gfx_sun_mid.h), 0 none
                needs_dx12(slider('Middle shadow range', FX, 'sun_mid', 0, 0, 200, 5, '%d')),
                needs_dx12(toggle('Shadows from things off screen', FX, 'sun_seek', true)),
                -- on characters too: their own narrower blur (sun_soft_chars, ini alone, 0 by default) let a rocky monster's
                -- shading flicker from frame to frame
                slider('Edge smoothing', FX, 'sun_soft_zone', 0.5, 0, 2, 0.05, '%.2f'),
                slider('Ignore shadows from closer than', FX, 'sun_min', 0, 0, 5, 0.1, '%.1f'),
                -- off (the default): the maps alone shade; on, the shadows are weighed against the game's own smooth shading,
                -- which lost them where the game's light and the shadows' parted (gfx_d3d12.c sun_game_shading; Direct3D 11's
                -- too, gfx_d3d11_smooth.h)
                toggle('Blend with the game\'s shading', FX, 'sun_game_shading', false),
                -- the zone's curved rock and ground shaded as the smooth surface their normals describe, not polygon by polygon
                -- (zonemesh.cpp curve_of; zn_draw in gfx_d3d12.c and gfx_d3d11_smooth.h)
                toggle('Round shadows on rock', FX, 'sun_curved', true),
                inner_sub('Sun and weather', nil, 'in'),
                slider('Fade when cloudy', FX, 'sun_direct', 0.5, 0, 1, 0.05),
                toggle('Long shadows at dusk and dawn', FX, 'sun_dusk', true),
                slider('Strength indoors', FX, 'moghouse', 0, 0, 1, 0.05),
                toggle('Custom sun position', FX, 'sun_free', false),
                slider('Sun direction', FX, 'sun_free_az', 170, 0, 360, 1, '%d'),
                slider('Sun height', FX, 'sun_free_el', 41, -20, 90, 1, '%d'),
                -- the shadows' redraw speed, moved here from Client > Developer, in red: the far map's still casters drawn every
                -- hold+1 frames (gfx_d3d12.c sun_map, g_still), the sharp map's kept while the camera stays in their layer
                -- (g_still_near), each layer drawn over 4 frames, a slice each (gfx_sun_layers.h; it strobed on a player's card,
                -- 2026-11-05, and is off by default)
                (function ()
                    local sub = inner_sub('GPU Redraw Options', nil, 'in')
                    sub.danger = true
                    return sub
                end)(),
                slider('Hold far shadows (frames)', FX, 'sun_far_hold', 3, 0, 15, 1, '%d'),
                needs_dx12(slider('Hold near shadows (frames)', FX, 'sun_near_hold', 30, 0, 600, 10, '%d')),
                needs_dx12(toggle('Spread redraws over 4 frames', FX, 'sun_roll', false)),
                -- (Day & Night till 2026-11-07, when it was named Lighting and put under Shadows)
                header('Lighting'),
                -- the least ambient light everything the game lights has, day and night, indoors and out (gfx/d3d8_night.h):
                -- Windower's game_forceambientlight, Ashita's /ambient; 1 lights everything full. At the top (2026-11-07)
                -- no heading and no switch (2026-11-24): above 0 is on, 0 is off. A Force ambient light switched off before
                -- (ambient_force.enabled=0) with a strength is switched on, as the number says; moving the slider does too
                M.unswitched(slider('Ambient Light Strength', FX, 'ambient_force', 0, 0, 1, 0.05), 'ambient_force'),
                -- the sun's light raised through the day, outdoors (gfx/d3d8_sun.c d3d8_brighten: 1 + it, 06:00 to 18:00): what
                -- faces the sun brighter up to full light, the shade left; with Bloom's threshold down and the grade's Whites
                -- up, a blown-out day (2026-11-12). Under the ambient light with no heading or switch too (2026-11-24)
                M.unswitched(slider('Sun Strength', FX, 'day_bright', 0, 0, 3, 0.05), 'day_bright'),
                -- the same raise indoors (gfx/d3d8_night.h night_light, d3d8_sun.c d3d8_inside): a Mog House, a zone lit by a
                -- fixed light alone (caves, dungeons, rooms), or one whose sun hasn't moved 10 s after zoning in; lamps and effects left.
                -- Up here with no heading or switch too (2026-11-24)
                M.unswitched(slider('Indoor Brightness', FX, 'indoor_bright', 0, 0, 5, 0.05), 'indoor_bright'),
                -- the light on players, NPCs and monsters (VanaCore's renderer): they come up out of the dark, the zone stays as it is.
                -- Under these since 2026-11-24 (they were the Characters sub-panel's rows)
                needs_dx(free(slider('Character brightness', 'graphics', 'characters.brightness', 1, 0.5, 3, 0.05, '%.2fx'), 0, 10)),
                needs_dx(slider('Character ambient light', 'graphics', 'characters.ambient', 0, 0, 2, 0.1, '%.1f')),
                -- Night: Off, Dark or Bright on its heading, their sliders side by side under it (see `night` above)
                night.dark,
                night.bright,
                night.head,
                night.columns,
                -- a light hung over the player, shining down: a pool round them (gfx_d3d12.c player_light, both renderers);
                -- Strength lights the pool at night with Dark night on only (it gives back the light Dark night took: gfx_shared.c
                -- gfx_player_light); by day the world round it goes darker (Darken around); at night without Dark night, nothing
                effect_sub('Player light', 'player_light', 0, 1),
                slider('Strength', FX, 'player_light', 0, 0, 2, 0.05),
                slider('Radius', FX, 'player_light_radius', 8, 3, 20, 0.5, '%.1f'),
                slider('Warm colour', FX, 'player_light_warm', 0.5, 0, 1, 0.05),
                slider('Darken around by day', FX, 'player_light_day', 0.5, 0, 1, 0.05),
                subheader('Moon'),
                slider('Size', FX, 'moon_size', 1, 0.25, 4, 0.05),
                slider('Brightness', FX, 'moon_brightness', 1, 0, 2, 0.05),
                effect('Ambient occlusion', 'ao', 1.0),
                slider('Strength', FX, 'ao', 1.0, 0, 2, 0.05),
                slider('Reach', FX, 'radius', 2.0, 0.2, 10, 0.1, '%.1f'),
                slider('Characters Strength', FX, 'ao_chars', 1.25, 0, 2, 0.05),
                slider('Smoothing on characters', FX, 'ao_soft_chars', 0.4, 0, 2, 0.05, '%.2f'),
                -- the taps a pixel (6 to 32), every one at the scene's full size
                -- (4, the 32 taps, is an ini edit: the box lights none; Max is the 24 it has always run with)
                choice('Quality', FX, 'ao_quality', '3', { { '0', 'Low' }, { '1', 'Medium' }, { '2', 'High' }, { '3', 'Max' } }),
                -- fewer of those taps past each of three rings out from the camera (fx_ao): all, then 75%, 50%, 30%
                toggle('Rings', FX, 'ao_rings', true),
                slider('Ring spacing', FX, 'ao_ring_spacing', 15, 5, 60, 1, '%d'),
                -- how much of the frame before the occlusion and the shadows keep (0 none, 0.99 most): the temporal pass
                slider('Smoothing over frames', FX, 'temporal', 0.99, 0, 0.99, 0.01, '%.2f'),
                -- reflections: screen space, a layer at the occlusion's size laid over the finished scene (DirectX 12); on the
                -- Graphics page since 2026-11-04 (it was the Experimental page's); bounce light's rows follow
                needs_dx12(effect('Reflections', 'refl', 0, 1)),
                note('On floors and flat ground, from what is on screen: they fade at its edges. DirectX 12.'),
                slider('Strength', FX, 'refl', 1, 0, 2, 0.05),
                subheader('Surface'),
                slider('Wetness', FX, 'refl_wet', 0.5, 0, 1, 0.05),
                slider('Roughness', FX, 'refl_rough', 0.3, 0, 1, 0.05),
                -- the floor's picture read as a height tilts the reflection's normal (fx_ssr); 0 is a flat mirror
                slider('Surface depth', FX, 'refl_bump', 1.3, 0, 2, 0.05),
                slider('Sun glint', FX, 'refl_sun', 1, 0, 3, 0.1),
                subheader('Reach'),
                slider('Distance', FX, 'refl_dist', 40, 5, 150, 5, '%d'),
                -- low rays tilted up (fx_ssr): from a low camera a true mirror shows only the walls' dark feet; 0 is the mirror
                slider('Lift', FX, 'refl_lift', 0.15, 0, 0.5, 0.01, '%.2f'),
                subheader('Ground and quality'),
                -- which ground reflects, by the zone's collision (the kind of ground the footsteps sound for, under each pixel: floors.c,
                -- fx_ssr); Any is the colour rule alone
                choice('Ground', FX, 'refl_floors', '1', { { '0', 'Any' }, { '1', 'Stone and metal' }, { '2', 'Stone, metal, wood' },
                                                           { '3', 'Anything built' } }),
                -- the ray's steps (16 to 128) and the blur's taps: more is cleaner and costs more
                -- (4 is an ini edit, as the occlusion's)
                choice('Quality', FX, 'refl_quality', '3', { { '0', 'Low' }, { '1', 'Medium' }, { '2', 'High' }, { '3', 'Max' } }),
                -- normal maps from a texture pack folder ([graphics] textures), bending the sun's per-pixel light (the renderer:
                -- DirectX 12); shown on the Experimental page (vana.lua MOVED_PANELS, by the heading)
                warn(effect('Normal maps (Experimental)', 'normalmap', 0, 1)),
                callout('In development', 'Needs normal maps generated with xi-tools.'),
                note('Bends the sunlight on textures that have a normal map in a texture pack (textures= in vanacore.ini), where the sun is lit per pixel (Light, below). DirectX 12.'),
                slider('Strength', FX, 'normalmap_strength', 1, 0, 4, 0.1),
                choice('View', FX, 'normalmap_view', '0', { { '0', 'Lit' }, { '1', 'Normals as colour' }, { '2', 'Lit, Y flipped' } }),
                -- the detail the maps add goes into a per-pixel normal target, which these effects read in place of the normal worked out from depth
                choice('Reflections follow them', FX, 'normalmap_refl', '1', { { '0', 'Off' }, { '1', 'On' } }),
                choice('Occlusion and sun shadows follow them', FX, 'normalmap_fx', '1', { { '0', 'Off' }, { '1', 'On' } }),
                effect('Bounce light', 'gi', 0, 0.5),
                note('Light picked up from lit surfaces nearby: colour on what is beside them, fill in the corners. DirectX 12.'),
                slider('Strength', FX, 'gi', 0, 0, 2, 0.05),
                slider('Radius', FX, 'gi_radius', 5, 0.5, 12, 0.5, '%.1f'),
                -- its samples a pixel (6 to 40; 4 to 14 a frame where the temporal pass gathers them) and its layer's size: more is
                -- less grainy and costs more. Twelve places, Performance to Quality, kept as gi_quality 0 to 4 (a place is 4/11 of
                -- it, so the old whole numbers 0 to 4 fall at places 1, 4, 6.5, 9 and 12)
                marks(1, 12, { { 1, 'Performance' }, { 4, 'Balanced' }, { 12, 'Quality' } }),
                (function ()
                    local row = slider('Quality', FX, 'gi_quality', 4, 1, 12, 1, '%d')
                    row.get = function ()
                        local q = tonumber(vana.config.get(FX, 'gi_quality') or '')
                        if q == nil then
                            return row.default
                        end
                        return math.max(row.min, math.min(row.max, math.floor(q * 11 / 4 + 0.5) + 1))
                    end
                    row.set = function (value)
                        vana.config.set(FX, 'gi_quality', string.format('%.2f', (value - 1) * 4 / 11))
                        vana.graphics.reload()
                    end
                    return row
                end)(),
                effect('Height fog', 'fog', 0.004),
                subheader('Amount'),
                slider('Density', FX, 'fog', 0.004, 0, 0.05, 0.001, '%.3f'),
                slider('Thinning with height', FX, 'fog_falloff', 0.08, 0, 0.5, 0.01),
                slider('Measured below the camera', FX, 'fog_height', 2.0, 0, 20, 0.5, '%.1f'),
                slider('Thickening stops below', FX, 'fog_floor', 4.0, 0, 30, 0.5, '%.1f'),
                slider('Most fog', FX, 'fog_max', 0.5, 0, 1, 0.05),
                subheader('Light'),
                slider('Sunlight in the fog', FX, 'fog_sun', 0.5, 0, 2, 0.05),
                slider('Glow toward the sun', FX, 'fog_g', 0.6, 0, 0.95, 0.05),
                subheader('Colour'),
                dropdown('Fog colour', FX, 'fog_color', 'game', { { 'game', "Zone's own" }, { 'sky', 'Match the sky' }, { 'custom', 'Custom' } }),
                colour_pick('Custom colour', FX, 'fog_custom', '#B8C8DC'),
                effect('Bloom', 'bloom', 1.5),
                slider('Strength', FX, 'bloom', 1.5, 0, 5, 0.05),
                slider('Threshold', FX, 'threshold', 0.85, 0, 1, 0.05),
                effect('God rays', 'rays', 0.6),
                slider('Strength', FX, 'rays', 0.6, 0, 2, 0.05),
                slider('Fade', FX, 'rays_decay', 0.965, 0.8, 1, 0.005, '%.3f'),
                slider('Length', FX, 'rays_length', 0.85, 0, 2, 0.05),
                header('Texture filtering'),
                toggle('Less shimmer (scene mips)', FX, 'filter', true),
                slider('Anisotropic', FX, 'aniso', 16, 1, 16, 1, '%dx'),
                -- the game's own Bump Mapping option, live (src/core/bumpmap.cpp): unset, [ffxi] bump_mapping decides
                toggle('Bump mapping', 'graphics', 'bump', (vana.config.get('ffxi', 'bump_mapping') or '0') == '1'),
                needs_dx(toggle('Untextured', 'graphics', 'untextured', false)),
                -- VanaCore's water (src/gfx/gfx_hlsl_water.h, d3d8_water.h): the zone's flat water tiles and level water models shaded
                -- with waves and ripples, the bed through them by its depth, the sky in them and the sun's glint, in place of the
                -- game's layers over them ([graphics] water* in vanacore.ini)
                warn(needs_dx({ kind = 'header', text = 'Water Shader (Experimental)', section = 'graphics', key = 'water',
                                switch = { get = function () return tonumber(vana.config.get('graphics', 'water') or '') == 1 end,
                                           set = function (on) vana.config.set('graphics', 'water', on and '1' or '0') end } })),
                callout('Extremely beta', 'Being heavily tested and debugged right now. What you see is not the final look.', 'danger'),
                note("VanaCore's own water in place of the game's: the zone's flat water tiles (Bastok Markets'), the game's level water models (the sea) and, with Find water at All, the ponds and streams in the zone's own meshes, wherever your footsteps say there's water. Waterfalls stay the game's. Off, the game's water is drawn."),
                needs_dx(choice('Find water', 'graphics', 'water.detect', '2', { { '0', 'Tiles' }, { '1', 'Water models' }, { '2', 'All' } })),
                subheader('Depth'),
                needs_dx(toggle('Water pass', 'graphics', 'water.pass', true)),
                needs_dx(toggle('Sea to the horizon (experimental)', 'graphics', 'water.horizon', false)),
                needs_dx(toggle('VanaCore sea', 'graphics', 'water.sea', true)),
                needs_dx(toggle('Shadows on the water', 'graphics', 'water.shadows', false)),
                note("The water drawn after the rest of the world, over it: the bed shows through where the water is shallow, its own colour takes over as it deepens, and the shore gets a soft edge and a line of foam. Off, the water is a solid sheet, as before."),
                needs_dx(slider('See-through depth', 'graphics', 'water.depth', 4, 0.5, 20, 0.5, '%.1f')),
                needs_dx(slider('Refraction', 'graphics', 'water.refraction', 1, 0, 3, 0.05)),
                needs_dx(slider('Shore width', 'graphics', 'water.shore', 0.5, 0, 3, 0.05)),
                needs_dx(slider('Foam', 'graphics', 'water.foam', 1, 0, 3, 0.05)),
                needs_dx(slider('Waterfalls', 'graphics', 'water.falls', 1, 0, 3, 0.05)),
                needs_dx(slider('Caustics', 'graphics', 'water.caustics', 1, 0, 3, 0.05)),
                needs_dx(slider('Relief', 'graphics', 'water.relief', 1, 0, 3, 0.05)),
                -- VanaCore's rain (src/gfx/d3d8_rain.h, gfx_hlsl_rain.h): while the weather is rain
                subheader('Rain'),
                needs_dx(toggle('Rain', 'graphics', 'rain', true)),
                note("VanaCore's own rain while the weather is rain: streaks that the world and the characters hide, no rain under roofs, bridges or in caves, splashes where the drops land on the ground and on the characters, and a wet sheen on the ground."),
                needs_dx(slider('Density', 'graphics', 'rain.density', 1, 0.2, 3, 0.05)),
                needs_dx(slider('Splashes', 'graphics', 'rain.splash', 1, 0, 3, 0.05)),
                needs_dx(slider('Splash Range', 'graphics', 'rain.range', 48, 8, 500, 1, '%.0f yalms')),
                needs_dx(slider('Rain Range', 'graphics', 'rain.far', 150, 52, 500, 1, '%.0f yalms')),
                needs_dx(slider('Rings on water', 'graphics', 'rain.water', 1, 0, 3, 0.05)),
                needs_dx(slider('Spatter size', 'graphics', 'rain.spatter', 1, 0, 3, 0.05)),
                needs_dx(toggle('Shelter', 'graphics', 'rain.shelter', true)),
                needs_dx(toggle('Rain in thunder', 'graphics', 'rain.thunder', true)),
                needs_dx(toggle("Keep the game's rain too", 'graphics', 'rain.game', false)),
                needs_dx(toggle('Preview rain', 'graphics', 'rain.force', false)),
                subheader('Lens drops'),
                needs_dx(toggle('Lens drops', 'graphics', 'rain.lens.on', true)),
                needs_dx(slider('Lens droplets', 'graphics', 'rain.lens', 0.5, 0, 3, 0.05)),
                subheader('Character wetness'),
                needs_dx(toggle('Wet characters', 'graphics', 'rain.wet', true)),
                needs_dx(slider('Reflections', 'graphics', 'rain.sheen', 1, 0, 3, 0.05)),
                needs_dx(slider('Drying time', 'graphics', 'rain.dry', 45, 1, 300, 1, '%d s')),
                subheader('Wet ground'),
                needs_dx(toggle('Wet ground', 'graphics', 'rain.ground', true)),
                needs_dx(slider('Reflections', 'graphics', 'rain.reflect', 1, 0, 2, 0.05)),
                needs_dx(slider('Puddles', 'graphics', 'rain.puddles', 1, 0, 2, 0.05)),
                subheader('Colour'),
                needs_dx(colour_pick('Water colour', 'graphics', 'water.tint', '#0F3D42')),
                note("A picked colour is the water as it looks in the shallows, the deep a darker one of it, and replaces the blue to green slider. \"Game's\" means no colour is picked."),
                needs_dx(slider('Blue to green', 'graphics', 'water.colour', 0.5, 0, 1, 0.02)),
                needs_dx(slider('Brightness', 'graphics', 'water.brightness', 1, 0.2, 3, 0.05)),
                needs_dx(slider('Shallowness', 'graphics', 'water.shallowness', 1, 0, 2, 0.05)),
                subheader('Waves'),
                needs_dx(slider('Turbulence', 'graphics', 'water.turbulence', 1, 0, 4, 0.05)),
                needs_dx(slider('Height', 'graphics', 'water.waves', 1, 0, 3, 0.05)),
                needs_dx(slider('Speed', 'graphics', 'water.speed', 1, 0, 3, 0.05)),
                needs_dx(slider('Size', 'graphics', 'water.size', 1, 0.25, 4, 0.05)),
                needs_dx(slider('Direction', 'graphics', 'water.direction', 0, 0, 360, 5, '%d\194\176')),
                needs_dx(slider('Ripples', 'graphics', 'water.ripples', 1, 0, 3, 0.05)),
                needs_dx(slider('Ripple size', 'graphics', 'water.ripple.size', 1, 0.25, 4, 0.05)),
                subheader('Reflections'),
                needs_dx(slider('Sky reflection', 'graphics', 'water.reflection', 1, 0, 2, 0.05)),
                needs_dx(slider('Sun and moon glint', 'graphics', 'water.glint', 1, 0, 3, 0.05)),
                needs_dx(slider('Glint size', 'graphics', 'water.glint.size', 1, 0.2, 4, 0.05)),
                needs_dx(slider('World mirror', 'graphics', 'water.mirror', 1, 0, 2, 0.05)),
                note("The shore, trees, walls and cliffs mirrored in the water, upside down and broken up by the waves, and the sky as it is (clouds, a dusk's colours), where the water showed two sky colours alone. One level of water at a time: the lake, pond or sea nearest the camera. 0 is off. Needs the water pass."),
                needs_dx(slider('Mirror detail', 'graphics', 'water.mirror.scale', 0.5, 0.25, 1, 0.05)),
                needs_dx(slider('Mirror reach', 'graphics', 'water.mirror.reach', 300, 0, 2000, 10, '%d')),
                needs_dx(slider('Mirror ripple', 'graphics', 'water.mirror.waves', 1, 0, 4, 0.05)),
                -- Mass Grass (src/gfx/grass.c): tufts and flowers scattered on the zone's grass-textured ground round the player;
                -- shown as Game Options' last sub-tab since 2026-11-06, it was the Experimental page's (vana.lua MOVED_PANELS)
                -- reset_also: its settings no longer on the panel, taken out by its reset too (reset_rows). grass.tufts and grass.limit
                -- are the old Most tufts ceiling (24 thousand by default until 2026-11-22, written by any preset loaded before then):
                -- left in with the new Range of 150 it spread the tufts to a fortieth of the default's, and Reset left it
                needs_dx({ kind = 'header', text = 'Mass Grass', section = 'graphics', key = 'grass',
                           switch = { get = function () return tonumber(vana.config.get('graphics', 'grass') or '') == 1 end,
                                      set = function (on) vana.config.set('graphics', 'grass', on and '1' or '0') end },
                           reset_also = { { 'graphics', 'grass.tufts' }, { 'graphics', 'grass.limit' },
                                          { 'graphics', 'grass.ring1' }, { 'graphics', 'grass.ring1.amount' },
                                          { 'graphics', 'grass.ring2' }, { 'graphics', 'grass.ring2.amount' },
                                          { 'graphics', 'grass.ring3' }, { 'graphics', 'grass.ring3.amount' },
                                          { 'graphics', 'grass.dry.saturation' }, { 'graphics', 'grass.dry.hue' },
                                          { 'graphics', 'grass.textures' }, { 'graphics', 'grass.blended' } } }),
                note("Made for West Ronfaure; other zones are untried. It's a work in progress: the grass is drawn over the ground the game "
                     .. "already shows, where its texture is grass. A wider range and a higher quantity cost frame rate."),
                subheader('Coverage'),
                -- Range in tens of yalms (grass.radius / 10: 7 is 70), 1 to 20 on the slider, typed up to 50 (500, VanaCore's most)
                needs_dx(shown_as(free(slider('Range', 'graphics', 'grass.radius', 15, 1, 20, 1, '%g'), 0.4, 50), 0.1)),
                -- Quantity (grass.density, tufts to a square yalm): the tufts kept follow it and the range (grass.c tuft_limit), so
                -- Most tufts (grass.tufts, in thousands) isn't on the panel since 2026-11-22; a change here takes out a ceiling left
                -- set by it, or by grass.limit from before it, which would keep a higher quantity from showing
                needs_dx(free((function (row)
                    row.changed = function ()
                        if vana.config.get('graphics', 'grass.tufts') ~= nil or vana.config.get('graphics', 'grass.limit') ~= nil then
                            vana.config.set('graphics', 'grass.tufts', nil)
                            vana.config.set('graphics', 'grass.limit', nil)
                            vana.graphics.reload()
                        end
                    end
                    return row
                end)(slider('Quantity', 'graphics', 'grass.density', 100, 1, 100, 1, '%g%%')), 0.1, 100)),
                -- every spot of ground the zone's collision calls grass grows, whatever its texture's colour there (grass.c grass_floor)
                needs_dx(toggle('Mega dense', 'graphics', 'grass.mega', false)),
                -- (Show rings: the Debug window's Rings tab, Show covered grass its Grass tab, since 2026-11-22, debug.lua; Ring
                -- spacing, now Draw distance detail, and Placement under Advanced Settings: "i dont want people messing it up")
                subheader('Blades'),
                -- Height as a percent: 0.10 tall is 1%, 1.50 is 100% (grass.height = 0.1 + (percent - 1) * 1.4 / 99); typed past
                -- 100 up to 700 (9.9)
                needs_dx(shown_as(free(slider('Height', 'graphics', 'grass.height', 28, 1, 100, 1, '%d%%'), 0, 700), 99 / 1.4, 1 - 0.1 * 99 / 1.4)),
                needs_dx(slider('Height variability', 'graphics', 'grass.variation', 0.5, 0, 1, 0.05)),
                needs_dx(free(slider('Width', 'graphics', 'grass.width', 1, 0.2, 2, 0.1, '%.2f'), 0.1, 10)),
                needs_dx(slider('Shorter near edges (yalms)', 'graphics', 'grass.edge.fade', 2, 0, 10, 0.25, '%.2f')),
                needs_dx(slider('Height at edges', 'graphics', 'grass.edge.height', 25, 5, 100, 1, '%d%%')),
                subheader('Colour'),
                needs_dx(free(slider('Brightness', 'graphics', 'grass.brightness', 1, 0.2, 3, 0.05), 0.05, 10)),
                needs_dx(slider('Match ground colour', 'graphics', 'grass.match', 0.75, 0, 1, 0.05)),
                needs_dx((function (row) row.rebuild = true return row end)( -- Night brightness locks and unlocks by it
                    toggle('Dim at night', 'graphics', 'grass.brightness.auto', true))),
                needs_dx((function (row) row.lock_if = function () return (tonumber(vana.config.get('graphics', 'grass.brightness.auto') or '') or 1) == 0 end return row end)(
                    slider('Night brightness', 'graphics', 'grass.brightness.night', 0.5, 0.1, 1, 0.05))),
                subheader('Wind'),
                needs_dx(slider('Wind speed', 'graphics', 'grass.wind.speed', 1, 0, 3, 0.05, '%.2f')),
                needs_dx(slider('Wind turbulence', 'graphics', 'grass.wind.turbulence', 1, 0, 3, 0.05, '%.2f')),
                subheader('Flowers'),
                -- (1: a tenth of the tufts with a flower, graphics.cpp; it was the share itself till 2026-11-22)
                needs_dx(slider('Flowers (drawn)', 'graphics', 'grass.flowers', 0, 0, 1, 0.1, '%.1f')),
                needs_dx(free(slider('Flower brightness', 'graphics', 'grass.flower_brightness', 1, 0.2, 3, 0.05), 0.05, 10)),
                needs_dx(toggle('Flowers only', 'graphics', 'grass.flowers_only', false)),
                -- what grows: the catalogue's assets, each on or off with its share (grass_asset_rows)
                { kind = 'none', grass_assets = true },
                -- Advanced Settings, as Shadows' are: hidden unless Enable Advanced Graphics Settings is on (2026-11-22: "reduce
                -- the amount of options"); a group runs to the next heading, Snow Trails'
                advanced_group('Fine-tuning for the grass: how it thins with distance, where it may grow, the free camera, and how fast '
                               .. 'it fills in. Most players never need these.'),
                -- the rings (grass.ring.spacing: rings 1-3 at it, twice it and three times it, a share of the Range)
                needs_dx(free(slider('Draw distance detail', 'graphics', 'grass.ring.spacing', 25, 10, 50, 1, '%d%%'), 1, 100)),
                needs_dx(toggle('Follow free camera', 'graphics', 'grass.freecam', true)),
                needs_dx(free(slider('Growing time a frame', 'graphics', 'grass.build', 1.5, 0.25, 8, 0.25, '%.2f ms'), 0.1, 50)),
                -- (grass.c layer_in_view: a square of tufts wholly outside the camera's view isn't drawn, still made and kept)
                needs_dx(toggle('Grass culling', 'graphics', 'grass.cull', true)),
                inner_sub('Placement', nil, 'in'),
                needs_dx(free(slider('Max slope angle', 'graphics', 'grass.slope', 40, 10, 80, 1, '%d\194\176'), 5, 85)),
                needs_dx(toggle('Edge detection', 'graphics', 'grass.edges', true)),
                needs_dx(toggle('Dry grass', 'graphics', 'grass.dry', false)),
                needs_dx(toggle('Walkable ground only', 'graphics', 'grass.walkable', true)),
                needs_dx(toggle('Shaded ground', 'graphics', 'grass.shade', true)),
                needs_dx(toggle('No grass in water', 'graphics', 'grass.nowater', true)),
                needs_dx((function (row) row.sub = true return row end)(toggle('Moss in water', 'graphics', 'grass.moss', false))),
                needs_dx(toggle('Overgrown', 'graphics', 'grass.overgrown', false)),
                -- Path padding (grass.pathpad): the default for the zones data\grass_dirt.json gives none of their own; in one that
                -- has its own (vana.graphics.grass_pathpad: Wajaom's 0) locked, showing that (2026-11-22, per zone since 11-22)
                needs_dx((function (row)
                    local function own()
                        if vana.graphics.grass_pathpad == nil then
                            return nil
                        end
                        local ok, value = pcall(vana.graphics.grass_pathpad)
                        return ok and value or nil
                    end
                    row.lock_if = function () return own() ~= nil end
                    row.get = function ()
                        local here = own()
                        if here ~= nil then
                            return here
                        end
                        local value = tonumber(vana.config.get('graphics', 'grass.pathpad') or '')
                        return value ~= nil and math.max(row.min, math.min(row.max, value)) or row.default
                    end
                    return row
                end)(slider('Path padding', 'graphics', 'grass.pathpad', 0.25, 0, 4, 0.25, '%.2f'))),
                -- Snow Trails (src/gfx/snow.c): footprints pressed into ground the zone's collision calls snow, drawn as shading
                warn(needs_dx({ kind = 'header', text = 'Snow Trails (Experimental)', section = 'graphics', key = 'snow',
                                switch = { get = function () return tonumber(vana.config.get('graphics', 'snow') or '') == 1 end,
                                           set = function (on) vana.config.set('graphics', 'snow', on and '1' or '0') end } })),
                callout('In development', 'Super early stages. Expect it to change a lot.', 'warning'),
                needs_dx(free(slider('Depth', 'graphics', 'snow.depth', 0.12, 0.03, 0.4, 0.01, '%.2f'), 0.02, 0.5)),
                needs_dx(free(slider('Shading', 'graphics', 'snow.strength', 1, 0, 2, 0.05), 0, 2)),
                needs_dx(slider('Softness', 'graphics', 'snow.soft', 0.6, 0, 1, 0.05)),
                needs_dx(free(slider('Fill-in time', 'graphics', 'snow.fade', 120, 0, 600, 10, '%d s'), 0, 86400)),
                needs_dx(slider('Sink into snow', 'graphics', 'snow.sink', 0, 0, 0.6, 0.02, '%.2f')),
                needs_dx(toggle('Trails in sand', 'graphics', 'snow.sand', false)),
                -- (a Graphics panel since 2026-11-04: it was the Experimental page's Glow Effects Lighting)
                needs_dx12(effect('Glow Effects', 'elight', 2.5, 2.5)),
                needs_dx12(slider('Strength', FX, 'elight', 2.5, 0, 10, 0.05)),
                needs_dx12(slider('Reach', FX, 'elight_radius', 30, 1, 50, 0.5, '%.1f')),
                needs_dx12(slider('Lanterns', FX, 'elight_lantern', 0.5, 0, 1, 0.05)),
                needs_dx12(slider('Spells and abilities', FX, 'elight_actor', 0.4, 0, 10, 0.05)),
            }
            return grass_asset_rows(rows)
        end,
    },
    {
        name = 'Effects',
        search = true,
        aliases = { 'image', 'style', 'characters' },
        rows = function ()
            -- clarity, sharpening and smoothing had their own switches (<key>.enabled) before they were the colour grade's
            -- sliders: one switched off is set to 0 instead, so the slider can turn it on again
            for _, key in ipairs({ 'clarity', 'sharpen', 'smooth' }) do
                if vana.config.get(FX, key .. '.enabled') == '0' then
                    vana.config.set(FX, key, '0')
                    vana.config.set(FX, key .. '.enabled', nil)
                end
            end
            return {
                effect('Colour grade', 'grade', 1.0),
                slider('Strength', FX, 'grade', 1.0, 0, 1, 0.05),
                slider('Saturation', FX, 'sat', 1.12, 0, 2, 0.02),
                slider('Contrast', FX, 'contrast', 0.2, 0, 1, 0.05),
                slider('Warmth', FX, 'warmth', 0, -1, 1, 0.05),
                -- the grade's warmth through the Vana'diel day (GfxScene.day_night): warmer by day, cooler at night, outdoors.
                -- Laid by the Colour grade, so it needs that on (gfx_d3d12.c grade_wanted, the D3D11 composite's u.grade.x). On
                -- Graphics > Lighting as Temperature, with its own heading and switch, till 2026-11-24
                M.unswitched(slider('Day/night warmth', FX, 'day_night_warmth', 0, 0, 2, 0.05), 'day_night_warmth'),
                slider('Hue', FX, 'hue', 0, -180, 180, 1, '%d\194\176'),
                slider('Brightness', FX, 'brightness', 0, -1, 1, 0.05),
                slider('Shadows', FX, 'shadows', 0, -1, 1, 0.05),
                slider('Highlights', FX, 'highlights', 0, -1, 1, 0.05),
                -- zones between the shadows and the highlights, and a shoulder so lifted lights ease into white rather than clip
                slider('Midtones', FX, 'midtones', 0, -1, 1, 0.05),
                slider('Whites', FX, 'whites', 0, -1, 1, 0.05),
                slider('Soft highlights', FX, 'rolloff', 0, 0, 1, 0.05),
                slider('Light tint', FX, 'light_tint', 0, -1, 1, 0.05),
                -- local contrast (each pixel's lead on its surroundings), sharpening at Present, the cel look's smoothing: the
                -- grade's, off with it (the renderer)
                slider('Clarity', FX, 'clarity', 0, 0, 2, 0.05),
                slider('Sharpening', FX, 'sharpen', 0.3, 0, 1, 0.05),
                slider('Smoothing', FX, 'smooth', 0, 0, 1, 0.05),
                effect('Ink lines', 'outline', 0, 1),
                slider('Darkness', FX, 'outline', 0, 0, 1, 0.05),
                slider('Line width', FX, 'outline_width', 1, 0.5, 4, 0.25),
                needs_dx12(slider('Character line width', FX, 'outline_width_chars', 0, 0, 4, 0.25)),
                slider('Edge sensitivity', FX, 'outline_depth', 0.1, 0.01, 0.5, 0.01),
                slider('Fold sensitivity', FX, 'outline_crease', 0.25, 0, 1, 0.05),
                slider('Lines fade by', FX, 'outline_fade', 60, 5, 200, 5, '%d'),
                -- VanaCore's grass (Mass Grass): apart from the scenery, the scenery's lines found as if it weren't there, and its
                -- own lines (fx_comp, toon4: gfx_grass_lines); all locked while Mass Grass is off, the grass's own lines while
                -- it's outlined with the scenery
                subheader('Grass'),
                grass_lines(toggle('Separate from scenery', FX, 'outline_nograss', true)),
                grass_lines(slider('Grass darkness', FX, 'outline_grass', 0, 0, 1, 0.05), 'Off', '%.2f'),
                grass_lines(slider('Grass line width', FX, 'outline_width_grass', 0, 0, 4, 0.25), 'Same', '%.2f'),
                grass_lines(slider('Grass edge sensitivity', FX, 'outline_depth_grass', 0, 0, 0.5, 0.01), 'Same', '%.2f'),
                grass_lines(slider('Grass lines fade by', FX, 'outline_fade_grass', 0, 0, 200, 5, '%d'), 'Same', '%d'),
                effect('Brightness bands', 'bands', 0, 4),
                slider('Bands', FX, 'bands', 0, 0, 8, 1, '%d'),
                slider('Band softness', FX, 'band_soft', 0.15, 0, 1, 0.05),
                effect('Depth of field', 'dof', 0, 12),
                slider('Blur', FX, 'dof', 0, 0, 40, 1, '%d'),
                slider('Sharp zone', FX, 'dof_range', 0.5, 0, 3, 0.05),
                slider('Blur builds over', FX, 'dof_far', 3, 0.2, 10, 0.1, '%.1f'),
                slider('Blur in front', FX, 'dof_near', 0, 0, 1, 0.05),
                slider('Fixed focus (0: you)', FX, 'dof_focus', 0, 0, 100, 1, '%d'),
                toggle('Keep the sky sharp', FX, 'dof_sky', false),
            }
        end,
    },
    {
        name = 'Options',
        search = true,
        aliases = { 'world', 'camera', 'trusts', 'npc', 'damage' }, -- (Zones and Loading Screen are the Mods page's: vana.lua MOVED_OPTIONS)
        rows = function ()
            -- A kind of damage number's colours on one line (vana.lua kind 'colours'): its colour (`none` what it is unset),
            -- the gradient's second colour, then a Gradient checkbox; one colour, the first, while that's off (the second
            -- greyed). The gradient is True Font's alone; `locked` locks the whole line
            local function sharp() return (vana.config.get('graphics', 'damage.font') or '') == 'sharp' end
            local function damage_colours(label, prefix, first, second, none, locked)
                local one = colour_pick(label .. ' colour', 'graphics', prefix .. '.color', first)
                local two = colour_pick(label .. ' colour 2', 'graphics', prefix .. '.color2', second)
                local grad = toggle('Gradient', 'graphics', prefix .. '.gradient', false)
                one.none, two.none = none, 'Default'
                grad.rebuild = true -- (Gradient direction locks and unlocks by it)
                grad.lock_if = function () return not sharp() end
                two.lock_if = function () return not sharp() or vana.config.get('graphics', prefix .. '.gradient') ~= '1' end
                local row = { kind = 'colours', label = label, parts = { one, grad, two }, source = one.key, lock_if = locked }
                row.reset = function () -- (the colours; the checkbox, a switch, as it is)
                    vana.config.set('graphics', one.key, nil)
                    vana.config.set('graphics', two.key, nil)
                end
                return row
            end
            local function crit_off() return vana.config.get('graphics', 'damage.crit') == '0' end
            return {
                header('Camera'),
                subheader('Zoom'),
                (function ()
                    local row = toggle('Use true zoom', 'camera', 'zoom', false)
                    row.get = true_zoom -- 'distance' in vanacore.ini is on too
                    row.rebuild = true -- the field of view locks and unlocks by it
                    return row
                end)(),
                note('The mouse wheel and zoom keys move the camera nearer and farther, where the game\'s zoom narrows the view.'),
                -- free: a value typed into the box goes past the slider's ends, as far as camera.cpp takes it
                zoomed(free(slider('Field of view', 'camera', 'fov', 57.5, 20, 100, 0.5, '%.1f\194\176'), 5, 170)),
                zoomed(free(slider('Max zoom distance', 'camera', 'distance.max', 30, 6, 50, 1, '%d'), 1, 500)),
                zoomed(free(slider('Starting zoom', 'camera', 'distance', 6, 1.5, 30, 0.5, '%.1f'), 0.5, 500)),
                zoomed(toggle('Remember zoom', 'camera', 'distance.remember', false)),
                -- either zoom (camera.cpp points the game's at its speed too); shown as it's dragged
                (function (row) row.preview = function (value) vana.camera.preview(row.key, value) end return row end)(
                    free(slider('Zoom speed', 'camera', 'zoom.speed', 1, 0.25, 4, 0.05, '%.2fx'), 0.1, 10)),
                -- what xicamera did besides the distance (it's held off while True Zoom is on)
                subheader('Turning'),
                zoomed(free(slider('Turn speed', 'camera', 'turn', 1, 0.25, 4, 0.05, '%.2fx'), 0.01, 20)),
                zoomed(free(slider('Vertical turn speed', 'camera', 'turn.vertical', 1, 0.25, 4, 0.05, '%.2fx'), 0.01, 20)),
                toggle('Turn at screen edge', 'camera', 'mouse.edgeturn', true),
                toggle('Disable left-click camera move', 'camera', 'mouse.noleftdrag', true),
                toggle('Hide the cursor while turning the camera', 'camera', 'mouse.hidecursor', false),
                toggle('Home key resets the camera', 'camera', 'home.reset', false),
                toggle('Change View button resets the camera', 'camera', 'view.reset', false),
                toggle('Free look', 'camera', 'freelook', false),
                subheader('Following'),
                toggle('Keep the camera out of walls', 'camera', 'walls', true),
                toggle('Lock the camera to your character', 'camera', 'lock', false),
                toggle('Swing behind when moving', 'camera', 'behind', false),
                (function (row) row.preview = function (value) vana.camera.preview(row.key, value) end return row end)(
                    free(slider('Swing speed', 'camera', 'behind.speed', 1, 0.25, 4, 0.05, '%.2fx'), 0.1, 10)),
                (function (row) row.preview = function (value) vana.camera.preview(row.key, value) end return row end)(
                    free(slider('Swing delay', 'camera', 'behind.delay', 1.5, 0, 5, 0.1, '%.1f s'), 0, 10)),
                subheader('Lock-on'),
                zoomed(free(slider('Lock-on distance', 'camera', 'battle.distance', 1, 0.5, 3, 0.05, '%.2fx'), 0.1, 10)),
                zoomed(free(slider('Lock-on range', 'camera', 'battle.range', 4, 0, 30, 0.5, '%.1f'), 0, 100)),
                note('Lock-on range: how far round the target can get before the locked-on camera turns to keep it in view (the game\'s is 4). xicamera is held off while true zoom is on.'),
                -- where your character sits on screen, with or without true zoom (camera.cpp moves the camera); shown as it's dragged
                subheader('Position on screen'),
                (function (row) row.preview = function (value) vana.camera.preview(row.key, value) end return row end)(
                    free(slider('Horizontal position', 'camera', 'offset.x', 0, -30, 30, 1, '%d%%'), -45, 45)),
                (function (row) row.preview = function (value) vana.camera.preview(row.key, value) end return row end)(
                    free(slider('Vertical position', 'camera', 'offset.y', 0, -30, 30, 1, '%d%%'), -45, 45)),
                header('Damage Numbers'),
                -- (cyan, name and switch, and first: asked for 2026-11-10)
                (function (row) row.colour = M.P.cyan row.lock_if = function () return (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    toggle('Show preview', 'graphics', 'damage.preview', false)),
                free(slider('Time on screen', 'graphics', 'damage.duration', 2.5, 1, 6, 0.1, '%.1f s'), 0.5, 10),
                subheader('Text'),
                -- their True Font on one line: their own font (no default font), italic, bold, size a multiple of the game's
                -- and outline, its switch (damage.font sharp or game; the rows locked by it unlock as it's built again)
                (function (row) row.switch = area_switch('damage.font', nil, nil, 'graphics') return row end)(
                    font_line(dropdown('Font', 'graphics', 'damage.family', 'Segoe UI', font_list('graphics', 'damage.family')),
                              free(slider('Text size', 'graphics', 'damage.textsize', 1, 0.5, 2, 0.05, '%.2fx'), 0.5, 3),
                              toggle('Italic', 'graphics', 'damage.italic', true),
                              toggle('Bold', 'graphics', 'damage.bold', true), nil,
                              free(slider('Outline', 'graphics', 'damage.outline', 1, 0, 3, 0.5, '%.2fx'), 0, 4))),
                (function (row) row.none = 'Black' row.lock_if = function () return (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    colour_pick('Outline colour', 'graphics', 'damage.outline.color', '#000000')),
                (function (row) row.lock_if = function () return (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    toggle('Commas (1,337)', 'graphics', 'damage.commas', false)),
                (function (row) row.lock_if = function () return (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    toggle('Capitalise MISS!', 'graphics', 'damage.misscaps', false)),
                toggle('Always on top', 'graphics', 'damage.ontop', true),
                subheader('Critical Hits'),
                (function (row) row.rebuild = true return row end)( -- the critical hit rows lock and unlock by it
                    toggle('Critical hits', 'graphics', 'damage.crit', true)),
                (function (row) row.lock_if = function () return vana.config.get('graphics', 'damage.crit') == '0' or (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    free(slider('Critical size', 'graphics', 'damage.crit.size', 1.3, 1, 2, 0.05, '%.2fx'), 1, 3)),
                (function (row) row.lock_if = function () return vana.config.get('graphics', 'damage.crit') == '0' or (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    toggle('Critical !!', 'graphics', 'damage.crit.exclaim', false)),
                subheader('Placement'),
                (function (row) row.rebuild = true return row end)( -- Spread locks and unlocks by it
                    choice('Spawn style', 'graphics', 'damage.spawn', 'game', { { 'game', "Game's" }, { 'stacked', 'Stacked' }, { 'random', 'Random' } })),
                (function (row) row.lock_if = function () return (vana.config.get('graphics', 'damage.spawn') or 'game') == 'game' end return row end)(
                    free(slider('Spread', 'graphics', 'damage.spread', 1, 0, 3, 0.1, '%.1f'), 0, 10)),
                -- the tests on one line (vana.lua: an action row's `more` buttons before its own)
                { kind = 'action', label = 'Test', text = 'A fight', action = 'damage_test_barrage',
                  tip = 'Shows nine hits over two seconds: hits, a critical hit, a miss, a hit on you and a double attack.',
                  more = {
                      { kind = 'action', label = 'One hit', text = 'One hit', action = 'damage_test_hit',
                        tip = 'Shows one number over your character, placed as the settings here say.' },
                      { kind = 'action', label = 'Three hits', text = 'Three hits', action = 'damage_test_round',
                        tip = 'Shows three hits of one weapon skill at once: where Stacked and Random put them, the multi-hit icon and the skill name.' },
                  } },
                free(slider('Offset left / right', 'graphics', 'damage.offset.x', 0, -3, 3, 0.1, '%.1f'), -10, 10),
                free(slider('Offset up / down', 'graphics', 'damage.offset.y', 0, -3, 3, 0.1, '%.1f'), -10, 10),
                subheader('Colours'),
                damage_colours('Hit', 'damage', '#FF8282', '#C81E1E', "Game's"),
                damage_colours('Miss', 'damage.miss', '#9CFF9C', '#2E9E4A', "Game's"),
                damage_colours('Hit taken', 'damage.taken', '#FFFFFF', '#9AA4B4', "Hit's"),
                damage_colours('Heal', 'damage.heal', '#B0FFB0', '#2EA860', "Game's"),
                damage_colours('Critical', 'damage.crit', '#FFD040', '#FF6A00', 'Default', crit_off),
                (function (row)
                    row.lock_if = function ()
                        for _, prefix in ipairs({ 'damage', 'damage.miss', 'damage.taken', 'damage.heal', 'damage.crit' }) do
                            if vana.config.get('graphics', prefix .. '.gradient') == '1' then
                                return not sharp()
                            end
                        end
                        return true
                    end
                    return row
                end)(choice('Gradient direction', 'graphics', 'damage.gradient.direction', 'vertical',
                            { { 'vertical', 'Top to bottom' }, { 'horizontal', 'Left to right' } })),
                (function (row) row.lock_if = function () return not sharp() end return row end)(
                    toggle('Heal +', 'graphics', 'damage.heal.plus', false)),
                subheader('Multi-hit'),
                (function (row) row.rebuild = true row.lock_if = function () return (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)( -- the icon rows lock and unlock by it
                    toggle('Multi-hit icon', 'graphics', 'damage.multi', false)),
                (function (row) row.lock_if = function () return vana.config.get('graphics', 'damage.multi') ~= '1' or (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    choice('Icon', 'graphics', 'damage.multi.icon', 'swords', { { 'swords', 'Crossed swords' }, { 'count', 'Hit count (x2)' } })),
                (function (row) row.lock_if = function () return vana.config.get('graphics', 'damage.multi') ~= '1' or (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    free(slider('Icon size', 'graphics', 'damage.multi.size', 0.6, 0.3, 1.2, 0.05, '%.2fx'), 0.2, 2)),
                subheader('Skill names'),
                (function (row) row.rebuild = true row.lock_if = function () return (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    toggle('Show skill name', 'graphics', 'damage.label', false)),
                (function (row) row.lock_if = function () return vana.config.get('graphics', 'damage.label') ~= '1' or (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    free(slider('Name size', 'graphics', 'damage.label.size', 0.5, 0.3, 1, 0.05, '%.2fx'), 0.2, 1.5)),
                subheader('Size Limits'),
                warning('Numbers shrink with distance and grow up close. 0 = no limit.', 'grey'),
                (function (row) row.lock_if = function () return (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    free(slider('Minimum size', 'graphics', 'damage.min', 0, 0, 48, 1, '%.0f px'), 0, 200)),
                (function (row) row.lock_if = function () return (vana.config.get('graphics', 'damage.font') or '') ~= 'sharp' end return row end)(
                    free(slider('Maximum size', 'graphics', 'damage.max', 0, 0, 200, 2, '%.0f px'), 0, 400)),
                header('NPC effects'),
                -- the glow is VanaCore's renderer's (its targets made larger); the arrival effect any renderer's
                needs_dx(choice('Trust and avatar glow', 'graphics', 'glow', 'custom', { { 'off', 'Off' }, { '256', '256 (game)' }, { '512', '512' }, { '1024', '1024' }, { 'custom', 'Custom' } })),
                needs_dx(slider('Glow strength', 'graphics', 'glow.strength', 1, 0, 3, 0.05, '%.2fx')),
                needs_dx(slider('Glow size', 'graphics', 'glow.size', 1, 0.1, 2, 0.05, '%.2fx')),
                needs_dx(choice('Glow quality', 'graphics', 'glow.quality', '1', { { '0', 'Low' }, { '1', 'Medium' }, { '2', 'High' }, { '3', 'Max' } })),
                choice('Trust arrival effect', 'graphics', 'trusts.spawn', 'cast', { { 'never', 'Never' }, { 'cast', 'When summoned' }, { 'always', 'Always' } }),
                -- zone lines are drawn by VanaCore's renderer
                header('Zones'),
                subheader('Zone Lines'),
                needs_dx(toggle('Show zone lines', 'graphics', 'zonelines', true)),
                needs_dx(choice('Style', 'graphics', 'zonelines.style', 'dots', { { 'fill', 'Fill' }, { 'line', 'Line' }, { 'dots', 'Dots' }, { 'stars', 'Stars' }, { 'floor', 'Floor dots' } })),
                needs_dx(toggle('Glow', 'graphics', 'zonelines.glow', true)),
                needs_dx(colour_pick('Colour', 'graphics', 'zonelines.color', '#60C0FF')),
                needs_dx(slider('Strength', 'graphics', 'zonelines.strength', 1, 0, 1, 0.05)),
                needs_dx(free(slider('Height above ground', 'graphics', 'zonelines.height', 1.5, -2, 8, 0.1, '%.1f'), -10, 10)),
                needs_dx(free(slider('Size', 'graphics', 'zonelines.size', 1, 0.25, 3, 0.05, '%.2fx'), 0.25, 4)),
                needs_dx(free(slider('Edge fade', 'graphics', 'zonelines.fade', 4, 0, 5, 0.1, '%.1f'), 0, 10)),
                -- the zone's name in the middle of the screen when you zone in (zonename.lua): Show holds it on screen to set it up
                -- by, Preview plays it as it runs; the gradient is the letters' colours from the first to the second, left to right
                subheader('Zone Name'),
                toggle('Show the zone name', 'client', 'zonename', true),
                font_line(dropdown('Font', 'client', 'zonename.family', 'Georgia', font_list('client', 'zonename.family')),
                          nil, -- (its size is the slider below, before Height)
                          toggle('Italic', 'client', 'zonename.italic', false),
                          toggle('Bold', 'client', 'zonename.bold', true),
                          toggle('Capitalise', 'client', 'zonename.uppercase', false),
                          slider('Outline', 'client', 'zonename.outline', 1, 0, 3, 0.5, '%.1fx')),
                (function ()
                    local one = colour_pick('Colour', 'client', 'zonename.color', '#FFFFFF')
                    local two = colour_pick('Colour 2', 'client', 'zonename.color2', '#FFC850')
                    local grad = toggle('Gradient', 'client', 'zonename.gradient', false)
                    grad.rebuild = true -- (the second colour locks and unlocks by it)
                    two.lock_if = function () return vana.config.get('client', 'zonename.gradient') ~= '1' end
                    local row = { kind = 'colours', label = 'Colour', parts = { one, grad, two }, source = one.key }
                    row.reset = function ()
                        vana.config.set('client', one.key, nil)
                        vana.config.set('client', two.key, nil)
                    end
                    return row
                end)(),
                (function (row) row.lock_if = function () return vana.config.get('client', 'zonename.gradient') ~= '1' end return row end)(
                    choice('Gradient direction', 'client', 'zonename.gradient.direction', 'horizontal',
                           { { 'horizontal', 'Left to right' }, { 'vertical', 'Top to bottom' } })),
                colour_pick('Outline colour', 'client', 'zonename.outline.color', '#000000'),
                free(slider('Font size', 'client', 'zonename.fontsize', 60, 20, 125, 5, '%d px'), 6, 128),
                pair('Position', 'Where the middle of the name sits, as a share of the screen: X from the left edge, Y from the top.',
                     { slider('X', 'client', 'zonename.x', 50, 0, 100, 1, '%d%%'),
                       slider('Y', 'client', 'zonename.y', 40, 0, 100, 1, '%d%%') }),
                -- the region's name above the zone's (zonename.lua): its own switch and look (zonename.region.*), the animation shared
                subheader('Region Name'),
                toggle('Show the region name', 'client', 'zonename.region', false),
                font_line(dropdown('Font', 'client', 'zonename.region.family', 'Georgia', font_list('client', 'zonename.region.family')),
                          nil, -- (its size is the slider below, before Position)
                          toggle('Italic', 'client', 'zonename.region.italic', false),
                          toggle('Bold', 'client', 'zonename.region.bold', false),
                          toggle('Capitalise', 'client', 'zonename.region.uppercase', false),
                          slider('Outline', 'client', 'zonename.region.outline', 1, 0, 3, 0.5, '%.1fx')),
                (function ()
                    local one = colour_pick('Colour', 'client', 'zonename.region.color', '#FFD98A')
                    local two = colour_pick('Colour 2', 'client', 'zonename.region.color2', '#FFFFFF')
                    local grad = toggle('Gradient', 'client', 'zonename.region.gradient', false)
                    grad.rebuild = true -- (the second colour locks and unlocks by it)
                    two.lock_if = function () return vana.config.get('client', 'zonename.region.gradient') ~= '1' end
                    local row = { kind = 'colours', label = 'Colour', parts = { one, grad, two }, source = one.key }
                    row.reset = function ()
                        vana.config.set('client', one.key, nil)
                        vana.config.set('client', two.key, nil)
                    end
                    return row
                end)(),
                (function (row) row.lock_if = function () return vana.config.get('client', 'zonename.region.gradient') ~= '1' end return row end)(
                    choice('Gradient direction', 'client', 'zonename.region.gradient.direction', 'horizontal',
                           { { 'horizontal', 'Left to right' }, { 'vertical', 'Top to bottom' } })),
                colour_pick('Outline colour', 'client', 'zonename.region.outline.color', '#000000'),
                free(slider('Font size', 'client', 'zonename.region.fontsize', 32, 20, 125, 5, '%d px'), 6, 128),
                pair('Position', 'Where the middle of the region name sits, as a share of the screen: X from the left edge, Y from the top.',
                     { slider('X', 'client', 'zonename.region.x', 50, 0, 100, 1, '%d%%'),
                       slider('Y', 'client', 'zonename.region.y', 31, 0, 100, 1, '%d%%') }),
                -- what the zone's name and the region's share
                subheader('Name Animation'),
                toggle('Show them at login', 'client', 'zonename.login', true),
                slider('Time on screen', 'client', 'zonename.seconds', 3, 0.5, 15, 0.5, '%.1f s'),
                (function (row) row.rebuild = true return row end)( -- (Slide's two drop-downs lock and unlock by it)
                    dropdown('Animation', 'client', 'zonename.animation', 'fade',
                             { { 'fade', 'Fade' }, { 'scale', 'Scale' }, { 'slide', 'Slide' }, { 'wave_io', 'Wave in-out' },
                               { 'wave', 'Wave - Continuous' }, { 'dissolve', 'Dissolve' } })),
                (function (row) row.lock_if = function () return not (vana.config.get('client', 'zonename.animation') or ''):match('^wave') end return row end)(
                    slider('Wave speed', 'client', 'zonename.wavespeed', 1, 0.1, 2, 0.05, '%.2fx')),
                (function (row) row.lock_if = function () return (vana.config.get('client', 'zonename.animation') or '') ~= 'dissolve' end return row end)(
                    colours_line('Burning edge', toggle('Burning edge', 'client', 'zonename.dissolve.burn', true),
                                 colour_pick('Edge colour', 'client', 'zonename.dissolve.color', '#FF7A1A'))),
                -- Slide From and Slide To on one line (row.also: vana.lua draw_control); its reset is both
                (function ()
                    local sides = { { 'left', 'Left' }, { 'right', 'Right' }, { 'top', 'Top' }, { 'bottom', 'Bottom' } }
                    local from = dropdown('Slide from', 'client', 'zonename.slide', 'left', sides)
                    local to = dropdown('Slide to', 'client', 'zonename.slide.to', 'opposite',
                                        { { 'opposite', 'Opposite' }, sides[1], sides[2], sides[3], sides[4] })
                    local row = dropdown('Slide', 'client', 'zonename.slide', 'left', sides)
                    row.prefix, to.prefix, row.also, row.parts = 'From', 'To', to, { from, to }
                    row.tip = 'Which side the name comes in from with Slide, and which side it leaves toward (Opposite: the other side).'
                    row.lock_if = function () return (vana.config.get('client', 'zonename.animation') or 'fade') ~= 'slide' end
                    return row
                end)(),
                -- two buttons on one line (row.also: vana.lua draw_action_row): Show, left of Preview
                { kind = 'action', label = 'Try it', text = 'Preview', icon = 'play', action = 'zonename_preview',
                  also = { text = 'Show', icon = 'eye', action = 'zonename_show' },
                  tip = "Show holds the zone's name and the region's (whichever are switched on) on screen, as they will look, to set their font, size, colours and height by; press it again to hide them.\nPreview plays them as they run when you zone in: they fade in, stay and fade away." },
                -- the loadscreen addon's settings ([loadscreen], read by loadscreen.lua each frame it shows); /loadscreen opens
                -- here. Its switch loads or unloads the addon and its boot.txt line; the rows under it lock while it's off.
                header('Loading Screen'),
                (function ()
                    local row = toggle('Show the loading screen', 'loadscreen', 'enabled', true)
                    row.get = function () return M.loadscreen_loaded() end
                    row.set = function (on) M.loadscreen_switch(on) end
                    row.rebuild = true
                    return row
                end)(),
                subheader('Options'),
                M.loadscreen_row(toggle('Show previous zone', 'loadscreen', 'from', true)),
                M.loadscreen_row(toggle('Progress bar', 'loadscreen', 'bar', true)),
                M.loadscreen_row(toggle('Show client/server information', 'loadscreen', 'status', true)),
                M.loadscreen_row(toggle('Show dat file loads', 'loadscreen', 'file', true)),
                (function (row)
                    row.tip = 'A line at the top of the screen counting the people still loading once the zone is in.'
                    return row
                end)(M.loadscreen_row(toggle('Show npc/player load counts', 'loadscreen', 'people', false))),
                subheader('Data Display'),
                M.loadscreen_row(slider('Widget scale', 'loadscreen', 'scale', 100, 50, 200, 5, '%d%%')),
                M.loadscreen_row(pair('Position', 'Where its middle sits, as a share of the screen: X from the left edge, Y from the top.',
                     { slider('X', 'loadscreen', 'x', 50, 0, 100, 1, '%d%%'),
                       slider('Y', 'loadscreen', 'y', 50, 0, 100, 1, '%d%%') })),
                M.loadscreen_row(colour_pick('Progress bar colour', 'loadscreen', 'bar.color', '#7AA2F7')),
                M.loadscreen_row(colour_pick('Panel background colour', 'loadscreen', 'panel.color', '#0A0D14')),
                (function (row)
                    row.tip = 'The box behind the zone name, bar and status. 0% for none.'
                    return row
                end)(M.loadscreen_row(slider('Panel opacity', 'loadscreen', 'panel.opacity', 70, 0, 100, 5, '%d%%'))),
                (function (row)
                    row.tip = 'Where from and where to on one line, as From → To\n(Lower Jeuno → Port Bastok).'
                    return row
                end)(M.loadscreen_row(toggle('Single line zone info', 'loadscreen', 'oneline', false))),
                subheader('Screenshots Display'),
                (function (row)
                    row.tip = 'One of your screenshots behind it, a new one at random for each load.'
                    row.rebuild = true
                    return row
                end)(M.loadscreen_row(toggle('Random screenshot', 'loadscreen', 'image', false))),
                M.loadscreen_image_row(slider('Edge shadow', 'loadscreen', 'image.edges', 80, 0, 100, 5, '%d%%')),
                M.loadscreen_image_row(slider('Fade in', 'loadscreen', 'image.fadein', 0.4, 0, 3, 0.05, '%.2f s')),
                (function (row)
                    row.tip = 'How long the screenshot takes to fade away once the zone is in.'
                    return row
                end)(M.loadscreen_image_row(slider('Fade out', 'loadscreen', 'image.fadeout', 0.8, 0, 3, 0.05, '%.2f s'))),
                M.loadscreen_row({ kind = 'action', label = 'Preview', text = 'Preview', icon = 'play', action = 'loadscreen_preview' }),
                -- sparkles rising from portals ([graphics] sparkles.places; by default the one place, sparkles.cpp's)
                header('Sparkles'),
                warning('Affects the Nyzul Isle portal in the Alzadaal Undersea Ruins.', 'grey'),
                needs_dx(toggle('Show sparkles', 'graphics', 'sparkles', true)),
                needs_dx(slider('Strength', 'graphics', 'sparkles.strength', 1, 0, 2, 0.05, '%.2fx')),
                needs_dx(free(slider('Size', 'graphics', 'sparkles.size', 1, 0.25, 3, 0.05, '%.2fx'), 0.25, 4)),
                needs_dx(free(slider('Amount', 'graphics', 'sparkles.density', 1, 0.1, 3, 0.05, '%.2fx'), 0.1, 4)),
                needs_dx(free(slider('Speed', 'graphics', 'sparkles.speed', 1, 0.1, 3, 0.05, '%.2fx'), 0.1, 4)),
                needs_dx(toggle('Colour from the ground', 'graphics', 'sparkles.match', true)),
            }
        end,
    },
    {
        -- every game window that can be moved, sized or hidden, and the chat's look, in one place (none of these are
        -- on Game Options too): /vana shows the Settings and Presets, each window's section (element) is the HUD Manager's
        -- (hud.lua, /hud)
        name = 'HUD',
        search = true,
        aliases = { 'hud', 'hudmanager', 'network', 'target', 'map', 'regionmap', 'conquest' },
        rows = function ()
            -- a Position row of X and Y with its switch and eye (no Width)
            local function place(label, key, tip, x, y, outline)
                local row = mover(label, key .. '.move', tip,
                                  { slider('X', 'client', key .. '.x', x, 0, 100, 0.5, '%.1f%%'),
                                    slider('Y', 'client', key .. '.y', y, 0, 100, 0.5, '%.1f%%') }, outline)
                row.slots = 2
                return row
            end
            -- which side of a window's box snaps to the grid as it's dragged ([vana] hud.anchor.<id>; vana.lua's hud.anchor)
            local function anchor(id, right)
                return choice('Anchor', 'vana', 'hud.anchor.' .. id, right and 'right' or 'left', { { 'left', 'Left' }, { 'right', 'Right' } })
            end
            local function full() return (tonumber(vana.config.get('client', 'map.fullscreen') or '') or 1) ~= 0 end
            local function region_full() return (tonumber(vana.config.get('client', 'regionmap.fullscreen') or '') or 1) ~= 0 end
            local function no_grid() return (tonumber(vana.config.get('vana', 'hud.grid') or '') or 1) == 0 end
            local function no_fill() return no_grid() or (tonumber(vana.config.get('vana', 'hud.grid.background') or '') or 1) == 0 end
            -- the HUD's own: its sub-tab first, with a bullet, in yellow (vana.lua's list of pages; nav_icon)
            local settings = header('Settings')
            settings.nav_icon, settings.nav_color = 'bullet', M.P.hud
            -- a game window's section: the HUD Manager's list of elements, not a sub-tab of /vana's (vana.lua's subtabs_of).
            -- `group`: the category it is listed under in the drop-down (hud.lua's GROUP_ORDER; none: Misc)
            local function element(text, group)
                local row = header(text)
                row.element = true
                row.group = group
                return row
            end
            -- a part of a window's sub-panel whose switch shows it ([client] `key`, on while unset), its rows greyed while off
            local function shown(text, key)
                return subheader(text, nil, {
                    get = function () return (tonumber(vana.config.get('client', key) or '') or 1) ~= 0 end,
                    set = function (on) vana.config.set('client', key, on and '1' or '0') end,
                }, 'client', key)
            end
            -- what the mouse does to the boxes in the HUD Manager, as keys (vana.lua's draw_row: controls)
            local controls = { kind = 'controls', label = 'Controls',
                               items = { { keys = { 'Click' }, text = 'Edit its settings' },
                                         { keys = { 'Shift', 'Drag' }, text = 'Move it (Ctrl+Z: undo)' },
                                         { keys = { 'Shift', 'Right click' }, text = 'Select several' } } }
            return {
                settings,
                controls,
                subheader('Grid'),
                (function (row) row.rebuild = true return row end)( -- the grid's rows lock and unlock by it
                    toggle('Draw HUD grid', 'vana', 'hud.grid', true)),
                (function (row) row.lock_if = no_grid return row end)(
                    free(slider('Grid size', 'vana', 'hud.grid.size', 50, 10, 200, 5, '%d px'), 4, 2000)),
                (function (row) row.lock_if = no_grid return row end)(
                    colour_pick('Grid colour', 'vana', 'hud.grid.color', '#A3A3A3')),
                (function (row) row.lock_if = no_grid return row end)(
                    slider('Grid opacity', 'vana', 'hud.grid.opacity', 16, 5, 100, 1, '%d%%')),
                (function (row) row.lock_if = no_grid return row end)(
                    slider('Centre line opacity', 'vana', 'hud.grid.center.opacity', 60, 5, 100, 1, '%d%%')),
                toggle('Snap to grid', 'vana', 'hud.snap', true),
                subheader('Grid background'),
                (function (row) row.lock_if = no_grid row.rebuild = true return row end)( -- the two below lock and unlock by it
                    toggle('Grid background', 'vana', 'hud.grid.background', true)),
                (function (row) row.lock_if = no_fill return row end)(
                    colour_pick('Background colour', 'vana', 'hud.grid.background.color', '#3D5781')),
                (function (row) row.lock_if = no_fill return row end)(
                    slider('Background opacity', 'vana', 'hud.grid.background.opacity', 15, 0, 100, 1, '%d%%')),
                subheader('Widgets'),
                toggle('Show element names', 'vana', 'hud.names', true),
                subheader('Presets'),
                toggle('Auto save', 'vana', 'hud.autosave', true),
                -- each window: its hide first, then a Position (or Layout) sub-panel, then anything else in its own
                element('Action Menu', 'Action Menu'),
                toggle('Hide', 'client', 'action.hide', false),
                toggle('Show the frame', 'client', 'action.background', true),
                toggle('Solid buttons', 'client', 'action.solid', false),
                subheader('Position'),
                placed('Position', 'chat.action', 'action.move', 'Game, on the chat at its own left; Chat, at the chat\'s left; or X (its left edge) and Y (its top edge).',
                       { slider('X', 'client', 'action.x', 1.7, 0, 100, 0.5, '%.1f%%'),
                         slider('Y', 'client', 'action.y', 60, 0, 100, 0.5, '%.1f%%') }, 'action'),
                anchor('action'),
                element('Action Menu: Magic Sub-Menu', 'Action Menu'), -- (the list Magic opens to its right: Black Magic, Songs, Ninjutsu...)
                toggle('Hide', 'client', 'actionmagic.hide', false),
                toggle('Show the frame', 'client', 'actionmagic.background', true),
                subheader('Position'),
                place('Position', 'actionmagic', 'X: its left edge. Y: its top edge. The list the Action Menu\'s Magic opens (Black Magic, Songs, Ninjutsu...). Off, it is the Interaction Menu\'s.', 0.8, 70.5, 'actionmagic'),
                anchor('actionmagic'),
                element('Action Menu: Abilities Sub-Menu', 'Action Menu'), -- (the list Abilities opens: Job Abil., Weapon Skl., Mount...)
                toggle('Hide', 'client', 'actionability.hide', false),
                toggle('Show the frame', 'client', 'actionability.background', true),
                subheader('Position'),
                place('Position', 'actionability', 'X: its left edge. Y: its top edge. The list the Action Menu\'s Abilities opens (Job Abil., Weapon Skl., Mount...). Off, it is the Interaction Menu\'s.', 0.8, 70.5, 'actionability'),
                anchor('actionability'),
                element('Action Menu: Magic/Abilities', 'Action Menu'), -- (the spell and ability lists, at the bottom left over the chat)
                toggle('Hide', 'client', 'actionlist.hide', false),
                toggle('Show the frame', 'client', 'actionlist.background', true),
                subheader('Position'),
                place('Position', 'actionlist', 'X: its left edge. Y: its bottom edge. The lists the Action Menu\'s Magic and Abilities open (the spells, the abilities). Off, they are the Interaction Menu\'s.', 0.8, 95.8, 'actionlist'),
                anchor('actionlist'),
                element('Action Menu: Mounts', 'Action Menu'), -- (the list Mounts opens, at the bottom left over the chat)
                toggle('Hide', 'client', 'actionmount.hide', false),
                toggle('Show the frame', 'client', 'actionmount.background', true),
                subheader('Position'),
                place('Position', 'actionmount', 'X: its left edge. Y: its bottom edge. The list the Action Menu\'s Mounts opens (Raptor, Tiger, Coeurl...). Off, it is the Interaction Menu\'s.', 0.8, 95.8, 'actionmount'),
                anchor('actionmount'),
                element('Alliance List 1', 'Party'),
                hides(toggle('Hide', 'client', 'alliance1.hide', false), 'alliance_lists'),
                toggle('Show the frame', 'client', 'alliance1.background', true),
                subheader('Position'),
                place('Position', 'alliance1', 'X: its right edge. Y: its bottom edge.', 99.4, 80, 'alliance1'),
                anchor('alliance1', true),
                element('Alliance List 2', 'Party'),
                hides(toggle('Hide', 'client', 'alliance2.hide', false), 'alliance_lists'),
                toggle('Show the frame', 'client', 'alliance2.background', true),
                subheader('Position'),
                place('Position', 'alliance2', 'X: its right edge. Y: its bottom edge.', 99.4, 60, 'alliance2'),
                anchor('alliance2', true),
                element('Auction Menu', 'Windows'),
                toggle('Hide', 'client', 'auction.hide', false),
                toggle('Show the frame', 'client', 'auction.background', true),
                subheader('Position'),
                place('Position', 'auction', 'X: its right edge. Y: its top edge.', 99.2, 4, 'auction'),
                anchor('auction', true),
                element('Battle Timer', 'Combat'), -- (the time left in a battlefield or an instance: an hourglass and 29:27)
                toggle('Hide', 'client', 'battletimer.hide', false),
                toggle('Show the frame', 'client', 'battletimer.background', true),
                subheader('Position'),
                place('Position', 'battletimer', 'X: its left edge. Y: its top edge. The time left in a battlefield or an instance. Off, it goes with Menu Content.', 0.6, 6, 'battletimer'),
                anchor('battletimer'),
                element('Besieged: Orders', 'Maps'), -- (the Besieged map's Orders and Battle Status, at the left)
                toggle('Hide', 'client', 'besorders.hide', false),
                toggle('Show the frame', 'client', 'besorders.background', true),
                subheader('Position'),
                place('Position', 'besorders', 'X: its left edge. Y: its top edge. Off, it goes with Menu Content.', 0.6, 3, 'besorders'),
                anchor('besorders'),
                element('Besieged: Standing', 'Maps'), -- (the Besieged map's Imperial Standing, at the bottom right)
                toggle('Hide', 'client', 'besstanding.hide', false),
                toggle('Show the frame', 'client', 'besstanding.background', true),
                subheader('Position'),
                place('Position', 'besstanding', 'X: its right edge. Y: its bottom edge.', 99.4, 96.5, 'besstanding'),
                anchor('besstanding', true),
                element('Cast Bar', 'Combat'),
                toggle('Hide', 'client', 'castbar.hide', false),
                toggle('Show the frame', 'client', 'castbar.background', true),
                subheader('Position'),
                place('Position', 'castbar', 'X: its middle. Y: its top edge.', 4.2, 3.9, 'castbar'),
                anchor('castbar'),
                element('Change Jobs', 'Windows'), -- (the job list the Mog House's moogle opens)
                toggle('Hide', 'client', 'jobchange.hide', false),
                toggle('Show the frame', 'client', 'jobchange.background', true),
                subheader('Position'),
                place('Position', 'jobchange', 'X: its left edge. Y: its top edge. The job list the Mog House\'s moogle opens. Off, it is the Interaction Menu\'s.', 0.8, 54, 'jobchange'),
                anchor('jobchange'),
                element('Chat: Input Line', 'Chat'),
                hides(toggle('Hide', 'client', 'input.hide', false), 'chat_input'),
                toggle('Hide the mode tab', 'client', 'input.hidetab', false),
                subheader('Position'),
                mover('Position', 'input.move', 'X: its left edge. Y: its bottom edge.',
                      { slider('X', 'client', 'input.x', 0.5, 0, 100, 0.5, '%.1f%%'),
                        slider('Y', 'client', 'input.y', 99, 0, 100, 0.5, '%.1f%%') }, 'input'),
                anchor('input'),
                element('Chat: Maximized', 'Chat'), -- (the chat opened to the full height)
                toggle('Hide', 'client', 'fulllog.hide', false),
                subheader('Position'),
                place('Position', 'fulllog', 'X: its left edge. Y: its top edge.', 0.8, 4, 'fulllog'),
                slider('Height', 'client', 'fulllog.height', 80, 10, 100, 0.5, '%.1f%%'),
                anchor('fulllog'),
                element('Chat: Window 1 (Main)', 'Chat'),
                hides(toggle('Hide', 'client', 'chat.hide', false), 'chat_log'),
                subheader('Position'),
                mover('Position', 'chat.move', 'X: its left edge. Y: its bottom edge.',
                      { slider('X', 'client', 'chat.x', 0.5, 0, 100, 0.5, '%.1f%%'),
                        slider('Y', 'client', 'chat.y', 99, 0, 100, 0.5, '%.1f%%'),
                        slider('Width', 'client', 'chat.width', 95, 5, 100, 0.5, '%.1f%%') }, 'chat'),
                anchor('chat'),
                element('Chat: Window 2 (Secondary)', 'Chat'),
                hides(toggle('Hide', 'client', 'chat2.hide', false), 'chat_log'),
                subheader('Position'),
                mover('Position', 'chat2.move', 'Off, it sits on the main chat.',
                      { slider('X', 'client', 'chat2.x', 0.5, 0, 100, 0.5, '%.1f%%'),
                        slider('Y', 'client', 'chat2.y', 85, 0, 100, 0.5, '%.1f%%'),
                        slider('Width', 'client', 'chat2.width', 45, 5, 100, 0.5, '%.1f%%') }, 'chat2'),
                anchor('chat2'),
                element('Check Window', 'Windows'), -- (/check: another's gear)
                toggle('Hide', 'client', 'checkwin.hide', false),
                toggle('Show the frame', 'client', 'checkwin.background', true),
                subheader('Position'),
                place('Position', 'checkwin', 'X: its left edge. Y: its top edge. Off, it goes with Menu Content.', 0.6, 3, 'checkwin'),
                anchor('checkwin'),
                element('Check Window Stats', 'Windows'), -- (equipstats_check.h: what a /checked player's gear adds, under the Check window or in a box of their own)
                (function (row) -- equip.stats.check (Stats under /check, on by default) the other way round, as every other window's Hide
                    row.get = function () return (tonumber(vana.config.get('client', 'equip.stats.check') or '') or 1) == 0 end
                    row.set = function (hide) vana.config.set('client', 'equip.stats.check', hide and '0' or nil) end
                    return row
                end)(toggle('Hide', 'client', 'equip.stats.check', false)),
                subheader('Position'),
                place('Position', 'equip.stats.check', 'X: its left edge. Y: its top edge. Off, they sit under the Check window. Shown while Equipment Stats is on.', 11, 3, 'equip.stats.check'),
                anchor('equip.stats.check'),
                element('Colonization: Bayld', 'Maps'), -- (the Colonization map's Bayld, at the bottom right)
                toggle('Hide', 'client', 'colobayld.hide', false),
                toggle('Show the frame', 'client', 'colobayld.background', true),
                subheader('Position'),
                place('Position', 'colobayld', 'X: its right edge. Y: its bottom edge.', 99.4, 96.5, 'colobayld'),
                anchor('colobayld', true),
                element('Colonization: Rank', 'Maps'), -- (the Colonization map's Coalition Rank, at the left)
                toggle('Hide', 'client', 'colorank.hide', false),
                toggle('Show the frame', 'client', 'colorank.background', true),
                subheader('Position'),
                place('Position', 'colorank', 'X: its left edge. Y: its top edge. Off, it goes with Menu Content.', 0.6, 3, 'colorank'),
                anchor('colorank'),
                element('Comment Window', 'Windows'), -- (the box at the bottom left: a checked player's bazaar message, a search comment)
                toggle('Hide', 'client', 'commentwin.hide', false),
                toggle('Show the frame', 'client', 'commentwin.background', true),
                toggle('Keep open over items', 'client', 'commentwin.keep', false),
                subheader('Position'),
                place('Position', 'commentwin', 'X: its left edge. Y: its bottom edge. A checked player\'s bazaar message under the Check window, and a search result\'s comment. Off, the game puts it at the bottom left.', 0.8, 87.3, 'commentwin'),
                anchor('commentwin'),
                element('Compass'),
                toggle('Hide', 'client', 'compass.hide', false),
                subheader('Position'),
                placed('Position', 'chat.compass', 'compass.move', 'Game, on the top chat, or X (its middle) and Y (its bottom edge).',
                       { slider('X', 'client', 'compass.x', 3, 0, 100, 0.5, '%.1f%%'),
                         slider('Y', 'client', 'compass.y', 90, 0, 100, 0.5, '%.1f%%') }, 'compass'),
                anchor('compass'),
                subheader('Clock'),
                toggle('Hide', 'client', 'clock.hide', false),
                toggle('Keep over menus', 'client', 'clock.always', false),
                (function ()
                    local row = mover('Position', 'clock.move', 'X: its left edge, middle or right edge (Alignment). Y: its top edge.',
                                      { slider('X', 'client', 'clock.x', 3, 0, 100, 0.5, '%.1f%%'),
                                        slider('Y', 'client', 'clock.y', 92, 0, 100, 0.5, '%.1f%%') }, 'clock')
                    row.slots = 2
                    return row
                end)(),
                choice('Alignment', 'client', 'clock.align', 'left', { { 'left', 'Left' }, { 'centre', 'Centre' }, { 'right', 'Right' } }),
                anchor('clock'),
                element('Conquest: Balance', 'Maps'), -- (the conquest map's Balance of Power, at the top right)
                toggle('Hide', 'client', 'conqbalance.hide', false),
                toggle('Show the frame', 'client', 'conqbalance.background', true),
                subheader('Position'),
                place('Position', 'conqbalance', 'X: its right edge. Y: its top edge.', 99.4, 3, 'conqbalance'),
                anchor('conqbalance', true),
                element('Conquest: Points', 'Maps'), -- (the conquest map's Conquest Points, at the bottom right)
                toggle('Hide', 'client', 'conqpoints.hide', false),
                toggle('Show the frame', 'client', 'conqpoints.background', true),
                subheader('Position'),
                place('Position', 'conqpoints', 'X: its right edge. Y: its bottom edge.', 99.4, 96.5, 'conqpoints'),
                anchor('conqpoints', true),
                element('Conquest: Regional Control', 'Maps'), -- (the box a zone under the conquest map's cursor shows)
                toggle('Hide', 'client', 'conqcontrol.hide', false),
                toggle('Show the frame', 'client', 'conqcontrol.background', true),
                subheader('Position'),
                place('Position', 'conqcontrol', 'X: its left edge. Y: its top edge. Off, it goes with Menu Content.', 0.6, 3, 'conqcontrol'),
                anchor('conqcontrol'),
                element('Equipment', 'Windows'), -- (the Status and gil windows the Equipment window opens beside it: hide.windows)
                hide_window('Hide the Status window', 'persona@equip'),
                hide_window('Hide gil', 'money@equip'),
                element('Equipment Stats', 'Windows'), -- (equipstats.cpp: the stats under the Equipment window, or in a box of their own)
                (function (row) -- equip.stats ('Stats under Equipment', off by default) the other way round, as every other window's Hide
                    row.get = function () return (tonumber(vana.config.get('client', 'equip.stats') or '') or 0) == 0 end
                    row.set = function (hide) vana.config.set('client', 'equip.stats', (not hide) and '1' or nil) end
                    return row
                end)(toggle('Hide', 'client', 'equip.stats', true)),
                subheader('Position'),
                place('Position', 'equip.stats', 'X: its left edge. Y: its top edge. Off, they sit under the Equipment window.', 26, 11, 'equip.stats'),
                anchor('equip.stats'),
                element('EXP Window', 'Windows'), -- (the level and EXP box under the Status window)
                toggle('Hide', 'client', 'expwin.hide', false),
                toggle('Show the frame', 'client', 'expwin.background', true),
                subheader('Position'),
                place('Position', 'expwin', 'X: its left edge. Y: its top edge. Off, it sits under the Status window and goes with it.', 0.6, 13, 'expwin'),
                anchor('expwin'),
                element('Gil', 'Windows'), -- (the gil window: on the Status and Equipment screens, under a shop's list)
                hide_window('Hide', 'money'),
                toggle('Hide in Status and Equipment', 'client', 'menucontent.hidegil', false),
                hide_window('Hide beside Equipment', 'money@equip'),
                element('Help Bar', 'Help Bar'), -- (the long bar across the top while a menu is up; its title box is the next element)
                toggle('Hide', 'client', 'helpbar.hide', false),
                toggle('Show the frame', 'client', 'helpbar.background', true),
                toggle('Show the description', 'client', 'helpbar.text', true),
                toggle('Show the "Help" caption', 'client', 'helpbar.label', true),
                subheader('Position'),
                place('Position', 'helpbar', 'X: its left edge. Y: its top edge.', 4.5, 1, 'helpbar'),
                slider('Width', 'client', 'helpbar.width', 91, 10, 100, 0.5, '%.1f%%'),
                element('Help Bar: Title Box', 'Help Bar'), -- (the small box at the help bar's left with the page's name)
                (function (row) -- helpbar.title=0 hid it before helptitle.hide: read as hidden, and cleared once Hide is set
                    row.get = function ()
                        return (tonumber(vana.config.get('client', 'helptitle.hide') or '') or 0) ~= 0
                            or (tonumber(vana.config.get('client', 'helpbar.title') or '') or 1) == 0
                    end
                    row.set = function (hide)
                        vana.config.set('client', 'helptitle.hide', hide and '1' or nil)
                        vana.config.set('client', 'helpbar.title', nil)
                    end
                    return row
                end)(toggle('Hide', 'client', 'helptitle.hide', false)),
                toggle('Show the frame', 'client', 'helptitle.background', true),
                subheader('Position'),
                place('Position', 'helptitle', 'X: its left edge. Y: its top edge.', 0.6, 1, 'helptitle'),
                element('HP Gauge', 'Combat'), -- (your HP, MP and TP the Commands menu shows while you fight)
                toggle('Hide', 'client', 'hpgauge.hide', false),
                toggle('Show the frame', 'client', 'hpgauge.background', true),
                subheader('Position'),
                place('Position', 'hpgauge', 'X: its left edge. Y: its top edge. Your HP, MP and TP that the Commands menu shows while you fight. Off, it goes with Menu Content.', 0.6, 3, 'hpgauge'),
                anchor('hpgauge'),
                element('Interaction Menu', 'Windows'), -- (the menus over the chat: an NPC's, a target's, the Action menu's lists)
                toggle('Hide', 'client', 'menus.hide', false),
                toggle('Show the frame', 'client', 'menus.background', true),
                subheader('Position'),
                placed('Position', 'chat.menus', 'menus.move', 'Game, at the chat\'s left, or X (its left edge) and Y (its bottom edge).',
                       { slider('X', 'client', 'menus.x', 0.5, 0, 100, 0.5, '%.1f%%'),
                         slider('Y', 'client', 'menus.y', 90, 0, 100, 0.5, '%.1f%%') }, 'menus'),
                anchor('menus'),
                element('Inventory', 'Windows'),
                toggle('Hide', 'client', 'inventory.hide', false),
                toggle('Show the frame', 'client', 'inventory.background', true),
                subheader('Position'),
                place('Position', 'inventory', 'X: its left edge. Y: its top edge. The Items list from the main menu, its item descriptions with it.', 0.6, 3, 'inventory'),
                anchor('inventory'),
                element('Item Details', 'Windows'), -- (an item's description: under a list, the Check window's, a shop's...)
                toggle('Hide', 'client', 'itemdetails.hide', false),
                toggle('Show the frame', 'client', 'itemdetails.background', true),
                subheader('Position'),
                place('Position', 'itemdetails', 'X: its left edge. Y: its top edge. Every item\'s description, wherever it comes up. Off, it goes with its list.', 0.6, 20, 'itemdetails'),
                anchor('itemdetails'),
                element('Macro Bar', 'Combat'),
                hides(toggle('Hide', 'client', 'macrobar.hide', false), 'macro_bar'),
                toggle('Show the frame', 'client', 'macrobar.background', true),
                subheader('Position'),
                place('Position', 'macrobar', 'X: its left edge. Y: its top edge.', 0.8, 4, 'macrobar'),
                anchor('macrobar'),
                element('Macro Bar: Edit Options', 'Combat'),
                toggle('Hide', 'client', 'macroeditoptions.hide', false),
                toggle('Show the frame', 'client', 'macroeditoptions.background', true),
                toggle('Solid buttons', 'client', 'macroeditoptions.solid', false),
                subheader('Position'),
                place('Position', 'macroeditoptions', 'X: its right edge. Y: its top edge. The box at the right while macros are edited (Button Edit: Copy, Paste, Delete; a macro\'s own). Off, where the game puts it.', 99.2, 11.5, 'macroeditoptions'),
                anchor('macroeditoptions', true),
                element('Macro Bar: Editing', 'Combat'),
                toggle('Hide', 'client', 'macroedit.hide', false),
                toggle('Show the frame', 'client', 'macroedit.background', true),
                subheader('Position'),
                place('Position', 'macroedit', 'X: its left edge. Y: its top edge. The macro bar while Edit Macro edits it (Ctrl and Alt) and the editor a macro opens in its place. Off, it goes with Menu Content.', 0.8, 4, 'macroedit'),
                anchor('macroedit'),
                element('Main Menu', 'Main Menu'),
                toggle('Hide', 'client', 'mainmenu.hide', false),
                toggle('Show the frame', 'client', 'mainmenu.background', true),
                toggle('Solid buttons', 'client', 'mainmenu.solid', false),
                toggle('Single main menu', 'client', 'singlemenu', false),
                subheader('Position'),
                place('Position', 'mainmenu', 'X: its right edge. Y: its top edge. The menus it opens go there too.', 99.4, 3, 'mainmenu'),
                anchor('mainmenu', true),
                element('Main Menu: Status Sub-Menu', 'Main Menu'),
                toggle('Hide', 'client', 'statusmenu.hide', false),
                toggle('Show the frame', 'client', 'statusmenu.background', true),
                toggle('Solid buttons', 'client', 'statusmenu.solid', false),
                subheader('Position'),
                place('Position', 'statusmenu', 'X: its right edge. Y: its top edge. The list Status opens (Combat Skl., Magic Skl., Currencies...), Unity\'s and Merit Points\'. Off, it goes with the main menu.', 99.4, 3, 'statusmenu'),
                anchor('statusmenu', true),
                element('Main Menu: Mission', 'Main Menu'),
                toggle('Hide', 'client', 'missions.hide', false),
                subheader('Position'),
                place('Position', 'missions', 'X: its right edge. Y: its top edge. The list Missions opens (Nations, Zilart...). Off, it goes with the main menu.', 99.4, 3, 'missions'),
                anchor('missions', true),
                element('Main Menu: Magic', 'Main Menu'),
                toggle('Hide', 'client', 'magicmenu.hide', false),
                subheader('Position'),
                place('Position', 'magicmenu', 'X: its right edge. Y: its top edge. The list Magic opens (White Magic, Black Magic...). Off, it goes with the main menu.', 99.4, 3, 'magicmenu'),
                anchor('magicmenu', true),
                element('Main Menu: Abilities', 'Main Menu'),
                toggle('Hide', 'client', 'abilitymenu.hide', false),
                subheader('Position'),
                place('Position', 'abilitymenu', 'X: its right edge. Y: its top edge. The list Abilities opens (Job Traits, Job Abil....). Off, it goes with the main menu.', 99.4, 3, 'abilitymenu'),
                anchor('abilitymenu', true),
                element('Main Menu: Synthesis', 'Main Menu'),
                toggle('Hide', 'client', 'synthmenu.hide', false),
                subheader('Position'),
                place('Position', 'synthmenu', 'X: its right edge. Y: its top edge. The list Synthesis opens (Crystal Synthesis, History). Off, it goes with the main menu.', 99.4, 3, 'synthmenu'),
                anchor('synthmenu', true),
                element('Main Menu: Party', 'Main Menu'),
                toggle('Hide', 'client', 'partymenu.hide', false),
                subheader('Position'),
                place('Position', 'partymenu', 'X: its right edge. Y: its top edge. The list Party opens (Seek Party, Find Member...). Off, it goes with the main menu.', 99.4, 3, 'partymenu'),
                anchor('partymenu', true),
                element('Main Menu: Options', 'Main Menu'),
                toggle('Hide', 'client', 'sortmenu.hide', false),
                subheader('Position'),
                place('Position', 'sortmenu', 'X: its right edge. Y: its top edge. The sort options beside the Magic, Abilities and Items lists. Off, it goes with the main menu.', 99.4, 3, 'sortmenu'),
                anchor('sortmenu', true),
                element('Main Menu: Options Commands', 'Main Menu'),
                toggle('Hide', 'client', 'sortconfirm.hide', false),
                toggle('Show the frame', 'client', 'sortconfirm.background', true),
                toggle('Solid buttons', 'client', 'sortconfirm.solid', false),
                subheader('Position'),
                place('Position', 'sortconfirm', 'X: its right edge. Y: its top edge. The Yes/No the Options box\'s Auto asks before it sorts. Off, it stays where the game puts it.', 99.2, 10.3, 'sortconfirm'),
                anchor('sortconfirm', true),
                element('Main Menu: Search', 'Main Menu'),
                toggle('Hide', 'client', 'searchmenu.hide', false),
                subheader('Position'),
                place('Position', 'searchmenu', 'X: its right edge. Y: its top edge. The list Search opens (Area, Name, Job...) and the lists it opens. Off, it goes with the main menu.', 99.4, 3, 'searchmenu'),
                anchor('searchmenu', true),
                element('Main Menu: Region Info', 'Main Menu'),
                toggle('Hide', 'client', 'regionmenu.hide', false),
                subheader('Position'),
                place('Position', 'regionmenu', 'X: its right edge. Y: its top edge. The list Region Info opens (Conquest, Besieged, Campaign, Colonization). Off, it goes with the main menu.', 99.4, 3, 'regionmenu'),
                anchor('regionmenu', true),
                element('Main Menu: View House', 'Main Menu'),
                toggle('Hide', 'client', 'housemenu.hide', false),
                subheader('Position'),
                place('Position', 'housemenu', 'X: its right edge. Y: its top edge. The list View House opens (Mog Safe ... Mog Wardrobe 8). Off, it goes with the main menu.', 99.4, 3, 'housemenu'),
                anchor('housemenu', true),
                element('Main Menu: Bazaar', 'Main Menu'),
                toggle('Hide', 'client', 'bazaarmenu.hide', false),
                subheader('Position'),
                place('Position', 'bazaarmenu', 'X: its right edge. Y: its top edge. The list Bazaar opens (Set Prices, Edit Comment). Off, it goes with the main menu.', 99.4, 3, 'bazaarmenu'),
                anchor('bazaarmenu', true),
                element('Main Menu: Macro', 'Main Menu'),
                toggle('Hide', 'client', 'macromenu.hide', false),
                subheader('Position'),
                place('Position', 'macromenu', 'X: its right edge. Y: its top edge. The list Macros opens (Edit Macro, Edit Equip Set) and its books\' and equip sets\' lists. Off, it goes with the main menu.', 99.4, 3, 'macromenu'),
                anchor('macromenu', true),
                element('Main Menu: Macro Options', 'Main Menu'),
                toggle('Hide', 'client', 'macrooptions.hide', false),
                toggle('Show the frame', 'client', 'macrooptions.background', true),
                toggle('Solid buttons', 'client', 'macrooptions.solid', false),
                toggle('Equip first', 'client', 'macrooptions.equipfirst', false),
                subheader('Position'),
                place('Position', 'macrooptions', 'X: its right edge. Y: its top edge. The box an equip set or a macro book opens beside its list (Name, Equipment...). Off, it opens at the chosen row, on the list\'s other side when there\'s no room.', 94.2, 4.6, 'macrooptions'),
                anchor('macrooptions', true),
                element('Main Menu: Config', 'Main Menu'),
                toggle('Hide', 'client', 'configmenu.hide', false),
                toggle('Enable extended game settings', 'client', 'gamesettings.extended', false),
                subheader('Position'),
                place('Position', 'configmenu', 'X: its right edge. Y: its top edge. The list Config opens (Gameplay, Chat Filters...). Off, it goes with the main menu.', 99.4, 3, 'configmenu'),
                anchor('configmenu', true),
                element('Map', 'Maps'),
                subheader('Layout'),
                (function (row) row.rebuild = true return row end)( -- the rows below lock and unlock by it
                    toggle('Full screen', 'client', 'map.fullscreen', true)),
                (function ()
                    local row = pair('Position', 'Its left and top edges. Shift + drag the map (or its box) to move it.',
                                     { slider('X', 'client', 'map.x', 30, 0, 100, 0.5, '%.1f%%'),
                                       slider('Y', 'client', 'map.y', 12, 0, 100, 0.5, '%.1f%%') })
                    row.outline, row.title_w = 'map', 106 -- (its eye: the box, while it isn't full screen)
                    row.place = true -- (a window's place, as a mover's)
                    row.lock_if = full
                    for _, sub in ipairs(row.sliders) do
                        sub.lock_if = full
                    end
                    return row
                end)(),
                (function (row) row.lock_if = full return row end)(
                    slider('Scale', 'client', 'map.scale', 70, 20, 100, 1, '%d%%')),
                (function (row) row.lock_if = full return row end)(anchor('map')),
                element('Map: Controls', 'Maps'), -- (the map's Markers, Wide Scan and Change Map, at the top right)
                toggle('Hide', 'client', 'mapcontrols.hide', false),
                subheader('Position'),
                place('Position', 'mapcontrols', 'X: its right edge. Y: its top edge. Off, it goes with the main menu.', 99.4, 3, 'mapcontrols'),
                anchor('mapcontrols', true),
                element('Menu Content', 'Windows'),
                toggle('Show the frames', 'client', 'menucontent.background', true),
                subheader('Position'),
                place('Position', 'menucontent', 'X: the left edge of the first menu (Status, Items, Quests). Y: its top edge. The others keep their places from it.', 0, 0, 'menucontent'),
                anchor('menucontent'),
                element('Network Window'),
                (function (row) -- network.panel the other way round, as every other window's Hide
                    row.get = function () return (tonumber(vana.config.get('client', 'network.panel') or '') or 1) == 0 end
                    row.set = function (hide) vana.config.set('client', 'network.panel', hide and '0' or nil) end
                    return hides(row, 'network_window')
                end)(toggle('Hide', 'client', 'network.panel', false)),
                toggle('Only while a menu is up', 'client', 'network.hide', false),
                toggle('Show the frame', 'client', 'network.background', true),
                subheader('Position'),
                place('Position', 'netwin', 'X: its right edge. Y: its top edge.', 99.4, 1, 'netwin'),
                pair('Contents', 'Moves what is drawn in it. X: left or right. Y: up or down. Dot: the size of the Dot icon.',
                     { slider('X', 'client', 'network.x', 0, -40, 40, 1, '%+d'),
                       slider('Y', 'client', 'network.y', 0, -10, 10, 1, '%+d'),
                       slider('Dot', 'client', 'network.dot.size', 10, 4, 24, 1, '%d') }),
                element('Party List', 'Party'),
                hides(toggle('Hide', 'client', 'party.hide', false), 'party_list'),
                toggle('Show the frame', 'client', 'party.background', true),
                subheader('Position'),
                place('Position', 'party', 'X: its right edge. Y: its bottom edge.', 99.4, 99, 'party'),
                anchor('party', true),
                element('Region Map', 'Maps'),
                subheader('Layout'),
                (function (row) row.rebuild = true return row end)( -- the rows below lock and unlock by it
                    toggle('Full screen', 'client', 'regionmap.fullscreen', true)),
                (function ()
                    local row = pair('Position', 'Its left and top edges. Shift + drag the map (or its box) to move it.',
                                     { slider('X', 'client', 'regionmap.x', 30, 0, 100, 0.5, '%.1f%%'),
                                       slider('Y', 'client', 'regionmap.y', 12, 0, 100, 0.5, '%.1f%%') })
                    row.outline, row.title_w = 'regionmap', 106 -- (its eye: the box, while it isn't full screen)
                    row.place = true -- (a window's place, as a mover's)
                    row.lock_if = region_full
                    for _, sub in ipairs(row.sliders) do
                        sub.lock_if = region_full
                    end
                    return row
                end)(),
                (function (row) row.lock_if = region_full return row end)(
                    slider('Scale', 'client', 'regionmap.scale', 70, 20, 100, 1, '%d%%')),
                (function (row) row.lock_if = region_full return row end)(anchor('regionmap')),
                element('Shop Confirm', 'Windows'), -- (a shop's Yes/No before it sells or buys)
                toggle('Show the frame', 'client', 'shopconfirm.background', true),
                subheader('Position'),
                place('Position', 'shopconfirm', 'X: its left edge. Y: its top edge. A shop\'s Yes/No before it sells or buys. Off, it stays where the game puts it.', 0.6, 40, 'shopconfirm'),
                anchor('shopconfirm'),
                element('Spell Info', 'Action Menu'), -- (the box beside the Magic and Abilities lists: a spell's MP, the recast)
                toggle('Hide', 'client', 'spellinfo.hide', false),
                toggle('Show the frame', 'client', 'spellinfo.background', true),
                subheader('Position'),
                place('Position', 'spellinfo', 'X: its right edge. Y: its top edge. The box beside the Magic and Abilities lists with the MP and recast.', 99.2, 1.3, 'spellinfo'),
                anchor('spellinfo', true),
                element('Status Icons', 'Combat'),
                hides(toggle('Hide', 'client', 'buffs.hide', false), 'status_icons'),
                subheader('Position'),
                place('Position', 'buffs', 'X: its left edge. Y: the bottom of the first row.', 7.4, 6, 'buffs'),
                anchor('buffs'),
                subheader('Size'),
                slider('Scale', 'client', 'buffs.scale', 100, 25, 300, 5, '%d%%'),
                slider('Icons per row', 'client', 'buffs.columns', 9, 1, 36, 1, '%d'),
                -- the timers' letters and True Font (the menus' until set, as chatfont.cpp reads it: kBuffs)
                subheader('Timers'),
                area_font('buffs.font', 13, true),
                element('Status Screen Panels', 'Windows'), -- (the panels the Status sub-menu opens beside the Status window: hide.windows,
                                                            -- their layouts' names from the game's menu table, which panel each is not checked in game)
                hide_window('Hide Profile', 'profile'),
                hide_window('Hide Job Levels', 'joblevel'),
                hide_window('Hide Combat Skills', 'btlskill'),
                hide_window('Hide Magic Skills', 'mgcskill'),
                hide_window('Hide Crafting Skills', 'trdskill'),
                hide_window('Hide Master Levels', 'masterle'),
                hide_window('Hide Merit Points', 'levmerit'),
                hide_window('Hide Unity Points', 'ut_point'),
                element('Status Window', 'Windows'), -- (name, jobs, HP, stats: on the Status screen and beside the Equipment window)
                hide_window('Hide', 'persona'),
                hide_window('Hide beside Equipment', 'persona@equip'),
                element('Storage List', 'Windows'), -- (the bags Equipment and Items list beside them: Inventory, Mog Wardrobe...)
                toggle('Hide', 'client', 'storagemenu.hide', false),
                toggle('Show the frame', 'client', 'storagemenu.background', true),
                subheader('Position'),
                place('Position', 'storagemenu', 'X: its right edge. Y: its top edge. The list of bags Equipment and Items open beside them (Inventory, Mog Wardrobe...).', 99.4, 3, 'storagemenu'),
                anchor('storagemenu', true),
                element('Target Window', 'Combat'),
                hides(toggle('Hide', 'client', 'target.hide', false), 'target_window'),
                toggle('Show the frame', 'client', 'target.background', true),
                subheader('Layout'),
                place('Position', 'target', 'X: its middle. Y: its top edge.', 50, 4, 'target'),
                anchor('target'),
                slider('Width', 'client', 'target.width', 112, 80, 600, 4, '%d'),
                pair('Padding', 'Room added round what\'s in the window: Across at its left and right, Down at its top and bottom.',
                     { slider('Across', 'client', 'target.padding.x', 0, 0, 32, 1, '%d'),
                       slider('Down', 'client', 'target.padding.y', 0, 0, 32, 1, '%d') }),
                slider('Spacing', 'client', 'target.spacing', 0, 0, 32, 1, '%d'),
                -- the "Target" title ([client] target.label, its keys from when it was called the caption) and its own True
                -- Font (target.title.font: the titles' until set, those the buttons': chatfont.cpp key_for)
                shown('Title', 'target.label'),
                pair('Offset', 'Moves the "Target" title: X left or right, Y up or down.',
                     { slider('X', 'client', 'target.label.x', 0, -300, 300, 1, '%+d'),
                       slider('Y', 'client', 'target.label.y', 0, -10, 10, 1, '%+d') }),
                area_font('target.title.font', 11, 'titles.font'),
                -- the target's name and its own True Font (target.font: Status & Numbers' until set, the big letters it's
                -- drawn in: chatfont.cpp key_for)
                subheader('Name'),
                choice('Alignment', 'client', 'target.name.align', 'left', { { 'left', 'Left' }, { 'centre', 'Centre' }, { 'right', 'Right' } }),
                pair('Offset', 'Moves the target\'s name: X left or right, Y up or down.',
                     { slider('X', 'client', 'target.name.x', 0, -300, 300, 1, '%+d'),
                       slider('Y', 'client', 'target.name.y', 0, -20, 20, 1, '%+d') }),
                area_font('target.font', 11, 'status.font'),
                -- the game's lock-on picture's "Locked" (target.lock.label; its corner glows and side bars stay)
                shown('"Locked"', 'target.lock.label'),
                pair('Offset', 'Moves the "Locked" of the game\'s lock-on picture: X left or right, Y up or down.',
                     { slider('X', 'client', 'target.lock.label.x', 0, -300, 300, 1, '%+d'),
                       slider('Y', 'client', 'target.lock.label.y', 0, -20, 20, 1, '%+d') }),
                subheader('Bar'),
                toggle('Show the bar outline', 'client', 'target.bar.mask', true),
                toggle('Outline the bar when resized', 'client', 'target.bar.outline', true),
                pair('Offset', 'Moves the HP bar inside the window: X left or right, Y up or down.',
                     { slider('X', 'client', 'target.bar.x', 0, -100, 100, 1, '%+d'),
                       slider('Y', 'client', 'target.bar.y', 0, -10, 10, 1, '%+d') }),
            }
        end,
    },
    {
        -- True Font: the letters drawn from a font on the PC at the screen's own pixels (chatfont.cpp, damagetext.cpp,
        -- nameplates.cpp), Game Options' first sub-tab (merged before Interface: merge_tabs below; /vana truefont opens it). The
        -- default font, the fonts added to every list, then under Fonts each area left here on one line: its font, italic,
        -- bold, uppercase, size and outline, its switch (its renderer) at the end. The chat's, the damage numbers', the tooltips', the buttons', the
        -- help's, the item lists' and the nameplates' are on their own pages (area_font).
        name = 'True Font',
        search = true,
        aliases = { 'truefont', 'font', 'fonts', 'letters' },
        rows = function ()
            local function get(section, key) return vana.config.get(section, key) or '' end
            -- a sub-panel switched by section.key: on is sharp (True Font), off game; `follows`: the menus' until it's set (as
            -- VanaCore reads it, for settings made before the page). Each switch is its own: the menus' sets those still
            -- following it to what they are first, so they stay as they were (the buttons' does the titles' on Windows).
            -- (`follows` a key: that area's switch, the titles' the buttons'; its keys too, chatfont.cpp key_for)
            -- (every area but the chat's follows the menus' switch while unset, chatfont.cpp read_want: Clock, Trade Windows,
            -- Current Gil and Equipment were left out, and Menus & Lists' switch flipped Clock's, reported 2026-11-06; the titles,
            -- the main menu, the party and alliance lists follow buttons.font or status.font, pinned with them)
            local FOLLOWING = { ['menu.font'] = { 'items.font', 'tips.font', 'help.font', 'buttons.font', 'status.font', 'clock.font',
                                                  'input.font', 'phrases.font', 'trade.font', 'gil.font', 'profile.font', 'equip.font',
                                                  'equip.stats.font', 'buffs.font', 'lists.font', 'macrobar.name.font' },
                                ['status.font'] = { 'target.font' }, ['titles.font'] = { 'target.title.font' } }
            local function sharp(section, key, follows)
                local value = get(section, key)
                if value ~= '' or not follows then
                    return value == 'sharp'
                end
                return follows == true and get('client', 'menu.font') == 'sharp' or follows ~= true and sharp('client', follows, FOLLOWS[follows] or true)
            end
            local function switched(section, key, follows)
                return {
                    section = section,
                    key = key,
                    get = function () return sharp(section, key, follows) end,
                    set = function (on)
                        local now = sharp(section, key, follows) and 'sharp' or 'game'
                        for _, other in ipairs(section == 'client' and FOLLOWING[key] or {}) do
                            if get('client', other) == '' then
                                vana.config.set('client', other, now)
                            end
                        end
                        vana.config.set(section, key, on and 'sharp' or 'game')
                    end,
                }
            end
            local rows = {
                (function (row) row.nav_icon = 'font' return row end)(header('True Font')), -- (its icon by its sub-tab in the list)
                -- (the launcher's Font Quality: PageProfiles.xaml FormGfxFontQuality), first on the page
                callout('Font Quality', 'Set to Uncompressed in Launcher > Account > Graphics Settings.', nil, false),
                -- Disable True Font ([client] truefont.disable=1): every interface area's off at once, wherever its own
                -- switch is, each area's settings kept; the rest of the page locked meanwhile. Menus & Lists off kept on the
                -- areas following it, Buttons and Equipment's among them, whose switches are on Game Options (reported in game
                -- 2026-11-08). Not the nameplates or damage numbers: they keep their own switches (2026-11-09). A row at the
                -- just before Default Font, in a box of its own, as the Advanced Settings are (row.boxed: vana.lua's draw_section)
                (function (row) row.colour, row.bold, row.boxed = M.P.rose, true, true return row end)(
                    toggle('Disable True Font', 'client', 'truefont.disable', false)),
                subheader('Default Font'),
                -- (the nameplates' too while theirs isn't set: [graphics], read with the graphics settings, graphics.cpp)
                (function (row) row.changed = function () vana.graphics.reload() end return row end)(
                    dropdown('Font', 'client', 'truefont.family', 'Segoe UI', font_list('client', 'truefont.family'))),
                dropdown('Japanese font', 'client', 'truefont.japanese', '', japanese_font_list('client', 'truefont.japanese')),
                free(slider('Outline', 'client', 'truefont.outline', 0, 0, 3, 0.5, '%.2f px'), 0, 4),
                slider('Outline opacity', 'client', 'truefont.outline.opacity', 100, 0, 100, 1, '%d%%'),
                toggle('Draw all text with GDI', 'client', 'truefont.gdi', false, true),
                subheader('Custom Fonts'),
                font_browse('client'),
            }
            local added = vana.ui ~= nil and vana.ui.fonts ~= nil and vana.ui.fonts() or {}
            for _, family in ipairs(added) do
                local row = info(family, function () return 'In every font list' end)
                -- a delete icon at the end of the row: the font out of VanaCore's fonts folder (to the Recycle Bin), asked
                -- twice (vana.lua's info row; a DLL without remove_font has no icon)
                if vana.ui.remove_font ~= nil then
                    row.delete = { tip = 'Delete this font (its file goes to the Recycle Bin)',
                                   run = function ()
                                       local gone, why = vana.ui.remove_font(family)
                                       if gone then
                                           print('VanaCore: ' .. family .. ' is deleted.')
                                       else
                                           print('VanaCore: ' .. family .. ' can\'t be deleted: ' .. tostring(why))
                                       end
                                   end }
                end
                rows[#rows + 1] = row
            end
            if #added == 0 then
                rows[#rows + 1] = info('None added yet', function () return '' end)
            end
            -- an area of the interface's text on one line under Fonts (each was a sub-panel of two rows till asked for
            -- one line each, 2026-11-08): its name, its font (the default or its own), italic, bold, uppercase, size and
            -- outline (area_font), its switch at the end (off: the line greyed, still settable); `follows` a key, each of
            -- them that area's until set
            rows[#rows + 1] = subheader('Fonts')
            local column = {} -- (their labels one column, the widest's: vana.lua draw_font_row)
            -- `tip`: what the area mostly draws, on its name (asked for 2026-11-08)
            local function area(title, prefix, size, follows, tip)
                local row = area_font(prefix, size, follows)
                row.label, row.column, row.lock_if, row.tip = title, column, nil, tip -- (its switch greys it instead: vana.lua draw_font_row)
                row.switch = switched('client', prefix, follows)
                rows[#rows + 1] = row
            end
            area('General', 'menu.font', 15, nil,
                 'The text in most menus and lists: Config, the Magic and Abilities lists, the skill and job menus, quests and missions, Search, Linkshell, the Mog House, the macros\' names.\n'
                 .. 'Each area here that isn\'t set yet follows its switch.')
            -- (the Merit Points menu's text, General's until it was asked for apart, 2026-11-08: chatfont.cpp kListWindows;
            -- each part General's until set, as VanaCore reads it, key_for)
            area('Menu Lists', 'lists.font', 15, 'menu.font',
                 'The Merit Points menu\'s lists: its categories, the merits and their points box. Their buttons stay Main Commands\', their numbers Labels & Stats\'.\n'
                 .. 'Each setting follows General until it\'s set here.')
            area('Titles', 'titles.font', 11, 'buttons.font',
                 'The windows\' titles and the menus\' other labels, the ones not on a button. Each setting follows Buttons until it\'s set here.')
            area('Labels & Stats', 'status.font', 11, true,
                 'The Status window\'s numbers and words, and the larger numbers other windows draw: skill levels, merits\' [3] and 12 P. '
                 .. 'The party and alliance lists and the target\'s name follow it until theirs are set.')
            area('Clock', 'clock.font', 11, true, -- (its own alone: it followed Labels & Stats until 2026-11-06)
                 'The clock under the compass. Only the settings here change it.')
            area('Trade Windows', 'trade.font', 11, true, -- (every letter in a trade's window and the delivery box)
                 'Every letter in a trade\'s window and the delivery box (sent and received, with its Recipient box): amounts, text and buttons. Only the settings here change it.')
            area('Current Gil', 'gil.font', 11, true, -- (the gil box beside Equipment, Items, a trade, the delivery box)
                 'The Current Gil box wherever it shows (beside Equipment, Items, a trade, the delivery box): its title and amount. Only the settings here change it.')
            area('Status Timers', 'buffs.font', 13, true, -- (the status icons' timers, each centred under its icon; Game Options > Status Icons too)
                 'The timers under your status icons (29m, 3h), each centred under its icon. Also on Game Options > Status Icons.')
            -- (the Target window's title and name are on Game Options > Target and HUD > Target Window, not here: 2026-11-07)
            -- (the Equipment window's two, slot names and stats, are on Game Options > Equipment alone: 2026-11-06)
            for i = 5, #rows do -- (all but the heading, the notice, Default Font's own heading and Disable True Font locked while it's on)
                local was = rows[i].lock_if
                rows[i].lock_if = function () return get('client', 'truefont.disable') == '1' or (was ~= nil and was()) end
            end
            return rows
        end,
    },
    {
        name = 'Interface',
        search = true,
        aliases = { 'interface', 'chat', 'windows', 'menus', 'inventory', 'items', 'equipment', 'equip', 'status', 'macros', 'timestamps', 'nameplates', 'names', 'tooltips', 'tooltip' },
        rows = function ()
            local function off(key) return (tonumber(vana.config.get('client', key) or '') or 0) == 0 end
            -- a map full screen (the HUD Manager's Map / Region Map: Full screen, on unset): its box's rows on Maps locked
            local function full_screen(id) return (tonumber(vana.config.get('client', id .. '.fullscreen') or '') or 1) ~= 0 end
            -- the item tooltips' letters aren't True Font's: tips.font, the menus' (menu.font) until it's set, as VanaCore reads it
            local function tips_game()
                local value = vana.config.get('client', 'tips.font') or ''
                if value == '' then
                    value = vana.config.get('client', 'menu.font') or ''
                end
                return value ~= 'sharp'
            end
            local function tips_locked(row) row.lock_if = tips_game return row end
            -- a tooltip part's size (tips.font.<part>.size): unset, the rows' size (tips.font.size), which it is then
            local function tips_size(part)
                local key = 'tips.font.' .. part .. '.size'
                local row = tips_locked(free(slider('Size', 'client', key, 15, 0, 32, 1, '%d px'), 0, 48))
                row.get = function ()
                    return tonumber(vana.config.get('client', key) or '') or tonumber(vana.config.get('client', 'tips.font.size') or '') or 15
                end
                row.follows = true
                return row
            end
            local function tips_space(part)
                return tips_locked(free(slider('Space above', 'client', 'tips.font.' .. part .. '.space', 0, 0, 16, 1, '%d px'), 0, 48))
            end
            -- a tooltip part's letters' opacity (tips.font.<part>.opacity, 0 to 100; times tips.font.opacity's)
            local function tips_opacity(part)
                return tips_locked(slider('Opacity', 'client', 'tips.font.' .. part .. '.opacity', 100, 0, 100, 1, '%d%%'))
            end
            -- A tooltip part's line, its sliders in line with its other lines' (two slots, the second the Outline's), its
            -- switches and colours after them (row.extras: vana.lua draw_extras), every one locked with the rows
            local function tips_line(label, sliders, extras)
                local row = tips_locked(pair(label, nil, sliders))
                row.title_w, row.slots, row.slot_label, row.extras, row.parts = 96, 2, 'Outline', extras, {}
                for _, sub in ipairs(sliders) do
                    sub.value_w_base = 44 -- (every slot as wide: "1.00 px")
                    row.parts[#row.parts + 1] = tips_locked(sub)
                end
                for _, sub in ipairs(extras or {}) do
                    row.parts[#row.parts + 1] = tips_locked(sub)
                end
                row.reset = function ()
                    for _, sub in ipairs(row.parts) do
                        vana.config.set(sub.section, sub.key, nil)
                    end
                end
                return row
            end
            -- a part's switch as a chip (tips.font.<part>.bold, .italic, .uppercase: its letters in capitals; each the
            -- rows' until set, as chatfont.cpp read_part has them)
            local function tips_chip(part, what, label, letter)
                local key = 'tips.font.' .. part .. '.' .. what
                local row = toggle(label, 'client', key, false)
                row.chip = letter
                row.get = function ()
                    local text = vana.config.get('client', key) or ''
                    text = text ~= '' and text or vana.config.get('client', 'tips.font.' .. what) or ''
                    return (tonumber(text) or 0) ~= 0
                end
                row.follows = true
                return row
            end
            -- its three lines: Size with Bold, Italic, Uppercase and its colours; Opacity and Outline; Space above
            local function tips_look(part, game, numbers)
                local size = tips_size(part)
                size.label = ''
                local extras = { tips_chip(part, 'bold', 'Bold', 'B'), tips_chip(part, 'italic', 'Italic', 'I'),
                                 tips_chip(part, 'uppercase', 'Uppercase', 'AA'),
                                 colour_pick('Colour', 'client', 'tips.font.' .. part .. '.color', game) }
                if numbers then
                    extras[#extras + 1] = colour_pick('Numbers colour', 'client', 'tips.font.' .. part .. '.numbers.color', '#FFFFFF')
                end
                return tips_line('Size', { size }, extras)
            end
            local function tips_ink(part)
                local opacity = tips_opacity(part)
                opacity.label = ''
                return tips_line('Opacity', { opacity, outline_row('tips.font.' .. part, 'tips.font') })
            end
            local function tips_gap(part)
                local space = tips_space(part)
                space.label = ''
                return tips_line('Space above', { space })
            end
            -- the Single Inventory Number's True Font (always True Font's: 2026-11-09), the line every True Font area has
            -- (area_font, [client] items.count.font.*, itemcount.cpp) but its switch: its own font or the default font, its size
            -- in the interface's pixels (0 the count left out; unset, an older items.count.size's percent of 16, else 24),
            -- italic, bold, outline (no capitals: digits); locked while Single Inventory Number is off
            local function count_font()
                local row = area_font('items.count.font', 24)
                row.switch = nil
                for i = #row.parts, 1, -1 do
                    if row.parts[i] == row.upper then
                        table.remove(row.parts, i)
                    end
                end
                row.upper = nil
                local size = row.size
                size.get = function ()
                    local value = tonumber(vana.config.get('client', 'items.count.font.size') or '')
                    local game = tonumber(vana.config.get('client', 'items.count.size') or '') or 150
                    return within(size, value or math.floor(16 * math.max(50, math.min(300, game)) / 100 + 0.5))
                end
                size.follows = true
                row.lock_if = function () return off('items.count') end
                return row
            end
            local function no_stamps() return off('chat.timestamps') end
            local function no_font() return (vana.config.get('client', 'chat.font') or '') ~= 'sharp' end
            local function true_font() return not no_font() end
            -- a Chat tabs row (their names and backgrounds, drawn by chattabs.cpp), locked while the tabs are hidden or their
            -- letters are the game's, or while `when` ([client] chat.tabs.<key> is <value>, unset its `default`) isn't so
            local function tabs_own(row, key, default, ...)
                local wanted = { ... }
                row.lock_if = function ()
                    if (tonumber(vana.config.get('client', 'chat.tabs.hide') or '') or 0) ~= 0 or not area_sharp('chattabs.font', 'titles.font') then
                        return true
                    end
                    if key == nil then return false end
                    local value = vana.config.get('client', 'chat.tabs.' .. key) or ''
                    value = value ~= '' and value or default
                    for _, v in ipairs(wanted) do
                        if v == value then return false end
                    end
                    return true
                end
                return row
            end
            local function rebuilds(row) row.rebuild = true return row end
            local rows = {
                header('Windows'),
                pair('Opacity', nil, { slider('Background', 'client', 'window.opacity', 100, 0, 100, 1, '%d%%'),
                                       slider('Border', 'client', 'window.border.opacity', 100, 0, 100, 1, '%d%%') }),
                pair('Colour', nil, { colour_pick('Background', 'client', 'window.color', '#1A1D33'),
                                      colour_pick('Border', 'client', 'window.border.color', '#8A8FA6') }),
                image_browse('Background image', 'client', 'window.image',
                             'A .png or .jpg from anywhere on your PC, drawn in place of the windows\' background. It is copied into VanaCore\'s images folder.'),
                (function (row) row.lock_if = function () return (vana.config.get('client', 'window.image') or '') == '' end return row end)(
                    choice('Image fit', 'client', 'window.image.fit', 'fill',
                           { { 'fill', 'Fill' }, { 'stretch', 'Stretch' }, { 'tile', 'Tile' }, { 'screen', 'Screen' } })),
                toggle('Solid border line', 'client', 'window.border.solid', false),
                (function (row) row.rebuild = true return row end)( -- Corner size locks by it
                    choice('Corners', 'client', 'window.corners', 'square',
                           { { 'square', 'Square' }, { 'cut', 'Cut' }, { 'round', 'Round' } })),
                (function (row) row.lock_if = function () return (vana.config.get('client', 'window.corners') or 'square') == 'square' end return row end)(
                    slider('Corner size', 'client', 'window.corners.size', 6, 1, 32, 1, '%d')),
                toggle('Hide menu titles', 'client', 'titles.hide', false),
                subheader('Help'), -- the help bar, spell details and comments: their letters and True Font
                area_font('help.font', 15, true),
                -- the menus' buttons: Global, Main Menu and Action Menu tabs, each its set of keys (button_tabs), as
                -- buttons.cpp and chatfont.cpp (key_for) read them
                header('Buttons'),
            }
            for _, row in ipairs(button_tabs()) do
                rows[#rows + 1] = row
            end
            rows[#rows + 1] = header('Sliders')
            for _, row in ipairs(slider_rows()) do
                rows[#rows + 1] = row
            end
            for _, row in ipairs({
                header('Chat'),
                subheader('Text'),
                -- (its switch is the chat's renderer, chat.font: the game font's and True Font's rows lock and unlock by it;
                -- the Chat renderer row of Game and True Font till 2026-11-08)
                area_font('chat.font', 16),
                (function (row) row.lock_if = no_font return row end)(
                    free(slider('Letter spacing', 'client', 'chat.font.spacing', 0, -2, 4, 0.1, '%.1f px'), -4, 8)),
                slider('Line height', 'client', 'chat.lineheight', 16, 12, 32, 1, '%d px'),
                toggle('No indent on wrapped lines', 'client', 'chat.noindent', false),
                subheader('Game font', 'Disabled when True Font is enabled'),
                (function (row) row.rebuild = true row.lock_if = true_font return row end)( -- the shadow's opacity locks and unlocks by it
                    choice('Text shadow', 'client', 'chat.shadow', '0', { { '0', 'Off' }, { '1', 'Shadow' }, { '2', 'Outline' } })),
                (function (row) row.lock_if = function () return true_font() or off('chat.shadow') end return row end)(
                    slider('Shadow opacity', 'client', 'chat.shadow.opacity', 80, 0, 100, 1, '%d%%')),
                -- (the HUD Manager's Chat: Window 1 had them until 2026-11-04, then the Windows page until 2026-11-06)
                subheader('Chat windows'),
                (function (row) row.rebuild = true return row end)( -- the rows below lock and unlock by it
                    toggle('Match game windows', 'client', 'chat.match', false)),
                -- the background and the border: each its opacity and colour on a line
                pair('Background', nil, { matched(slider('Opacity', 'client', 'chat.opacity', 100, 0, 100, 1, '%d%%')),
                                          matched(colour_pick('Colour', 'client', 'chat.color', '#1A1D33')) }),
                pair('Border', nil, { matched(slider('Opacity', 'client', 'chat.border.opacity', 100, 0, 100, 1, '%d%%')),
                                      matched(colour_pick('Colour', 'client', 'chat.border.color', '#8A8FA6')) }),
                matched(toggle('Solid border line', 'client', 'chat.border.solid', false)),
                -- the tabs over the chat log open to scroll (Window, Say, Tell...): each name drawn whole in its tab in their
                -- own True Font (chattabs.font: the titles' until set, those the buttons': chatfont.cpp key_for), chattabs.cpp
                subheader('Chat tabs'),
                rebuilds(toggle('Hide the tabs', 'client', 'chat.tabs.hide', false)),
                area_font('chattabs.font', 11, 'titles.font'),
                tabs_own(choice('Names', 'client', 'chat.tabs.names', 'full', { { 'full', 'Full' }, { 'short', 'Short' } })),
                tabs_own(choice('Alignment', 'client', 'chat.tabs.align', 'left', { { 'left', 'Left' }, { 'centre', 'Centre' } })),
                tabs_own(pair('Offset', 'Moves the tabs\' names: X left or right, Y up or down.',
                              { slider('X', 'client', 'chat.tabs.x', 0, -16, 16, 1, '%+d'),
                                slider('Y', 'client', 'chat.tabs.y', 0, -16, 16, 1, '%+d') })),
                tabs_own(pair('Colour', nil, { colour_pick('Tabs', 'client', 'chat.tabs.color', '#FFFFFF'),
                                               colour_pick('Selected', 'client', 'chat.tabs.selected.color', '#FFFFFF') })),
                tabs_own(rebuilds(choice('Background', 'client', 'chat.tabs.background', 'game',
                                         { { 'game', "Game's" }, { 'flat', 'Flat' }, { 'none', 'None' } }))),
                tabs_own(pair('Tint', nil, { colour_pick('Tabs', 'client', 'chat.tabs.tint', '#FFFFFF'),
                                             colour_pick('Selected', 'client', 'chat.tabs.selected.tint', '#FFFFFF') }),
                         'background', 'game', 'game'),
                tabs_own(pair('Background colour', nil, { colour_pick('Tabs', 'client', 'chat.tabs.background.color', '#2A2E45'),
                                                          colour_pick('Selected', 'client', 'chat.tabs.background.selected.color', '#C87A2A') }),
                         'background', 'game', 'flat'),
                tabs_own(slider('Background opacity', 'client', 'chat.tabs.background.opacity', 100, 0, 100, 1, '%d%%'),
                         'background', 'game', 'game', 'flat'),
                tabs_own(slider('Height', 'client', 'chat.tabs.height', 12, 8, 32, 1, '%d')),
                tabs_own(rebuilds(choice('Width', 'client', 'chat.tabs.width', 'game', { { 'game', "Game's" }, { 'fit', 'Fit names' } }))),
                tabs_own(slider('Padding', 'client', 'chat.tabs.padding', 4, 0, 16, 1, '%d')),
                tabs_own(slider('Gap', 'client', 'chat.tabs.gap', 1, 0, 16, 1, '%d')),
                -- (the game's widths' alone: with Fit names each tab is its name's width)
                (function (row)
                    row.lock_if = function ()
                        return (tonumber(vana.config.get('client', 'chat.tabs.hide') or '') or 0) ~= 0
                            or (area_sharp('chattabs.font', 'titles.font') and (vana.config.get('client', 'chat.tabs.width') or '') == 'fit')
                    end
                    return row
                end)(slider('Tab spacing', 'client', 'chat.tabs.spacing', 30, 16, 64, 1, '%d')),
                subheader('Chat input'), -- (its look: the HUD Manager's Chat: Input Line had it until 2026-11-05)
                (function (row) row.rebuild = true return row end)( -- the rows below lock and unlock by it
                    choice('Match', 'client', 'input.match', 'windows', { { 'windows', 'Game windows' }, { 'chat', 'Chat window' }, { 'off', 'Custom' } })),
                pair('Background', nil, { input_own(slider('Opacity', 'client', 'input.opacity', 100, 0, 100, 1, '%d%%')),
                                          input_own(colour_pick('Colour', 'client', 'input.color', '#1A1D33')) }),
                pair('Border', nil, { input_own(slider('Opacity', 'client', 'input.border.opacity', 100, 0, 100, 1, '%d%%')),
                                      input_own(colour_pick('Colour', 'client', 'input.border.color', '#8A8FA6')) }),
                input_own(toggle('Solid border line', 'client', 'input.border.solid', false)),
                -- the input line's letters and True Font (the menus' until set, as chatfont.cpp reads it)
                area_font('input.font', 15, true),
                -- the auto-translate list's (Tab while typing: "menu    fep"), its own area (chatfont.cpp kPhrases)
                subheader('Auto-translate list'),
                area_font('phrases.font', 15, true),
                subheader('Prefixes'),
                (function (row) row.rebuild = true return row end)( -- the short names lock and unlock by it
                    toggle('Prefix message types', 'client', 'chat.prefix', false)),
                (function (row) row.lock_if = function () return off('chat.prefix') end return row end)(
                    toggle('Short prefixes', 'client', 'chat.prefix.short', false)),
                subheader('Timestamps'),
                (function (row) row.rebuild = true return row end)( -- the rows below lock and unlock by it
                    toggle('Add timestamps', 'client', 'chat.timestamps', false)),
                (function (row) row.lock_if = no_stamps return row end)(
                    toggle('System messages too', 'client', 'chat.timestamps.system', true)),
                (function (row) row.lock_if = no_stamps return row end)(
                    choice('Style', 'client', 'chat.timestamps.style', 'square', { { 'square', '[12:34:56] text' }, { 'round', '(12:34:56) text' }, { 'none', '12:34:56 - text' } })),
                (function (row) row.lock_if = no_stamps return row end)(
                    toggle('Seconds', 'client', 'chat.timestamps.seconds', true)),
                (function (row) row.lock_if = no_stamps row.rebuild = true return row end)( -- AM/PM case locks and unlocks by it
                    toggle('12-hour clock (8:34pm)', 'client', 'chat.timestamps.hour12', false)),
                (function (row) row.lock_if = function () return no_stamps() or off('chat.timestamps.hour12') end return row end)(
                    choice('AM/PM case', 'client', 'chat.timestamps.ampm', 'lower', { { 'lower', 'am / pm' }, { 'upper', 'AM / PM' }, { 'lower_spaced', 'am / pm (spaced)' }, { 'upper_spaced', 'AM / PM (spaced)' } })),
                (function (row) row.lock_if = no_stamps return row end)(
                    colour_pick('Colour', 'client', 'chat.timestamps.color', '#C0C0C0')),
                subheader('Typing and logs'),
                toggle('Ignore key binds while typing', 'input', 'keyboard.blockbindsduringinput', true),
                slider('Chat Input History', 'client', 'chat.history', 20, 9, 100, 1, '%d'),
                slider('Chat Scrollback', 'client', 'chat.scrollback', 12500, 100, 12500, 100, '%d lines'),
                toggle('Save chat logs', 'client', 'chatlog', false),
                header('Menus'),
                subheader('List rows'),
                slider('Selection menu rows', 'client', 'menu.rows', 3, 3, 10, 1, '%d'),
                slider('Auction house list rows', 'client', 'auction.rows', 10, 10, 50, 1, '%d'),
                slider('Shop list rows', 'client', 'shop.rows', 10, 10, 50, 1, '%d'),
                slider('Magic and ability list rows', 'client', 'lists.rows', 12, 12, 40, 1, '%d'),
                slider('Currency list rows', 'client', 'currencies.rows', 8, 8, 30, 1, '%d'),
                subheader('Behaviour'),
                toggle('Menus over fancychat', 'client', 'menus.ontop', true),
                toggle('Mouse picks a row only when it moves', 'client', 'menu.hovermove', true),
                toggle('Ignore the mouse in menus', 'client', 'menu.nomouse', false),
                toggle('Always auto-sort inventory', 'client', 'items.autosort', true),
                toggle('Shop purchase total in chat', 'client', 'shop.total', true),
                choice('Inventory sort order', 'client', 'items.sort', 'game', { { 'game', 'Game' }, { 'az', 'A-Z' } }),
                toggle('Always sort spells A-Z', 'client', 'spells.autosort', false),
                -- the party list and the alliance lists (until 2026-11-06 the Party list was under Menus)
                header('Party / Alliance'),
                -- made-up members in every empty place of the party and alliance lists while it's on (partypreview.lua);
                -- its button says which, read each frame
                setmetatable({ kind = 'action', label = 'Preview', icon = 'eye',
                               run = function () M.partypreview.toggle() end,
                               tip = 'Fills the party list and both alliance lists with made-up members, to see these settings on them.\n'
                                     .. 'Only on your screen. Ends when this window closes or you change zone.' },
                             { __index = function (_, key)
                                 if key ~= 'text' then
                                     return nil
                                 end
                                 return M.partypreview ~= nil and M.partypreview.on and 'Hide test party' or 'Show test party'
                             end }),
                subheader('Party list'), -- (TP bar: the HUD Manager's Party List had it until 2026-11-05)
                (function (row) row.rebuild = true return row end)( -- TP Bar Without MP locks and unlocks by it
                    toggle('TP bar', 'client', 'party.tp', false)),
                (function (row) -- party.tp's own TP bar takes its place
                    row.lock_if = function () return (tonumber(vana.config.get('client', 'party.tp') or '') or 0) ~= 0 end
                    return row
                end)(toggle('TP Bar Without MP', 'client', 'party.tpbar', false)),
                -- the lists' names and numbers (the name plates' big letters) in True Font: each key the Status & Numbers
                -- area's until set, as chatfont.cpp reads it (key_for)
                area_font('party.font', 11, 'status.font'),
                -- room between the members' rows, the list taller with its bottom where it was (partyrows.cpp)
                free(slider('Row spacing', 'client', 'party.spacing', 0, 0, 20, 1, '%d px'), 0, 40),
                numbers_size('party'),
                -- the names in capitals and the numbers' colours: the game's own drawing, whichever letters (partylist.cpp)
                toggle('Names in capitals', 'client', 'party.names.upper', false),
                colours_line('HP / MP / TP colours', nil, colour_pick('HP colour', 'client', 'party.hp.color', '#FFFFFF'),
                             colour_pick('MP colour', 'client', 'party.mp.color', '#FFFFFF'),
                             colour_pick('TP colour', 'client', 'party.tp.color', '#C0C0C0')),
                subheader('Alliance lists'),
                -- the party list's rows above onto these: each alliance key set as its party key is, or taken out where that's
                -- unset (both then follow Status & Numbers alike: chatfont.cpp key_for, read_numbers; partylist.hpp)
                { kind = 'action', label = 'Party list settings', text = 'Copy', icon = 'copy', action = 'alliance_copy_party',
                  rebuild = true, confirm = 'Click again to copy',
                  tip = 'The party list\'s letters, font, outline, numbers size, capitals and colours, copied to the alliance lists.',
                  run = function ()
                      for _, key in ipairs({ 'font', 'font.family', 'font.size', 'font.bold', 'font.italic', 'font.uppercase',
                                             'font.outline', 'font.outline.opacity', 'font.numbers.size', 'names.upper',
                                             'hp.color', 'mp.color', 'tp.color', 'spacing' }) do
                          local value = vana.config.get('client', 'party.' .. key)
                          vana.config.set('client', 'alliance.' .. key, value ~= '' and value or nil)
                      end
                  end },
                area_font('alliance.font', 11, 'status.font'),
                free(slider('Row spacing', 'client', 'alliance.spacing', 0, 0, 20, 1, '%d px'), 0, 40),
                numbers_size('alliance'),
                toggle('Names in capitals', 'client', 'alliance.names.upper', false),
                colours_line('HP / MP / TP colours', nil, colour_pick('HP colour', 'client', 'alliance.hp.color', '#FFFFFF'),
                             colour_pick('MP colour', 'client', 'alliance.mp.color', '#FFFFFF'),
                             colour_pick('TP colour', 'client', 'alliance.tp.color', '#C0C0C0')),
                -- Macro Bar and Maps: how they work and look (the HUD Manager's Macro Bar, Map and Region Map had them until
                -- 2026-11-05; their place, size and Full screen are still its). A map's box rows lock while it's full screen.
                header('Macro Bar'),
                subheader('Customise'),
                -- its keys' labels ("Ctrl1", "Alt0"): Ctrl / Alt left out (the game's letters too), and their own True Font,
                -- macrobar.font's keys alone (chatfont.cpp kMacroBar; here, not on the True Font page), its colour True Font's
                toggle('Show Ctrl / Alt', 'client', 'macrobar.prefix', true),
                -- the macros' names (and the book's) under the keys: macrobar.name.font, each setting General's until it's set
                -- (chatfont.cpp kMacroNames, key_for), drawn whole by macrotext.cpp
                (function (row) row.label = 'Name Font' return row end)(area_font('macrobar.name.font', 15, 'menu.font')),
                (function (row) row.label = 'Key Font' return row end)(area_font('macrobar.font', 11, true)),
                (function (row) row.lock_if = function () return not area_sharp('macrobar.font', true) end return row end)(
                    colour_pick('Colour', 'client', 'macrobar.font.color', '#FFFFFF')),
                subheader('Behaviour'),
                choice('Show', 'client', 'macrobar.mode', 'game', { { 'game', 'Game' }, { 'toggle', 'Toggle' }, { 'hold', 'Hold' } }),
                toggle('No delay (Ctrl / Alt)', 'client', 'macrobar.nodelay', false),
                toggle('Alliance Macro Bar Fix', 'client', 'alliance.macrobar', true),
                toggle('Enable extended macro book names', 'client', 'macronames.extended', false),
                header('Maps'),
                subheader('Map'),
                slider('Opacity', 'client', 'map.opacity', 100, 10, 100, 1, '%d%%'),
                (function (row) row.lock_if = function () return full_screen('map') end return row end)(
                    toggle('Border', 'client', 'map.border', false)),
                (function (row) row.lock_if = function () return full_screen('map') end row.rebuild = true return row end)( -- the colour below locks and unlocks by it
                    toggle('Background', 'client', 'map.background', false)),
                (function (row) row.lock_if = function () return full_screen('map') or off('map.background') end return row end)(
                    colour_pick('Background colour', 'client', 'map.background.color', '#000000')),
                toggle('Scroll wheel zooms', 'client', 'map.wheel', true),
                toggle('Drag to scroll', 'client', 'map.drag', true),
                toggle('Centre on you when opened', 'client', 'map.center', true),
                toggle('Instant close', 'client', 'map.instantclose', false),
                (function (row) row.lock_if = function () return full_screen('map') end return row end)(
                    toggle('Walk with it open', 'client', 'map.walk', true)),
                toggle('Live position', 'client', 'map.track', true),
                slider('Home point zoom', 'client', 'map.pointzoom', 160, 100, 400, 5, '%d%%'),
                subheader('Region Map'),
                slider('Opacity', 'client', 'regionmap.opacity', 100, 10, 100, 1, '%d%%'),
                (function (row) row.lock_if = function () return full_screen('regionmap') end return row end)(
                    toggle('Border', 'client', 'regionmap.border', false)),
                (function (row) row.lock_if = function () return full_screen('regionmap') end row.rebuild = true return row end)( -- the colour below locks and unlocks by it
                    toggle('Background', 'client', 'regionmap.background', false)),
                (function (row) row.lock_if = function () return full_screen('regionmap') or off('regionmap.background') end return row end)(
                    colour_pick('Background colour', 'client', 'regionmap.background.color', '#000000')),
                toggle('Scroll wheel zooms', 'client', 'regionmap.wheel', true),
                toggle('Drag to scroll', 'client', 'regionmap.drag', true),
                toggle('Centre on your zone when opened', 'client', 'regionmap.center', true),
                toggle('Instant close', 'client', 'regionmap.instantclose', false),
                (function (row) row.lock_if = function () return full_screen('regionmap') end return row end)(
                    toggle('Walk with it open', 'client', 'regionmap.walk', true)),
                -- Inventory: the item lists' letters and True Font, their rows, the rarity box, the item squares and the bag
                -- count (Item Tooltips, under it, has an item's details)
                header('Inventory'),
                subheader('Item Lists'),
                area_font('items.font', 15, true),
                slider('Inventory list rows', 'client', 'items.rows', 10, 10, 50, 1, '%d'),
                subheader('Item Rarity'),
                (function ()
                    local row = choice('Rarity box', 'client', 'items.rarity', 'game', { { 'game', 'Game' }, { 'hide', 'Hidden' }, { 'style', 'Restyled' } })
                    row.tip = 'The coloured box round a +1 to +4 item\'s icon.\n'
                        .. 'Hidden: taken out, so the Item Square shows instead.\n'
                        .. 'Restyled: in the colours below, with the Item Square\'s Brightness, Contrast and Opacity.'
                    return row
                end)(),
                (function ()
                    -- picking one writes the four colours below (which can then be changed one by one)
                    local row = choice('Colour preset', 'client', 'items.rarity.preset', 'game', {
                        { 'game', 'SoulspireXI Default' }, { 'mythic', 'Uncommon to Mythic' }, { 'mmo', 'Classic MMO' },
                        { 'arpg', 'Action RPG' }, { 'gw', 'Guild Wars style' }, { 'destiny', 'Destiny style' } })
                    row.changed = function ()
                        local colours = ({
                            mythic = { '#1EFF00', '#0070DD', '#FFD100', '#A335EE' },  -- uncommon, rare, legendary, mythic
                            mmo = { '#1EFF00', '#0070DD', '#A335EE', '#FF8000' },     -- uncommon, rare, epic, legendary
                            arpg = { '#6969FF', '#FFFF64', '#C7B377', '#00FF00' },    -- magic, rare, unique, set
                            gw = { '#1A9306', '#FCD00B', '#FFA405', '#FB3E8D' },      -- masterwork, rare, exotic, ascended
                            destiny = { '#4BA35C', '#5076A3', '#8A4FB0', '#CEAE33' }, -- uncommon, rare, legendary, exotic
                        })[vana.config.get('client', 'items.rarity.preset') or 'game'] or {}
                        for tier = 1, 4 do
                            vana.config.set('client', 'items.rarity.color' .. tier, colours[tier]) -- nil: the game's
                        end
                    end
                    row.rebuild = true -- the colours below show the preset's
                    return row
                end)(),
                colour_pick('+1 colour', 'client', 'items.rarity.color1', '#B8B8B8'),
                colour_pick('+2 colour', 'client', 'items.rarity.color2', '#3880A8'),
                colour_pick('+3 colour', 'client', 'items.rarity.color3', '#A89038'),
                colour_pick('+4 colour', 'client', 'items.rarity.color4', '#30A870'),
                toggle('Colour item names by rarity', 'client', 'items.rarity.names', false),
                subheader('Item Squares'),
                (function ()
                    local row = choice('Item Square', 'client', 'items.slot', 'game', { { 'game', 'Game' }, { 'tint', 'Tinted' }, { 'solid', 'Solid colour' }, { 'hide', 'Hidden' } })
                    row.tip = "The grey square behind an item's icon (item details, equipment, lists, trade).\n"
                        .. 'Tinted: the game\'s square in Colour, with Brightness and Contrast.\n'
                        .. 'Solid colour: filled with Colour.\n'
                        .. 'Opacity: 100% is the item details window\'s own square.'
                    return row
                end)(),
                colour_pick('Colour', 'client', 'items.slot.color', '#FFFFFF'),
                slider('Brightness', 'client', 'items.slot.brightness', 100, 0, 200, 5, '%d%%'),
                slider('Contrast', 'client', 'items.slot.contrast', 100, 0, 300, 5, '%d%%'),
                slider('Opacity', 'client', 'items.slot.opacity', 100, 0, 200, 5, '%d%%'),
                subheader('Inventory Count'),
                (function ()
                    local row = toggle('Single Inventory Number', 'client', 'items.count', false)
                    row.rebuild = true -- the rows below lock and unlock by it
                    return row
                end)(),
                -- its letters, True Font's (a Letters row of True Font, Menu and Counter till 2026-11-09)
                count_font(),
                moved(colour_pick('Colour', 'client', 'items.count.color', '#FFFFFF'), 'items.count'),
                pair('Position', 'X moves it left or right, Y up or down.',
                     { moved(slider('X', 'client', 'items.count.x', 0, -60, 20, 1, '%+d'), 'items.count'),
                       moved(slider('Y', 'client', 'items.count.y', 0, -15, 15, 1, '%+d'), 'items.count') }),
                moved(toggle('Hide the total', 'client', 'items.count.hidetotal', true), 'items.count'),
                moved(dropdown('Separator', 'client', 'items.count.separator', 'slash', { { 'slash', 'Current / Total' }, { 'brackets', 'Current (Total)' }, { 'dash', 'Current - Total' } }), 'items.count'),
                moved(toggle('Auto resize total', 'client', 'items.count.autototal', false), 'items.count'),
                note("The number on a bag window's title bar (Items, Satchel, Mog Safe...), drawn in True Font at your screen's pixels. Auto resize total draws the total as big as the count."),
                -- Equipment: the Equipment window's item squares (itemslot.cpp), its list of what fits a slot and the stats
                -- under it (equipstats.cpp), on their own sub-tab after Inventory
                header('Equipment'),
                -- the Equipment window's letters, two areas each of its own keys alone (chatfont.cpp kEquip, kEquipStats): its
                -- slot names (the game's, True Font, or hidden: equip.font=hide) and its stats (numbers and the stats under it)
                subheader('Slot Names'),
                area_font('equip.font', 11, true), -- (its switch: True Font or the game's; hidden is equip.font=hide, below)
                -- (a Letters row of Game's, True Font and Hidden till 2026-11-08) on, equip.font=hide; off, the key out again,
                -- so they follow the menus' switch (its line's switch puts True Font or the game's in its place)
                (function ()
                    local row = toggle('Hide slot names', 'client', 'equip.font', false)
                    row.rebuild = true
                    row.get = function () return (vana.config.get('client', 'equip.font') or '') == 'hide' end
                    row.set = function (on)
                        if on then
                            vana.config.set('client', 'equip.font', 'hide')
                        elseif (vana.config.get('client', 'equip.font') or '') == 'hide' then
                            vana.config.set('client', 'equip.font', nil)
                        end
                    end
                    return row
                end)(),
                subheader('Stats Text'),
                area_font('equip.stats.font', 11, true),
                subheader('Equipment Squares'),
                (function ()
                    local row = toggle('Own squares', 'client', 'items.equip', false)
                    row.tip = "Draws the Equipment window's item squares with the background and border below,\n"
                        .. 'whatever Item Square is set to. The Check and Equipment Set windows\' too, their slot names shown only on empty slots.'
                    row.rebuild = true -- the rows under it lock and unlock by it
                    return row
                end)(),
                equip_own(pair('Opacity', nil, { slider('Background', 'client', 'items.equip.background.opacity', 50, 0, 100, 1, '%d%%'),
                                                 slider('Border', 'client', 'items.equip.border.opacity', 60, 0, 100, 1, '%d%%') })),
                equip_own(pair('Colour', nil, { colour_pick('Background', 'client', 'items.equip.background', '#000000'),
                                                colour_pick('Border', 'client', 'items.equip.border', '#FFFFFF') })),
                equip_own(slider('Border size', 'client', 'items.equip.border.size', 1, 0, 8, 1, '%d')),
                toggle('Wardrobe number', 'client', 'items.equip.wardrobe', true),
                subheader('Beside Equipment'),
                hide_window('Hide the Status window', 'persona@equip'),
                hide_window('Hide gil', 'money@equip'),
                subheader('Equipment List'),
                slider('Equipment list rows', 'client', 'equip.rows', 10, 10, 50, 1, '%d'),
                subheader('Gear Changes'),
                (function (row) row.rebuild = true return row end)( -- Disable Weapon Blink locks and unlocks by it
                    toggle('Disable Character Blink', 'client', 'blink.disable', false)),
                (function (row) row.lock_if = function () return (tonumber(vana.config.get('client', 'blink.disable') or '') or 0) == 0 end return row end)(
                    toggle('Disable Weapon Blink', 'client', 'blink.weapons', false)),
                subheader('Extended Stats'),
                (function (row) row.rebuild = true return row end)(toggle('Stats under Equipment', 'client', 'equip.stats', false)),
                -- the rows under it, set in and locked while it's off: equipstats.cpp does none of them without it (g_on)
                (function (row)
                    row.sub, row.lock_if = true, function () return (tonumber(vana.config.get('client', 'equip.stats') or '') or 0) == 0 end
                    return row
                end)(toggle('Accuracy and attack from the server', 'client', 'equip.stats.checkparam', true)),
                (function (row) -- (their own box: the HUD Manager's Equipment Stats moves it)
                    row.sub, row.lock_if = true, function () return (tonumber(vana.config.get('client', 'equip.stats') or '') or 0) == 0 end
                    return row
                end)(toggle('Detach', 'client', 'equip.stats.move', false)),
                (function (row) -- (the Check window's stats are drawn only with the Equipment window's on: equipstats_check.h)
                    row.sub, row.lock_if = true, function () return (tonumber(vana.config.get('client', 'equip.stats') or '') or 0) == 0 end
                    return row
                end)(toggle('Stats under /check', 'client', 'equip.stats.check', true)),
                (function (row) -- (their own box: the HUD Manager's Check Window Stats moves it)
                    row.sub, row.lock_if = true, function () return (tonumber(vana.config.get('client', 'equip.stats') or '') or 0) == 0 end
                    return row
                end)(toggle('Detach /check stats', 'client', 'equip.stats.check.move', false)),
                -- an item's details (chatfont.cpp, d3d8_chatfont.h's cf_spread): their rows moved by True Font, so the rows
                -- below lock while the letters are the game's; their font, size, italic and bold on one line
                header('Item Tooltips'),
                area_font('tips.font', 15, true),
                -- itemtips.cpp: the game's pages (augments always on the second) put together into one box
                toggle('Extended tooltips', 'client', 'tips.extended', true),
                -- itemtips.cpp: the details window with no item (an empty slot), and the comment box with no comment, left out of chat.cpp's window draw
                toggle('Hide when empty', 'client', 'tips.empty.hide', true),
                -- tipicons.cpp: the type line under the name as the game writes it, or rewritten
                choice('Type line', 'client', 'tips.type', 'game', { { 'game', 'Default' }, { 'simple', 'Simple' }, { 'type', 'Type only' } }),
                tips_locked(free(slider('Line height', 'client', 'tips.font.lineheight', 16, 12, 32, 1, '%d px'), 12, 32)),
                tips_locked(free(slider('Top padding', 'client', 'tips.font.padding.top', 0, 0, 24, 1, '%d px'), 0, 48)),
                tips_locked(free(slider('Bottom padding', 'client', 'tips.font.padding.bottom', 0, 0, 24, 1, '%d px'), 0, 48)),
                tips_locked(slider('Text opacity', 'client', 'tips.font.opacity', 100, 0, 100, 1, '%d%%')),
                -- an item's rarity (itemrarity.cpp: the +1 to +4 box's tier, in Item Rarity's colours), with either letters
                subheader('Rarity'),
                toggle('Colour the name by rarity', 'client', 'tips.rarity.name', false),
                (function (row) row.rebuild = true return row end)( -- the gradient's rows lock and unlock by it
                    toggle('Rarity gradient', 'client', 'tips.rarity.gradient', false)),
                (function (row) row.lock_if = function () return vana.config.get('client', 'tips.rarity.gradient') ~= '1' end return row end)(
                    slider('Gradient strength', 'client', 'tips.rarity.gradient.strength', 30, 5, 100, 1, '%d%%')),
                (function (row) row.lock_if = function () return vana.config.get('client', 'tips.rarity.gradient') ~= '1' end return row end)(
                    free(slider('Gradient height', 'client', 'tips.rarity.gradient.height', 36, 8, 120, 1, '%d px'), 4, 160)),
                -- the game's green name for an equipped item (lists and its details: itemrarity.cpp); the colour locks unless Colour
                subheader('Equipped items'),
                (function (row) row.rebuild = true return row end)(
                    choice('Equipped name', 'client', 'items.equipped', 'game', { { 'game', 'Green (game)' }, { 'off', 'Off' }, { 'color', 'Colour' } })),
                (function (row) row.lock_if = function () return vana.config.get('client', 'items.equipped') ~= 'color' end return row end)(
                    colour_pick('Equipped colour', 'client', 'items.equipped.color', '#A0FFA0')),
                -- the item's picture and its Rare/Ex badges moved and sized where the game draws them (tipicons.cpp), either letters
                subheader('Picture'),
                pair('Position', 'X moves it left or right, Y up or down.',
                     { slider('X', 'client', 'tips.icon.x', 0, -32, 32, 1, '%+d'), slider('Y', 'client', 'tips.icon.y', 0, -32, 32, 1, '%+d') }),
                free(slider('Size', 'client', 'tips.icon.size', 100, 50, 200, 5, '%d%%'), 25, 400),
                subheader('Badges'),
                pair('Position', 'X moves them left or right, Y up or down.',
                     { slider('X', 'client', 'tips.badges.x', 0, -64, 32, 1, '%+d'), slider('Y', 'client', 'tips.badges.y', 0, -32, 32, 1, '%+d') }),
                free(slider('Size', 'client', 'tips.badges.size', 100, 50, 200, 5, '%d%%'), 25, 400),
                -- each part (d3d8_tips.h finds them by their text): the item's name, its type ("[Body]All Races"), its stats,
                -- its jobs ("Lv.57 WAR/MNK/..."), its crafter's signature ("[Voltiz]", the last line); a colour unset is the game's
                subheader('Title'),
                tips_look('title', '#A8F0A8'),
                tips_ink('title'),
                subheader('Type'),
                tips_look('type', '#FFFFFF'),
                tips_ink('type'),
                tips_gap('type'),
                subheader('Stats'),
                tips_look('stats', '#FFFFFF', true),
                tips_ink('stats'),
                tips_gap('stats'),
                subheader('Jobs'),
                tips_look('jobs', '#FFFFFF'),
                tips_ink('jobs'),
                tips_gap('jobs'),
                subheader('Signature'),
                tips_look('signature', '#FFFFFF'),
                tips_ink('signature'),
                tips_gap('signature'),
                subheader('Timer'),
                tips_look('timer', '#FFFFFF', true),
                tips_ink('timer'),
                tips_gap('timer'),
                header('Nameplates'),
                subheader('Text'),
                -- their True Font on one line, as every True Font area's (area_font): their own font or the default font, their
                -- size in the interface's pixels at their nearest (nameplates.cpp sharp_letter), italic, bold and capitals, its
                -- switch nameplates.font (the Letter spacing row below is built by it)
                (function (row) row.switch = area_switch('nameplates.font', nil, nil, 'graphics') return row end)(
                    (function ()
                        local fonts = font_list('graphics', 'nameplates.family')
                        table.insert(fonts, 1, { '', 'Default font' })
                        -- unset: the old Text size (nameplates.textsize, times the game's letters) times 18 px, as VanaCore
                        -- reads it (graphics.cpp); set, that's taken out
                        local size = free(slider('Font size', 'graphics', 'nameplates.fontsize', 18, 6, 40, 1, '%d px'), 4, 64)
                        size.get = function ()
                            local value = tonumber(vana.config.get('graphics', 'nameplates.fontsize') or '')
                            local old = tonumber(vana.config.get('graphics', 'nameplates.textsize') or '')
                            return within(size, value or (old ~= nil and math.floor(18 * math.max(0.5, math.min(2, old)) + 0.5)) or 18)
                        end
                        size.follows = true
                        size.changed = function ()
                            if (vana.config.get('graphics', 'nameplates.textsize') or '') ~= '' then
                                vana.config.set('graphics', 'nameplates.textsize', nil)
                                vana.graphics.reload()
                            end
                        end
                        return font_line(dropdown('Font', 'graphics', 'nameplates.family', '', fonts), size,
                                         toggle('Italic', 'graphics', 'nameplates.italic', false),
                                         toggle('Bold', 'graphics', 'nameplates.bold', true),
                                         toggle('Uppercase', 'graphics', 'nameplates.uppercase', false))
                    end)()),
                -- the sharp text's spacing or the game's letters', by Letters (the tab's built again when it changes)
                (vana.config.get('graphics', 'nameplates.font') or '') == 'sharp'
                    and free(slider('Letter spacing', 'graphics', 'nameplates.textspacing', 0, -4, 8, 0.5, '%+.1f'), -8, 16)
                    or free(slider('Letter spacing', 'graphics', 'nameplates.gap', 3, -5, 5, 0.5, '%+.1f'), -8, 16),
                subheader('Display'),
                free(slider('Size', 'graphics', 'nameplates.size', 1, 0.5, 2, 0.05, '%.2fx'), 0.25, 4),
                slider('Distance scaling', 'graphics', 'nameplates.distance', 100, 0, 100, 5, '%d%%'),
                toggle('Near names on top', 'graphics', 'nameplates.order', true),
                toggle('Target name blinks', 'graphics', 'nameplates.blink', true),
                needs_dx12(toggle('Smooth with frame generation', 'graphics', 'nameplates.framegen', true)),
                subheader('Shown'),
                toggle('Hide own nameplate', 'graphics', 'nameplates.hideown', false),
                toggle("Hide party members' names", 'graphics', 'nameplates.hideparty', false),
                toggle("Hide alliance members' names", 'graphics', 'nameplates.hidealliance', false),
                toggle("Hide other players' names", 'graphics', 'nameplates.hideplayers', false),
                toggle("Hide NPCs' names", 'graphics', 'nameplates.hidenpcs', false),
                toggle("Hide monsters' names", 'graphics', 'nameplates.hidemonsters', false),
                toggle("Hide trusts' names", 'graphics', 'nameplates.hidetrusts', false),
                toggle("Hide pets' names", 'graphics', 'nameplates.hidepets', false),
                toggle('Bazaar bag', 'graphics', 'nameplates.bazaar', true),
                toggle('Linkshell pearl', 'graphics', 'nameplates.linkshell', true),
                toggle('Mastery stars', 'graphics', 'nameplates.mastery', true),
                toggle('Staff GM icons', 'graphics', 'nameplates.gm', true),
                choice('Star style', 'graphics', 'nameplates.stars.style', 'game', { { 'game', "Game's" }, { 'one', 'One star' }, { 'small', 'Small stars' } }),
                subheader('Positioning'),
                toggle('Character Only', 'graphics', 'nameplates.characteronly', true),
                toggle('Centre name', 'graphics', 'nameplates.center', true),
                pair('Plate', nil, { free(slider('X', 'graphics', 'nameplates.plate.x', 0, -100, 100, 1, '%+.0f'), -100, 100),
                                 free(slider('Y', 'graphics', 'nameplates.plate.y', 0, -100, 100, 1, '%+.0f'), -100, 100) }),
                pair('Name', nil, { free(slider('X', 'graphics', 'nameplates.name.x', 0, -100, 100, 1, '%+.0f'), -100, 100),
                                 free(slider('Y', 'graphics', 'nameplates.name.y', 0, -100, 100, 1, '%+.0f'), -100, 100) }),
                pair('Pearl', nil, { free(slider('X', 'graphics', 'nameplates.pearl.x', 38, -100, 100, 1, '%+.0f'), -100, 100),
                                 free(slider('Y', 'graphics', 'nameplates.pearl.y', -1, -100, 100, 1, '%+.0f'), -100, 100) }),
                free(slider('Pearl size', 'graphics', 'nameplates.pearl.size', 0.45, 0.1, 2, 0.05, '%.2fx'), 0.1, 2),
                pair('Bazaar bag', nil, { free(slider('X', 'graphics', 'nameplates.bazaar.x', 4, -100, 100, 1, '%+.0f'), -100, 100),
                                 free(slider('Y', 'graphics', 'nameplates.bazaar.y', -7, -100, 100, 1, '%+.0f'), -100, 100) }),
                free(slider('Bazaar bag size', 'graphics', 'nameplates.bazaar.size', 1, 0.25, 4, 0.05, '%.2fx'), 0.25, 4),
                pair('GM icon', nil, { free(slider('X', 'graphics', 'nameplates.gm.x', 0, -100, 100, 1, '%+.0f'), -100, 100),
                                 free(slider('Y', 'graphics', 'nameplates.gm.y', -2, -100, 100, 1, '%+.0f'), -100, 100) }),
                free(slider('GM icon size', 'graphics', 'nameplates.gm.size', 1.25, 0.5, 3, 0.05, '%.2fx'), 0.5, 3),
                pair('Stars', nil, { free(slider('X', 'graphics', 'nameplates.stars.x', 1, -100, 100, 1, '%+.0f'), -100, 100),
                                 free(slider('Y', 'graphics', 'nameplates.stars.y', 20, -100, 100, 1, '%+.0f'), -100, 100) }),
                free(slider('Star spacing', 'graphics', 'nameplates.stars.spacing', 40, 0, 150, 5, '%d%%'), 0, 200),
                free(slider('Star size', 'graphics', 'nameplates.stars.size', 0.65, 0.25, 2, 0.05, '%.2fx'), 0.25, 3),
                -- (every icon of the game's by a name, not the pearl's alone: nameplates.cpp icon_across, still pearl.shape's key)
                choice('Nameplate icon ratio', 'graphics', 'nameplates.pearl.shape', '16:9', { { '16:9', '16:9' }, { '4:3', '4:3' }, { 'game', "Game's" } }),
                subheader('Colours'),
                colour_pick('Players', 'graphics', 'nameplates.color.players', '#FFFFFF'),
                colour_pick('NPCs', 'graphics', 'nameplates.color.npcs', '#C2FFC2'),
                colour_pick('Monsters (unclaimed)', 'graphics', 'nameplates.color.monsters', '#FFFFC2'),
                colour_pick('Monsters (claimed)', 'graphics', 'nameplates.color.claimed', '#FF82FF'),
                colour_pick('Monsters (claimed by you)', 'graphics', 'nameplates.color.mine', '#FF8282'),
                colour_pick('Trusts', 'graphics', 'nameplates.color.trusts', '#C2FFC2'),
                colour_pick('Pets', 'graphics', 'nameplates.color.pets', '#C2FFFF'),
                -- the target's arrow and lock-on (the HUD Manager's Target Window had them until 2026-11-05)
                header('Target'),
                toggle('Show the arrow over the target', 'client', 'target.arrow', true),
                -- how far away it can be targeted ([graphics] drawdistance.target; was on Graphics > Draw distance)
                choice('Targeting distance', 'graphics', 'drawdistance.target', 'game', { { 'game', "Game's distance" }, { 'drawn', 'As far as drawn' } }),
                subheader('Lock-on'),
                choice('Lock-on style', 'client', 'target.lock', 'game', { { 'game', 'Game' }, { 'glow', 'Glowing border' }, { 'none', 'None' } }),
                colour_pick('Lock-on colour', 'client', 'target.lock.color', '#FF2A2A'),
                toggle('Lock-on glow pulses', 'client', 'target.lock.pulse', true),
                -- the Target window's title and name, each its own True Font (target.title.font the titles' until set, target.font
                -- Status & Numbers': chatfont.cpp key_for); the HUD Manager's Target Window has them too
                subheader('Title'),
                area_font('target.title.font', 11, 'titles.font'),
                subheader('Name'),
                toggle('Name on one line', 'client', 'target.name.oneline', false), -- (targetwin.cpp name_hook)
                area_font('target.font', 11, 'status.font'),
                header('Status Window'),
                (function ()
                    local row = toggle('Custom status window', 'client', 'status', false)
                    row.rebuild = true -- the rows below lock and unlock by it
                    return row
                end)(),
                -- the player's name in its own True Font, drawn whole (statuswindow_name.h); off, the menu font's big name (the
                -- Name row's Large / Game until 2026-11-08: True Font made it redundant)
                moved((function (row) row.label = 'Name' return row end)(area_font('status.name.font', 16, true)), 'status'),
                moved(dropdown('Jobs', 'client', 'status.jobs', 'full', { { 'full', 'Full: Level 75 SAM' }, { 'simple', 'Simple: 75 SAM / 37 WHM' }, { 'short', 'Short: Lv 75 SAM' }, { 'long', 'Long: Lv 75 Samurai' }, { 'game', 'Game: Lv75 Samurai' } }), 'status'),
                moved(toggle('Show item level (ILv / Su)', 'client', 'status.itemlevel', true), 'status'),
                moved(toggle('Taller window (more spacing)', 'client', 'status.taller', true), 'status'),
                moved(colours_line('Separator line before STR', toggle('Separator line before STR', 'client', 'status.separator', true),
                                   colour_pick('Separator colour', 'client', 'status.separator.color', '#C8D8F8')), 'status'),
                moved(colours_line('Colour HP / MP / TP', toggle('Colour HP / MP / TP', 'client', 'status.colors', true),
                                   colour_pick('HP colour', 'client', 'status.hp.color', '#FA9C9C'),
                                   colour_pick('MP colour', 'client', 'status.mp.color', '#BFE07D'),
                                   colour_pick('TP colour', 'client', 'status.tp.color', '#78C4EE')), 'status'),
                subheader('EXP window'),
                toggle('Show Level', 'client', 'status.exp.level', true),
                toggle('NEXT instead of NEXTLevel', 'client', 'status.exp.shortnext', false),
                -- Status > Profile, the panel beside the Status window (profilewin.cpp; its own True Font, chatfont.cpp's
                -- profile.font, here rather than on the True Font page, as the Target window's are on Target)
                subheader('Profile'),
                toggle('Clean layout', 'client', 'profile.clean', false),
                colour_pick('Title bar colour', 'client', 'profile.title.color', '#505B73'),
                (function (row) -- unset: the game's 38% for the game's colour, solid for one picked (profilewin.cpp reload)
                    row.get = function ()
                        local value = tonumber(vana.config.get('client', 'profile.title.opacity') or '')
                        return value or ((vana.config.get('client', 'profile.title.color') or '') ~= '' and 100 or 38)
                    end
                    row.follows = true
                    return row
                end)(slider('Title bar opacity', 'client', 'profile.title.opacity', 38, 0, 100, 1, '%d%%')),
                subheader('Profile text'),
                area_font('profile.font', 11, true),
                -- its titles (Profile, Monstrosity) their own True Font, the text's keys until set (chatfont.cpp key_for)
                subheader('Profile titles'),
                area_font('profile.title.font', 11, 'profile.font'),
                -- Status > Craft Skills: its levels' "(60)" made "60" when off (profilewin.cpp sync_brackets, 2026-11-09)
                subheader('Craft Skills'),
                toggle('Brackets around levels', 'client', 'craft.brackets', true),
                -- what the Network window reads (the HUD Manager's Network Window had it until 2026-11-05; its hide, frame,
                -- place and contents' nudges are still its)
                header('Network'),
                choice('Layout', 'client', 'network.layout', 'game', { { 'game', 'Game' }, { 'line', 'One line' } }),
                choice('Icon', 'client', 'network.icon', 'arrows', { { 'arrows', 'Arrows (game)' }, { 'dot', 'Dot' }, { 'none', 'None' } }),
                toggle('Show the quality %', 'client', 'network.percent', true),
                toggle('Show S (sent)', 'client', 'network.sent', true),
                toggle('Show R (received)', 'client', 'network.received', true),
                toggle('Simple: Connected / Failing / Disconnected', 'client', 'network.simple', false),
                toggle('Connected in green', 'client', 'network.simple.green', false),
            }) do
                rows[#rows + 1] = row
            end
            return rows
        end,
    },
    {
        name = 'Plugins',
        aliases = { 'plugin' },
        buttons = { { 'Refresh', nil, 'refresh', 'Refresh: look at the folders and what is loaded again' } },
        rows = function () return extension_rows('plugin') end,
    },
    {
        name = 'Addons',
        aliases = { 'addon' },
        buttons = { { 'Refresh', nil, 'refresh', 'Refresh: look at the folders and what is loaded again' } },
        page = function () return layout.page end, -- (M.rebuild() has all the upvalues a function may)
        rows = function ()
            if layout.page == 'layout' then
                return layout.rows()
            end
            return extension_rows('addon')
        end,
    },
    {
        name = 'Pivot',
        buttons = { { 'Refresh', nil, 'refresh', 'Refresh: look at the folder again' } }, -- the tab built again: the folder looked at afresh
        rows = function ()
            local on, off, root, server = pivot_overlays()
            -- the folder at the top, and what keeps the overlays (soulspirexi's above all) from being used
            local rows = { header('Folder'), info('Path', function () return root end) }
            local enabled = ini_value('pivot', 'enabled')
            if enabled == nil or enabled == '0' then
                rows[#rows + 1] = alert('Pivot is off ([pivot] enabled): no overlay is used')
            end
            if not vana.fs.is_dir(root) then
                rows[#rows + 1] = alert('Folder not found: no overlay is used')
            elseif not server then
                rows[#rows + 1] = alert("No soulspirexi folder in it: SoulspireXI's own DATs (items, custom content) aren't used")
            end
            rows[#rows + 1] = header('Overlays')
            rows[#rows + 1] = note('Top to bottom: the first with a file serves it. Applies from the next zone.')
            if server then
                rows[#rows + 1] = overlay('soulspirexi', true, nil, nil, true)
            end
            for i, name in ipairs(on) do
                local row = overlay(name, true, i, #on)
                local folder = (name:match('^%a:[\\/]') or name:match('^[\\/][\\/]')) and name or root .. '\\' .. name
                if not vana.fs.is_dir(folder) then
                    row.label = name .. '  (not found)'
                end
                rows[#rows + 1] = row
            end
            if not server and #on == 0 then
                rows[#rows + 1] = note('None in use: switch one on below.')
            end
            rows[#rows + 1] = header('Available')
            for _, name in ipairs(off) do
                rows[#rows + 1] = overlay(name, false)
            end
            if #off == 0 then
                rows[#rows + 1] = note(#on == 0 and 'No overlays in ' .. root or 'Every overlay is in use.')
            end
            return rows
        end,
    },
}

-- Two tabs shown as one page named `name`, at the first's place: the second's sections after the first's, each of its
-- rows marking the tab it came from (old_tab: Options' aren't a HUD preset's). Both old names still open it (/vana effects).
local function merge_tabs(name, first, second)
    local into, from, at = nil, nil, nil
    for i, tab in ipairs(TABS) do
        if tab.name == first then
            into = tab
        elseif tab.name == second then
            from, at = tab, i
        end
    end
    table.remove(TABS, at)
    local aliases = {}
    for _, tab in ipairs({ into, from }) do
        if tab.name ~= name then
            aliases[#aliases + 1] = tab.name:lower()
        end
        for _, alias in ipairs(tab.aliases or {}) do
            aliases[#aliases + 1] = alias
        end
    end
    local first_rows, second_rows = into.rows, from.rows
    into.name, into.aliases = name, aliases
    into.rows = function ()
        local rows = first_rows()
        for _, row in ipairs(second_rows()) do
            row.old_tab = second
            rows[#rows + 1] = row
        end
        return rows
    end
end
merge_tabs('Graphics', 'Graphics', 'Effects')
-- Toggles: Graphics' first section, its sub-tab under Presets (layers.rows; /vana toggles or /vana layers)
for _, graphics in ipairs(TABS) do
    if graphics.name == 'Graphics' then
        local graphics_rows = graphics.rows
        graphics.rows = function ()
            local rows = graphics_rows()
            local all = M.layers.rows(rows)
            for _, row in ipairs(rows) do
                all[#all + 1] = row
            end
            return all
        end
    end
end
merge_tabs('Interface', 'True Font', 'Interface') -- True Font first: Game Options' top sub-tab
merge_tabs('Game Options', 'Interface', 'Options')

-- VanaCore: Client's first sub-tab (About's heading, banner and Info, then Client's VanaCore settings), no page of its
-- own; /vana about and /vana vanacore open Client at it
do
    local about, client = nil, nil
    for i = #TABS, 1, -1 do
        if TABS[i].name == 'About' then
            about = table.remove(TABS, i)
        elseif TABS[i].name == 'Client' then
            client = TABS[i]
        end
    end
    local about_rows, client_rows = about.rows, client.rows
    client.aliases[#client.aliases + 1] = 'about'
    client.aliases[#client.aliases + 1] = 'vanacore'
    client.rows = function ()
        local rows = about_rows()
        for _, row in ipairs(client_rows()) do
            rows[#rows + 1] = row
        end
        return rows
    end
end

local function tab_named(name)
    name = (name or ''):lower()
    for _, tab in ipairs(TABS) do
        if tab.name:lower() == name then
            return tab
        end
        for _, alias in ipairs(tab.aliases or {}) do
            if alias == name then
                return tab -- a tab it was merged into (/vana lighting)
            end
        end
    end
    return nil
end


local LIVE_GRAPHICS = { graphics = true, ui = true, window = true, [FX] = true }

local function read(row)
    if row.hides ~= nil and hidden_by(row.hides) ~= nil then -- an addon has the window hidden: its Hide shows on (locked)
        return true
    end
    if row.get ~= nil then -- a setting kept elsewhere than vanacore.ini
        return row.get()
    end
    local text = vana.config.get(row.section, row.key)
    if text == nil or text == '' then
        return row.default
    end
    if row.kind == 'toggle' then
        local number = tonumber(text)
        if number ~= nil then
            return number ~= 0
        end
        return text:lower() ~= 'off' and text:lower() ~= 'false'
    elseif row.kind == 'slider' then
        local number = tonumber(text)
        if number == nil then
            return row.default
        end
        -- a free row's typed value beyond the slider's ends stays as it is (the slider shows its end)
        return within(row, number)
    elseif row.kind == 'colour' then
        local hex = text:gsub('%s', ''):gsub('^#', ''):upper()
        return hex:match('^%x%x%x%x%x%x$') and '#' .. hex or row.default
    end
    text = text:lower():gsub('%s', '')
    for _, option in ipairs(row.options) do
        if option[1] ~= nil and option[1]:lower():gsub('%s', '') == text then -- spaces out of both: "Segoe UI"; not a line
            return option[1]
        end
    end
    return row.default
end

-- The value as vanacore.ini keeps it.
local function format_value(row, value)
    if row.kind == 'toggle' then
        return value and '1' or '0'
    elseif row.kind == 'slider' then
        if row.step >= 1 then
            return tostring(math.floor(value + 0.5))
        end
        local text = string.format('%.4f', value):gsub('0+$', ''):gsub('%.$', '')
        return text
    end
    return value
end

-- A slider's writes while it's dragged are made once a frame ('present'); other changes at once.
local pending = {}   -- 'section\0key' -> { row, value }
local last_flush = 0

local function flush()
    local reload = false
    for id, entry in pairs(pending) do
        local value = format_value(entry.row, entry.value)
        vana.config.set(entry.row.section, entry.row.key, value ~= '' and value or nil) -- '': the default
        reload = reload or LIVE_GRAPHICS[entry.row.section]
        pending[id] = nil
    end
    if reload then
        vana.graphics.reload()
    end
    last_flush = vana.time.clock()
end

local function write(row, value, now)
    if row.set ~= nil then
        row.set(value, now)
        return
    end
    if row.preview ~= nil and not now then
        row.preview(value) -- shown at once, as it's dragged; vanacore.ini is written when it's let go
        return
    end
    pending[row.section .. '\0' .. row.key] = { row = row, value = value }
    if now then
        flush()
    end
end

local BOOT = vana.paths.install .. '\\scripts\\boot.txt'
-- scripts\boot.txt, so what's switched on loads at the next start too: its `/addon load` (or `/plugin load`) lines
-- name what loads, a kind last. On adds a line unless one loads it; off takes it out of each line that does.
local function persist(row, on)
    local command = row.what == 'addon' and 'addon' or 'plugin'
    -- VanaCore's own window (/vana) and VanaStore load by themselves at start unless [addons] vana=0 or vanastore=0, not
    -- by a boot.txt line (keybinds too, always, and has no switch; src/core/addons.cpp's kOwnDefaults); every other
    -- addon, freecam and loadscreen too, is a boot.txt line. Off also takes out a boot.txt line from before (VanaStore
    -- switched on before it loaded by itself), which would load it again at login
    local own = { vana = true, vanastore = true }
    if command == 'addon' and own[row.name:lower()] then
        vana.config.set('addons', row.name:lower(), on and '1' or '0')
        if on then
            return
        end
    end
    local file = io.open(BOOT, 'rb')
    local text = file and file:read('*a') or ''
    if file then file:close() end
    local eol = text:find('\r\n', 1, true) and '\r\n' or '\n'
    local lines = {}
    for line in (text:gsub('\r\n', '\n') .. '\n'):gmatch('(.-)\n') do
        lines[#lines + 1] = line
    end
    if lines[#lines] == '' then lines[#lines] = nil end
    local name, changed, listed = row.name:lower(), false, false
    for i = #lines, 1, -1 do
        local rest = lines[i]:match('^%s*//?' .. command .. '%s+load%s+(.-)%s*$')
        if rest ~= nil then
            local words, found = {}, false
            for word in rest:gmatch('%S+') do
                if word:lower() == name then found = true else words[#words + 1] = word end
            end
            if found then
                listed = true
                if not on then
                    local kind = words[#words] ~= nil and KIND_NAMES[words[#words]:lower()] ~= nil
                    if #words == 0 or (kind and #words == 1) then
                        table.remove(lines, i)
                    else
                        lines[i] = '/' .. command .. ' load ' .. table.concat(words, ' ')
                    end
                    changed = true
                end
            end
        end
    end
    if on and not listed then
        lines[#lines + 1] = '/' .. command .. ' load ' .. row.name .. ' ' .. row.source
        changed = true
    end
    if changed then
        vana.fs.create_dir(vana.paths.install .. '\\scripts')
        local out = io.open(BOOT, 'wb')
        if out then
            out:write(table.concat(lines, eol), eol)
            out:close()
        end
    end
end

-- A row's switch: the command to load or unload it, and boot.txt; the tab is built again once it has run.
local function set_extension(row, on)
    local command = row.what == 'addon' and '/addon ' or '/plugin '
    vana.chat.run(command .. (on and 'load ' .. row.name:lower() .. ' ' .. row.source or 'unload ' .. row.name:lower()))
    pcall(persist, row, on)
    M.refresh_at = vana.time.clock() + 0.4
end

-- Mods > Loading Screen: the loadscreen addon loaded now, its switch (loaded or unloaded and its boot.txt line, as
-- the Addons tab's switch does), its rows locked while it's off, and Preview (/loadscreen preview)
function M.loadscreen_loaded()
    for _, entry in ipairs(vana.addons.loaded and vana.addons.loaded() or {}) do
        if entry.name:lower() == 'loadscreen' then
            return true
        end
    end
    return false
end

function M.loadscreen_switch(on)
    set_extension({ what = 'addon', name = 'loadscreen', source = 'vanacore' }, on)
end

function M.loadscreen_row(row)
    row.lock_if = function () return not M.loadscreen_loaded() end
    return row
end

-- a Screenshots Display row: locked too while Random screenshot is off
function M.loadscreen_image_row(row)
    M.loadscreen_row(row)
    local lock = row.lock_if
    row.lock_if = function () return lock() or vana.config.get('loadscreen', 'image') ~= '1' end
    return row
end

function M.loadscreen_preview()
    if M.loadscreen_loaded() then
        vana.chat.run('/loadscreen preview')
    end
end

-- A row's reload button: the command, and the tab built again after it.
local function reload_extension(row)
    vana.chat.run((row.what == 'addon' and '/addon ' or '/plugin ') .. 'reload ' .. row.name:lower())
    M.refresh_at = vana.time.clock() + 0.6
end

local function effects_available()
    return vana.graphics.info().effects == true
end

-- an effect's switch as vanacore.ini has it, whatever draws the game: on while it isn't switched off (<key>.enabled=0)
-- and its strength is above 0; and the strength
local function effect_switched(row)
    local enabled = tonumber(vana.config.get(row.section, row.key .. '.enabled') or '') ~= 0
    local strength = tonumber(vana.config.get(row.section, row.key) or '') or row.default
    return enabled and strength > 0, strength
end

local function effect_on(row)
    local on, strength = effect_switched(row)
    return effects_available() and (not row.needs_dx12 or dx12()) and on, strength
end

-- An effect's switch turned (its panel's heading, its Layers chip): off writes <key>.enabled=0, the strength kept; on
-- takes that out, and sets the strength to its `on` when it's 0 (one off by default: the cel look, depth of field). The
-- caller reloads the graphics.
local function set_effect(row, on)
    if not on then
        vana.config.set(row.section, row.key .. '.enabled', '0')
        return
    end
    vana.config.set(row.section, row.key .. '.enabled', nil)
    local _, strength = effect_switched(row)
    if strength <= 0 then
        vana.config.set(row.section, row.key, tostring(row.on))
    end
end

local function resettable(row)
    return row.section ~= nil and row.key ~= nil and not row.keep
end

-- An on/off switch (a toggle, a panel's switch): a reset leaves it as it is, but for a toggle marked resets. An effect's
-- strength is reset, its switch kept.
local function switch_row(row)
    return (row.kind == 'toggle' and not row.resets) or row.switch ~= nil
end

-- Rows' settings back to their defaults (their keys taken out of vanacore.ini). With `switches` (a panel's Reset, at the top of
-- it: vana.lua draw_heading) everything in it, its switches too (switch_back); without (a sub-panel's reset arrow) the on/off
-- switches stay as they are. A heading's own switch (the panel's on/off beside its Reset) always stays. A row's reset_also
-- ({ { section, key } }: a heading's settings no longer on its panel, Mass Grass's old Most tufts ceiling) is taken out with
-- it, whatever resets it: the panel's Reset, Reset all settings. (2026-11-23: a player's "reset your shadows" left Blend with
-- the game's shading on, and Mass Grass's Dry grass, Overgrown and plants as they were: "the main reset on the top should
-- reset EVERYTHING for that category")
local function reset_rows(rows, switches)
    flush()
    local live = false
    local function take(section, key)
        vana.config.set(section, key, nil)
        live = live or LIVE_GRAPHICS[section] or false
    end
    -- a switch back to its default: a toggle's key taken out (one with a set of its own given its default: the Hide switches,
    -- which share hide.windows, take out their own name alone); a sub-panel's switch on one key taken out (Night time's
    -- sun_moon, DLSS 5's dlss5, Auto Login's; one switched as an effect, effect_sub, its strength's .enabled too); a line's
    -- switch (a font line's True Font, row.switch with its key; a colours line's, row.toggle) and the switches among its parts
    -- (a colours line's Gradient). A switch whose key its row doesn't name (a get and set of its own alone) stays
    local function switch_back(row)
        if row.kind == 'toggle' and resettable(row) then
            if row.set ~= nil then
                row.set(row.default)
                live = live or LIVE_GRAPHICS[row.section] or false
            else
                take(row.section, row.key)
            end
            return
        end
        if row.kind == 'subheader' and row.switch ~= nil and resettable(row) then
            take(row.section, row.key)
            if row.effect then
                take(row.section, row.key .. '.enabled')
            end
            return
        end
        local sw = row.switch
        if sw ~= nil and sw.section ~= nil and sw.key ~= nil then
            take(sw.section, sw.key)
        end
        if row.toggle ~= nil and resettable(row.toggle) then
            take(row.toggle.section, row.toggle.key)
        end
        for _, part in ipairs(row.parts or {}) do
            if part.kind == 'toggle' then
                switch_back(part)
            end
        end
    end
    -- an effect off by default (its strength's default 0: reflections, the cel look) has its strength's slider set to the
    -- slider's own default, not taken out: taken out, it was 0 again and the effect went off with it
    local off_by_default = {}
    for _, row in ipairs(rows) do
        if row.effect and row.default <= 0 then
            off_by_default[row.section .. '.' .. row.key] = row
        end
    end
    -- (... while it's on: one off, or never turned on, is taken out as any other and stays off)
    local function strength_kept(row)
        local head = row.kind == 'slider' and off_by_default[row.section .. '.' .. row.key]
        if not head then
            return false
        end
        local enabled = tonumber(vana.config.get(row.section, row.key .. '.enabled') or '') ~= 0
        if not enabled or (tonumber(vana.config.get(row.section, row.key) or '') or 0) <= 0 then
            return false
        end
        vana.config.set(row.section, row.key, tostring(row.default > 0 and row.default or head.on))
        return true
    end
    for _, row in ipairs(rows) do
        for _, also in ipairs(row.reset_also or {}) do
            take(also[1], also[2])
        end
        if switches and row.kind ~= 'header' then
            switch_back(row)
        end
        if row.reset ~= nil then
            -- a setting kept elsewhere (a window's place: vana.layout), or a line of parts (a font line's, a colours line's):
            -- its own. Before the switch test: a font line, which has a switch, was left whole, its font and size too
            row.reset()
        elseif switch_row(row) then
            -- a switch: left on or off here (with `switches`, put back above)
        elseif resettable(row) then
            if row.effect then
                -- on while it isn't switched off and its strength is above 0 (effect_on): the strength's default
                -- mustn't turn it on or off
                local enabled = tonumber(vana.config.get(row.section, row.key .. '.enabled') or '') ~= 0
                local strength = tonumber(vana.config.get(row.section, row.key) or '') or row.default
                local was_on = enabled and strength > 0
                if was_on and row.default <= 0 then
                    vana.config.set(row.section, row.key, tostring(row.on))
                else
                    vana.config.set(row.section, row.key, nil)
                    if not was_on and enabled and row.default > 0 then
                        vana.config.set(row.section, row.key .. '.enabled', '0')
                    end
                end
            elseif not strength_kept(row) then
                vana.config.set(row.section, row.key, nil)
            end
            live = live or LIVE_GRAPHICS[row.section] or false
        end
    end
    if live then
        vana.graphics.reload()
    end
    M.rebuild()
end

local function reset_tab(tab)
    reset_rows(tab.rows())
end

-------------------------------------------------------------------------------
-- Toggles (Graphics > Toggles, under Presets; was Client > Layers)
-------------------------------------------------------------------------------

-- Every effect and picture setting as one switch, a chip that's green while it's on and red while it's off, to turn them
-- on one at a time: a video showing what each adds, not just DLSS. Each is the switch of its panel on the Graphics page
-- (or the Client page's own row), found there by its key, so it turns on and off as that one does. All off
-- remembers the ones that were on ([vana] layers.kept, so a restart keeps them); Put back turns those on again and the
-- rest off. In the order shown: what makes the picture, the light, the air, the world, the look. `card`: the field of
-- vana.graphics.info() that's false on a card it can't run on.
local layers = {
    list = {
        { 'DLSS', 'graphics', 'dlss', card = 'dlss_card' },
        { 'FSR', 'graphics', 'fsr', card = 'fsr_card' },
        { 'DLSS 5', 'graphics', 'dlss5', card = 'dlss5' },
        { 'Anti-aliasing', 'graphics', 'antialias' },
        { 'Colour grade', FX, 'grade' },
        { 'Shadows', FX, 'sun' },
        -- the Lighting sub-panels (Graphics > Shadows > Lighting): left on, the night light and the rest stayed with all off
        -- (a slider now, no switch: off is 0, the level it had kept for on)
        { 'Ambient light', FX, 'ambient_force', value = { off = 0, on = 0.5 } },
        { 'Sun strength', FX, 'day_bright', value = { off = 0, on = 1 } },
        { 'Dark night', FX, 'night' },
        { 'Bright night', FX, 'night_bright' },
        { 'Indoor light', FX, 'indoor_bright', value = { off = 0, on = 1 } },
        { 'Player light', FX, 'player_light' },
        { 'Temperature', FX, 'day_night_warmth', value = { off = 0, on = 1 } },
        -- a slider, not a switch: off is the game's 1x, the level it had kept ([vana] layers.value.<id>) for on
        { 'Character light', 'graphics', 'characters.brightness', value = { off = 1, on = 1.5 } },
        { 'Ambient occlusion', FX, 'ao' },
        { 'Bounce light', FX, 'gi' },
        { 'Reflections', FX, 'refl' },
        { 'Normal maps', FX, 'normalmap' },
        { 'Glow effects', FX, 'elight' },
        { 'Height fog', FX, 'fog' },
        { 'Bloom', FX, 'bloom' },
        { 'God rays', FX, 'rays' },
        { 'Draw distance', 'graphics', 'drawdistance.enabled' },
        { 'Mass Grass', 'graphics', 'grass' },
        { 'Snow Trails', 'graphics', 'snow' },
        { 'Water', 'graphics', 'water' },
        { 'Less shimmer', FX, 'filter' },
        { 'Bump mapping', 'graphics', 'bump' },
        { 'Ink lines', FX, 'outline' },
        { 'Brightness bands', FX, 'bands' },
        { 'Depth of field', FX, 'dof' },
    },
    info = nil, info_at = -1,
}

-- vana.graphics.info() for the chips, asked at most once a second (it builds a table each time; what they read of it,
-- the renderer and the card, changes rarely)
function layers.graphics()
    local now = vana.time.clock()
    if layers.info == nil or now - layers.info_at >= 1 then
        layers.info, layers.info_at = vana.graphics.info(), now
    end
    return layers.info
end

-- A layer from its entry in the list and the row found for it: id ('section.key'), label, tip, and get (on, as
-- vanacore.ini has it), set (vanacore.ini only: the caller reloads the graphics) and why (what keeps it from being
-- switched, or nil)
function layers.item(spec, row)
    local id = spec[2] .. '.' .. spec[3]
    local item = { id = id, label = spec[1], tip = TIPS.panels[row.text or ''] or TIPS.rows[id] }
    if spec.value ~= nil then
        -- a level whose off is spec.value.off: on while it's anything else; off keeps the level, on brings it back
        local off, keep = spec.value.off, 'layers.value.' .. id
        local function level() return tonumber(vana.config.get(row.section, row.key) or '') or row.default end
        item.get = function () return math.abs(level() - off) > 1e-6 end
        item.set = function (on)
            if not on then
                vana.config.set('vana', keep, tostring(level()))
                vana.config.set(row.section, row.key, nil)
                return
            end
            local kept = tonumber(vana.config.get('vana', keep) or '')
            vana.config.set(row.section, row.key, tostring(kept ~= nil and math.abs(kept - off) > 1e-6 and kept or spec.value.on))
            vana.config.set('vana', keep, nil)
        end
    elseif row.effect then
        item.get = function () return (effect_switched(row)) end
        item.set = function (on) set_effect(row, on) end
    elseif row.switch ~= nil then
        item.get = function () local ok, on = pcall(row.switch.get) return ok and on == true end
        item.set = function (on) pcall(row.switch.set, on) end
    elseif row.kind == 'toggle' then
        item.get = function () return read(row) == true end
        item.set = function (on) vana.config.set(row.section, row.key, format_value(row, on)) end
    else
        -- a choice: on is its first option that isn't off (anti-aliasing's FXAA)
        local first = nil
        for _, option in ipairs(row.options) do
            if first == nil and option[1] ~= 'off' then
                first = option[1]
            end
        end
        item.get = function () return read(row) ~= 'off' end
        item.set = function (on) vana.config.set(row.section, row.key, on and first or 'off') end
    end
    item.why = function ()
        if row.needs_dx12 and not dx12() then
            return 'Needs DirectX 12. The renderer is under Client > Game.'
        end
        local info = layers.graphics()
        -- (every [graphics.effects] one is VanaCore's renderer's: Less shimmer does nothing without it)
        if (row.needs_dx or row.effect or row.section == FX) and info.effects ~= true then
            return 'Needs DirectX 11 or 12. The renderer is under Client > Game.'
        end
        -- a card it can't run on (DLSS without an NVIDIA RTX): one that's on can still be switched off
        if spec.card ~= nil and info[spec.card] == false and not item.get() then
            local why = info[spec.card .. '_why']
            return 'Not available here' .. (why ~= nil and why ~= '' and ': ' .. why or '') .. '.'
        end
        return nil
    end
    return item
end

-- The Toggles section: its heading (a bullet in yellow, as Presets) and the chips' row. Each layer's row is found by its
-- key in the Graphics and Effects pages' rows (`own`) and the Client page's (anti-aliasing), the first of the key that
-- isn't a slider (an effect's heading comes before its Strength); one this VanaCore hasn't got is left out.
function layers.rows(own)
    local ok, items = pcall(function ()
        local found, sliders = {}, {}
        for _, rows in ipairs({ own, tab_named('client').rows() }) do
            for _, row in ipairs(rows) do
                if row.section ~= nil and row.key ~= nil then
                    local id = row.section .. '.' .. row.key
                    local into = row.kind == 'slider' and sliders or found
                    into[id] = into[id] or row
                end
            end
        end
        local list = {}
        for _, spec in ipairs(layers.list) do
            local id = spec[2] .. '.' .. spec[3]
            local row = spec.value ~= nil and sliders[id] or found[id]
            if row ~= nil then
                list[#list + 1] = layers.item(spec, row)
            end
        end
        return list
    end)
    local head = header('Toggles')
    head.nav_icon, head.nav_color = 'bullet', M.P.hud
    if not ok then
        return { head, warning("The toggles couldn't be listed: " .. tostring(items)) }
    end
    return { head, { kind = 'layers', label = 'Toggles', items = items } }
end

-- [vana] layers.kept: the ids of the layers that were on before All off, as a set
function layers.kept()
    local kept = {}
    for id in (vana.config.get('vana', 'layers.kept') or ''):gmatch('[^,%s]+') do
        kept[id] = true
    end
    return kept
end

-- Each layer switched as `want` says of it (true or false; nil leaves it), those that can't be left as they are, and the
-- graphics reloaded once for them all
function layers.switch(items, want)
    flush()
    local changed = false
    for _, item in ipairs(items) do
        local on = want(item)
        if on ~= nil and item.why() == nil and item.get() ~= on then
            item.set(on)
            changed = true
        end
    end
    if changed then
        vana.graphics.reload()
    end
    M.rebuild()
end

-- All off: every layer off, the ones on remembered along with those already kept (a second All off forgets none)
function layers.all_off(items)
    local kept, ids = layers.kept(), {}
    for _, item in ipairs(items) do
        if kept[item.id] or (item.why() == nil and item.get()) then
            ids[#ids + 1] = item.id
        end
    end
    vana.config.set('vana', 'layers.kept', #ids > 0 and table.concat(ids, ',') or nil)
    layers.switch(items, function () return false end)
end

-- Put back: the layers that were on before All off on again, the others off, and none kept
function layers.put_back(items)
    local kept = layers.kept()
    layers.switch(items, function (item) return kept[item.id] == true end)
    vana.config.set('vana', 'layers.kept', nil)
end

-- Client > Developer > Reset all settings: every tab's settings back to their defaults, as each tab's reset puts them;
-- not addon windows' places (the Addons tab's Layout page, config\layout.json)
function M.reset_all()
    local rows = {}
    for _, tab in ipairs(TABS) do
        for _, row in ipairs(tab.rows()) do
            if row.section ~= 'layout' then
                rows[#rows + 1] = row
            end
        end
    end
    reset_rows(rows)
    print('VanaCore: every setting is back to its default; the on/off switches are as they were.')
end

-- Client > Developer > Reload settings: the settings file as it is on disk (the account's, else vanacore.ini) is taken up
-- now (the game works from a copy of it, which what changes the file meanwhile doesn't reach unless [client] ini.upstream
-- is on)
function M.reload_settings()
    if vana.config.reload == nil or not vana.config.reload() then
        print("VanaCore: couldn't take the settings file up.")
        return
    end
    vana.graphics.reload()
    M.rebuild()
    print('VanaCore: the settings file taken up.')
end

-- Whether the apiexplorer addon is loaded (it answers /api itself then)
function M.apiexplorer_loaded()
    for _, entry in ipairs(vana.addons.loaded and vana.addons.loaded() or {}) do
        if entry.name:lower() == 'apiexplorer' then
            return true
        end
    end
    return false
end

-- Client > Developer > API Explorer, and /api before the addon is loaded (vana.lua): the apiexplorer addon's window shown
-- (/api show), the addon loaded first when it isn't (its load runs within a frame or two: /api show waits for it, up to
-- 2 seconds)
function M.open_apiexplorer()
    local loaded = M.apiexplorer_loaded
    if loaded() then
        vana.chat.run('/api show')
        return
    end
    vana.chat.run('/addon load apiexplorer')
    local frames = 0
    local function show()
        frames = frames + 1
        if loaded() then
            vana.chat.run('/api show')
        elseif frames < 120 then
            vana.tasks.oncef(1, show)
        else
            print("VanaCore: API Explorer didn't load (addons\\vanacore\\apiexplorer).")
        end
    end
    vana.tasks.oncef(1, show)
end

-- Damage Numbers > Placement > the test buttons: numbers VanaCore makes over the player (the game's come only with a real
-- hit), placed by the settings
local function damage_test(kind)
    if vana.graphics.damage_test == nil then
        print('VanaCore: the damage number tests need a newer VanaCore.')
        return
    end
    vana.graphics.damage_test(kind)
end
function M.damage_test_hit() damage_test('hit') end
function M.damage_test_round() damage_test('round') end
function M.damage_test_barrage() damage_test('barrage') end

-- Client > VanaCore > Recompile shaders: the scene effects' shaders compiled again from the end of this frame (the effects
-- are off till they're done; the Compiling shaders toast shows it)
function M.recompile_shaders()
    if vana.graphics.recompile == nil then
        print('VanaCore: Recompile shaders needs a newer VanaCore.')
        return
    end
    if not vana.graphics.recompile() then
        print("VanaCore: the shaders can't be recompiled: the game isn't drawing with VanaCore's renderer.")
        return
    end
    print("VanaCore: compiling the scene effects' shaders again.")
end

-- Client > VanaCore > Reset ALL Advanced Settings: every setting in the Advanced Settings groups (the Graphics tab's DLSS,
-- Shadows and Mass Grass boxes, the headings built by advanced_group) back to its default, its switches too. The tab's rows
-- are built again to find them, so a box hidden or never opened is reset all the same.
function M.reset_advanced()
    local ok, rows = pcall(function () return M.tab_named('graphics').rows() end)
    if not ok then
        print('VanaCore: the advanced settings could not be found: ' .. tostring(rows))
        return
    end
    local list, inside = {}, false
    for _, row in ipairs(rows) do
        if row.kind == 'header' or row.kind == 'status' then
            inside = false
        elseif row.kind == 'subheader' and not row.in_group then
            inside = row.advanced == true
        elseif inside then
            list[#list + 1] = row
        end
    end
    M.reset_rows(list, true)
    M.rebuild()
    print('VanaCore: every advanced setting is back to its default.')
end

-- Client > Gamepads > Rescan gamepads: the list of controllers searched again (a pad plugged in does the same by itself)
function M.gamepad_rescan()
    vana.gamepad.list(true)
    M.rebuild()
end

local function user_scale()
    return math.max(0.5, math.min(3, (tonumber(vana.config.get('vana', 'scale')) or 100) / 100))
end

-- Per-controller profiles (Client > Gamepads): vanacore.ini [gamepads] holds, by the controller's instance GUID,
-- "<guid>.mode", "<guid>.buttons" and "<guid>.buttons_ex" in the form [ffxi] gamepad.mode / .buttons / .buttons_ex have.
-- A controller with none uses [ffxi]'s (what the launcher copies from the game's config tool). They are applied to the
-- running game (vana.gamepad.apply) when a controller is picked, when a switch is turned, and once at start.
-- Kept in vana.gamepad (the Gamepads rows call it), in a function of its own so it adds no locals to this chunk.
;(function ()
    if vana.gamepad == nil or vana.gamepad.apply == nil then
        return
    end
    local gp = vana.gamepad
    local DEFAULT_MODE = '1,0,0,0,1,1' -- gamepad, force feedback, sliders, hat switches, works while inactive, XInput

    -- 'auto' stands for Automatic (no GUID); a GUID is kept as the ini names it (upper case)
    local function base(guid)
        guid = (guid or ''):upper()
        if guid == '' or guid == '{00000000-0000-0000-0000-000000000000}' then
            return 'auto'
        end
        return guid
    end

    function gp.profile(guid, field)
        local own = vana.config.get('gamepads', base(guid) .. '.' .. field)
        if own ~= nil and own ~= '' then
            return own, true
        end
        local global = vana.config.get('ffxi', 'gamepad.' .. field)
        -- (all six off is the launcher's "no gamepad" (no FFXI pad settings, or turned off), not switches to keep: a
        -- controller picked on it then got XInput off, and an XInput pad's buttons read through DirectInput)
        if global ~= nil and global ~= '' and not (field == 'mode' and global:gsub('%s', '') == '0,0,0,0,0,0') then
            return global, false
        end
        return field == 'mode' and DEFAULT_MODE or '', false
    end

    local function put(guid, field, value)
        vana.config.set('gamepads', base(guid) .. '.' .. field, value)
    end

    -- [input] gamepad.disable (Client > Gamepads > Gamepad: Disable)
    local function disabled()
        return (tonumber(vana.config.get('input', 'gamepad.disable') or '') or 0) ~= 0
    end

    -- the game uses this controller's settings now, with the gamepad (mode's first switch) on unless gamepads are disabled
    function gp.apply_profile(guid)
        local mode = (gp.profile(guid, 'mode')):gsub('^[^,]*', disabled() and '0' or '1', 1)
        local buttons = gp.profile(guid, 'buttons')
        local buttons_ex = gp.profile(guid, 'buttons_ex')
        return vana.gamepad.apply({ mode = mode, buttons = buttons, buttons_ex = buttons_ex, guid = base(guid) == 'auto'
                                    and '{00000000-0000-0000-0000-000000000000}' or base(guid) })
    end

    -- Disable (true): no controller works, kept for the next start; the game lets go of its pad (the profile of `guid`,
    -- the picked one, with the gamepad off). Enabled again, the controller picked next is applied as usual.
    function gp.set_disabled(off, guid)
        vana.config.set('input', 'gamepad.disable', off and '1' or '0')
        if gp.disable ~= nil then
            gp.disable(off)
        end
        if off then
            gp.apply_profile(guid)
        end
    end

    local function split(text)
        local list = {}
        for value in (text .. ','):gmatch('([^,]*),') do
            list[#list + 1] = value
        end
        return list
    end

    -- the six switches of gamepad.mode: gamepad, force feedback, sliders, hat switches, works while inactive, XInput
    function gp.switch(guid, index)
        return split((gp.profile(guid, 'mode')))[index] == '1'
    end

    function gp.set_switch(guid, index, on)
        local list = split((gp.profile(guid, 'mode')))
        for slot = 1, 6 do
            list[slot] = list[slot] ~= nil and list[slot] ~= '' and list[slot] or '0'
        end
        list[index] = on and '1' or '0'
        list[1] = '1'
        local mode = table.concat(list, ',', 1, 6)
        put(guid, 'mode', mode)
        local ok, why = vana.gamepad.apply({ mode = mode })
        if not ok then
            print('VanaCore: the gamepad setting is saved for the next start (' .. tostring(why) .. ').')
        end
    end

    -- the camera stick's two axes in the button map (slots 17 to 20, counted from 0: vertical twice, horizontal twice):
    -- the Xbox pad's right stick is axes 35 and 36 in DirectInput, a DualSense's 34 and 37 (35 and 36 are its triggers;
    -- its vertical axis runs the other way round: tried on a DualSense, -37 had up and down swapped)
    local CAMERA = { xbox = { '-36', '-36', '35', '35' }, playstation = { '37', '37', '34', '34' } }

    function gp.camera(guid)
        local list = split((gp.profile(guid, 'buttons')))
        for name, axes in pairs(CAMERA) do
            if list[18] == axes[1] and list[19] == axes[2] and list[20] == axes[3] and list[21] == axes[4] then
                return name
            end
        end
        return 'custom'
    end

    function gp.set_camera(guid, name)
        local axes = CAMERA[name]
        if axes == nil then
            return
        end
        local list = split((gp.profile(guid, 'buttons')))
        if #list < 32 then
            return
        end
        for slot = 1, 4 do
            list[17 + slot] = axes[slot]
        end
        local buttons = table.concat(list, ',')
        put(guid, 'buttons', buttons)
        local ok, why = vana.gamepad.apply({ buttons = buttons })
        if not ok then
            print('VanaCore: the gamepad setting is saved for the next start (' .. tostring(why) .. ').')
        end
    end

    -- At start, once the game has set its pad up: the profile of the controller it was given, if it has one
    local frames = 0
    vana.events.on('present', function ()
        frames = frames + 1
        if frames ~= 300 then
            return
        end
        local guid = vana.config.get('ffxi', 'gamepad.guid') or ''
        local _, has_mode = gp.profile(guid, 'mode')
        local _, has_buttons = gp.profile(guid, 'buttons')
        local _, has_ex = gp.profile(guid, 'buttons_ex')
        if has_mode or has_buttons or has_ex or disabled() then
            gp.apply_profile(guid)
        end
    end)
end)()

-- Switch Game (Client > Gamepads): a button, or two pressed together, on the gamepad that brings the next game on this PC
-- in front (for an Alt on a second account). [input] gamepad.switch holds it as tokens joined by '+': 'x<n>' an XInput
-- control (Ashita's numbering: 16 and 17 the triggers), 'd<n>' a DirectInput button (48 + its number), 'd<hat><u|r|d|l>' a
-- DirectInput D-pad direction. Picked by pressing them after clicking its button: what's held when the first is let go.
-- A trigger alone isn't taken (it pairs with another). Sent to the other games running, so each has the same.
-- The buttons go to the game only when they're not the switch; it acts only in the game in front.
;(function ()
    if vana.gamepad == nil or vana.clients == nil then
        return
    end
    local gp = vana.gamepad
    local PAD = { [0] = 'Up', 'Down', 'Left', 'Right', 'Start', 'Back', 'L3', 'R3', 'LB', 'RB', nil, nil, 'A', 'B', 'X', 'Y',
                  'LT', 'RT' }
    -- a DualSense's or DualShock 4's buttons in DirectInput
    local PS = { [0] = 'Square', 'Cross', 'Circle', 'Triangle', 'L1', 'R1', 'L2', 'R2', 'Create', 'Options', 'L3', 'R3', 'PS',
                 'Touchpad', 'Mute' }
    local HAT = { u = 'Up', r = 'Right', d = 'Down', l = 'Left' }
    local held = {}          -- token -> true while down
    local hats = {}          -- DirectInput hat offset -> its directions down ({ u = true, ... })
    local capture = nil      -- { list = { tokens }, frames = frames left }
    local swallow = {}       -- token -> true: its release isn't the game's either
    local pending = false    -- the switch was pressed: done at the next present
    local unfocused_at = 0   -- vana.time.clock() when the game was last seen in the background
    local listening = false

    local function saved()
        return vana.config.get('input', 'gamepad.switch') or ''
    end

    local function tokens(text)
        local list = {}
        for token in text:gmatch('[^+]+') do
            list[#list + 1] = token
        end
        return list
    end

    local function playstation()
        for _, pad in ipairs((gp.list(false))) do
            local name = (pad.name or ''):lower()
            if name:find('dualsense', 1, true) or name:find('dualshock', 1, true) or name:find('wireless controller', 1, true) then
                return true
            end
        end
        return false
    end

    local function trigger(token, ps)
        return token == 'x16' or token == 'x17' or (ps and (token == 'd54' or token == 'd55'))
    end

    local function name_of(token, ps)
        local x = token:match('^x(%d+)$')
        if x ~= nil then
            return PAD[tonumber(x)] or ('XInput ' .. x)
        end
        local hat, dir = token:match('^d(%d+)([urdl])$')
        if hat ~= nil then
            return (hat == '32' and 'D-pad ' or 'Hat ' .. ((tonumber(hat) - 32) / 4 + 1) .. ' ') .. HAT[dir]
        end
        local n = tonumber(token:match('^d(%d+)$') or '')
        if n ~= nil and n >= 48 then
            return ps and PS[n - 48] or ('Button ' .. (n - 47))
        end
        return token
    end

    -- the switch as the panel shows it ('L2 + Up'), or nil when there's none
    function gp.switch_text()
        local list = tokens(saved())
        if #list == 0 then
            return nil
        end
        local ps = playstation()
        for i, token in ipairs(list) do
            list[i] = name_of(token, ps)
        end
        return table.concat(list, ' + ')
    end

    function gp.switch_capturing()
        return capture ~= nil
    end

    local function store(text)
        vana.config.set('input', 'gamepad.switch', text)
        if vana.ipc ~= nil then
            vana.ipc.send('gamepad.switch=' .. text)
        end
    end

    local function finish(list)
        capture = nil
        if #list == 1 and trigger(list[1], playstation()) then
            print('VanaCore: a trigger alone isn\'t a Switch Game button; hold it and press another.')
        elseif #list > 0 then
            store(table.concat(list, '+'))
            print('VanaCore: Switch Game is ' .. gp.switch_text() .. '.')
        end
        if M.rebuild ~= nil then
            M.rebuild()
        end
    end

    -- a control went down or up; true: the game doesn't see it
    local function edge(token, down)
        if down == (held[token] == true) then
            -- no change (a trigger's every move is an event): as its last change went
            return capture ~= nil or swallow[token] == true
        end
        if down then
            held[token] = true
        else
            held[token] = nil
        end
        if capture ~= nil then
            if down then
                local list = capture.list
                local known = false
                for _, have in ipairs(list) do
                    known = known or have == token
                end
                if not known and #list < 2 then
                    list[#list + 1] = token
                end
            elseif #capture.list > 0 then
                finish(capture.list)
            end
            return true
        end
        if not down then
            if swallow[token] then
                swallow[token] = nil
                return true
            end
            return false
        end
        local list = tokens(saved())
        local mine = false
        for _, want in ipairs(list) do
            mine = mine or want == token
        end
        if not mine then
            return false
        end
        for _, want in ipairs(list) do
            if not held[want] then
                return false -- a part of it, waiting for the rest (a trigger goes to the game as it is)
            end
        end
        -- the game in front only; one that came in front within 0.15 s has it still held from the switch in the other
        -- game (its first read in front: within ~5 ms of it, measured), hidden from this one too
        local now = vana.time.clock()
        if vana.system ~= nil and vana.system.focused ~= nil and not vana.system.focused() then
            unfocused_at = now
        end
        pending = now - unfocused_at > 0.15
        swallow[token] = true
        return true
    end

    local function listen()
        if listening then
            return
        end
        listening = true
        vana.events.on('xinput_button', function (e)
            if e.injected or e.button > 17 then
                return false
            end
            return edge('x' .. e.button, e.button >= 16 and e.state > 30 or e.button < 16 and e.state ~= 0)
        end, 'vana.switchgame')
        vana.events.on('dinput_button', function (e)
            if e.injected then
                return false
            end
            local code = e.button
            if code >= 48 then
                return edge('d' .. code, bit.band(e.state, 0x80) ~= 0)
            end
            if code < 32 or code > 44 or code % 4 ~= 0 then
                return false
            end
            -- a hat: its angle (hundredths of a degree, centred -1 or 0xFFFFFFFF) as up to two directions
            local now = {}
            local angle = e.state
            if angle >= 0 and angle < 36000 then
                if angle >= 31500 or angle <= 4500 then now.u = true end
                if angle >= 4500 and angle <= 13500 then now.r = true end
                if angle >= 13500 and angle <= 22500 then now.d = true end
                if angle >= 22500 and angle <= 31500 then now.l = true end
            end
            local before = hats[code] or {}
            hats[code] = now
            local blocked = false
            for _, dir in ipairs({ 'u', 'r', 'd', 'l' }) do
                if before[dir] ~= now[dir] then
                    blocked = edge('d' .. code .. dir, now[dir] == true) or blocked
                end
            end
            return blocked
        end, 'vana.switchgame')
    end

    -- Switch Game's button: press the buttons now (up to 15 s); clicked again while waiting, the switch is cleared
    function gp.switch_capture()
        if capture ~= nil then
            capture = nil
            store('')
            print('VanaCore: Switch Game is cleared.')
            return
        end
        listen()
        held, hats = {}, {}
        capture = { list = {}, frames = 900 }
    end

    -- [input] gamepad.switch from another game (the 'ipc' event goes to this addon in every other game)
    vana.events.on('ipc', function (e)
        local text = (e.message or ''):match('^gamepad%.switch=(.*)$')
        if text ~= nil then
            vana.config.set('input', 'gamepad.switch', text)
            if text ~= '' then
                listen()
            end
            if M.rebuild ~= nil then
                M.rebuild()
            end
        end
    end, 'vana.switchgame')

    vana.events.on('present', function ()
        if vana.system ~= nil and vana.system.focused ~= nil and not vana.system.focused() then
            unfocused_at = vana.time.clock()
        end
        if capture ~= nil then
            capture.frames = capture.frames - 1
            if capture.frames <= 0 then
                capture = nil
                print('VanaCore: Switch Game: no buttons pressed.')
            end
        end
        if not pending then
            return
        end
        pending = false
        local me, others = nil, {}
        for _, client in ipairs(vana.clients.list()) do
            if client.self then
                me = client.pid
            else
                others[#others + 1] = client.pid
            end
        end
        if #others == 0 then
            print('VanaCore: Switch Game: no other game is running.')
            return
        end
        table.sort(others)
        local target = others[1]
        for _, pid in ipairs(others) do
            if me ~= nil and pid > me then
                target = pid
                break
            end
        end
        if vana.clients.front == nil or not vana.clients.front(target) then
            print("VanaCore: Switch Game: the other game couldn't be brought in front.")
        end
    end, 'vana.switchgame')

    if saved() ~= '' then
        listen()
    end
end)()

-------------------------------------------------------------------------------
-- What vana.lua uses
-------------------------------------------------------------------------------

M.TABS, M.TIPS, M.tab_named = TABS, TIPS, tab_named
-- the effects' debug views, picked in the Debug window (debug.lua), in no panel or preset; never saved (ini.cpp session_only).
-- By category, each A-Z (asked for, 2026-11-18)
M.DEBUG_VIEW = needs_dx(choice('Debug view', FX, 'debug', '0', {
    { '0', 'Off' },
    { header = 'Effects' },
    { '3', 'Bloom' }, { '8', 'Bounce light' }, { '7', 'Depth of field' }, { '2', 'Fog' }, { '9', 'Glow effects' }, { '4', 'God rays' },
    { '6', 'Ink lines' }, { '1', 'Occlusion' },
    { header = 'Rain & Water' },
    { '30', 'Puddles' }, { '31', 'Rain' }, { '18', 'Reflection ground' }, { '17', 'Reflections' }, { '11', 'Water' },
    { header = 'Rings' },
    { '16', 'AO rings' }, { '32', 'Rain rings' }, { '15', 'Shadow rings' },
    { header = 'Shadows' },
    { '21', 'Edge-on fade' }, { '25', 'Facing the sun' }, { '27', 'Far shadow map' }, { '29', 'Round shadows on rock' },
    { '22', 'Shadow maps alone' }, { '5', 'Shadows' }, { '23', 'Sharp & middle map share' }, { '24', 'Smooth normals' },
    { '28', 'Underside by smooth normal' },
    { header = 'World' },
    { '19', 'Character mask' }, { '26', 'Grass' } }))
-- ... and a part of the sun's shadows turned off (gfx_d3d12.c sun_gate, SG_* bits), to find the one that changes them as the
-- camera moves; never saved either
M.SUN_GATE = needs_dx12(choice('Shadow part off', FX, 'sun_gate', '0', {
    { '0', 'None' }, { '3583', 'All but the maps (simplest)' }, { '64', 'One look in the maps' }, { '1', 'Smooth normals' },
    { '2', 'Turned-face look' }, { '4', 'Edge-on fade' }, { '8', 'Angle shading' }, { '16', 'Map edge fade' },
    { '128', 'Normal maps' }, { '256', 'Sharp map' }, { '512', 'Kept casters' },
    { '1024', 'Culling' }, { '2048', 'Still layers' }, { '4096', 'Plain-view let-go' } }))
M.read, M.write, M.flush, M.format_value, M.within, M.pending, M.LIVE_GRAPHICS = read, write, flush, format_value, within, pending, LIVE_GRAPHICS
M.resettable, M.reset_rows, M.reset_tab = resettable, reset_rows, reset_tab
M.effect_on, M.set_effect, M.effects_available, M.dx12, M.dim, M.FADED = effect_on, set_effect, effects_available, dx12, dim, FADED
M.layers = layers
M.set_extension, M.reload_extension, M.set_overlay, M.move_overlay = set_extension, reload_extension, set_overlay, move_overlay
M.layout, M.user_scale, M.status_text, M.query, M.KIND_NAMES, M.FX, M.ini_value = layout, user_scale, status_text, query, KIND_NAMES, FX, ini_value
M.rows = { toggle = toggle, choice = choice, slider = slider, header = header, note = note } -- the constructors (M.slider is the control)

