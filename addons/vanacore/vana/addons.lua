--[[
vana\addons.lua: the Addons and Plugins pages' list, and each addon's panel. Loaded by vana.lua after data.lua, with the
shared table M.

The list: every addon (or plugin) VanaCore's folders hold, in one list by name with its switch, loaded or not, so one
switched on or off stays where it is. It takes the place of data.lua's Enabled and Available lists (extension_rows) in
the Addons and Plugins tabs' rows.

The panel: the ? on a row (or a click on its name) opens a panel beside the window with what its files say about it:
  About     a table of its details (version, author, link, kind, its folder), then its
            description (from its code) and its README (from its folder)
Nothing is asked of the addon itself: it reads the same loaded or not.
--]]

local M = ...
local imgui = vana.imgui
local S, P, fonts = M.S, M.P, M.fonts
local BODY, SMALL = M.BODY, M.SMALL

local function clock()
    return vana.time.clock()
end

-------------------------------------------------------------------------------
-- The list
-------------------------------------------------------------------------------

-- Plugins VanaCore refuses because it is them (its own addon host, Sequencer): not listed, as data.lua's HIDDEN_PLUGINS
local HIDDEN_PLUGINS = { addons = true, sequencer = true }
-- first in the list whatever the name; keybinds (always on) and SoulspireXI's protected core addon (stays loaded)
-- can't be switched off
local FIRST = { addon = { vana = 1, keybinds = 2, soulspire_core = 3 }, plugin = {} }
local FIXED = { addon = { keybinds = true, soulspire_core = true }, plugin = {} }

-- whether SoulspireXI requires the addon or plugin (its page's Unload doesn't show)
function M.extension_fixed(what, name)
    return FIXED[what] ~= nil and FIXED[what][name:lower()] == true
end

-- What's loaded now: 'source\0name' (the name lower case) -> its entry (vana.addons.loaded()'s: version, desc, state, ...),
-- or nil when this VanaCore can't say
local function loaded_now(what)
    local api = vana[what == 'addon' and 'addons' or 'plugins']
    if api == nil or api.loaded == nil then
        return nil
    end
    local set = {}
    for _, entry in ipairs(api.loaded()) do
        set[entry.source .. '\0' .. entry.name:lower()] = entry
    end
    return set
end

local function loaded_entry(row)
    local loaded = loaded_now(row.what)
    return loaded ~= nil and loaded[row.source .. '\0' .. row.name:lower()] or nil
end

-- Every addon (a folder with <name>\<name>.lua in it, under addons\vanacore, addons\ashita or addons\windower) or plugin
-- (a .dll in plugins\ashita or plugins\windower): { name, source }
local function installed(what)
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

local function by_name(a, b)
    if a.rank ~= b.rank then
        return a.rank < b.rank
    end
    if a.name:lower() ~= b.name:lower() then
        return a.name:lower() < b.name:lower()
    end
    return a.source < b.source
end

-- The Addons and Plugins pages' own search and filters (the row under List / Layout on Addons, at the top of Plugins):
-- the words in a name, only those loaded, only those of the kinds picked (none picked: every kind). Each page keeps its
-- own, while the window is open and shut.
local filters = {
    addon = { query = { '' }, active = false, kinds = {} },
    plugin = { query = { '' }, active = false, kinds = {} },
}

local function filtering(what)
    local filter = filters[what]
    return filter.query[1]:match('%S') ~= nil or filter.active or next(filter.kinds) ~= nil
end

local function passes(row)
    local filter = filters[row.what]
    local query = filter.query[1]:lower():match('^%s*(.-)%s*$')
    if query ~= '' and not row.name:lower():find(query, 1, true) then
        return false
    end
    if filter.active and not row.on then
        return false
    end
    return next(filter.kinds) == nil or filter.kinds[row.source] == true
end

-- The page's rows: a heading saying how many are loaded, then every one by name (CatsEyeXI's own first). `filtered`:
-- each row shown while it passes the filters (looked at each frame), and a line when none does.
local function list_rows(what, filtered)
    local plural = what == 'addon' and 'Addons' or 'Plugins'
    local loaded = loaded_now(what)
    if loaded == nil then
        return { M.rows.header(plural), M.rows.note("This VanaCore can't say what is loaded: update it.", 'warning') }
    end
    local list = {}
    for _, entry in ipairs(installed(what)) do
        local lower = entry.name:lower()
        list[#list + 1] = { kind = 'extension', label = entry.name, name = entry.name, source = entry.source, what = what,
                            on = loaded[entry.source .. '\0' .. lower] ~= nil, rank = FIRST[what][lower] or 99,
                            fixed = FIXED[what][lower] == true }
    end
    table.sort(list, by_name)
    local head = M.rows.header('Installed')
    -- counted as it's drawn: a switch flips its row's `on` at once
    head.tag = function ()
        local on = 0
        for _, row in ipairs(list) do
            if row.on then
                on = on + 1
            end
        end
        local loaded_tag = string.format('%d of %d loaded', on, #list)
        if filtered and filtering(what) then
            local shown = 0
            for _, row in ipairs(list) do
                if passes(row) then
                    shown = shown + 1
                end
            end
            return string.format('%d shown  ·  %s', shown, loaded_tag)
        end
        return loaded_tag
    end
    local rows = { head }
    for _, row in ipairs(list) do
        if filtered then
            row.show_if = function () return passes(row) end
        end
        rows[#rows + 1] = row
    end
    if #list == 0 then
        rows[#rows + 1] = M.rows.note('None in the ' .. plural:lower() .. ' folder.', 'info')
    elseif filtered then
        local none = M.rows.note('No ' .. plural:lower() .. ' match the search and filters.', 'info')
        none.show_if = function ()
            for _, row in ipairs(list) do
                if passes(row) then
                    return false
                end
            end
            return true
        end
        rows[#rows + 1] = none
    end
    return rows
end
M.list_rows = list_rows

do
    local addons_tab, plugins_tab = M.tab_named('addons'), M.tab_named('plugins')
    if addons_tab ~= nil then
        addons_tab.rows = function ()
            if M.layout.page == 'layout' then
                return M.layout.rows()
            end
            local rows = list_rows('addon', true)
            table.insert(rows, 1, { kind = 'extension_filter', what = 'addon' })
            -- an addon package (.vcaddon): picked, shown, then installed (install.lua). Files dropped in addons\install
            -- are offered the same way.
            if vana.addons.package_pick ~= nil then
                table.insert(rows, 2, { kind = 'action', label = 'Install an addon package', text = 'Install from file',
                                        icon = 'download', release = true, run = function () M.install.pick() end })
            end
            return rows
        end
    end
    if plugins_tab ~= nil then
        plugins_tab.rows = function ()
            local rows = list_rows('plugin', true)
            table.insert(rows, 1, { kind = 'extension_filter', what = 'plugin' })
            return rows
        end
    end
end

-- The filter row (vana.lua's draw_row): the search box, then the switches on the right: Active, and the kinds (plugins
-- are Ashita's or Windower's; VanaCore has none of its own)
local FILTER_KINDS = {
    addon = { { 'windower', 'Windower' }, { 'ashita', 'Ashita' }, { 'vanacore', 'VanaCore' } },
    plugin = { { 'windower', 'Windower' }, { 'ashita', 'Ashita' } },
}
local FILTER_LABELS = {
    addon = { 'Windower', 'Ashita', 'VanaCore' },
    plugin = { 'Windower', 'Ashita' },
}

-- toggles drawn as one set of segments, each on or off by itself: `items` { label, on } each; the one clicked, or nil
local function toggles(id, x, cy, items, alpha)
    local labels = {}
    for i, item in ipairs(items) do
        labels[i] = item[1]
    end
    local total, widths = M.segments_width(labels)
    local h = S(M.SEG_SIZE + 7)
    local y = cy - h / 2
    local dl = imgui.GetWindowDrawList()
    M.fill(dl, x, y, total, h, P.well, S(4), 0, alpha)
    M.outline(dl, x, y, total, h, P.well_edge, S(4), 0, 1, alpha)
    local at, clicked = x + S(2), nil
    for i, item in ipairs(items) do
        local hovered, hit = M.hit(id .. i, at, y + S(2), widths[i], h - S(4))
        if hit then
            clicked = i
        end
        local t = M.ease(id .. i, item[2] and 1 or 0)
        if t > 0.01 then
            M.fill(dl, at, y + S(2), widths[i], h - S(4), P.seg, S(3), 0, t * alpha)
        end
        if hovered and not item[2] then
            M.fill(dl, at, y + S(2), widths[i], h - S(4), '#FFFFFF', S(3), 0, 0.05)
        end
        M.text_centred(dl, at, y + S(2), widths[i], h - S(4), item[1], M.mix(P.dim, P.text, t), fonts.text, M.SEG_SIZE, alpha)
        at = at + widths[i] + S(1)
    end
    return clicked, total
end

function M.draw_extension_filter(id, what, lx, right, cy, alpha)
    local dl = imgui.GetWindowDrawList()
    local filter, kind_list = filters[what], FILTER_KINDS[what]
    id = id .. '.' .. what -- its own switches' fades and clicks on each page
    -- the kinds at the right end, Active before them
    local kinds = {}
    for i, k in ipairs(kind_list) do
        kinds[i] = { k[2], filter.kinds[k[1]] == true }
    end
    local kinds_w = M.segments_width(FILTER_LABELS[what])
    local active_w = M.segments_width({ 'Active' })
    local kx = right - kinds_w
    local ax = kx - S(12) - active_w
    local picked = toggles(id .. '.kinds', kx, cy, kinds, alpha)
    if picked ~= nil then
        local kind = kind_list[picked][1]
        filter.kinds[kind] = not filter.kinds[kind] or nil
    end
    if toggles(id .. '.active', ax, cy, { { 'Active', filter.active } }, alpha) ~= nil then
        filter.active = not filter.active
    end
    if M.over(ax, cy - S(12), active_w, S(24)) then
        M.tooltip('Only those loaded')
    end

    -- the search box: from the left to the switches, as tall as they are, its text their size and on their line (the
    -- field's text lifted as text_mid lifts theirs)
    local bw, bh = ax - S(16) - lx, S(M.SEG_SIZE + 7)
    local by = cy - bh / 2
    M.fill(dl, lx, by, bw, bh, P.well, S(4), 0, alpha)
    M.outline(dl, lx, by, bw, bh, P.well_edge, S(4), 0, 1, alpha)
    M.icon_centred(dl, lx + S(14), cy, 'search', P.faint, 10, alpha)
    local searching = filter.query[1] ~= ''
    local text_h = S(M.SEG_SIZE)
    local lift = math.floor(text_h * 0.06 + 0.5)
    imgui.SetCursorScreenPos({ lx + S(22), by - lift })
    imgui.SetNextItemWidth(bw - S(22) - (searching and S(26) or S(4)))
    imgui.PushStyleColor(M.COL.FrameBg, M.rgba(P.well, 0))
    imgui.PushStyleColor(M.COL.FrameBgHovered, M.rgba(P.well, 0))
    imgui.PushStyleColor(M.COL.FrameBgActive, M.rgba(P.well, 0))
    imgui.PushStyleVar(M.VAR.FramePadding, { S(6), math.max(0, (bh - text_h) / 2) })
    imgui.PushStyleVar(M.VAR.FrameBorderSize, 0)
    M.push(fonts.text, M.SEG_SIZE)
    imgui.InputTextWithHint(id .. '.search', what == 'addon' and 'Search addons' or 'Search plugins', filter.query, 64)
    M.pop()
    imgui.PopStyleVar(2)
    imgui.PopStyleColor(3)
    if searching and M.icon_button(id .. '.clear', lx + bw - S(13), cy, 'close', 10, P.rose, P.faint) then
        filter.query[1] = ''
    end
end

-------------------------------------------------------------------------------
-- What an addon's own files say: its header, its README
-------------------------------------------------------------------------------

local function main_file(row)
    if row.what == 'addon' then
        return vana.paths.install .. '\\addons\\' .. row.source .. '\\' .. row.name .. '\\' .. row.name .. '.lua'
    end
    return vana.paths.install .. '\\plugins\\' .. row.source .. '\\' .. row.name .. '.dll'
end

local function folder_of(row)
    if row.what == 'addon' then
        return vana.paths.install .. '\\addons\\' .. row.source .. '\\' .. row.name
    end
    return vana.paths.install .. '\\plugins\\' .. row.source
end

-- An addon's text (the first 512 KB); '' for a plugin
local function read_main(row)
    if row.what ~= 'addon' then
        return ''
    end
    local file = io.open(main_file(row), 'rb')
    if file == nil then
        return ''
    end
    local text = file:read(524288) or ''
    file:close()
    return text
end

-- A field of an addon's header as its code sets it: addon.<field> (Ashita's, VanaCore's) or _addon.<field> (Windower's)
-- set to a quoted string at the start of a line; '' when it isn't (set from a variable, say: the launcher's LuaMeta
-- follows those, this doesn't)
local function header_field(text, field)
    for line in text:gmatch('[^\r\n]+') do
        local rest = line:match('^%s*_?addon%.' .. field .. '%s*=%s*(.*)$')
        if rest ~= nil then
            local value = rest:match('^"(.-)"') or rest:match("^'(.-)'") or rest:match('^%[%[(.-)%]%]')
            if value ~= nil then
                return value
            end
        end
    end
    return ''
end

-- Markdown's marks taken out of a line of text: images, links (their text kept), HTML tags, bold, code
local function unmark(text)
    text = text:gsub('!%[[^%]]*%]%([^)]*%)', '')
    text = text:gsub('%[([^%]]*)%]%([^)]*%)', '%1')
    text = text:gsub('<[^>]*>', '')
    text = text:gsub('%*%*(.-)%*%*', '%1'):gsub('__(.-)__', '%1'):gsub('`([^`]*)`', '%1')
    text = text:gsub('&nbsp;', ' '):gsub('&lt;', '<'):gsub('&gt;', '>'):gsub('&amp;', '&')
    return text
end

-- A table row's cells: { text, code } each (code: all in `backticks`), an escaped \| kept in its cell
-- A cell with `code` in it as words, each a list of pieces { text, code }: a piece with no space before it is part of
-- the word before (a code span and the comma straight after it stay one word)
local function cell_words(cell)
    local words, at, code, spaced = {}, 1, false, true
    while at <= #cell do
        local tick = cell:find('`', at, true) or #cell + 1
        local part = cell:sub(at, tick - 1)
        if not code then
            part = unmark(part)
        end
        for space, word in part:gmatch('(%s*)(%S+)') do
            if space ~= '' or spaced then
                words[#words + 1] = {}
            end
            local pieces = words[#words]
            pieces[#pieces + 1] = { text = word, code = code }
            spaced = false
        end
        -- a space at the end of this part (before a backtick) parts its last word from the next part's first
        spaced = spaced or part:match('%s$') ~= nil
        at, code = tick + 1, not code
    end
    return words
end

-- A table row's cells: { text, words } each (words when it has `code` in it), an escaped \| kept in its cell
local function table_cells(line)
    line = line:gsub('\\|', '\0'):gsub('^%s*|', ''):gsub('|%s*$', '')
    local cells = {}
    for cell in (line .. '|'):gmatch('(.-)|') do
        cell = cell:gsub('%z', '|'):match('^%s*(.-)%s*$')
        cells[#cells + 1] = { text = unmark(cell), words = cell:find('`', 1, true) and cell_words(cell) or nil }
    end
    return cells
end

-- A README as lines to draw: { text, style } with style 'heading', 'code', 'table' (and its grid) or nil; fences, rules
-- and runs of blank lines out, bullets as dots; the first README_LINES of it (the panel lays every line out each frame)
local README_LINES = 600
local function readme_lines(text)
    text = text:gsub('^\239\187\191', ''):gsub('\r\n', '\n'):gsub('\r', '\n'):gsub('\t', '    ')
    local lines, fenced = {}, false
    local function add(line, style)
        if line == '' and (#lines == 0 or lines[#lines].text == '') then
            return
        end
        lines[#lines + 1] = { text = line, style = style }
    end
    -- the lines of a table (each starting with |) as they come; a table once its second line is the |---|---| under
    -- its header, else they're text as any other
    local pending = {}
    local function flush()
        if #pending == 0 then
            return
        end
        local sep = pending[2] ~= nil and pending[2]:find('-', 1, true) and pending[2]:match('^[%s|:%-]+$')
        if not sep then
            for _, line in ipairs(pending) do
                add(unmark(line))
            end
        else
            local rows, cols = {}, 0
            for i, line in ipairs(pending) do
                if i ~= 2 then
                    rows[#rows + 1] = table_cells(line)
                    cols = math.max(cols, #rows[#rows])
                end
            end
            local copy = {}
            for _, row in ipairs(rows) do
                for c = #row + 1, cols do
                    row[c] = { text = '' }
                end
                local texts = {}
                for c, cell in ipairs(row) do
                    texts[c] = cell.text
                end
                copy[#copy + 1] = table.concat(texts, ' | ')
            end
            lines[#lines + 1] = { text = table.concat(copy, '\n'), style = 'table', grid = { rows = rows, header = true } }
        end
        pending = {}
    end
    for raw in (text .. '\n'):gmatch('(.-)\n') do
        if #lines >= README_LINES then
            add('')
            add('... the rest is in the file.', 'code')
            break
        end
        local line = raw:gsub('[%z\1-\31]', ''):gsub('%s+$', '')
        if not fenced and line:match('^%s*|') then
            pending[#pending + 1] = line
            goto next
        end
        flush()
        if line:match('^%s*```') or line:match('^%s*~~~') then
            fenced = not fenced
        elseif fenced then
            add(line, 'code')
        else
            local heading = line:match('^%s*#+%s+(.-)%s*#*$')
            if heading ~= nil then
                add('')
                add(unmark(heading), 'heading')
            elseif line:match('^%s*[-*_][-*_%s]*$') and #line:gsub('%s', '') >= 3 then
                add('') -- a rule
            else
                add(unmark((line:gsub('^(%s*)[-*+]%s+', '%1• '))))
            end
        end
        ::next::
    end
    flush()
    while #lines > 0 and lines[#lines].text == '' do
        lines[#lines] = nil
    end
    if #lines > 0 and lines[1].text == '' then
        table.remove(lines, 1)
    end
    return lines
end

-- An addon's README: README.md, readme.txt, ... in its folder (else the one .md there that isn't a change log, licence
-- or contributing note): its file's name and its lines, or nil
local function find_readme(row)
    if row.what ~= 'addon' then
        return nil
    end
    local folder = folder_of(row)
    local names = vana.fs.list(folder, 'read[ _-]?me(\\..*)?', 'files') or {}
    if #names == 0 then
        local others = {}
        for _, name in ipairs(vana.fs.list(folder, '.+\\.md', 'files') or {}) do
            local lower = name:lower()
            if not lower:find('change', 1, true) and not lower:find('licen', 1, true) and not lower:find('contrib', 1, true) then
                others[#others + 1] = name
            end
        end
        if #others == 1 then
            names = others
        end
    end
    -- README.md before README.txt, before README
    table.sort(names, function (a, b)
        local am, bm = a:lower():find('%.md$') ~= nil, b:lower():find('%.md$') ~= nil
        if am ~= bm then return am end
        return a:lower() < b:lower()
    end)
    for _, name in ipairs(names) do
        local file = io.open(folder .. '\\' .. name, 'rb')
        if file ~= nil then
            local text = file:read(262144) or ''
            file:close()
            local lines = readme_lines(text)
            if #lines > 0 then
                return name, lines
            end
        end
    end
    return nil
end

local KIND_WORDS = { vanacore = 'VanaCore', ashita = 'Ashita', windower = 'Windower' }

-- What its code says it is (the loaded addon's own answer first): its description, and the rest as { label, value } each
local function details_of(row, text, entry)
    local list = {}
    local function add(label, value)
        if value ~= nil and value ~= '' then
            list[#list + 1] = { label, value }
        end
    end
    local function field(name, ...)
        local value = entry ~= nil and entry[name] or nil
        if value ~= nil and value ~= '' then
            return value
        end
        for _, other in ipairs({ name, ... }) do
            value = header_field(text, other)
            if value ~= '' then
                return value
            end
        end
        return ''
    end
    local description = ''
    if row.what == 'addon' then
        description = field('desc', 'description')
        add('Version', field('version'))
        add('Author', field('author'))
        add('Link', field('link'))
    end
    add('Kind', (KIND_WORDS[row.source] or row.source) .. (row.what == 'addon' and ' addon' or ' plugin'))
    add(row.what == 'addon' and 'Folder' or 'File', row.what == 'addon' and folder_of(row) or main_file(row))
    return description, list
end

-------------------------------------------------------------------------------
-- The panel beside the window
-------------------------------------------------------------------------------

-- the open panel: { row, key, view, on, about, details, poll }
local panel = nil

-- a row's key: 'addon:ashita/distance'
local function key_of(row)
    return row.what .. ':' .. row.source .. '/' .. row.name:lower()
end

function M.help_open(row)
    local text = read_main(row)
    local entry = loaded_entry(row)
    local description, details = details_of(row, text, entry)
    local readme_name, readme = find_readme(row)
    -- under the details: the description, then the README under its file's name
    local about = {}
    if description ~= '' then
        about[#about + 1] = { text = 'Description', style = 'label' }
        about[#about + 1] = { text = description }
    end
    if readme ~= nil then
        if #about > 0 then
            about[#about + 1] = { text = '' }
        end
        about[#about + 1] = { text = readme_name, style = 'label' }
        for _, line in ipairs(readme) do
            about[#about + 1] = line
        end
    end
    panel = { row = row, key = key_of(row), on = entry ~= nil, about = about, details = details, view = 'about',
              poll = clock() + 0.5 }
    M.st.picker = nil
end

function M.help_close()
    panel = nil
end

function M.help_showing(row)
    return panel ~= nil and panel.key == key_of(row)
end

local PANEL_W = 440
local TITLE_H, VIEWS_H = 36, 34
-- NoTitleBar, NoResize, NoMove, NoScrollbar, NoScrollWithMouse, NoCollapse, NoBackground, NoSavedSettings: drawn here
local PANEL_FLAGS = 1 + 2 + 4 + 8 + 16 + 32 + 128 + 256 + 65536 -- NoNavInputs: Tab stays the game's
local KIND_COLOURS = { windower = 'teal', ashita = 'amber', vanacore = 'accent' }

-- `str` in lines no wider than `room`, broken between words; a word too long for a line alone (a folder's path) broken
-- where it runs out of room
local function wrap(str, room, font, size)
    local lines, line = {}, ''
    local function put(word)
        local try = line == '' and word or line .. ' ' .. word
        if line ~= '' and M.measure(try, font, size) > room then
            lines[#lines + 1] = line
            line = word
        else
            line = try
        end
    end
    for word in str:gmatch('%S+') do
        if M.measure(word, font, size) > room then
            local piece = ''
            for letter in word:gmatch('[%z\1-\127\194-\244][\128-\191]*') do
                if piece ~= '' and M.measure(piece .. letter, font, size) > room then
                    put(piece)
                    piece = letter
                else
                    piece = piece .. letter
                end
            end
            word = piece
        end
        put(word)
    end
    if line ~= '' then
        lines[#lines + 1] = line
    end
    return lines
end

local CELL_X, CELL_Y = 10, 6

local function word_width(pieces, font, size)
    local w = 0
    for _, piece in ipairs(pieces) do
        w = w + M.measure(piece.text, font, size)
    end
    return w
end

-- words of pieces on one line: the width they're drawn at
local function words_width(words, font, size)
    local w = M.measure(' ', font, size) * (#words - 1)
    for _, pieces in ipairs(words) do
        w = w + word_width(pieces, font, size)
    end
    return w
end

-- words of pieces (cell_words) in lines no wider than `room`, broken between words: each line a list of words
local function wrap_words(words, room, font, size)
    local lines, line, line_w = {}, {}, 0
    local space = M.measure(' ', font, size)
    for _, pieces in ipairs(words) do
        local w = word_width(pieces, font, size)
        if #line > 0 and line_w + space + w > room then
            lines[#lines + 1] = line
            line, line_w = {}, 0
        end
        line_w = line_w + (#line > 0 and space or 0) + w
        line[#line + 1] = pieces
    end
    if #line > 0 then
        lines[#lines + 1] = line
    end
    return lines
end

-- A cell's font, size and colour: a header row in the semibold, a label column small and dim, a command (/ at the
-- start) brighter than the rest; `code` in it is teal as it's drawn
local function cell_style(grid, r, c)
    local cell = grid.rows[r][c]
    if grid.header and r == 1 then
        return fonts.semi, BODY - 1.5, P.text
    elseif grid.label_col and c == 1 then
        return fonts.text, SMALL, P.dim
    end
    local colour = grid.colours ~= nil and grid.colours[r] or cell.words == nil and cell.text:match('^%s*/') and P.text
                   or P.text2
    return fonts.text, BODY - 1.5, colour
end

-- The columns' widths in `room`: each its widest cell's when they all fit (the last then takes what's over), else the
-- narrow ones keep theirs and the rest share what's left evenly
local function column_widths(natural, room)
    local n, total = #natural, 0
    for _, w in ipairs(natural) do
        total = total + w
    end
    local widths = {}
    if total <= room then
        for i, w in ipairs(natural) do
            widths[i] = w
        end
        widths[n] = widths[n] + room - total
        return widths
    end
    local left, open_n, settled = room, n, true
    while settled and open_n > 0 do
        settled = false
        local share = left / open_n
        for i, w in ipairs(natural) do
            if widths[i] == nil and w <= share then
                widths[i], left, open_n, settled = w, left - w, open_n - 1, true
            end
        end
    end
    for i in ipairs(natural) do
        widths[i] = widths[i] or math.floor(left / open_n)
    end
    return widths
end

-- A table, `w` wide, from the scrolling child's cursor: `grid` is { rows = { { { text, code }, ... }, ... }, header,
-- label_col, colours }: the header row and the label column on a darker ground, each cell wrapped in its column, hairlines
-- between. Laid out once for each width (grid.layout).
local function draw_grid(grid, w)
    local t = grid.layout
    if t == nil or t.w ~= w or t.scale ~= M.scale then
        local cols = #grid.rows[1]
        local _, line_h = M.measure('Ag', fonts.text, BODY - 1.5)
        local natural = {}
        for c = 1, cols do
            natural[c] = 0
            for r, row in ipairs(grid.rows) do
                local font, size = cell_style(grid, r, c)
                -- a cell with code in it measured as it's drawn, a piece at a time
                local words = row[c].words
                natural[c] = math.max(natural[c], words ~= nil and words_width(words, font, size) or M.measure(row[c].text, font, size))
            end
        end
        local widths = column_widths(natural, w - S(CELL_X) * 2 * cols)
        t = { w = w, scale = M.scale, line_h = line_h, widths = {}, rows = {}, h = 0 }
        for c = 1, cols do
            t.widths[c] = widths[c] + S(CELL_X) * 2
        end
        for r, row in ipairs(grid.rows) do
            local cells, most = {}, 1
            for c = 1, cols do
                local font, size, colour = cell_style(grid, r, c)
                local lines = row[c].words ~= nil and wrap_words(row[c].words, widths[c], font, size)
                              or wrap(row[c].text, widths[c], font, size)
                local _, h = M.measure('Ag', font, size)
                cells[c] = { lines = lines, font = font, size = size, colour = colour, dy = (line_h - h) / 2 }
                most = math.max(most, #lines)
            end
            local h = most * line_h + S(CELL_Y) * 2
            t.rows[r] = { cells = cells, h = h }
            t.h = t.h + h
        end
        grid.layout = t
    end
    local dl = imgui.GetWindowDrawList()
    local x, y = imgui.GetCursorScreenPos()
    local radius = S(4)
    M.fill(dl, x, y, w, t.h, P.well, radius)
    if grid.header then
        M.fill(dl, x, y, w, t.rows[1].h, P.side, radius, 16 + 32) -- rounded at the top only (ImDrawFlags)
    end
    if grid.label_col then
        M.fill(dl, x, y, t.widths[1], t.h, P.side, radius, 16 + 64) -- rounded on the left only
    end
    local cx = x
    for c = 1, #t.widths - 1 do
        cx = cx + t.widths[c]
        M.vline(dl, cx, y, y + t.h, P.well_edge)
    end
    local ry = y
    for r, row in ipairs(t.rows) do
        if r > 1 then
            M.hline(dl, x, x + w, ry, P.well_edge)
        end
        cx = x
        for c, cell in ipairs(row.cells) do
            for j, line in ipairs(cell.lines) do
                local ly = ry + S(CELL_Y) + (j - 1) * t.line_h + cell.dy
                if type(line) == 'string' then
                    M.text(dl, cx + S(CELL_X), ly, line, cell.colour, cell.font, cell.size)
                else
                    -- words of pieces: each piece where the text before it ends, code in teal
                    local px = cx + S(CELL_X)
                    for k, pieces in ipairs(line) do
                        if k > 1 then
                            px = px + M.measure(' ', cell.font, cell.size)
                        end
                        for _, piece in ipairs(pieces) do
                            M.text(dl, px, ly, piece.text, piece.code and P.teal or cell.colour, cell.font, cell.size)
                            px = px + M.measure(piece.text, cell.font, cell.size)
                        end
                    end
                end
            end
            cx = cx + t.widths[c]
        end
        ry = ry + row.h
    end
    M.outline(dl, x, y, w, t.h, P.well_edge, radius)
    imgui.Dummy({ w, t.h })
end

local DETAIL_COLOURS = { Link = 'accent' }

-- The details as a grid: labels down the left, each value beside its label
local function details_grid(details)
    local grid = { rows = {}, label_col = true, colours = {} }
    for i, d in ipairs(details) do
        grid.rows[i] = { { text = d[1] }, { text = d[2] } }
        grid.colours[i] = P[DETAIL_COLOURS[d[1]] or 'text2']
    end
    return grid
end

-- The text in its scrolling child: the details' table (when given), then the lines wrapped, headings in the accent,
-- labels small and dim, tables as tables
local function draw_text(lines, open, w)
    if open ~= nil then
        open.grid = open.grid or details_grid(open.details)
        draw_grid(open.grid, w - S(12))
        imgui.Dummy({ 1, S(10) })
    end
    imgui.PushTextWrapPos(w - S(12))
    for _, line in ipairs(lines) do
        if line.text == '' then
            imgui.Dummy({ 1, S(7) })
        elseif line.style == 'table' then
            imgui.Dummy({ 1, S(3) })
            draw_grid(line.grid, w - S(12))
            imgui.Dummy({ 1, S(3) })
        elseif line.style == 'label' then
            M.push(fonts.text, SMALL)
            imgui.PushStyleColor(M.COL.Text, M.rgba(P.dim))
            imgui.TextWrapped(line.text)
            imgui.PopStyleColor(1)
            M.pop()
        else
            local heading = line.style == 'heading'
            local colour = heading and P.accent or line.style == 'code' and P.teal
                           or line.text:match('^%s*/') and P.text or P.text2
            M.push(heading and fonts.semi or fonts.text, heading and BODY or BODY - 1.5)
            imgui.PushStyleColor(M.COL.Text, M.rgba(colour))
            imgui.TextWrapped(line.text)
            imgui.PopStyleColor(1)
            M.pop()
        end
    end
    imgui.PopTextWrapPos()
end

-- The panel: drawn beside the window's frame (wx, wy, ww, wh; on its left when the screen has no room on its right), as
-- tall as the window
function M.draw_help(wx, wy, ww, wh, screen_w, screen_h)
    local open = panel
    if open == nil then
        return
    end
    local now = clock()
    if now >= open.poll then
        open.poll = now + 0.5
        open.on = loaded_entry(open.row) ~= nil -- switched, reloaded or unloaded by a command
    end
    local row = open.row
    local pw, ph = S(PANEL_W), wh
    -- in ImGui's pixels, where the window and the panel are drawn k times their size (M.display)
    local dw, dh, k = M.display(screen_w, screen_h)
    local x = wx + (ww + S(8)) * k
    if x + pw * k > dw then
        x = wx - (pw + S(8)) * k
    end
    x = math.max(0, math.min(dw - pw * k, x))
    local y = math.max(0, math.min(dh - ph * k, wy))

    imgui.SetNextWindowPos({ x, y }, 1)
    imgui.SetNextWindowSize({ pw, ph }, 0)
    local COL, VAR = M.COL, M.VAR
    imgui.PushStyleColor(COL.ChildBg, M.rgba(P.bg, 0))
    imgui.PushStyleColor(COL.ScrollbarBg, M.rgba(P.bg, 0))
    imgui.PushStyleColor(COL.ScrollbarGrab, M.rgba('#3A4158'))
    imgui.PushStyleColor(COL.ScrollbarGrabHovered, M.rgba('#4A5270'))
    imgui.PushStyleColor(COL.ScrollbarGrabActive, M.rgba(P.accent_lo))
    imgui.PushStyleVar(VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleVar(VAR.WindowBorderSize, 0)
    imgui.PushStyleVar(VAR.ChildBorderSize, 0)
    imgui.PushStyleVar(VAR.ItemSpacing, { 0, S(3) })
    imgui.PushStyleVar(VAR.ScrollbarSize, S(6))
    imgui.PushStyleVar(VAR.ScrollbarRounding, S(3))
    if imgui.Begin('##vana_help', nil, PANEL_FLAGS) then
        local dl = imgui.GetWindowDrawList()
        local radius = S(5)
        M.fill(dl, x, y, pw, ph, P.bg, radius)
        M.outline(dl, x, y, pw, ph, P.border, radius)
        local lx, right = x + S(18), x + pw - S(18)

        -- the title line: its icon and name, its kind and whether it's loaded, the close x
        local cy = y + S(TITLE_H) / 2
        M.icon_centred(dl, lx + S(5), cy, row.what == 'addon' and 'puzzle' or 'plug', open.on and P.accent or P.faint, 10.5)
        local tx = lx + S(18)
        local name = M.fit(row.label, (right - tx) * 0.55, fonts.semi, BODY + 2)
        M.text_mid(dl, tx, cy, name, P.white, fonts.semi, BODY + 2)
        tx = tx + M.measure(name, fonts.semi, BODY + 2) + S(10)
        local kind = KIND_WORDS[row.source] or row.source
        M.text_mid(dl, tx, cy + S(1), kind, P[KIND_COLOURS[row.source] or 'dim'], fonts.text, SMALL)
        tx = tx + M.measure(kind, fonts.text, SMALL) + S(10)
        M.circle(dl, tx + S(2), cy + S(1), S(2.5), open.on and P.teal or P.faint)
        M.text_mid(dl, tx + S(9), cy + S(1), open.on and 'loaded' or 'not loaded', open.on and P.teal or P.faint, fonts.text, SMALL)
        if M.icon_button('##help.close', right, cy, 'close', 10, P.rose) then
            panel = nil
        end
        M.hline(dl, x, x + pw, y + S(TITLE_H), P.border)

        -- the views, when there's more than About
        local views = { { 'about', 'About' } }
        local views_h = 0
        if #views > 1 then
            views_h = S(VIEWS_H)
            local labels, index = {}, 1
            for i, v in ipairs(views) do
                labels[i] = v[2]
                if v[1] == open.view then
                    index = i
                end
            end
            local vy = y + S(TITLE_H) + S(VIEWS_H) / 2 + S(4)
            local picked, moved = M.segments('##help.views', lx, vy, labels, index, true)
            if moved then
                open.view = views[picked][1]
            end
        else
            open.view = 'about'
        end

        -- the text
        local lines, details = {}, nil
        if open.view == 'about' then
            lines, details = open.about, open.details
        end
        local top = y + S(TITLE_H) + views_h + S(14)
        local bottom = y + ph - S(6)
        local cw = pw - (lx - x) - S(8)
        imgui.SetCursorScreenPos({ lx, top })
        if imgui.BeginChild('##help.text.' .. open.key .. '.' .. open.view, { cw, bottom - top - S(6) }, 0, 65536) then
            draw_text(lines, details ~= nil and open or nil, cw)
            imgui.Dummy({ 1, S(6) })
        end
        imgui.EndChild()
    end
    imgui.End()
    imgui.PopStyleVar(6)
    imgui.PopStyleColor(5)
end

-- /vana help <name>: the row of the addon (or plugin) of that name, for the window to open at
function M.help_command(arg)
    arg = arg ~= nil and arg:lower() or nil
    if arg == nil then
        print('/vana help <addon or plugin>: its details and README beside the window.')
        return nil
    end
    for _, what in ipairs({ 'addon', 'plugin' }) do
        for _, row in ipairs(list_rows(what)) do
            if row.kind == 'extension' and row.name:lower() == arg then
                return row
            end
        end
    end
    print('VanaCore: no addon or plugin is named ' .. arg .. '.')
    return nil
end
