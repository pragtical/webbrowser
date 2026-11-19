# Web Browser for Pragtical

A browser tab powered by prebuilt Chromium Embedded Framework (CEF). Pages render
inside Pragtical through `renderer.draw_pixels()`, with navigation, an address bar,
keyboard and mouse input, selection, scrolling, Unicode text and IME composition.

CEF runs in a separate helper process shared by browser tabs. Its callbacks and
platform event loops stay outside LuaJIT. Cookies and browsing data are stored in
`USERDIR/storage/webbrowser/`.

One editor instance can use this profile at a time. Another instance reports that
it is already in use, rather than opening a standalone Chromium window. Close the
editor using the browser before opening one in the other instance. No additional
profiles are created.

Requires Pragtical 3.14 with LuaJIT. Build targets cover Linux x86_64/ARM64,
Windows x86_64 (MSVC), and macOS Intel/Apple Silicon. macOS requires 13 or newer.

## Use

Run **Web Browser: Open** from the command palette, or
**Tools → Web Browser → Open Browser**.
Enter a URL in the address field and press Enter or Go.

Right-click an `.html`, `.htm` or `.xhtml` file in TreeView and choose
**Open with Web Browser** to preview it in a new browser tab.

For an open HTML document, run **Web Browser: Preview HTML**, choose
**Tools → Web Browser → Preview HTML**, or right-click the document and select
**Preview HTML**.
This previews the current buffer, including unsaved edits.
**Settings → Plugins → Web Browser → Preview Mode** selects Right (default),
New Tab, Bottom, Top or Left. Lua configuration uses
`config.plugins.webbrowser.preview_mode` with `right`, `newtab`, `bottom`, `top`
or `left`.
Open preview tabs refresh automatically whenever you save the source document,
including after **Save As**, without switching away from the source.
Relative CSS, scripts and images resolve from the document's directory (the
project directory for an untitled HTML document).

**Web Browser: Open App** prompts for a website and opens a new tab with the toolbar
and status strip hidden. **Web Browser: Toggle Controls** (also under
**Tools → Web Browser**) shows or hides those controls. Ctrl+L reveals the address
field when needed.

Right-click a page for navigation, editing, link/image actions and **Inspect
Element**. **F12** toggles Developer Tools inside the browser tab. Use its header
to dock it on the right or below the page, drag the divider to resize it, or close
it to restore the full page area. The address field includes a clear button.

- **Ctrl+L:** focus and select the address.
- **Ctrl+R:** reload.
- **Ctrl+C / Cmd+C:** copy the page or input-field selection through Pragtical’s clipboard.
- **Alt+Left / Alt+Right:** back / forward.
- **Ctrl+W:** close the tab.

Browser content receives its own typing and editing shortcuts. The address field
uses the editor's normal text widget. Links requesting another window open in
the current browser tab.

Settings → Plugins → Web Browser provides the home page, an optional runtime
directory and a sandbox override. The sandbox is enabled by default. Restart
Pragtical after replacing the native runtime or changing its directory.
Closing the last browser view stops CEF and its subprocesses. Opening another
view starts it again; cookies and other profile data remain on disk.

## Install and build

The plugin consists of `plugins/webbrowser/` and a platform runtime placed in
`plugins/webbrowser/runtime/`. PPM's manifest downloads and extracts that runtime.
Manual installations extract the matching runtime archive inside the plugin
folder, preserving its directory structure and executable permissions.

For development, install Python 3.10+, Meson 0.61+, Ninja and a C++20 compiler.
Use MSVC on Windows and Xcode on macOS. Linux also needs the desktop libraries
required by CEF, including GTK3, NSS, ALSA, X11 and GBM.

```sh
python3 scripts/build.py --install
ln -s "$PWD/plugins/webbrowser" ~/.config/pragtical/plugins/webbrowser
```

The script verifies and downloads the pinned binary SDK from `cef.json`, then
compiles the small transport/host and CEF's API wrapper. **Chromium is not built.**
CEF includes its own engine, resources and locales, so the runtime is considerably
larger than a single shared library. Linux release binaries are stripped.

Runtime archives and their SHA-256 files are written to `dist/`. CI builds the
five platforms and publishes runtime assets for version tags. Source archives are
not generated. CEF's license and bundled third-party notices ship in the runtime;
see [licenses/](licenses/).

## Plugin API

```lua
local browser = require "plugins.webbrowser"
local command = require "core.command"
command.perform("web-browser:open", "https://example.com")
command.perform("web-browser:open-app", "https://example.com") -- skips the URL prompt

-- Website app; normal browser.open(url) shows controls by default.
local app = browser.open("https://example.com", {controls = false})
app:set_controls_visible(true)
app:toggle_controls()

-- HTML view; controls are hidden by default.
local view = browser.open_html([[<!doctype html>
  <title>Preview</title><h1 id="heading">Hello</h1>
]], {
  name = "My Preview", -- optional fixed tab name
  on_change = function(view, state)
    -- state contains URL, title, loading/ready, navigation flags and error text.
  end
})
view:execute_script("document.getElementById('heading').textContent = 'Updated'")
view:load_html("<title>Replacement</title><h1>A new document</h1>")
view:copy_selection() -- asynchronous plain-text copy through the editor clipboard
view:show_devtools()
view:set_devtools_position("bottom") -- "right" is the default
view:hide_devtools()
```

`BrowserView(url, options)` creates an unattached view for placement by another
plugin. `options.html` initializes it from HTML. Set `options.parent` to embed it
in a Widget container, or `options.floating` for a floating view; size and position
it with the usual Widget methods. Call `view:close()` when disposing of a view
outside the normal tab-close path.

`show_devtools(x, y)` optionally inspects a position in physical pixels relative
to the HTML viewport. `toggle_devtools()` opens or closes the same inspector.

`load_html(html, base_url)` serves UTF-8 HTML directly from memory. Its optional
`base_url` is the document URL used to resolve relative resources; for local CSS
or images, use a file URL such as `file:///path/to/assets/preview.html`. That HTML
file does not need to exist. Without a base URL, the view uses a private synthetic
URL. HTML is not passed through a command-line argument or written to a temp file.

`execute_script(code)` runs JavaScript in the main frame after pending navigation
finishes. Both methods return whether the command was accepted, with a 1 MiB
limit on the encoded input queue. Execution is asynchronous; JavaScript return
values are not sent back to Lua. `get_state()` returns a metadata snapshot, and
`on_change(view, state)` observes metadata changes without polling or exposing
native handles. These methods also work on regular website views.

`copy_selection()` copies selected text from the focused frame, including input
and textarea selections. It also has a **Web Browser: Copy Selection** command.
The renderer reads the selection on demand and Pragtical writes it to the system
clipboard, so the helper process does not need Wayland clipboard ownership.
Clipboard transfers support up to 1 MiB of UTF-8 text.

Each view owns its document, navigation and frames. Views share the persistent
CEF profile at `USERDIR/storage/webbrowser/`, including website and HTML apps.

The plugin does not provide a download manager, drag-and-drop integration or
custom permission dialogs. Context menus use Pragtical's UI. Proprietary codecs
and DRM depend on the supplied CEF build.
