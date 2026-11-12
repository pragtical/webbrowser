#include "transport.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_parser.h"
#include "include/cef_resource_handler.h"
#include "include/cef_resource_request_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include "include/cef_task.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include "include/cef_sandbox_win.h"
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include "include/wrapper/cef_library_loader.h"
#include "include/cef_sandbox_mac.h"
extern void wb_prepare_application();
#endif

static double number(CefRefPtr<CefDictionaryValue> data, const char* key) {
    return data->GetType(key) == VTYPE_INT ? data->GetInt(key) : data->GetDouble(key);
}

static int debugging_port() {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2,2), &data)) return 0;
    auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int length = sizeof(sockaddr_in);
#else
    auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
    socklen_t length = sizeof(sockaddr_in);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 0;
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0) port = ntohs(address.sin_port);
#ifdef _WIN32
    closesocket(socket); WSACleanup();
#else
    ::close(socket);
#endif
    return port;
}

// Each response owns an immutable snapshot while the view may replace its HTML.
// CEF invokes resource callbacks on IO sequences, never on Lua's thread.
class HtmlResource : public CefResourceHandler {
public:
    explicit HtmlResource(std::shared_ptr<const std::string> content) : content_(std::move(content)) {}
    bool Open(CefRefPtr<CefRequest>, bool& handle, CefRefPtr<CefCallback>) override {
        handle = true; return true;
    }
    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& length, CefString&) override {
        response->SetStatus(200);
        response->SetMimeType("text/html");
        response->SetCharset("utf-8");
        response->SetHeaderByName("Cache-Control", "no-store", true);
        length = static_cast<int64_t>(content_->size());
    }
    bool Read(void* target, int capacity, int& count, CefRefPtr<CefResourceReadCallback>) override {
        count = static_cast<int>(std::min<size_t>(std::max(0, capacity), content_->size() - offset_));
        if (!count) return false;
        std::memcpy(target, content_->data() + offset_, count);
        offset_ += count; return true;
    }
    void Cancel() override {}
private:
    std::shared_ptr<const std::string> content_;
    size_t offset_ = 0;
    IMPLEMENT_REFCOUNTING(HtmlResource);
};

class BrowserClient : public CefClient, public CefRenderHandler,
                      public CefLifeSpanHandler, public CefLoadHandler,
                      public CefDisplayHandler, public CefContextMenuHandler,
                      public CefRequestHandler, public CefResourceRequestHandler,
                      public CefDevToolsMessageObserver {
public:
    BrowserClient() { wb::text(state.cursor, sizeof(state.cursor), "arrow"); }
    wb::Mapping map;
    wb::State state{};
    CefRefPtr<CefBrowser> browser;
    int width = 800, height = 600, fps = 60;
    double scale = 1, zoom = 0;
    bool visible = true, focused = true, closing = false;
    int owner_id = 0, devtools_id = 0;
    std::function<void()> on_closed;
    std::function<void(const std::string&)> on_target;
    std::function<void(int)> on_inspect;
    CefRefPtr<CefRegistration> devtools_registration;
    std::string frontend_url;
    int inspect_node = 0;
    int target_request = 0, node_request = 0, request_sequence = 0;
    void load_frontend() {
        if (browser && !frontend_url.empty()) { begin_navigation(); browser->GetMainFrame()->LoadURL(frontend_url); }
    }

    void inspect_at(int x, int y) {
        if (!browser) return;
        if (!devtools_registration) devtools_registration = browser->GetHost()->AddDevToolsMessageObserver(this);
        auto params = CefDictionaryValue::Create();
        const double factor = scale * std::pow(1.2,zoom);
        params->SetInt("x", static_cast<int>(std::floor(x/factor)));
        params->SetInt("y", static_cast<int>(std::floor(y/factor)));
        params->SetBool("includeUserAgentShadowDOM",true);
        node_request = ++request_sequence;
        browser->GetHost()->ExecuteDevToolsMethod(node_request, "DOM.getNodeForLocation", params);
    }
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int id, bool success,
                               const void* result, size_t size) override {
        if (!success || !size) {
            if (id == target_request && on_target) on_target("");
            return;
        }
        auto value = CefParseJSON(std::string(static_cast<const char*>(result), size), JSON_PARSER_RFC);
        if (!value || value->GetType() != VTYPE_DICTIONARY) return;
        auto data = value->GetDictionary();
        if (id == target_request && on_target && data->HasKey("targetInfo")) {
            on_target(data->GetDictionary("targetInfo")->GetString("targetId"));
        } else if (id == node_request && on_inspect) on_inspect(data->GetInt("backendNodeId"));
    }
    void reveal_node() {
        if (!inspect_node || !browser || browser->IsLoading() || navigating || frontend_url.empty() ||
            browser->GetMainFrame()->GetURL() != frontend_url) return;
        const auto script = R"JS((async()=>{
            const SDK=await import(new URL('./core/sdk/sdk.js',location.href).href);
            const Common=await import(new URL('./core/common/common.js',location.href).href);
            for(let attempt=0;attempt<100;attempt++){
                const target=SDK.TargetManager.TargetManager.instance().primaryPageTarget();
                const model=target?.model(SDK.DOMModel.DOMModel);
                if(model){
                    const nodes=await model.pushNodesByBackendIdsToFrontend(new Set([)JS" + std::to_string(inspect_node) + R"JS(]));
                    const node=nodes?.get()JS" + std::to_string(inspect_node) + R"JS();
                    if(node){await Common.Revealer.reveal(node);return;}
                }
                await new Promise(resolve=>setTimeout(resolve,50));
            }
        })().catch(console.error))JS";
        browser->GetMainFrame()->ExecuteJavaScript(script, frontend_url, 0);
        inspect_node = 0;
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
        bool, bool, const CefString&, bool&) override { return this; }
    CefRefPtr<CefResourceHandler> GetResourceHandler(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request) override {
        std::lock_guard<std::mutex> lock(html_mutex);
        if (html && request->GetURL().ToString() == html_url) return new HtmlResource(html);
        return nullptr;
    }
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, std::max(1, width), std::max(1, height));
    }
    bool GetScreenInfo(CefRefPtr<CefBrowser> current, CefScreenInfo& info) override {
        // The transport is in physical pixels. Browser zoom handles editor
        // scaling, avoiding platform-specific OSR device-scale rounding.
        info.device_scale_factor = 1;
        CefRect rect; GetViewRect(current, rect); info.rect = rect; info.available_rect = rect;
        info.depth = 32; info.depth_per_component = 8; return true;
    }
    bool GetScreenPoint(CefRefPtr<CefBrowser>, int x, int y, int& sx, int& sy) override {
        sx = x; sy = y; return true;
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> current) override {
        browser = current;
        state.status = 1;
        browser->GetHost()->SetZoomLevel(std::log(scale) / std::log(1.2) + zoom);
        browser->GetHost()->SetFocus(focused);
        browser->GetHost()->WasHidden(!visible);
        changed();
        load_frontend();
        if (closing) browser->GetHost()->CloseBrowser(true);
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        devtools_registration = nullptr; on_target = nullptr; on_inspect = nullptr;
        browser = nullptr; closing = true; state.status = 2; changed();
        flush(); if (on_closed) on_closed();
    }
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
        const CefString& url, const CefString&, CefLifeSpanHandler::WindowOpenDisposition, bool,
        const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
        CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override {
        if (browser) browser->GetMainFrame()->LoadURL(url);
        return true;
    }
    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
        CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model) override {
        model->Clear();
        context_frame = frame; context_flags = params->GetEditStateFlags(); context_editable = params->IsEditable();
        context_token++;
        auto data = CefDictionaryValue::Create();
        data->SetInt("token", context_token);
        data->SetInt("x", params->GetXCoord()); data->SetInt("y", params->GetYCoord());
        data->SetString("link", params->GetLinkUrl());
        data->SetString("image", params->GetMediaType() == CM_MEDIATYPE_IMAGE ? params->GetSourceUrl() : CefString());
        data->SetBool("editable", params->IsEditable());
        data->SetBool("copy", (context_flags & CM_EDITFLAG_CAN_COPY) != 0);
        data->SetBool("cut", (context_flags & CM_EDITFLAG_CAN_CUT) != 0);
        // CEF's clipboard belongs to the helper. Paste availability uses the editor clipboard.
        data->SetBool("paste", params->IsEditable());
        data->SetBool("select_all", (context_flags & CM_EDITFLAG_CAN_SELECT_ALL) != 0);
        auto value = CefValue::Create(); value->SetDictionary(data);
        pending_context = CefWriteJSON(value, JSON_WRITER_DEFAULT);
        if (pending_context.size() > wb::context_capacity) {
            clear_context();
            wb::text(state.error, sizeof(state.error), "Context menu exceeds the 1 MiB transport");
        }
        context_dirty = true; changed(); flush();
    }
    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool loading, bool back, bool forward) override {
        state.loading = loading; state.can_back = back; state.can_forward = forward; changed();
        if (!loading && browser) {
            navigating = false;
            auto scripts = std::move(pending_scripts); pending_scripts.clear(); pending_script_bytes = 0;
            for (const auto& code : scripts) browser->GetMainFrame()->ExecuteJavaScript(code, state.url, 0);
            reveal_node();
        }
    }
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& url) override {
        if (frame->IsMain()) { wb::text(state.url, sizeof(state.url), url.ToString()); changed(); }
    }
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
        wb::text(state.title, sizeof(state.title), title.ToString()); changed();
    }
    void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) override {
        if (frame->IsMain()) {
            clear_context(); state.error[0] = 0;
            // Chromium stores zoom per origin; apply the editor scale after navigation too.
            if (browser) browser->GetHost()->SetZoomLevel(std::log(scale) / std::log(1.2) + zoom);
            changed();
        }
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode error,
        const CefString& message, const CefString&) override {
        if (frame->IsMain() && error != ERR_ABORTED) {
            wb::text(state.error, sizeof(state.error), message.ToString()); changed();
        }
    }
    bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t cursor,
        const CefCursorInfo&) override {
        const char* value = "arrow";
        if (cursor == CT_IBEAM) value = "ibeam";
        else if (cursor == CT_HAND) value = "hand";
        else if (cursor == CT_EASTWESTRESIZE || cursor == CT_COLUMNRESIZE) value = "sizeh";
        else if (cursor == CT_NORTHSOUTHRESIZE || cursor == CT_ROWRESIZE) value = "sizev";
        wb::text(state.cursor, sizeof(state.cursor), value); changed(); return true;
    }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus,
        int, const CefString& message) override {
        wb::text(state.error, sizeof(state.error), "Browser renderer stopped: " + message.ToString()); changed();
    }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefProcessId source,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (source != PID_RENDERER || message->GetName() != "pragtical-copy-selection") return false;
        pending_clipboard = message->GetArgumentList()->GetString(0).ToString();
        clipboard_dirty = true; flush(); return true;
    }
    void OnImeCompositionRangeChanged(CefRefPtr<CefBrowser>, const CefRange&,
                                      const RectList& bounds) override {
        if (bounds.empty()) return;
        state.caret_x = bounds[0].x; state.caret_y = bounds[0].y;
        state.caret_width = bounds[0].width; state.caret_height = bounds[0].height;
        changed();
    }
    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override {
        if (!show) popup.clear();
        frame_dirty = true;
    }
    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override { popup_rect = rect; }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                 const void* pixels, int w, int h) override {
        if (w <= 0 || h <= 0 || uint64_t(w) * h * 4 > wb::capacity) return;
        auto& target = type == PET_VIEW ? page : popup;
        target.assign(static_cast<const unsigned char*>(pixels),
                      static_cast<const unsigned char*>(pixels) + size_t(w) * h * 4);
        if (type == PET_VIEW) { state.width = w; state.height = h; }
        else { popup_width = w; popup_height = h; }
        frame_dirty = true; flush();
    }
    void flush() {
        if ((!frame_dirty && !metadata_dirty && !clipboard_dirty && !context_dirty) || !map.lock()) return;
        if (frame_dirty && !page.empty()) {
            auto* out = map.pixels();
            const size_t count = size_t(state.width) * state.height;
            for (size_t i = 0; i < count; i++) {
                out[i*4] = page[i*4+2]; out[i*4+1] = page[i*4+1];
                out[i*4+2] = page[i*4]; out[i*4+3] = 255;
            }
            if (!popup.empty()) {
                int ox = popup_rect.x, oy = popup_rect.y;
                for (int py = 0; py < popup_height; py++) for (int px = 0; px < popup_width; px++) {
                    int x = px + ox, y = py + oy;
                    if (x < 0 || y < 0 || x >= int(state.width) || y >= int(state.height)) continue;
                    const auto* p = popup.data() + (size_t(py) * popup_width + px) * 4;
                    auto* d = out + (size_t(y) * state.width + x) * 4;
                    int alpha = p[3];
                    d[0] = std::min(255, p[2] + d[0] * (255-alpha) / 255);
                    d[1] = std::min(255, p[1] + d[1] * (255-alpha) / 255);
                    d[2] = std::min(255, p[0] + d[2] * (255-alpha) / 255);
                }
            }
            state.frame++; frame_dirty = false;
        }
        if (clipboard_dirty) {
            auto* header = map.header;
            if (pending_clipboard.size() > wb::clipboard_capacity) {
                wb::text(state.error, sizeof(state.error), "Selected text exceeds the 1 MiB clipboard transport");
                pending_clipboard.clear(); state.revision++;
            }
            header->clipboard_size = static_cast<uint32_t>(pending_clipboard.size());
            std::memcpy(header->clipboard, pending_clipboard.data(), pending_clipboard.size());
            header->clipboard_revision++;
            pending_clipboard.clear(); clipboard_dirty = false;
        }
        if (context_dirty) {
            map.header->context_size = static_cast<uint32_t>(pending_context.size());
            std::memcpy(map.header->context, pending_context.data(), pending_context.size());
            map.header->context_revision++;
            context_dirty = false;
        }
        state.context_revision = map.header->context_revision;
        map.header->state = state;
        metadata_dirty = false; map.unlock();
    }
    void command(CefRefPtr<CefDictionaryValue> data) {
        const std::string type = data->GetString("type");
        auto host = browser ? browser->GetHost() : nullptr;
        if (type == "resize") {
            int w = data->GetInt("width"), h = data->GetInt("height");
            double s = number(data, "scale");
            if (w < 1 || h < 1 || w > 8192 || h > 8192 || uint64_t(w+4) * (h+4) * 4 > wb::capacity) return;
            width = w; height = h; scale = std::clamp(s, 0.5, 4.0);
            popup.clear();
            if (host) { host->SetZoomLevel(std::log(scale) / std::log(1.2) + zoom); host->WasResized(); }
        } else if (type == "visible") {
            visible = data->GetBool("value"); if (host) host->WasHidden(!visible);
            if (!visible) clear_context();
        } else if (type == "focus") {
            focused = data->GetBool("value"); if (host) host->SetFocus(focused);
        } else if (type == "close") {
            closing = true; clear_context();
            if (host) host->CloseBrowser(true);
        } else if (type == "context-cancel") {
            if (data->GetInt("token") == context_token) clear_context();
        } else if (type == "context-action") {
            if (!host || data->GetInt("token") != context_token || !context_frame || !context_frame->IsValid()) return;
            const auto action = data->GetString("action").ToString();
            if (action == "delete" && (context_flags & CM_EDITFLAG_CAN_CUT)) context_frame->Delete();
            else if (action == "select_all" && (context_flags & CM_EDITFLAG_CAN_SELECT_ALL)) context_frame->SelectAll();
            else if (action == "paste" && context_editable) {
                context_frame->ExecuteJavaScript("window.focus()", "", 0);
                host->ImeCommitText(data->GetString("text"), CefRange(-1,-1), 0);
            }
            clear_context();
        } else if (!browser) return;
        else if (type == "navigate") {
            begin_navigation();
            browser->GetMainFrame()->LoadURL(data->GetString("url"));
        } else if (type == "html") {
            auto content = std::make_shared<const std::string>(data->GetString("html").ToString());
            auto url = data->GetString("base_url").ToString();
            if (url.empty()) url = "https://pragtical-html.invalid/" + std::to_string(browser->GetIdentifier()) + "/";
            CefURLParts parts;
            if (!CefParseURL(url, parts)) {
                wb::text(state.error, sizeof(state.error), "Invalid HTML base URL"); changed(); return;
            }
            url = CefString(&parts.spec).ToString();
            {
                std::lock_guard<std::mutex> lock(html_mutex);
                html = std::move(content); html_url = url.substr(0, url.find('#'));
            }
            begin_navigation();
            browser->GetMainFrame()->LoadURL(url);
        }
        else if (type == "back") browser->GoBack();
        else if (type == "forward") browser->GoForward();
        else if (type == "reload") { begin_navigation(); browser->Reload(); }
        else if (type == "stop") browser->StopLoad();
        else if (type == "zoom") {
            zoom = std::clamp(number(data, "value"), -8.0, 8.0);
            host->SetZoomLevel(std::log(scale) / std::log(1.2) + zoom);
        }
        else if (type == "script") {
            auto code = data->GetString("code").ToString();
            if (navigating || browser->IsLoading()) {
                if (pending_script_bytes + code.size() > 1024 * 1024) {
                    wb::text(state.error, sizeof(state.error), "Pending JavaScript queue is full"); changed(); return;
                }
                pending_script_bytes += code.size(); pending_scripts.push_back(std::move(code));
            } else browser->GetMainFrame()->ExecuteJavaScript(code, state.url, 0);
        }
        else if (type == "copy") {
            const int token = data->GetInt("context");
            if (token && (token != context_token || !context_frame || !(context_flags & CM_EDITFLAG_CAN_COPY))) return;
            auto frame = token ? context_frame : browser->GetFocusedFrame();
            if (!frame) frame = browser->GetMainFrame();
            frame->SendProcessMessage(PID_RENDERER, CefProcessMessage::Create("pragtical-copy-selection"));
        }
        else if (type == "edit") {
            auto frame = browser->GetFocusedFrame();
            if (!frame) frame = browser->GetMainFrame();
            auto action = data->GetString("action").ToString();
            if (action == "copy") frame->Copy(); else if (action == "paste") frame->Paste();
            else if (action == "cut") frame->Cut(); else if (action == "select_all") frame->SelectAll();
            else if (action == "undo") frame->Undo(); else if (action == "redo") frame->Redo();
        } else if (type == "composition") {
            const auto text = data->GetString("text");
            if (text.empty()) { host->ImeCancelComposition(); return; }
            CefCompositionUnderline underline;
            underline.range = CefRange(0, text.length());
            underline.color = CefColorSetARGB(255, 0, 0, 0);
            std::vector<CefCompositionUnderline> underlines{underline};
            const int start = data->GetInt("start");
            host->ImeSetComposition(text, underlines, CefRange(-1, -1),
                                   CefRange(start, start + data->GetInt("length")));
        } else if (type == "commit") {
            host->ImeCommitText(data->GetString("text"), CefRange(-1, -1), 0);
        } else if (type == "text") {
            auto text = data->GetString("text").ToString16();
            for (auto character : text) {
                CefKeyEvent event; event.type = KEYEVENT_CHAR;
                event.character = character; event.unmodified_character = character;
                event.modifiers = data->GetInt("modifiers"); host->SendKeyEvent(event);
            }
        } else if (type == "key") {
            CefKeyEvent event;
            event.type = data->GetBool("up") ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;
            event.windows_key_code = data->GetInt("key");
            event.native_key_code = data->GetInt("native");
            event.modifiers = data->GetInt("modifiers");
            event.is_system_key = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
            host->SendKeyEvent(event);
        } else if (type == "move" || type == "button" || type == "wheel") {
            CefMouseEvent event;
            event.x = data->GetInt("x"); event.y = data->GetInt("y");
            event.modifiers = data->GetInt("modifiers");
            if (type == "move") host->SendMouseMoveEvent(event, data->GetBool("leave"));
            else if (type == "wheel") host->SendMouseWheelEvent(event, data->GetInt("dx"), data->GetInt("dy"));
            else host->SendMouseClickEvent(event, static_cast<cef_mouse_button_type_t>(std::clamp(data->GetInt("button"),0,2)),
                                           data->GetBool("up"), std::clamp(data->GetInt("clicks"),1,3));
        }
    }
private:
    void changed() { state.revision++; metadata_dirty = true; }
    bool metadata_dirty = true, frame_dirty = false, navigating = true;
    int context_token = 0, context_flags = 0;
    CefRefPtr<CefFrame> context_frame;
    bool context_dirty = false, context_editable = false;
    std::string pending_context;
    void clear_context() {
        if (!context_frame) return;
        context_frame = nullptr; pending_context.clear(); context_dirty = true; changed();
    }
    void begin_navigation() {
        clear_context();
        navigating = true; pending_scripts.clear(); pending_script_bytes = 0;
    }
    bool clipboard_dirty = false;
    std::string pending_clipboard;
    std::vector<std::string> pending_scripts;
    size_t pending_script_bytes = 0;
    std::mutex html_mutex;
    std::string html_url;
    std::shared_ptr<const std::string> html;
    std::vector<unsigned char> page, popup;
    CefRect popup_rect;
    int popup_width = 0, popup_height = 0;
    IMPLEMENT_REFCOUNTING(BrowserClient);
};

class App : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler, public CefTask {
public:
    std::map<int, CefRefPtr<BrowserClient>> clients;
    std::string first_url = "about:blank", input;
    int port = 0;
    bool closing = false;
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefProcessId source,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (source != PID_BROWSER || message->GetName() != "pragtical-copy-selection") return false;
        CefString text;
        auto context = frame->GetV8Context();
        if (context && context->Enter()) {
            CefRefPtr<CefV8Value> value;
            CefRefPtr<CefV8Exception> exception;
            const char* script = R"JS((function() {
                const field = document.activeElement;
                if (field && typeof field.selectionStart === 'number') {
                    if (field.type === 'password') return '';
                    return field.value.slice(field.selectionStart, field.selectionEnd);
                }
                return String(window.getSelection());
            })())JS";
            if (context->Eval(script, "", 0, value, exception) && value && value->IsString()) text = value->GetStringValue();
            context->Exit();
        }
        auto response = CefProcessMessage::Create("pragtical-copy-selection");
        response->GetArgumentList()->SetString(0, text);
        frame->SendProcessMessage(PID_BROWSER, response);
        return true;
    }
    void create(int id, CefRefPtr<BrowserClient> client, const std::string& url,
                CefRefPtr<BrowserClient> owner = nullptr, int x = -1, int y = -1) {
        if (closing || clients.count(id)) return;
        clients[id] = client;
        client->on_closed = [this, id] {
            auto it = clients.find(id);
            if (it == clients.end()) return;
            const int child = it->second->devtools_id, parent = it->second->owner_id;
            clients.erase(it);
            if (auto parent_it = clients.find(parent); parent_it != clients.end()) {
                parent_it->second->devtools_id = 0;
                parent_it->second->on_target = nullptr; parent_it->second->on_inspect = nullptr;
                parent_it->second->devtools_registration = nullptr;
            }
            if (auto child_it = clients.find(child); child_it != clients.end()) {
                auto inspector = child_it->second;
                inspector->closing = true;
                if (inspector->browser) inspector->browser->GetHost()->CloseBrowser(true);
            }
            if (closing && clients.empty()) CefQuitMessageLoop();
        };
        CefWindowInfo window;
        window.SetAsWindowless(0);
        window.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
        CefBrowserSettings settings;
        settings.windowless_frame_rate = client->fps;
        settings.background_color = CefColorSetARGB(255,255,255,255);
        if (owner) {
            owner->devtools_id = id;
            // Chrome-style ShowDevTools rejects OSR. Render the bundled frontend
            // as an Alloy browser connected to CEF's loopback debugging endpoint.
            owner->on_target = [this, id](const std::string& target) {
                auto it = clients.find(id);
                if (it == clients.end()) return;
                if (target.empty() || port <= 0 || port > 65535) {
                    wb::text(it->second->state.error, sizeof(it->second->state.error), "CEF debugging target or endpoint is unavailable");
                    it->second->state.status = 3; it->second->state.revision++; it->second->flush(); return;
                }
                auto child = it->second;
                child->frontend_url = "http://127.0.0.1:" + std::to_string(port) + "/devtools/devtools_app.html?ws=127.0.0.1:" +
                    std::to_string(port) + "/devtools/page/" + target;
                child->load_frontend();
            };
            owner->on_inspect = [this, id](int node) {
                auto it = clients.find(id);
                if (it != clients.end()) { it->second->inspect_node = node; it->second->reveal_node(); }
            };
            owner->devtools_registration = owner->browser->GetHost()->AddDevToolsMessageObserver(owner);
            owner->target_request = ++owner->request_sequence;
            owner->browser->GetHost()->ExecuteDevToolsMethod(owner->target_request,"Target.getTargetInfo",nullptr);
            if (x >= 0 && y >= 0) owner->inspect_at(x,y);
        }
        if (!CefBrowserHost::CreateBrowser(window, client, owner ? "about:blank" : url, settings, nullptr, nullptr)) {
            wb::text(client->state.error, sizeof(client->state.error), "CEF could not create a browser");
            client->state.status = 3; client->state.revision++; client->flush();
            clients.erase(id);
            if (owner) {
                owner->devtools_id = 0; owner->on_target = nullptr; owner->on_inspect = nullptr;
                owner->devtools_registration = nullptr;
            }
        }
    }
    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> line) override {
        if (process_type.empty()) {
            line->AppendSwitchWithValue("force-device-scale-factor", "1");
            line->AppendSwitchWithValue("remote-debugging-address", "127.0.0.1");
            line->AppendSwitchWithValue("remote-allow-origins", "http://127.0.0.1:" + std::to_string(port));
        }
    }
    void OnContextInitialized() override {
        auto first = clients.at(1); clients.clear();
        create(1, first, first_url); poll();
    }
    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine>, const CefString&) override {
        // Another editor owns this profile. Never open CEF's default Chrome window.
        return true;
    }
    void command(CefRefPtr<CefDictionaryValue> data) {
        auto type = data->GetString("type").ToString();
        if (type == "quit") { stop(); return; }
        if (closing) return;
        int id = data->GetInt("id");
        if (type == "create" || type == "devtools") {
            CefRefPtr<BrowserClient> owner;
            if (type == "devtools") {
                auto it = clients.find(id);
                if (it == clients.end() || !it->second->browser || it->second->owner_id || it->second->devtools_id) return;
                owner = it->second;
                id = data->GetInt("child_id");
            }
            if (id < 1 || clients.count(id) || clients.size() >= 32) return;
            CefRefPtr<BrowserClient> client = new BrowserClient;
            if (!client->map.open(data->GetString("transport"), false)) return;
            auto resize = data->Copy(false); resize->SetString("type", "resize");
            client->command(resize);
            client->fps = std::clamp(data->GetInt("fps"), 1, 60);
            client->owner_id = owner ? data->GetInt("id") : 0;
            create(id, client, data->GetString("url"), owner,
                   data->HasKey("x") ? data->GetInt("x") : -1,
                   data->HasKey("y") ? data->GetInt("y") : -1);
        } else if (type == "inspect") {
            auto it = clients.find(id);
            if (it == clients.end() || !it->second->browser || !it->second->devtools_id) return;
            it->second->inspect_at(data->GetInt("x"),data->GetInt("y"));
        } else {
            auto it = clients.find(id);
            if (it != clients.end()) it->second->command(data);
        }
    }
    void stop() {
        if (closing) return;
        closing = true;
        if (clients.empty()) { CefQuitMessageLoop(); return; }
        auto snapshot = clients;
        for (auto& [id, client] : snapshot) {
            client->closing = true;
            if (client->browser) client->browser->GetHost()->CloseBrowser(true);
        }
    }
    void Execute() override { poll(); }
    void poll() {
        char bytes[16384];
        for (int reads = 0; reads < 16; reads++) {
#ifdef _WIN32
            DWORD available = 0, got = 0;
            HANDLE pipe = GetStdHandle(STD_INPUT_HANDLE);
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) { stop(); break; }
            if (!available) break;
            if (!ReadFile(pipe, bytes, std::min<DWORD>(available, sizeof(bytes)), &got, nullptr) || !got) { stop(); break; }
            int count = got;
#else
            int count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count == 0) { stop(); break; }
            if (count < 0) break;
#endif
            input.append(bytes, count);
            if (input.size() > 1024 * 1024) { stop(); break; }
        }
        size_t consumed = 0;
        for (int commands = 0; commands < 128; commands++) {
            size_t end = input.find('\n', consumed);
            if (end == std::string::npos) break;
            auto value = CefParseJSON(input.substr(consumed, end-consumed), JSON_PARSER_RFC);
            consumed = end+1;
            if (value && value->GetType() == VTYPE_DICTIONARY) command(value->GetDictionary());
            if (closing) break;
        }
        input.erase(0, consumed);
        bool visible = false;
        for (auto& [id, client] : clients) { client->flush(); visible |= client->visible; }
        if (!closing) CefPostDelayedTask(TID_UI, this, visible ? 16 : 100);
    }
private:
    IMPLEMENT_REFCOUNTING(App);
};

#ifdef _WIN32
CEF_BOOTSTRAP_EXPORT int RunConsoleMain(int argc, char** argv, void* sandbox_info,
                                       cef_version_info_t*) {
#else
int main(int argc, char** argv) {
    void* sandbox_info = nullptr;
#endif
#ifdef __APPLE__
    CefScopedLibraryLoader loader;
    bool helper = false;
    for (int i = 1; i < argc; i++) if (std::string(argv[i]).starts_with("--type=")) helper = true;
    CefScopedSandboxContext sandbox;
    if (helper && !sandbox.Initialize(argc, argv)) return 1;
    if (!(helper ? loader.LoadInHelper() : loader.LoadInMain())) { std::cerr << "Cannot load CEF framework\n"; return 1; }
    if (!helper) wb_prepare_application();
#endif
#ifdef _WIN32
    CefMainArgs args(GetModuleHandle(nullptr));
    _setmode(_fileno(stdin), _O_BINARY);
#else
    CefMainArgs args(argc, argv);
#endif
    CefRefPtr<App> app = new App;
    int result = CefExecuteProcess(args, app, sandbox_info);
    if (result >= 0) return result;
    auto options = CefCommandLine::CreateCommandLine();
#ifdef _WIN32
    options->InitFromString(GetCommandLineW());
#else
    options->InitFromArgv(argc, argv);
#endif
    CefRefPtr<BrowserClient> client = new BrowserClient;
    if (!client->map.open(options->GetSwitchValue("transport"), false)) {
        std::cerr << client->map.error << '\n'; return 1;
    }
    if (options->HasSwitch("width")) client->width = std::clamp(std::atoi(options->GetSwitchValue("width").ToString().c_str()),1,4096);
    if (options->HasSwitch("height")) client->height = std::clamp(std::atoi(options->GetSwitchValue("height").ToString().c_str()),1,4096);
    if (options->HasSwitch("scale")) client->scale = std::clamp(std::atof(options->GetSwitchValue("scale").ToString().c_str()),0.5,4.0);
    if (options->HasSwitch("fps")) client->fps = std::clamp(std::atoi(options->GetSwitchValue("fps").ToString().c_str()),1,60);
    if (options->HasSwitch("url")) app->first_url = options->GetSwitchValue("url");
    app->port = debugging_port();
    app->clients[1] = client;
#ifndef _WIN32
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
#endif
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.remote_debugging_port = app->port;
#ifdef __APPLE__
    const auto contents = std::filesystem::path(argv[0]).parent_path().parent_path();
    CefString(&settings.browser_subprocess_path) = (contents / "Frameworks/WebBrowserHost Helper.app/Contents/MacOS/WebBrowserHost Helper").string();
#endif
    settings.no_sandbox = options->HasSwitch("no-sandbox");
    CefString(&settings.root_cache_path) = options->GetSwitchValue("profile");
    CefString(&settings.cache_path) = options->GetSwitchValue("profile");
    CefString(&settings.log_file) = options->GetSwitchValue("log-file");
    settings.log_severity = LOGSEVERITY_WARNING;
    if (!CefInitialize(args, settings, app, sandbox_info)) {
        const int code = CefGetExitCode();
        const auto error = code == CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED
            ? "The browser profile is already in use by another Pragtical instance. Close that instance before opening a browser here."
            : "CEF initialization failed (exit " + std::to_string(code) + ")";
        std::cerr << error << '\n';
        wb::text(client->state.error, sizeof(client->state.error), error);
        client->state.status = 3; client->state.revision++; client->flush();
        return code ? code : 1;
    }
    CefRunMessageLoop();
    app->clients.clear(); client = nullptr;
    CefShutdown(); return 0;
}
