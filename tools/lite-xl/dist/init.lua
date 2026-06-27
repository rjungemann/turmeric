-- init.lua -- shipped inside the Turmeric Studio bundle. Enables the
-- Turmeric plugin + selects the Turmeric color theme by default.
--
-- Per Phase 5 of turmeric-lite-xl-desktop-plan.md the bundle does not ship
-- `tur` itself; the user is expected to have it on PATH. The plugin
-- surfaces a clear error in the log pane otherwise.

local core   = require "core"
local config = require "core.config"

config.plugins.turmeric = config.plugins.turmeric or {}
-- Honor a PATH-resolved `tur` by default; user can override here.
config.plugins.turmeric.tur = config.plugins.turmeric.tur or "tur"

-- Pick the dark theme unless the user has already chosen one.
if not config._user_theme_set then
  core.try(function()
    core.reload_module("colors.turmeric-dark")
  end)
end
