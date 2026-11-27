--[[
example-hello: VanaCore's first addon. Shows a box that says hello world.

In game:
  /addon load example-hello      shows the box
  /hello <text>           changes what it says
  /addon unload example-hello    removes it
--]]

addon.name    = 'example-hello'
addon.author  = 'SoulspireXI / VanaCore SDK Example'
addon.version = '1.0'
addon.desc    = 'Shows a box that says hello world.'

local box

vana.events.on('load', function ()
    box = vana.ui.text({
        text       = 'Hello world!',
        x          = 200,
        y          = 200,
        font       = 'Segoe UI',
        size       = 24,
        bold       = true,
        color      = '#FFFFFF',
        background = '#1E1E2ECC',
        padding    = 14,
    })
    print('Hello world! Type /hello <text> to change the box.')
end)

vana.events.on('command', function (e)
    local text = e.command:match('^/hello%s+(.+)$')
    if text ~= nil then
        box.text = text
        return true -- handled: the game doesn't see it
    end
end)

vana.events.on('unload', function ()
    -- VanaCore removes an addon's boxes when it unloads; this just shows how.
    box:destroy()
end)
