-- One CEF service per editor. CEF callbacks never enter Lua.
local core = require "core"
local common = require "core.common"
local config = require "core.config"
local json = require "core.json"
local process = require "process"
local system = require "system"
local ffi = require "ffi"

ffi.cdef [[
typedef struct {
  uint64_t revision, frame, context_revision;
  uint32_t width, height, status, loading, can_back, can_forward;
  int32_t caret_x, caret_y, caret_width, caret_height;
  char url[4096], title[512], error[1024], cursor[16];
} wb_state;
unsigned wb_abi_version(void);
unsigned wb_state_size(void);
const char *wb_error(void);
void *wb_create(const char *path);
int wb_poll(void *handle, wb_state *state);
const void *wb_lock_frame(void *handle, uint64_t last_frame, wb_state *state);
const char *wb_lock_clipboard(void *handle, uint64_t last, uint32_t *size, uint64_t *revision);
const char *wb_lock_context(void *handle, uint64_t last, uint32_t *size, uint64_t *revision);
void wb_unlock(void *handle);
void wb_destroy(void *handle);
]]

local directory = debug.getinfo(1, "S").source:sub(2):match("^(.*)[/\\]")
local service, library, library_path
local Browser = {}
Browser.__index = Browser
local runtime = {}

local function load_runtime()
  local path = config.plugins.webbrowser.runtime_path
  if not path or path == "" then path = directory .. PATHSEP .. "runtime" end
  path = common.home_expand(path)
  if not library then
    local filename = path .. PATHSEP .. "webbrowser-bridge" .. (PLATFORM == "Windows" and ".dll" or ".so")
    local ok, result = pcall(ffi.load, filename)
    assert(ok, "Web Browser runtime missing or unloadable. Install the runtime for this platform in "
      .. path .. ". " .. tostring(result))
    assert(result.wb_abi_version() == 5 and result.wb_state_size() == ffi.sizeof("wb_state"),
      "Web Browser runtime ABI mismatch; restart Pragtical after installing the matching runtime")
    library, library_path = result, path
  end
  return library, library_path
end

local function enqueue(data)
  if not service or service.stopping or (service.process and not service.process:running()) then return false end
  local bytes = json.encode(data) .. "\n"
  if service.queued + #bytes > 1024 * 1024 then return false end
  service.queue[#service.queue+1] = bytes
  service.queued = service.queued + #bytes
  return true
end

local function stop(current)
  if not current or current.stopping then return end
  current.stopping = true
  if current.process then current.process:close_stream(process.STREAM_STDIN) end
end

local function read_clipboard(browser)
  if not browser.copy_pending then return end
  local pointer = library.wb_lock_clipboard(browser.handle, browser.clipboard_revision,
    browser.clipboard_size, browser.clipboard_sequence)
  if pointer == nil then return end
  local ok, text = pcall(ffi.string, pointer, browser.clipboard_size[0])
  library.wb_unlock(browser.handle)
  browser.clipboard_revision = tonumber(browser.clipboard_sequence[0])
  browser.copy_pending = false
  if not ok then error(text) end
  if text ~= "" then
    system.set_clipboard(text)
    if browser.cut_token then
      browser:send("context-action", {token=browser.cut_token,action="delete"})
    end
  end
  if browser.copy_context and (not browser.cut_token or text == "") then
    browser:send("context-cancel", {token=browser.copy_context})
  end
  browser.cut_token, browser.copy_context = nil, nil
end

local function read_context(browser)
  local pointer = library.wb_lock_context(browser.handle, browser.context_revision,
    browser.context_size, browser.context_sequence)
  if pointer == nil then return end
  local ok, bytes = pcall(ffi.string, pointer, browser.context_size[0])
  library.wb_unlock(browser.handle)
  browser.context_revision = tonumber(browser.context_sequence[0])
  if not ok then error(bytes) end
  if browser.on_context then browser.on_context(bytes ~= "" and json.decode(bytes) or nil) end
end

local function fail(current, message)
  for _, browser in pairs(current.clients) do
    local error = message
    if library.wb_poll(browser.handle, browser.state) ~= 0 and browser.state[0].status == 3 then
      error = ffi.string(browser.state[0].error)
    end
    browser.failed, browser.error = true, error
    if browser.on_change then browser.on_change(browser) end
    core.error("Web Browser: %s", error)
  end
end

local function pump(current)
  local function finish()
    current.finished = true
    if service == current then service = nil end
  end
  while current.previous and not current.previous.finished do coroutine.yield(0.025) end
  current.previous = nil
  while service == current or current.stopping do
    if not current.process then
      if current.stopping then finish(); return end
      if current.prepare then
        while current.prepare:running() do coroutine.yield(0.025) end
        if current.prepare:returncode() ~= 0 then
          fail(current, "Could not grant the Chromium sandbox read access to the runtime directory")
          finish(); return
        end
        current.prepare = nil
      end
      if current.stopping then finish(); return end
      local child, err = process.start(current.args, {stdout=process.REDIRECT_DISCARD, stderr=process.REDIRECT_PIPE})
      if not child then fail(current, tostring(err)); finish(); return end
      current.process = child
    end
    for _ = 1, 16 do
      local bytes = current.process:read_stderr(8192)
      if not bytes or bytes == "" then break end
      current.log = (current.log .. bytes):sub(-8192)
    end
    if not current.process:running() then
      if not current.stopping then fail(current, "CEF host exited. " .. current.log) end
      finish(); break
    end
    for _ = 1, 16 do
      if current.stopping then break end
      local bytes = current.queue[current.head]
      if not bytes then break end
      local count = current.process:write(bytes)
      if not count or count == 0 then break end
      current.queued = current.queued - count
      if count < #bytes then current.queue[current.head] = bytes:sub(count+1); break end
      current.queue[current.head] = false
      current.head = current.head+1
    end
    if current.head > #current.queue then current.queue, current.head = {}, 1 end
    local visible = false
    for id in pairs(current.ids) do
      local browser = current.clients[id]
      if not browser then enqueue {type="close",id=id}; current.ids[id] = nil
      elseif not browser.closed then
        if browser.on_tick then browser.on_tick(browser) end
        visible = visible or browser.visible
        if not browser.closed and library.wb_poll(browser.handle, browser.state) ~= 0 then
          local state = browser.state[0]
          if state.status == 1 then
            while #browser.pending > 0 do
              local data = browser.pending[1]
              if not enqueue(data) then break end
              browser.pending_bytes = browser.pending_bytes - #json.encode(data) - 1
              table.remove(browser.pending, 1)
            end
          end
          if state.revision ~= browser.revision or state.frame ~= browser.frame then
            browser.revision, browser.frame = tonumber(state.revision), tonumber(state.frame)
            browser.url, browser.title = ffi.string(state.url), ffi.string(state.title)
            browser.error, browser.cursor = ffi.string(state.error), ffi.string(state.cursor)
            if browser.cursor == "" then browser.cursor = "arrow" end
            browser.caret = {x=state.caret_x,y=state.caret_y,w=state.caret_width,h=state.caret_height}
            browser.loading = state.loading ~= 0
            browser.can_back, browser.can_forward = state.can_back ~= 0, state.can_forward ~= 0
            if browser.on_change then browser.on_change(browser) end
          end
          if not browser.closed and state.context_revision ~= browser.context_revision then read_context(browser) end
        end
        if not browser.closed then read_clipboard(browser) end
      end
    end
    if not next(current.ids) then stop(current) end
    coroutine.yield(visible and 1 / math.min(60,math.max(1,config.fps)) or 0.1)
  end
end

function Browser.new(url, options)
  options = options or {}
  local lib, path = load_runtime()
  local filename = core.temp_filename(".webbrowser")
  local handle = lib.wb_create(filename)
  assert(handle ~= nil, "Cannot create browser transport: " .. ffi.string(lib.wb_error()))
  handle = ffi.gc(handle, function(pointer) lib.wb_destroy(pointer); os.remove(filename) end)
  local self = setmetatable({handle=handle,filename=filename,state=ffi.new("wb_state[1]"),
    revision=-1,frame=0,copied_frame=0,title="Web Browser",url=url or "about:blank",
    error="",cursor="arrow",visible=true,pending={},pending_bytes=0,
    clipboard_revision=0,clipboard_size=ffi.new("uint32_t[1]"),clipboard_sequence=ffi.new("uint64_t[1]"),
    context_revision=0,context_size=ffi.new("uint32_t[1]"),context_sequence=ffi.new("uint64_t[1]")}, Browser)
  local width,height,scale = options.width or 800,options.height or 600,options.scale or SCALE
  if not service or service.stopping then
    local previous = service
    local executable = path .. PATHSEP .. (PLATFORM == "Windows" and "webbrowser-host.exe"
      or PLATFORM == "Mac OS X" and "webbrowser-host.app/Contents/MacOS/webbrowser-host" or "webbrowser-host")
    local profile = USERDIR .. PATHSEP .. "storage" .. PATHSEP .. "webbrowser"
    common.mkdirp(profile)
    local args = {executable,"--transport="..filename,"--url="..self.url,"--profile="..profile,
      "--log-file="..profile..PATHSEP.."cef.log","--width="..width,"--height="..height,
      "--scale="..scale,"--fps="..math.min(60,math.max(1,config.fps))}
    if config.plugins.webbrowser.disable_sandbox then args[#args+1] = "--no-sandbox" end
    local prepare
    if PLATFORM == "Windows" and not config.plugins.webbrowser.disable_sandbox then
      local err
      prepare,err = process.start({"icacls",path,"/grant","*S-1-15-2-2:(OI)(CI)(RX)"},
        {stdout=process.REDIRECT_DISCARD,stderr=process.REDIRECT_DISCARD})
      if not prepare then self:close(); error(err) end
    end
    service = {args=args,prepare=prepare,previous=previous,clients=setmetatable({}, {__mode="v"}),ids={},next_id=1,
      queue={},head=1,queued=0,log=""}
    local current = service
    core.add_background_thread(function() pump(current) end)
  end
  self.id,self.service = service.next_id,service
  service.next_id = service.next_id+1
  service.clients[self.id],service.ids[self.id] = self,true
  if self.id ~= 1 then
    local data = {transport=filename,url=self.url,width=width,height=height,scale=scale,
      fps=math.min(60,math.max(1,config.fps))}
    if options.owner then
      data.child_id,data.x,data.y = self.id,options.x,options.y
      assert(options.owner:send("devtools", data), "Cannot queue DevTools creation")
    else self:send("create", data) end
  end
  return self
end

function Browser:send(kind, data)
  if self.closed or self.failed or self.service ~= service then return false end
  data = data or {}; data.type,data.id = kind,self.id
  local deferred = kind=="navigate" or kind=="html" or kind=="script" or kind=="copy" or kind=="devtools"
  local accepted
  if deferred and (self.state[0].status==0 or #self.pending>0) then
    local size = #json.encode(data)+1
    if self.pending_bytes+size > 1024*1024 then return false end
    self.pending[#self.pending+1] = data
    self.pending_bytes = self.pending_bytes+size
    accepted = true
  else accepted = enqueue(data) end
  if accepted and kind=="copy" then
    self.copy_pending = true
    self.copy_context = data.context
    self.cut_token = data.cut and data.context or nil
  end
  return accepted
end

function Browser:read_frame()
  if self.closed then return end
  local pointer = library.wb_lock_frame(self.handle,self.copied_frame,self.state)
  if pointer == nil then return end
  local state = self.state[0]
  local ok,pixels = pcall(ffi.string,pointer,tonumber(state.width)*tonumber(state.height)*4)
  library.wb_unlock(self.handle)
  if not ok then error(pixels) end
  self.copied_frame = tonumber(state.frame)
  self.pixels,self.width,self.height = pixels,tonumber(state.width),tonumber(state.height)
  return true
end

function Browser:close()
  if self.closed then return end
  self:send("close")
  self.closed = true
  if self.service then self.service.clients[self.id],self.service.ids[self.id] = nil,nil end
  if self.handle then
    library.wb_destroy(ffi.gc(self.handle,nil)); self.handle = nil
    os.remove(self.filename)
  end
  self.pixels,self.pending = nil,{}
  self.pending_bytes = 0
  if self.service and not next(self.service.ids) then stop(self.service) end
end

function runtime.shutdown()
  stop(service)
end
runtime.Browser = Browser
return runtime
