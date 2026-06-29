-- mod-version:3
-- Custom interactive welcome plugin for Trowel.
-- Replaces the default empty view with a rebranded, high-fidelity splash screen
-- featuring keyboard shortcuts and clickable native links.

local core = require "core"
local style = require "core.style"
local EmptyView = require "core.emptyview"

-- Save the original EmptyView draw function in case of fallbacks
local orig_draw = EmptyView.draw

function EmptyView:draw()
  self:draw_background(style.background)
  
  local cx = self.position.x + self.size.x / 2
  local cy = self.position.y + self.size.y / 2
  
  -- Load custom fonts dynamically to adapt to High DPI SCALE changes
  local title_font = style.font:copy(36 * SCALE)
  local subtitle_font = style.font:copy(15 * SCALE)
  local text_font = style.font
  
  -- Draw title: "trowel"
  local title_text = "trowel"
  local title_w = title_font:get_width(title_text)
  local title_y = cy - 110 * SCALE
  renderer.draw_text(title_font, title_text, cx - title_w / 2, title_y, style.accent)
  
  -- Draw subheading: "the lightweight Turmeric IDE"
  local sub_text = "the lightweight Turmeric IDE"
  local sub_w = subtitle_font:get_width(sub_text)
  local sub_y = title_y + title_font:get_height() + 5 * SCALE
  renderer.draw_text(subtitle_font, sub_text, cx - sub_w / 2, sub_y, style.dim)
  
  -- Keyboard Shortcuts
  local is_mac = (PLATFORM == "Mac OS X")
  local cmd_key = is_mac and "Cmd" or "Ctrl"
  
  local shortcuts = {
    { label = "New Document", key = cmd_key .. "+N" },
    { label = "Open File / Project", key = cmd_key .. "+O" },
    { label = "Command Palette", key = cmd_key .. "+Shift+P" },
    { label = "Run Active File", key = cmd_key .. "+R" },
  }
  
  local label_spacing = 25 * SCALE
  local shortcut_y = sub_y + subtitle_font:get_height() + 30 * SCALE
  
  for _, s in ipairs(shortcuts) do
    local label_w = text_font:get_width(s.label)
    renderer.draw_text(text_font, s.label, cx - label_spacing - label_w, shortcut_y, style.dim)
    renderer.draw_text(text_font, s.key, cx + label_spacing, shortcut_y, style.text)
    shortcut_y = shortcut_y + text_font:get_height() + 8 * SCALE
  end
  
  -- Interactive Links
  if not self.custom_links then
    self.custom_links = {
      { text = "🌐 Website", url = "https://turmeric-lang.com" },
      { text = "💻 Source Code", url = "https://github.com/rjungemann/turmeric" }
    }
  end
  
  local link_spacing = 40 * SCALE
  local total_links_w = 0
  for i, link in ipairs(self.custom_links) do
    link.width = text_font:get_width(link.text)
    total_links_w = total_links_w + link.width
    if i > 1 then
      total_links_w = total_links_w + link_spacing
    end
  end
  
  local link_x = cx - total_links_w / 2
  local link_y = shortcut_y + 25 * SCALE
  
  for _, link in ipairs(self.custom_links) do
    link.x1 = link_x
    link.y1 = link_y
    link.x2 = link_x + link.width
    link.y2 = link_y + text_font:get_height()
    
    local is_hover = (self.hovered_link == link)
    local color = is_hover and style.accent or style.text
    
    renderer.draw_text(text_font, link.text, link_x, link_y, color)
    if is_hover then
      renderer.draw_rect(link_x, link_y + text_font:get_height() - 1, link.width, 1 * SCALE, style.accent)
    end
    
    link_x = link_x + link.width + link_spacing
  end
end

-- Override mouse moved
local orig_on_mouse_moved = EmptyView.on_mouse_moved
function EmptyView:on_mouse_moved(x, y, dx, dy)
  if orig_on_mouse_moved then
    orig_on_mouse_moved(self, x, y, dx, dy)
  end
  
  self.hovered_link = nil
  if self.custom_links then
    for _, link in ipairs(self.custom_links) do
      if link.x1 and x >= link.x1 and x <= link.x2 and y >= link.y1 and y <= link.y2 then
        self.hovered_link = link
        break
      end
    end
  end
  if self.hovered_link then
    system.set_cursor("hand")
  end
end

-- Override mouse pressed
local orig_on_mouse_pressed = EmptyView.on_mouse_pressed
function EmptyView:on_mouse_pressed(button, x, y, clicks)
  if button == "left" and self.hovered_link then
    local url = self.hovered_link.url
    local cmd
    if PLATFORM == "Windows" then
      cmd = { "cmd", "/c", "start", "", url }
    elseif PLATFORM == "Mac OS X" then
      cmd = { "open", url }
    else
      -- Linux/BSD
      cmd = { "xdg-open", url }
    end
    system.exec(cmd)
    return
  end
  if orig_on_mouse_pressed then
    orig_on_mouse_pressed(self, button, x, y, clicks)
  end
end
