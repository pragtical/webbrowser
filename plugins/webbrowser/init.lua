-- mod-version:3.14.0
-- Off-screen CEF browser rendered by Pragtical. @license MIT
local core = require "core"
local common = require "core.common"
local command = require "core.command"
local config = require "core.config"
local ContextMenu = require "core.contextmenu"
local Doc = require "core.doc"
local DocView = require "core.docview"
local ime = require "core.ime"
local keymap = require "core.keymap"
local style = require "core.style"
local Widget = require "widget"
local Button = require "widget.button"
local TextBox = require "widget.textbox"
local renderer = require "renderer"
local system = require "system"

config.plugins.webbrowser = common.merge({
  home = "https://pragtical.dev", runtime_path = "", disable_sandbox = false,
  preview_mode = "right",
  config_spec = {
    name = "Web Browser",
    {label="Home Page", path="home", type="string", default="https://pragtical.dev"},
    {label="Preview Mode", path="preview_mode", type="selection", default="right",
      description="Where to open HTML previews relative to their source document.",
      values={{"Right", "right"}, {"New Tab", "newtab"}, {"Bottom", "bottom"},
        {"Top", "top"}, {"Left", "left"}}},
    {label="Runtime Directory", path="runtime_path", type="string", default="",
      description="CEF runtime directory; empty uses the runtime bundled with the plugin. Restart to change."},
    {label="Disable Sandbox", path="disable_sandbox", type="toggle", default=false,
      description="Only for environments without Chromium sandbox support. Restart the browser host to change."}
  }
}, config.plugins.webbrowser)

local runtime = require "plugins.webbrowser.runtime"
---@class plugins.webbrowser.view : widget
local BrowserView = Widget:extend()
local previews = setmetatable({}, {__mode="k"})
local webbrowser
local buttons = {left=0, middle=1, right=2}
local cursors = {arrow=true, ibeam=true, hand=true, sizeh=true, sizev=true}
local masks = {left=16, middle=32, right=64}
local modifiers = require("core.modkeys-" .. (PLATFORM == "Mac OS X" and "macos" or "generic")).map
local function flags(view)
  local mods = keymap.modkeys
  local value = (mods.shift and 2 or 0) + (mods.ctrl and 4 or 0)
    + ((mods.alt or mods.altgr or mods.option) and 8 or 0) + ((mods.cmd or mods.super) and 128 or 0)
  for button in pairs(view.pressed) do value = value + (masks[button] or 0) end
  return value
end

local html_extensions = {html=true, htm=true, xhtml=true}
local function is_html(filename)
  return html_extensions[(filename:match("%.([^./\\]+)$") or ""):lower()]
end

local function file_url(path)
  path = path:gsub("\\", "/")
  if path:sub(1,1) ~= "/" then path = "/" .. path end
  return "file://" .. path:gsub("[^%w/%.~_:%-]", function(c) return string.format("%%%02X", c:byte()) end)
end

local function normalize_url(url)
  url = (url or ""):match("^%s*(.-)%s*$")
  if url == "" then return "about:blank" end
  if url:match("^[%a][%w+.-]*:") and not url:match("^%a:[/\\]") then return url end
  local path = common.home_expand(url)
  if system.get_file_info(path) then
    return file_url(system.absolute_path(path))
  end
  return "https://" .. url
end

---@class plugins.webbrowser.options
---@field controls? boolean Show toolbar and status; HTML views default to false
---@field html? string Initial UTF-8 HTML, instead of a website
---@field base_url? string Document URL for resolving relative HTML resources
---@field name? string Fixed view/tab name; otherwise follows the document title
---@field parent? widget Optional widget to embed the browser in
---@field floating? boolean Create a floating view instead of an unattached node view
---@field on_change? fun(view:plugins.webbrowser.view, state:table) Metadata change callback

---@param url? string Website or local file URL
---@param options? plugins.webbrowser.options
function BrowserView:new(url, options)
  options = options or {}
  BrowserView.super.new(self, options.parent, options.floating or false)
  self.devtools_owner = options._devtools_owner
  self.devtools_position, self.devtools_sizes = "right", {right=0.4,bottom=0.4}
  self.fixed_name = options.name
  self.name, self.pressed = options.name or "Web Browser", {}
  self.on_change = options.on_change or self.on_change
  self.controls_visible = true
  self.border.width, self.scrollable = 0, false
  self.controls = {}
  local function button(label, tooltip, action)
    local item = Button(self, label == "Go" and label or "")
    if label ~= "Go" then item:set_icon(label) end
    item:set_tooltip(tooltip)
    item.on_click = function(_, which) if which == "left" then action() end end
    self.controls[#self.controls+1] = item
    return item
  end
  button("<", "Back (Alt+Left)", function() self:send("back") end)
  button(">", "Forward (Alt+Right)", function() self:send("forward") end)
  button("↻", "Reload (Ctrl+R)", function() self:send("reload") end)
  button("C", "Stop loading", function() self:send("stop") end)
  self.address = TextBox(self, url or config.plugins.webbrowser.home, "URL", {clearable=true})
  self.go = button("Go", "Navigate", function() self:navigate(self.address:get_text()) end)
  if self.devtools_owner then
    self.dock_button = Button(self, "Dock below")
    self.dock_button.on_click = function(_, which)
      if which == "left" then
        local owner = self.devtools_owner
        owner:set_devtools_position(owner.devtools_position == "right" and "bottom" or "right")
      end
    end
    self.close_button = Button(self, "")
    self.close_button:set_icon("C")
    self.close_button:set_tooltip("Close Developer Tools")
    self.close_button.on_click = function(_, which)
      if which == "left" then self.devtools_owner:hide_devtools() end
    end
  end
  self:set_controls_visible(options.controls ~= false and (options.html == nil or options.controls == true))
  self:show()
  self:start(options.html ~= nil and "about:blank" or normalize_url(url or config.plugins.webbrowser.home),
    self.devtools_owner and {owner=self.devtools_owner.browser,x=options._inspect_x,y=options._inspect_y})
  if options.html ~= nil and self.browser and not self:load_html(options.html, options.base_url) then
    self.error = "Cannot queue HTML document: the encoded command must fit within the 1 MiB input queue"
    core.error("Web Browser: %s", self.error)
  end
end

function BrowserView:send(kind, data)
  if self.browser then return self.browser:send(kind, data) end
end

function BrowserView:start(url, options)
  local ok, browser = pcall(runtime.Browser.new, url, options)
  if not ok then self.error = tostring(browser); core.error("Web Browser: %s", self.error); return end
  self.browser, self.error = browser, nil
  self.shape, self.last_revision = nil, nil
  -- Weak ownership lets a view removed by a plugin be collected too.
  local owner = setmetatable({self}, {__mode="v"})
  browser.on_tick = function(b)
    local view = owner[1]
    if not view then b:close(); return end
    local root = view
    while root.parent do root = root.parent end
    local node = core.root_view.root_node:get_node_for_view(root)
    local visible = core.window_mode ~= "minimized" and view:is_visible()
      and (node and node.active_view == root or root.floating_visible or false)
      and view.size.x > 0 and view.size.y > 0
    if visible ~= b.visible then b.visible = visible; b:send("visible", {value=visible}) end
    local focused = visible and core.active_view == view
    if focused ~= b.focused then b.focused = focused; b:send("focus", {value=focused}) end
  end
  browser.on_change = function(b)
    local view = owner[1]
    if not view then return end
    view.name = view.fixed_name or (b.title ~= "" and b.title or "Web Browser")
    if core.active_view ~= view.address.textview and b.url ~= "" and view.address:get_text() ~= b.url then
      view.address:set_text(b.url)
    end
    if view.last_revision ~= b.revision then
      view.last_revision = b.revision
      core.try(view.on_change, view, view:get_state())
    end
    if b.visible then
      core.redraw = true
      if core.active_view == view and view.page_top and b.caret and b.caret.h > 0 then
        local r = b.caret
        ime.set_location(view.position.x+r.x, view.position.y+view.page_top+r.y, r.w, r.h)
      end
    end
  end
  browser.on_context = function(context)
    local view = owner[1]
    if view then view:show_context_menu(context) end
  end
end

function BrowserView:show_context_menu(context)
  if context and self.pressed.right then self.pending_context = context; return end
  self.pending_context = nil
  if self.context_menu then self.context_menu:hide() end
  if not context or not self.browser or not self.browser.visible then return end
  local menu, browser = ContextMenu(), self.browser
  self.context_menu = menu
  local base_selected, base_visibility = menu.call_selected_item, menu.on_floating_visibility_changed
  function menu:call_selected_item(item)
    self.dispatching = true
    base_selected(self, item)
    self.dispatching = false
  end
  function menu:on_floating_visibility_changed(visible)
    base_visibility(self, visible)
    if not visible and not self.dispatching then browser:send("context-cancel", {token=context.token}) end
  end
  local items = {}
  local function add(text, action, enabled)
    items[#items+1] = {text=text,command=action,enabled=enabled ~= false}
  end
  local function cancel() browser:send("context-cancel", {token=context.token}) end
  local function copy(cut) browser:send("copy", {context=context.token,cut=cut}) end
  local function edit(action)
    self:focus_page()
    browser:send("context-action", {token=context.token,action=action,
      text=action == "paste" and system.get_clipboard() or nil})
  end
  if not self.devtools_owner then
    add("Back", function() cancel(); self:send("back") end, browser.can_back)
    add("Forward", function() cancel(); self:send("forward") end, browser.can_forward)
    add("Reload", function() cancel(); self:send("reload") end)
    items[#items+1] = ContextMenu.DIVIDER
  end
  if context.editable then add("Cut", function() copy(true) end, context.cut) end
  if context.copy or context.editable then add("Copy", function() copy(false) end, context.copy) end
  if context.editable then add("Paste", function() edit("paste") end, system.get_clipboard() ~= "") end
  add("Select All", function() edit("select_all") end, context.select_all)
  if context.link ~= "" then
    items[#items+1] = ContextMenu.DIVIDER
    add("Open Link in New Tab", function() cancel(); webbrowser.open(context.link) end)
    add("Copy Link Address", function() system.set_clipboard(context.link); cancel() end)
  end
  if context.image ~= "" then
    items[#items+1] = ContextMenu.DIVIDER
    add("Copy Image Address", function() system.set_clipboard(context.image); cancel() end)
  end
  if not self.devtools_owner then
    items[#items+1] = ContextMenu.DIVIDER
    add("Inspect Element", function() self:show_devtools(context.x,context.y); cancel() end)
  end
  menu:show_items(items, self.position.x+context.x, self.position.y+self.page_top+context.y,
    {owner=self,disabled=true})
  -- Keep disabled entries without treating the browser as part of an application menu.
  menu:set_floating(true, {dismiss_on_owner_click=true})
end

---Open the inspector; optional coordinates are physical pixels relative to the HTML viewport.
---@param x? number
---@param y? number
---@return boolean accepted
function BrowserView:show_devtools(x, y)
  if self.devtools_owner then return self.devtools_owner:show_devtools(x,y) end
  if not self.browser or self.browser.failed then return false end
  if not self.devtools then
    self.devtools = BrowserView("about:blank", {parent=self,controls=false,name="Developer Tools",
      _devtools_owner=self,_inspect_x=x,_inspect_y=y})
  elseif x and y then self:send("inspect", {x=x,y=y}) end
  self:layout()
  self.devtools:focus_page()
  self:schedule_update()
  return true
end

function BrowserView:hide_devtools()
  if self.devtools_owner then return self.devtools_owner:hide_devtools() end
  local child = self.devtools
  if not child then return end
  self.devtools = nil
  local focus = core.active_view
  while focus and focus ~= child do focus = focus.parent end
  if focus then self:focus_page() end
  child:close()
  self:remove_child(child)
  self:layout()
  self:schedule_update()
end

function BrowserView:toggle_devtools()
  if self.devtools_owner then return self.devtools_owner:toggle_devtools() end
  if self.devtools then self:hide_devtools() else self:show_devtools() end
end

---@param position "right"|"bottom"
function BrowserView:set_devtools_position(position)
  assert(position == "right" or position == "bottom", "DevTools position must be right or bottom")
  if self.devtools_owner then return self.devtools_owner:set_devtools_position(position) end
  self.devtools_position = position
  self:layout()
  self:schedule_update()
end

function BrowserView:navigate(url)
  url = normalize_url(url)
  self.address:set_text(url)
  if not self.browser or self.browser.failed then
    self:hide_devtools()
    if self.browser then self.browser:close() end
    self:start(url)
  else self:send("navigate", {url=url}) end
  self:focus_page()
end

---Load HTML directly from memory. base_url is its document URL, not a request to download it.
---Relative CSS/images resolve from that URL; local file URLs are supported.
---@param html string UTF-8 HTML document
---@param base_url? string Defaults to a private per-view URL
---@return boolean accepted False if the browser cannot accept another command
function BrowserView:load_html(html, base_url)
  assert(type(html) == "string", "HTML content must be a string")
  assert(base_url == nil or type(base_url) == "string", "base_url must be a string")
  return self:send("html", {html=html, base_url=base_url and normalize_url(base_url) or ""}) or false
end

---Run JavaScript in the main frame, after a pending document load completes.
---@param code string JavaScript source; execution is asynchronous and returns no JS value
---@return boolean accepted
function BrowserView:execute_script(code)
  assert(type(code) == "string", "JavaScript source must be a string")
  return self:send("script", {code=code}) or false
end

---Copy the focused frame's selection through the editor's native clipboard.
---@return boolean accepted
function BrowserView:copy_selection()
  return self:send("copy") or false
end

---Snapshot of the browser metadata. Does not expose native handles or mutable state.
function BrowserView:get_state()
  local b = self.browser
  return {
    url=b and b.url or "", title=b and b.title or self.name,
    loading=b and b.loading or false, ready=b and b.state[0].status == 1 or false,
    can_back=b and b.can_back or false, can_forward=b and b.can_forward or false,
    error=self.error or (b and b.error) or ""
  }
end

---Override in a subclass or provide options.on_change to observe metadata changes.
function BrowserView:on_change(state) end

---Show or hide all browser chrome, returning its space to the HTML viewport.
---@param visible boolean
function BrowserView:set_controls_visible(visible)
  assert(type(visible) == "boolean", "Control visibility must be a boolean")
  if self.controls_visible == visible then return end
  if not visible and core.active_view == self.address.textview then self:focus_page() end
  self.controls_visible = visible
  for _, item in ipairs(self.controls) do
    if visible then item:show() else item:hide() end
  end
  if visible then self.address:show() else self.address:hide() end
  self:layout()
  self:schedule_update()
end

function BrowserView:toggle_controls()
  self:set_controls_visible(not self.controls_visible)
end

function BrowserView:focus_page()
  self:swap_active_child()
  core.set_active_view(self)
  self:send("focus", {value=true})
end

function BrowserView:focus_url()
  self:set_controls_visible(true)
  self:send("focus", {value=false})
  self:swap_active_child(self.address)
  self.address.textview.doc:set_selection(1, 1, 1, #self.address:get_text()+1)
end

function BrowserView:layout()
  if self.controls_visible then
    local gap, height = math.ceil(style.padding.x / 2), self.address:get_height()
    local x = gap
    for i=1,4 do
      local item = self.controls[i]
      item:set_size(height, height)
      item.padding.x = math.floor((height - style.icon_font:get_width(item.icon.code)) / 2)
      item.padding.y = math.floor((height - style.icon_font:get_height()) / 2)
      item:set_position(x, gap)
      x = x + height + gap
    end
    local go_width = self.go:get_width()
    self.address:set_size(math.max(1, self.size.x - x - go_width - gap*2))
    self.address:set_position(x, gap)
    self.go:set_position(self.size.x - go_width - gap, gap)
    self.page_top = height + gap*2
    self.status_height = math.ceil(style.font:get_height() + gap)
  else
    self.page_top, self.status_height = 0, 0
  end
  if self.devtools_owner then
    local height = math.ceil(style.font:get_height()+style.padding.y*2)
    self.page_top = height
    self.close_button:set_size(height,height)
    self.close_button:set_position(math.max(0,self.size.x-height),0)
    local label = self.devtools_owner.devtools_position == "right" and "Dock below" or "Dock right"
    if self.dock_button.label ~= label then self.dock_button:set_label(label) end
    self.dock_button:set_position(math.max(0,self.size.x-height-self.dock_button:get_width()),0)
  end
  self.page_width = math.max(1, math.floor(self.size.x))
  self.page_height = math.max(1, math.floor(self.size.y - self.page_top - self.status_height))
  self.divider = nil
  if self.devtools then
    local child, gap = self.devtools, math.max(4,math.ceil(4*SCALE))
    local right = self.devtools_position == "right"
    local available = math.max(2,(right and self.page_width or self.page_height)-gap)
    local minimum = math.min(120*SCALE,available/4)
    local extent = math.floor(common.clamp(available*self.devtools_sizes[self.devtools_position],minimum,available-minimum))
    if right then
      self.page_width = math.max(1,self.page_width-extent-gap)
      self.divider = {x=self.position.x+self.page_width,y=self.position.y+self.page_top,w=gap,h=self.page_height}
      child:set_size(extent,self.page_height)
      child:set_position(self.page_width+gap,self.page_top)
    else
      self.page_height = math.max(1,self.page_height-extent-gap)
      self.divider = {x=self.position.x,y=self.position.y+self.page_top+self.page_height,w=self.page_width,h=gap}
      child:set_size(self.page_width,extent)
      child:set_position(0,self.page_top+self.page_height+gap)
    end
    child:layout()
  end
  local shape = self.page_width .. ":" .. self.page_height .. ":" .. SCALE
  if self.shape ~= shape then
    self.shape = shape
    self:send("resize", {width=self.page_width, height=self.page_height, scale=SCALE})
  end
end

function BrowserView:update()
  if not BrowserView.super.update(self) then return end
  self:layout()
  local b = self.browser
  if self.devtools and self.devtools.browser and self.devtools.browser.state[0].status == 2 then self:hide_devtools() end
  self.cursor = (self.dragging_divider or self:over_divider(self.mouse.x,self.mouse.y))
    and (self.devtools_position == "right" and "sizeh" or "sizev")
    or self:over_page(self.mouse.x, self.mouse.y) and b and cursors[b.cursor] and b.cursor or "arrow"
end

function BrowserView:draw_background(color)
  self:layout()
  local x, y = math.floor(self.position.x), math.floor(self.position.y)
  renderer.draw_rect(x, y, self.size.x, self.size.y, color)
  if self.divider then
    local d = self.divider
    renderer.draw_rect(d.x,d.y,d.w,d.h,style.divider)
  end
  if self.devtools_owner then
    renderer.draw_text(style.font,"Developer Tools",x+style.padding.x,y+style.padding.y,style.text)
  end
  renderer.draw_rect(x, y+self.page_top, self.page_width, self.page_height, style.background)
  local browser = self.browser
  for index, enabled in pairs({[1]=browser and browser.can_back or false,
    [2]=browser and browser.can_forward or false, [4]=browser and browser.loading or false}) do
    self.controls[index].icon.color = enabled and style.text or style.dim
    self.controls[index].icon.hover_color = enabled and style.accent or style.dim
  end
  core.push_clip_rect(x, y+self.page_top, self.page_width, self.page_height)
  if browser then
    browser:read_frame()
    if browser.pixels then renderer.draw_pixels(browser.pixels, x, y+self.page_top, browser.width, browser.height) end
  end
  local error_text = self.error or (browser and browser.error)
  if error_text and error_text ~= "" then
    renderer.draw_rect(x, y+self.page_top, self.page_width, self.page_height, style.background)
    self:draw_text_multiline(style.font, error_text, x+style.padding.x, y+self.page_top+style.padding.y, style.text)
  elseif browser and not browser.pixels then
    renderer.draw_text(style.font, "Starting browser…", x+style.padding.x, y+self.page_top+style.padding.y, style.text)
  end
  core.pop_clip_rect()
  if self.controls_visible then
    core.push_clip_rect(x, y+self.size.y-self.status_height, self.size.x, self.status_height)
    renderer.draw_text(style.font, browser and (browser.loading and "Loading… " or "") .. browser.url or "Web Browser",
      x+style.padding.x, y+self.size.y-self.status_height, style.dim)
    core.pop_clip_rect()
  end
end

function BrowserView:over_page(x, y)
  return x and y and self.page_top and x >= self.position.x and x < self.position.x+self.page_width
    and y >= self.position.y+self.page_top and y < self.position.y+self.page_top+self.page_height
end
function BrowserView:mouse_event(kind, x, y, data)
  data = data or {}
  data.x, data.y = math.floor(x-self.position.x), math.floor(y-self.position.y-self.page_top)
  data.modifiers = flags(self)
  self:send(kind, data)
end
function BrowserView:over_divider(x,y)
  local d = self.divider
  return d and x and y and x >= d.x and y >= d.y and x < d.x+d.w and y < d.y+d.h
end
function BrowserView:on_mouse_pressed(button, x, y, clicks)
  if button == "left" and self:over_divider(x,y) then
    self.dragging_divider = true
    self:capture_mouse(false,button)
    return true
  end
  if self:over_page(x, y) and buttons[button] then
    self:focus_page(); self.pressed[button] = true; self:capture_mouse(false, button)
    self:mouse_event("button", x, y, {button=buttons[button], clicks=clicks or 1, up=false})
    return true
  end
  self:send("focus", {value=false})
  return BrowserView.super.on_mouse_pressed(self, button, x, y, clicks)
end
function BrowserView:on_mouse_released(button, x, y)
  if self.dragging_divider and button == "left" then
    self.dragging_divider = false
    self:release_mouse(true)
    return true
  end
  if self.pressed[button] then
    self.pressed[button] = nil
    self:mouse_event("button", x, y, {button=buttons[button], clicks=1, up=true})
    self:release_mouse(true)
    if button == "right" and self.pending_context then self:show_context_menu(self.pending_context) end
    return true
  end
  return BrowserView.super.on_mouse_released(self, button, x, y)
end
function BrowserView:on_mouse_capture_lost()
  self.dragging_divider = false
  if self.pending_context then
    self:send("context-cancel", {token=self.pending_context.token})
    self.pending_context = nil
  end
  for button in pairs(self.pressed) do
    self.pressed[button] = nil
    self:mouse_event("button", self.mouse.x, self.mouse.y, {button=buttons[button], clicks=1, up=true})
  end
  BrowserView.super.on_mouse_capture_lost(self)
end
function BrowserView:on_mouse_moved(x, y, dx, dy)
  if self.dragging_divider then
    local right = self.devtools_position == "right"
    local extent = right and self.size.x or self.size.y-self.page_top-self.status_height
    local at = right and x-self.position.x or y-self.position.y-self.page_top
    self.devtools_sizes[self.devtools_position] = common.clamp(1-at/math.max(1,extent),0.15,0.85)
    self:layout()
    core.request_cursor(right and "sizeh" or "sizev")
    core.redraw = true
    return true
  end
  BrowserView.super.on_mouse_moved(self, x, y, dx, dy)
  local over = self:over_page(x, y)
  if over or self.over_browser or next(self.pressed) then self:mouse_event("move", x, y, {leave=not over}) end
  self.over_browser = over
end
function BrowserView:on_mouse_wheel(y, x)
  if self:over_page(self.mouse.x, self.mouse.y) then
    self:mouse_event("wheel", self.mouse.x, self.mouse.y, {dx=math.floor((x or 0)*120), dy=math.floor(y*120)})
    return true
  end
  return BrowserView.super.on_mouse_wheel(self, y, x)
end
function BrowserView:supports_text_input() return true end
function BrowserView:on_text_input(text)
  self:send(self.composing and "commit" or "text", {text=text, modifiers=flags(self)})
  self.composing = false
  return true
end
local function utf16_length(text)
  local length = 0
  for char in text:ugmatch(".") do length = length + (#char == 4 and 2 or 1) end
  return length
end
function BrowserView:on_ime_text_editing(text, start, length)
  self.composing = text ~= ""
  self:send("composition", {text=text, start=utf16_length(text:sub(1, start)),
    length=utf16_length(text:sub(start+1, start+length))})
end
---Release browser resources; the caller still owns the view's placement.
function BrowserView:close()
  previews[self] = nil
  self:release_mouse()
  self:hide_devtools()
  if self.context_menu then self.context_menu:hide(); self.context_menu = nil end
  if self.browser then self.browser:close(); self.browser = nil end
end
function BrowserView:try_close(do_close)
  self:close()
  do_close()
end

webbrowser = {BrowserView=BrowserView, normalize_url=normalize_url}
---@param url? string Website or local file to open
---@param options? plugins.webbrowser.options
---@return plugins.webbrowser.view
function webbrowser.open(url, options)
  assert(not options or not options.parent and not options.floating,
    "Use the BrowserView constructor for embedded or floating views")
  local view = BrowserView(url, options)
  core.root_view:get_active_node_default():add_view(view)
  return view
end
---Open an HTML document in a new tab; controls are hidden by default.
---@param html string UTF-8 HTML source
---@param options? plugins.webbrowser.options
---@return plugins.webbrowser.view
function webbrowser.open_html(html, options)
  local settings = {}
  for key, value in pairs(options or {}) do settings[key] = value end
  settings.html = html
  return webbrowser.open(nil, settings)
end
local function current()
  local view = core.active_view
  while view do
    if view:extends(BrowserView) then return view end
    view = view.parent
  end
end
command.add(nil, {
  ["web-browser:open"] = function(url, options) webbrowser.open(url, options) end,
  ["web-browser:open-app"] = function(url)
    if url then webbrowser.open(url, {controls=false}); return end
    core.command_view:enter("Open Website as App", {
      text=config.plugins.webbrowser.home, select_text=true,
      submit=function(url) if url:match("%S") then webbrowser.open(url, {controls=false}) end end
    })
  end
})
local function html_document()
  local view = core.active_view
  if not view:extends(DocView) then return false end
  local doc = view.doc
  return is_html(doc:get_name()) or doc.syntax and doc.syntax.name == "HTML", doc
end
local function preview_options(doc)
  return {
    name="Preview: " .. common.basename(doc:get_name()),
    base_url=file_url(doc.abs_filename or core.project_absolute_path(doc:get_name()))
  }
end
local function refresh_previews(doc)
  local html, options
  for view, source in pairs(previews) do
    if source == doc and view.browser then
      html = html or doc:get_text(1, 1, math.huge, math.huge)
      options = options or preview_options(doc)
      if view:load_html(html, options.base_url) then
        view.fixed_name, view.name = options.name, options.name
      else
        core.warn("Web Browser: could not queue HTML preview refresh for %s", doc:get_name())
      end
    end
  end
end
local doc_save = Doc.save
function Doc:save(...)
  local result = table.pack(doc_save(self, ...))
  if not self.new_file then core.try(refresh_previews, self) end
  return table.unpack(result)
end
command.add(html_document, {
  ["web-browser:preview-html"] = function(doc)
    local node = core.root_view.root_node:get_node_for_view(core.active_view)
      or core.root_view:get_active_node_default()
    local options = preview_options(doc)
    options.html = doc:get_text(1, 1, math.huge, math.huge)
    local view = BrowserView(nil, options)
    local mode = config.plugins.webbrowser.preview_mode
    if mode == "newtab" then
      node:add_view(view)
    else
      local directions = {right="right", left="left", top="up", bottom="down"}
      node:split(directions[mode] or "right", view)
    end
    core.root_view.root_node:update_layout()
    previews[view] = doc
  end
})
if config.plugins.contextmenu ~= false then
  local menu = require "plugins.contextmenu"
  menu:register(html_document, {
    menu.DIVIDER,
    {text="Preview HTML", command="web-browser:preview-html"}
  })
end
command.add(function() local v=current(); return v ~= nil and core.active_view == v, v end, {
  ["web-browser:copy-selection"] = function(v) v:copy_selection() end,
})
command.add(function() local v=current(); v=v and (v.devtools_owner or v); return v ~= nil, v end, {
  ["web-browser:focus-url"] = function(v) v:focus_url() end,
  ["web-browser:navigate"] = function(v) v:navigate(v.address:get_text()) end,
  ["web-browser:back"] = function(v) v:send("back") end,
  ["web-browser:forward"] = function(v) v:send("forward") end,
  ["web-browser:reload"] = function(v) v:send("reload") end,
  ["web-browser:stop"] = function(v) v:send("stop") end,
  ["web-browser:toggle-controls"] = function(v) v:toggle_controls() end,
  ["web-browser:toggle-devtools"] = function(v) v:toggle_devtools() end,
  ["web-browser:devtools-dock-right"] = function(v) (v.devtools_owner or v):set_devtools_position("right") end,
  ["web-browser:devtools-dock-bottom"] = function(v) (v.devtools_owner or v):set_devtools_position("bottom") end,
})
keymap.add {
  ["ctrl+c"]="web-browser:copy-selection", ["cmd+c"]="web-browser:copy-selection",
  ["ctrl+l"]="web-browser:focus-url", ["alt+left"]="web-browser:back",
  ["alt+right"]="web-browser:forward", ["ctrl+r"]="web-browser:reload",
  ["f12"]="web-browser:toggle-devtools"
}
if core.menu_view then core.menu_view:register({"tools"}, {
  {id="web-browser", text="Web Browser", items={
    {id="open", text="Open Browser", command="web-browser:open"},
    {id="open-app", text="Open Website as App…", command="web-browser:open-app"},
    {id="preview-html", text="Preview HTML", command="web-browser:preview-html"},
    ContextMenu.DIVIDER,
    {id="controls", text="Web Browser Controls", command="web-browser:toggle-controls",
      checked=function() local view=current(); return view and view.controls_visible or false end},
    {id="devtools", text="Web Browser Developer Tools", command="web-browser:toggle-devtools",
      checked=function() local view=current(); return view and (view.devtools_owner or view).devtools ~= nil or false end}
  }}
}, "webbrowser") end

if config.plugins.treeview ~= false then
  local treeview = require "plugins.treeview"
  local function html_item()
    local item = treeview:get_context_item()
    return item and item.type == "file"
      and is_html(item.abs_filename)
      and (core.active_view == treeview or treeview.contextmenu.show_context_menu), item
  end
  treeview.contextmenu:register(html_item, {
    {text="Open with Web Browser", command="treeview:open-with-web-browser"}
  })
  command.add(html_item, {
    ["treeview:open-with-web-browser"] = function(item) webbrowser.open(item.abs_filename) end
  })
end

local keys = {backspace=8, tab=9, ["return"]=13, enter=13, escape=27, space=32,
  pageup=33, pagedown=34, ["end"]=35, home=36, left=37, up=38, right=39, down=40,
  [";"]=186, ["="]=187, [","]=188, ["-"]=189, ["."]=190, ["/"]=191,
  ["`"]=192, ["["]=219, ["\\"]=220, ["]"]=221, ["'"]=222,
  insert=45, delete=46, ["left shift"]=16, ["right shift"]=16,
  ["left ctrl"]=17, ["right ctrl"]=17, ["left alt"]=18, ["right alt"]=18}
local on_event = core.on_event
core.on_event = function(event, key, ...)
  local view = current()
  if view and core.active_view == view.address.textview and event == "keypressed" and key == "return" then
    view:navigate(view.address:get_text()); return true
  end
  if view and core.active_view == view then
    if event == "keypressed" or event == "keyreleased" then
      if event == "keypressed" and not modifiers[key] then
        if ime.editing then return on_event(event, key, ...) end
        local stroke = {}
        for _, name in ipairs({"ctrl", "alt", "option", "altgr", "shift", "cmd", "super"}) do
          if keymap.modkeys[name] then stroke[#stroke+1] = name end
        end
        stroke[#stroke+1] = key
        if core.root_view:on_key_pressed(table.concat(stroke, "+"), ...) then return true end
      end
      if modifiers[key] then
        if event == "keypressed" then keymap.on_key_pressed(key) else keymap.on_key_released(key) end
      else
        local primary = keymap.modkeys.ctrl or keymap.modkeys.cmd
        if event == "keypressed" and (key == "f12" or (primary and (key == "l" or key == "r" or key == "w" or key == "p" or key == "c"))
          or (keymap.modkeys.alt and (key == "left" or key == "right"))) then
          return on_event(event, key, ...)
        end
      end
      local code = keys[key] or (#key == 1 and key:upper():byte())
        or (key:match("^f%d+$") and 111+tonumber(key:sub(2)))
      if code then
        view:send("key", {key=code, native=0, modifiers=flags(view), up=event == "keyreleased"})
        if event == "keypressed" and (code == 13 or code == 9 or code == 8) then
          view:send("text", {text=string.char(code), modifiers=flags(view)})
        end
      end
      -- core.step interprets true as a command that should suppress the next
      -- SDL textinput event. Forwarding a printable key must preserve that event.
      return event == "keypressed" and (code == 13 or code == 9 or code == 8) or false
    elseif event == "focuslost" then view:send("focus", {value=false})
    elseif event == "focusgained" then view:send("focus", {value=true}) end
  end
  if event == "quit" then runtime.shutdown() end
  return on_event(event, key, ...)
end
return webbrowser
