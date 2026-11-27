--[[
example-minparty (MOCKUP, docs/addons/07-game-interface.md): an example of vana.gameui. A small party list of its own in place of the
game's, which it hides; the Target window moved to sit above it; a button in the game's Config menu (Config > Party)
that shows or hides it.

What it changes in the game's interface:
  - hides the game's party list and alliance lists (its own list is drawn in their corner)
  - moves the Target window to just above its list
  - adds a "Party" button to the game's Config menu
All of it is put back when it's unloaded.

In game:
  /addon load example-minparty
  /minparty         shows or hides its list (as Config > Party does)
--]]

addon.name    = 'example-minparty'
addon.author  = 'SoulspireXI / VanaCore SDK Example'
addon.version = '0.1'
addon.desc    = 'A smaller party list in place of the game\'s (an example of vana.gameui).'

local box
local shown = true

local function toggle()
    shown = not shown
    if shown then box:show() else box:hide() end
end

-- What it changes in the game's interface, once, at load. A move returns false, 'held by <addon>' while another addon
-- has the Target window placed, and takes effect when that one lets go.
local function claim()
    vana.gameui.hide('party_list')
    vana.gameui.hide('alliance_lists')
    local moved, why = vana.gameui.move('target_window', 88, 70) -- its middle 88% across, its top 70% down: above the list
    vana.gameui.add('config_menu', 'Party', toggle)
    if not moved then
        vana.log('minparty: the Target window stays where it is (' .. tostring(why) .. ')')
    end
end

-- Where the game's party list was: the bottom-right corner, the box's right edge and bottom a margin in from the screen's,
-- its text sized for the screen (13 at 1080 tall). A text box's top is its y, so its height is worked out from its lines.
-- Set only when it changes: a new size draws the text again.
local placed = ''
local function place(lines)
    local sw, sh = vana.ui.screen()
    if not sw or sw <= 0 or not sh or sh <= 0 then
        sw, sh = 1920, 1080
    end
    local scale = sh / 1080
    local size = math.floor(13 * scale + 0.5)
    local padding = math.floor(8 * scale + 0.5)
    local margin = math.floor(16 * scale + 0.5)
    local height = lines * math.ceil(size * 1.35) + 2 * padding
    local key = sw .. 'x' .. sh .. '/' .. lines
    if key ~= placed then
        placed = key
        box.size, box.padding = size, padding
        box.x, box.y = sw - margin, sh - margin - height
    end
end

local function draw()
    local party = vana.game.party()
    local rows = {}
    for slot = 1, 6 do
        local member = party and party.members[slot]
        if member and member.active then
            rows[#rows + 1] = string.format('%-12s %3d%%  %4d TP', member.name, member.hpp, member.tp)
        end
    end
    box.text = #rows > 0 and table.concat(rows, '\n') or '(no party)'
    place(math.max(#rows, 1))
end

vana.events.on('load', function ()
    box = vana.ui.text({ text = '', x = 0, y = 0, right = true, font = 'Consolas', size = 13,
                         color = '#FFFFFF', background = '#1E1E2ECC', padding = 8 })
    claim()
end)

vana.events.on('present', draw)

vana.events.on('command', function (e)
    if e.command:lower():match('^/minparty') then
        toggle()
        return true
    end
end)

-- Nothing to undo here: VanaCore puts the party list, the Target window and the Config menu back as the addon goes
vana.events.on('unload', function ()
    if box ~= nil then
        box:destroy()
    end
end)
