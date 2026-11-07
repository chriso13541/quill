// Quill: SDL front-end.
//
//   quill [--config FILE] [URL or search terms...]
//   quill --dump out.png [--width N] [--height N] URL     (headless screenshot of the whole page)
//
// The window is a software-rendered framebuffer blitted through SDL: tab strip
// and toolbar (back / forward / reload / address bar) on top, page in the
// middle, status line at the bottom. Each tab is its own Browser instance.
//
// Keys: Ctrl+T new tab, Ctrl+W close, Ctrl+Shift+T reopen closed tab,
// Ctrl+Tab / Ctrl+Shift+Tab or Ctrl+PgDn/PgUp switch, Ctrl+1..9 jump.
// Middle-click or Ctrl+click a link opens it in a background tab.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "browser.h"
#include "chrome.h"
#include "diskcache.h"
#include "filepick.h"
#include "history.h"
#include "font.h"
#include "framebuffer.h"
#include "settings.h"
#include "url.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" int stbi_write_png_to_func(void (*func)(void* context, void* data, int size), void* context, int w, int h,
                                      int comp, const void* data, int stride_in_bytes);

using namespace quill;

namespace {

using ui::kTabH;
using ui::kTop;
using ui::kMaxTabs;
constexpr int kStatusH = 0; // the status is a bubble over the page now, not a bar

void png_sink(void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

int run_dump(const Settings& s, const std::string& target, const std::string& out, int w, int h) {
    std::mutex              m;
    std::condition_variable cv;
    Browser                 b(s, [&] { cv.notify_all(); });
    b.set_viewport(w, h);
    b.navigate(target.empty() ? s.home_url : target);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        b.poll();
        if (b.idle()) break;
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(25));
    }
    for (int i = 0; i < 4; i++) { // let late images settle and relayout
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        b.poll();
    }
    int total = std::max(h, std::min(b.content_height(), 30000));
    b.set_viewport(w, total);
    Framebuffer fb;
    fb.resize(w, total);
    // CSS background / mask images are only discovered while painting: paint once to start their
    // downloads, wait for them, then paint for real.
    b.draw(fb, IRect(0, 0, w, total));
    for (int i = 0; i < 600 && !b.idle(); i++) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(25));
        b.poll();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    b.poll();
    total = std::max(h, std::min(b.content_height(), 30000));
    if (total != fb.h) { b.set_viewport(w, total); fb.resize(w, total); }
    b.draw(fb, IRect(0, 0, w, total));

    std::string rgb(static_cast<size_t>(w) * total * 3, '\0');
    for (size_t i = 0; i < fb.px.size(); i++) {
        uint32_t p      = fb.px[i];
        rgb[i * 3 + 0] = static_cast<char>((p >> 16) & 255);
        rgb[i * 3 + 1] = static_cast<char>((p >> 8) & 255);
        rgb[i * 3 + 2] = static_cast<char>(p & 255);
    }
    std::string png;
    if (!stbi_write_png_to_func(png_sink, &png, w, total, 3, rgb.data(), w * 3)) {
        std::fprintf(stderr, "quill: PNG encoding failed\n");
        return 1;
    }
    std::ofstream f(out, std::ios::binary);
    f.write(png.data(), static_cast<std::streamsize>(png.size()));
    if (!f) {
        std::fprintf(stderr, "quill: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("title: %s\nurl: %s\nsize: %dx%d (page height %d)\n", b.title().c_str(), b.url_for_bar().c_str(), w, total,
                b.content_height());
    return 0;
}

// ------------------------------------------------------------------ address bar
struct UrlBar {
    std::string text;
    size_t      cursor     = 0;
    bool        focused    = false;
    bool        select_all = false;

    void focus_all() {
        focused    = true;
        select_all = true;
        cursor     = text.size();
    }
    void insert(const std::string& s) {
        if (select_all) {
            text.clear();
            cursor     = 0;
            select_all = false;
        }
        text.insert(cursor, s);
        cursor += s.size();
    }
    size_t prev_boundary(size_t i) const {
        if (i == 0) return 0;
        i--;
        while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i--;
        return i;
    }
    size_t next_boundary(size_t i) const {
        if (i >= text.size()) return text.size();
        i++;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i++;
        return i;
    }
    void backspace() {
        if (select_all) {
            text.clear();
            cursor     = 0;
            select_all = false;
            return;
        }
        size_t p = prev_boundary(cursor);
        text.erase(p, cursor - p);
        cursor = p;
    }
    void del() {
        if (select_all) {
            backspace();
            return;
        }
        size_t n = next_boundary(cursor);
        text.erase(cursor, n - cursor);
    }
};


// ------------------------------------------------------------------ tabs
struct Tab {
    std::unique_ptr<Browser> br;
};

std::string tab_label(Browser& b) {
    std::string t = b.title();
    std::string u = b.url_for_bar();
    if (u == "about:blank" || t == "about:blank" || (t.empty() && u.empty())) t = "New Tab";
    else if (t.empty()) t = u;
    return t;
}

ui::TabInfo tab_info(Browser& b) {
    ui::TabInfo t;
    t.title   = tab_label(b);
    t.loading = b.loading();
    Browser::Security sec = b.security();
    t.host = sec.level == Browser::Security::Level::Internal ? std::string() : sec.host;
    if (sec.level != Browser::Security::Level::Internal) t.icon = b.favicon();
    return t;
}

ui::Security ui_security(const Browser::Security& s) {
    using L = Browser::Security::Level;
    switch (s.level) {
    case L::Internal: return ui::Security::Internal;
    case L::Secure: return ui::Security::Secure;
    case L::TrustedKey: return ui::Security::TrustedKey;
    case L::LocalHttp: return ui::Security::LocalHttp;
    case L::Insecure: return ui::Security::Insecure;
    case L::None: break;
    }
    return ui::Security::None;
}

enum MenuId { M_OpenTab = 1, M_CopyLink, M_OpenImage, M_CopyImage, M_Back, M_Forward, M_Reload, M_CopyAddr,
              M_Cut, M_Copy, M_Paste, M_SelectAll };

struct Gfx {
    SDL_Window*   win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture*  tex = nullptr;
    Framebuffer   fb;
    int           w = 0, h = 0;
};

void resize_gfx(Gfx& g) {
    SDL_GetWindowSize(g.win, &g.w, &g.h);
    if (g.tex) SDL_DestroyTexture(g.tex);
    g.tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, g.w, g.h);
    g.fb.resize(g.w, g.h);
}

int run_window(const Settings& s, const std::string& start) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "quill: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    Gfx g;
    g.win = SDL_CreateWindow("Quill", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, s.window_w, s.window_h, SDL_WINDOW_RESIZABLE);
    if (!g.win) {
        std::fprintf(stderr, "quill: cannot create window: %s\n", SDL_GetError());
        return 1;
    }
    g.ren = SDL_CreateRenderer(g.win, -1, 0);
    if (!g.ren) g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_SOFTWARE);
    if (!g.ren) {
        std::fprintf(stderr, "quill: cannot create renderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_StartTextInput();
    ui::set_icon_dir(s.icon_dir);
    DiskCache::instance().configure(s.cache_dir.empty() ? default_cache_dir() : s.cache_dir, s.disk_cache);
    if (s.preconnect) { // the search engine is where most sessions start: have its connection ready
        std::thread([s] {
            std::string u = s.search_url;
            size_t      p = u.find("%s");
            if (p != std::string::npos) u.replace(p, 2, "");
            Network n(s, nullptr);
            n.preconnect(u);
        }).detach();
    }
    History::instance().configure(s.history, s.history_file.empty() ? default_history_path() : s.history_file, s.history_days);
    Browser::set_file_picker([&g](bool multiple, const std::string& accept) {
        std::string              err;
        std::vector<std::string> got = pick_files(multiple, accept, &err);
        if (got.empty() && !err.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Quill", err.c_str(), g.win);
        return got;
    });
    Uint32 wake_event = SDL_RegisterEvents(1);

    SDL_Cursor* cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    SDL_Cursor* cur_hand  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    SDL_Cursor* cur_ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);

    // target=_blank links / forms: queued while the page handles the click, opened as tabs after the event loop turn
    std::vector<Browser::NewTab> tab_requests;
    auto make_browser = [&]() {
        auto b = std::make_unique<Browser>(s, [wake_event] {
            SDL_Event e;
            SDL_zero(e);
            e.type = wake_event;
            SDL_PushEvent(&e);
        });
        b->set_open_tab([&tab_requests](const Browser::NewTab& r) { tab_requests.push_back(r); });
        return b;
    };

    std::vector<Tab>            tabs;
    int                         active = 0;
    std::vector<std::string>    closed;   // URLs of closed tabs, for Ctrl+Shift+T
    struct Reaper {                       // tear-down of a closed tab happens off the UI thread
        std::thread                        t;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Reaper> reapers;
    auto cur = [&]() -> Browser& { return *tabs[static_cast<size_t>(active)].br; };

    UrlBar      bar;
    std::string last_bar_url, last_title, status, last_tabs_sig;
    bool        running = true, dirty = true;
    ui::Menu    menu;
    Browser::ContextInfo menu_ctx;
    bool        menu_on_bar = false, swallow_up = false, tab_click = false;
    // chrome hover state (for highlights) and the site-information panel
    ui::Button  tb_hover = ui::Button::None;
    int         tab_hover = -1;
    bool        tab_hover_close = false, tab_hover_plus = false, info_open = false;
    auto show_menu = [&](int mx, int my, std::vector<ui::MenuItem> items) {
        menu.items = std::move(items);
        menu.place(mx, my, g.w, g.h);
        dirty = true;
    };
    auto toolbar_state = [&]() {
        Browser&         b = cur();
        ui::ToolbarState t;
        t.can_back    = b.can_back();
        t.can_forward = b.can_forward();
        t.loading     = b.loading();
        t.hover       = tb_hover;
        t.editing     = bar.focused;
        t.text        = bar.text;
        t.cursor      = bar.cursor;
        t.select_all  = bar.select_all;
        t.info_open   = info_open;
        std::string u = b.url_for_bar();
        t.url         = u == "about:blank" || u == "about:home" ? std::string() : u;
        t.security    = bar.focused ? ui::Security::None : ui_security(b.security());
        t.zoom         = b.zoom_percent();
        t.default_zoom = s.zoom;
        return t;
    };
    auto site_info = [&]() {
        Browser::Security sec = cur().security();
        ui::SiteInfo      si;
        si.security = ui_security(sec);
        si.host     = sec.host;
        si.hsts     = sec.hsts;
        return si;
    };
    auto open_context = [&](int x, int y) {
        Browser& b = *tabs[static_cast<size_t>(active)].br;
        std::vector<ui::MenuItem> it;
        if (y < kTop) { // address bar
            if (ui::toolbar_hit(ui::toolbar_layout(g.w, cur().zoom_percent(), s.zoom), x, y) != ui::Button::Field) return;
            menu_on_bar = true;
            bar.focused = true;
            it = {{"Cut", M_Cut, bar.select_all}, {"Copy", M_Copy, bar.select_all}, {"Paste", M_Paste, true},
                  {"Select all", M_SelectAll, !bar.text.empty()}};
            show_menu(x, y, it);
            return;
        }
        menu_on_bar = false;
        int cx = x, cy = y - kTop;
        b.mouse_move(cx, cy);
        menu_ctx = b.context_at(cx, cy);
        if (menu_ctx.editable) {
            if (!b.form_focused()) { b.mouse_down(cx, cy); b.mouse_up(cx, cy); }
            bool sel = !b.form_selected_text().empty();
            it = {{"Cut", M_Cut, sel}, {"Copy", M_Copy, sel}, {"Paste", M_Paste, true}, {"Select all", M_SelectAll, true}};
        } else {
            if (b.has_selection()) {
                it.push_back({"Copy", M_Copy, true});
                it.push_back({"", 0, true});
            }
            if (!menu_ctx.link.empty()) {
                it.push_back({"Open link in new tab", M_OpenTab, true});
                it.push_back({"Copy link address", M_CopyLink, true});
                it.push_back({"", 0, true});
            }
            if (!menu_ctx.image.empty()) {
                it.push_back({"Open image in new tab", M_OpenImage, true});
                it.push_back({"Copy image address", M_CopyImage, true});
                it.push_back({"", 0, true});
            }
            it.push_back({"Back", M_Back, b.can_back()});
            it.push_back({"Forward", M_Forward, b.can_forward()});
            it.push_back({"Reload", M_Reload, true});
            it.push_back({"", 0, true});
            it.push_back({"Select all", M_SelectAll, true});
            it.push_back({"Copy page address", M_CopyAddr, true});
        }
        show_menu(x, y, it);
    };
    SDL_Cursor* shown   = nullptr;

    auto set_cursor = [&](SDL_Cursor* c) {
        if (c != shown) {
            SDL_SetCursor(c);
            shown = c;
        }
    };
    auto activate = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        active    = i;
        info_open = false;
        cur().set_viewport(g.w, g.h - kTop - kStatusH); // relayouts only if the window changed while this tab was in the background
        cur().refresh_zoom();                           // ...or its site was zoomed in another tab
        bar.focused = bar.select_all = false;
        last_bar_url = "\x01"; // force the address bar / title refresh below
        last_title   = "\x01";
        status.clear();
        dirty = true;
    };
    // Adds a tab. `url` empty -> blank tab with the address bar focused.
    auto open_tab = [&](const std::string& url, bool foreground, bool is_link, const Browser::NewTab* req = nullptr) {
        if (static_cast<int>(tabs.size()) >= kMaxTabs) return;
        Tab t;
        t.br = make_browser();
        t.br->set_viewport(g.w, g.h - kTop - kStatusH);
        if (req) t.br->open_request(*req);
        else if (url.empty()) t.br->navigate("about:blank");
        else if (is_link) t.br->navigate_link(url, tabs.empty() ? std::string() : tabs[static_cast<size_t>(active)].br->link_source());
        else t.br->navigate(url);
        // new tabs sit right after the active one when opened from a link, else at the end
        size_t pos = is_link && !tabs.empty() ? static_cast<size_t>(active) + 1 : tabs.size();
        tabs.insert(tabs.begin() + static_cast<std::ptrdiff_t>(pos), std::move(t));
        if (foreground) {
            activate(static_cast<int>(pos));
            if (url.empty()) {
                bar.text       = "";
                bar.cursor     = 0;
                bar.focused    = true;
                bar.select_all = false;
                last_bar_url   = cur().url_for_bar();
            }
        }
        dirty = true;
    };
    auto run_menu = [&](int id) {
        Browser& b = *tabs[static_cast<size_t>(active)].br;
        switch (id) {
        case M_OpenTab: open_tab(menu_ctx.link, false, true); break;
        case M_CopyLink: SDL_SetClipboardText(menu_ctx.link.c_str()); break;
        case M_OpenImage: open_tab(menu_ctx.image, false, true); break;
        case M_CopyImage: SDL_SetClipboardText(menu_ctx.image.c_str()); break;
        case M_Back: b.back(); break;
        case M_Forward: b.forward(); break;
        case M_Reload: b.reload(); break;
        case M_CopyAddr: SDL_SetClipboardText(b.url_for_bar().c_str()); break;
        case M_Cut: case M_Copy: {
            std::string sel = menu_on_bar ? bar.text : b.form_selected_text();
            if (sel.empty() && !menu_on_bar) sel = b.selected_text(); // the page's selected text
            if (!sel.empty()) SDL_SetClipboardText(sel.c_str());
            if (id == M_Cut) { if (menu_on_bar) bar.insert(""); else b.form_key(FormKey::Delete); }
            break;
        }
        case M_Paste:
            if (char* t = SDL_GetClipboardText()) {
                if (menu_on_bar) bar.insert(t); else b.form_text(t);
                SDL_free(t);
            }
            break;
        case M_SelectAll:
            if (menu_on_bar) bar.focus_all();
            else if (!b.form_key(FormKey::SelectAll)) b.select_all_text();
            break;
        }
        dirty = true;
    };
    auto close_tab = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        std::string u = tabs[static_cast<size_t>(i)].br->url_for_bar();
        if (!u.empty() && u != "about:blank") {
            closed.push_back(u);
            if (closed.size() > 20) closed.erase(closed.begin());
        }
        std::unique_ptr<Browser> dead = std::move(tabs[static_cast<size_t>(i)].br);
        tabs.erase(tabs.begin() + i);
        auto done = std::make_shared<std::atomic<bool>>(false);
        reapers.push_back({std::thread([b = std::move(dead), done]() mutable {
                               b.reset();
                               *done = true;
                           }),
                           done});
        if (tabs.empty()) {
            running = false;
            return;
        }
        if (i < active) active--;
        active = std::min(active, static_cast<int>(tabs.size()) - 1);
        activate(active);
    };
    auto submit = [&]() {
        bar.focused = bar.select_all = false;
        cur().navigate(bar.text);
        dirty = true;
    };

    resize_gfx(g);
    open_tab(start.empty() ? s.home_url : start, true, false);

    while (running) {
        SDL_Event ev;
        int       timeout = -1;
        for (auto& t : tabs) {
            int w = t.br->next_wake_ms();
            if (w >= 0 && (timeout < 0 || w < timeout)) timeout = w;
        }
        bool got = timeout < 0 ? SDL_WaitEvent(&ev) != 0 : SDL_WaitEventTimeout(&ev, timeout) != 0;
        while (got && running) {
            Browser& br = cur();
            switch (ev.type) {
            case SDL_QUIT: running = false; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    resize_gfx(g);
                    br.set_viewport(g.w, g.h - kTop - kStatusH);
                    dirty = true;
                } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    dirty = true;
                } else if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
                    if (br.mouse_leave()) dirty = true;
                    if (tb_hover != ui::Button::None || tab_hover >= 0 || tab_hover_plus) dirty = true;
                    tb_hover        = ui::Button::None;
                    tab_hover       = -1;
                    tab_hover_close = tab_hover_plus = false;
                }
                break;
            case SDL_MOUSEWHEEL:
                menu.open = false;
                info_open = false;
                if ((SDL_GetModState() & KMOD_CTRL) != 0 && ev.wheel.y != 0) { // ctrl+wheel: zoom
                    if (ev.wheel.y > 0) br.zoom_in();
                    else br.zoom_out();
                } else if ((SDL_GetModState() & KMOD_SHIFT) != 0 && ev.wheel.y != 0) br.scroll_by_x(-ev.wheel.y * 64); // shift+wheel: sideways
                else {
                    if (ev.wheel.y != 0) br.scroll_by(-ev.wheel.y * 64);
                    if (ev.wheel.x != 0) br.scroll_by_x(ev.wheel.x * 64);
                }
                dirty = true;
                break;
            case SDL_MOUSEMOTION: {
                int x = ev.motion.x, y = ev.motion.y;
                if (menu.open) {
                    int hv = menu.item_at(x, y);
                    if (hv != menu.hover) { menu.hover = hv; dirty = true; }
                    set_cursor(cur_arrow);
                    break;
                }
                {   // hover highlights in the tab strip and toolbar
                    ui::Button nb = y >= kTabH && y < kTop ? ui::toolbar_hit(ui::toolbar_layout(g.w, cur().zoom_percent(), s.zoom), x, y) : ui::Button::None;
                    ui::TabHit th = ui::tab_hit(x, y, static_cast<int>(tabs.size()), g.w, active);
                    if (nb != tb_hover || th.index != tab_hover || th.close != tab_hover_close || th.plus != tab_hover_plus) dirty = true;
                    tb_hover        = nb;
                    tab_hover       = th.index;
                    tab_hover_close = th.close;
                    tab_hover_plus  = th.plus;
                }
                if (y < kTop) {
                    set_cursor(tb_hover == ui::Button::Field ? cur_ibeam : cur_arrow);
                    if (br.mouse_leave()) dirty = true;
                } else {
                    if (br.mouse_move(x, y - kTop)) dirty = true;
                    {
                        std::string cn = br.cursor();
                        set_cursor(cn == "pointer" ? cur_hand : cn == "text" ? cur_ibeam : cur_arrow);
                    }
                    if (br.status() != status) dirty = true;
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN: {
                int  x = ev.button.x, y = ev.button.y;
                bool left = ev.button.button == SDL_BUTTON_LEFT, middle = ev.button.button == SDL_BUTTON_MIDDLE;
                if (menu.open) { // any click closes the menu; a left click on an enabled item runs it
                    int hv = menu.item_at(x, y);
                    menu.open  = false;
                    swallow_up = left;
                    dirty      = true;
                    if (left && hv >= 0 && menu.items[static_cast<size_t>(hv)].enabled) run_menu(menu.items[static_cast<size_t>(hv)].id);
                    break;
                }
                if (info_open) { // the site-information panel: clicks inside it do nothing, outside close it
                    ui::SiteInfo si = site_info();
                    if (ui::site_info_rect(g.w, si).contains(x, y)) break;
                    bool on_identity = y >= kTabH && y < kTop && ui::toolbar_hit(ui::toolbar_layout(g.w, cur().zoom_percent(), s.zoom), x, y) == ui::Button::Identity;
                    info_open = false;
                    dirty     = true;
                    if (on_identity) break; // a second click on the icon just closes it
                }
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    if (y >= kTabH && y < g.h - kStatusH) open_context(x, y);
                    break;
                }
                if (!left && !middle) break;
                if (y < kTabH) {
                    ui::TabHit h = ui::tab_hit(x, y, static_cast<int>(tabs.size()), g.w, active);
                    if (h.plus && left) open_tab("", true, false);
                    else if (h.index >= 0) {
                        if (middle || (left && h.close)) close_tab(h.index);
                        else activate(h.index);
                    }
                    dirty = true;
                } else if (y < kTop) {
                    if (!left) break;
                    ui::ToolbarLayout tl = ui::toolbar_layout(g.w, cur().zoom_percent(), s.zoom);
                    switch (ui::toolbar_hit(tl, x, y)) {
                    case ui::Button::Back: br.back(); break;
                    case ui::Button::Forward: br.forward(); break;
                    case ui::Button::Reload:
                        if (br.loading()) br.stop();
                        else br.reload();
                        break;
                    case ui::Button::Home:
                        bar.focused = bar.select_all = false;
                        br.navigate(s.home_url);
                        break;
                    case ui::Button::Zoom: br.zoom_reset(); break;
                    case ui::Button::Identity:
                        if (!bar.focused && br.security().level != Browser::Security::Level::None) {
                            info_open = true;
                            break;
                        }
                        [[fallthrough]];
                    case ui::Button::Field:
                        br.form_blur();
                        if (!bar.focused) bar.focus_all();
                        else {
                            bar.select_all = false;
                            ui::ToolbarState ts = toolbar_state();
                            bar.cursor = ui::text_index_at(bar.text, x - tl.text.x + ui::text_scroll(ts, tl.text.w));
                        }
                        break;
                    case ui::Button::None: break;
                    }
                    dirty = true;
                } else if (y < g.h - kStatusH) {
                    if (bar.focused) {
                        bar.focused = bar.select_all = false;
                        dirty = true;
                    }
                    bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0, shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
                    std::string link;
                    if (middle || (left && ctrl)) {
                        br.mouse_move(x, y - kTop); // make sure the hover state is current
                        link = br.hover_link();
                    }
                    if (!link.empty()) {
                        open_tab(link, false, true);
                        tab_click = left;
                        dirty = true;
                    } else if (left) { // ctrl+click off a link still clicks (ctrl/shift pick several options in a list box)
                        Browser::set_click_modifiers(ctrl, shift);
                        Browser::set_click_count(ev.button.clicks);
                        if (br.mouse_down(x, y - kTop)) dirty = true;
                        Browser::set_click_modifiers(false, false);
                        Browser::set_click_count(1);
                    }
                }
                break;
            }
            case SDL_MOUSEBUTTONUP:
                if (swallow_up && ev.button.button == SDL_BUTTON_LEFT) { swallow_up = false; break; }
                if (ev.button.button == SDL_BUTTON_LEFT && ev.button.y >= kTop && ev.button.y < g.h - kStatusH) {
                    if (tab_click) { tab_click = false; break; } // ctrl+click opened a tab instead
                    if (br.mouse_up(ev.button.x, ev.button.y - kTop)) dirty = true;
                }
                break;
            case SDL_TEXTINPUT:
                if (bar.focused) {
                    bar.insert(ev.text.text);
                    dirty = true;
                } else if (br.form_wants_text() && br.form_text(ev.text.text)) {
                    dirty = true;
                }
                break;
            case SDL_KEYDOWN: {
                SDL_Keycode k    = ev.key.keysym.sym;
                if (menu.open) { menu.open = false; dirty = true; if (k == SDLK_ESCAPE) break; }
                if (info_open) { info_open = false; dirty = true; if (k == SDLK_ESCAPE) break; }
                Uint16      mod  = ev.key.keysym.mod;
                bool        ctrl = (mod & KMOD_CTRL) != 0, alt = (mod & KMOD_ALT) != 0, shift = (mod & KMOD_SHIFT) != 0;
                int         nt   = static_cast<int>(tabs.size());
                if (ctrl && k == SDLK_q) { running = false; break; }
                if (ctrl && k == SDLK_t) {
                    if (shift) {
                        if (!closed.empty()) {
                            std::string u = closed.back();
                            closed.pop_back();
                            open_tab(u, true, false);
                        }
                    } else open_tab("", true, false);
                    break;
                }
                if (ctrl && k == SDLK_w) { close_tab(active); break; }
                if (ctrl && (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS)) { cur().zoom_in(); dirty = true; break; }
                if (ctrl && (k == SDLK_MINUS || k == SDLK_KP_MINUS)) { cur().zoom_out(); dirty = true; break; }
                if (ctrl && (k == SDLK_0 || k == SDLK_KP_0)) { cur().zoom_reset(); dirty = true; break; }
                if (ctrl && k == SDLK_h) { // history: the tab that already shows it, else a new one
                    int at = -1;
                    for (int i = 0; i < nt; i++)
                        if (tabs[static_cast<size_t>(i)].br->url_for_bar().compare(0, 13, "about:history") == 0) at = i;
                    if (at >= 0) {
                        activate(at);
                        tabs[static_cast<size_t>(at)].br->navigate("about:history"); // fresh: newest visits first
                    } else open_tab("about:history", true, false);
                    dirty = true;
                    break;
                }
                if (ctrl && (k == SDLK_TAB || k == SDLK_PAGEDOWN || k == SDLK_PAGEUP)) {
                    bool back = (k == SDLK_PAGEUP) || (k == SDLK_TAB && shift);
                    activate((active + (back ? nt - 1 : 1)) % nt);
                    break;
                }
                if (ctrl && k >= SDLK_1 && k <= SDLK_9) {
                    activate(k == SDLK_9 ? nt - 1 : std::min<int>(static_cast<int>(k - SDLK_1), nt - 1));
                    break;
                }
                if (ctrl && k == SDLK_l) { bar.focus_all(); dirty = true; break; }
                if (k == SDLK_F5 || (ctrl && k == SDLK_r)) { br.reload(); dirty = true; break; }
                if (alt && k == SDLK_LEFT) { br.back(); dirty = true; break; }
                if (alt && k == SDLK_RIGHT) { br.forward(); dirty = true; break; }
                if (bar.focused) {
                    dirty = true;
                    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) submit();
                    else if (k == SDLK_ESCAPE) {
                        bar.focused = bar.select_all = false;
                        bar.text                     = last_bar_url;
                    } else if (k == SDLK_BACKSPACE) bar.backspace();
                    else if (k == SDLK_DELETE) bar.del();
                    else if (k == SDLK_LEFT) { bar.cursor = bar.select_all ? 0 : bar.prev_boundary(bar.cursor); bar.select_all = false; }
                    else if (k == SDLK_RIGHT) { bar.cursor = bar.select_all ? bar.text.size() : bar.next_boundary(bar.cursor); bar.select_all = false; }
                    else if (k == SDLK_HOME) { bar.select_all = false; bar.cursor = 0; }
                    else if (k == SDLK_END) { bar.select_all = false; bar.cursor = bar.text.size(); }
                    else if (ctrl && k == SDLK_a) { bar.select_all = true; bar.cursor = bar.text.size(); }
                    else if (ctrl && k == SDLK_c) SDL_SetClipboardText(bar.text.c_str());
                    else if (ctrl && k == SDLK_v) {
                        if (char* t = SDL_GetClipboardText()) {
                            std::string clip = t;
                            SDL_free(t);
                            for (auto& c : clip)
                                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                            bar.insert(clip);
                        }
                    }
                    break;
                }
                if (!bar.focused) { // keys for a focused page control (text field, checkbox, dropdown, ...)
                    bool used = false;
                    if (ctrl && k == SDLK_a) {
                        used = br.form_key(FormKey::SelectAll);
                        if (!used) { br.select_all_text(); used = true; } // no field focused: the page's text
                    } else if (ctrl && k == SDLK_c) {
                        std::string sel = br.form_selected_text();
                        if (sel.empty()) sel = br.selected_text();
                        if (!sel.empty()) {
                            SDL_SetClipboardText(sel.c_str());
                            used = true;
                        }
                    } else if (ctrl && k == SDLK_v) {
                        if (br.form_wants_text()) {
                            if (char* t = SDL_GetClipboardText()) {
                                used = br.form_text(t);
                                SDL_free(t);
                            }
                        }
                    } else if (!ctrl && !alt) {
                        if (k == SDLK_BACKSPACE) used = br.form_key(FormKey::Backspace);
                        else if (k == SDLK_DELETE) used = br.form_key(FormKey::Delete);
                        else if (k == SDLK_LEFT) used = br.form_key(FormKey::Left);
                        else if (k == SDLK_RIGHT) used = br.form_key(FormKey::Right);
                        else if (k == SDLK_UP) used = br.form_key(FormKey::Up);
                        else if (k == SDLK_DOWN) used = br.form_key(FormKey::Down);
                        else if (k == SDLK_HOME) used = br.form_key(FormKey::Home);
                        else if (k == SDLK_END) used = br.form_key(FormKey::End);
                        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) used = br.form_key(FormKey::Enter);
                        else if (k == SDLK_TAB) used = br.form_key(shift ? FormKey::ShiftTab : FormKey::Tab);
                        else if (k == SDLK_ESCAPE) used = br.form_key(FormKey::Escape);
                        else if (k == SDLK_SPACE && br.form_focused()) used = br.form_wants_text() ? true : br.form_key(FormKey::Space); // text arrives as SDL_TEXTINPUT
                    }
                    if (used) {
                        dirty = true;
                        break;
                    }
                }
                if (k == SDLK_ESCAPE) { // Esc stops a page that is still loading
                    if (br.loading()) br.stop();
                    dirty = true;
                    break;
                }
                int page = std::max(60, g.h - kTop - kStatusH - 40);
                if (k == SDLK_DOWN) br.scroll_by(48);
                else if (k == SDLK_UP) br.scroll_by(-48);
                else if (k == SDLK_PAGEDOWN || (k == SDLK_SPACE && !shift)) br.scroll_by(page);
                else if (k == SDLK_PAGEUP || (k == SDLK_SPACE && shift)) br.scroll_by(-page);
                else if (k == SDLK_HOME) br.scroll_to(0);
                else if (k == SDLK_END) br.scroll_to(br.content_height());
                dirty = true;
                break;
            }
            default:
                if (ev.type == wake_event) { /* handled by poll() below */ }
                break;
            }
            got = running && SDL_PollEvent(&ev) != 0;
        }
        if (!running) break;
        if (!tab_requests.empty()) { // a page asked for a new window
            auto reqs = std::move(tab_requests);
            tab_requests.clear();
            for (auto& r : reqs) open_tab(r.url, true, true, &r);
        }

        for (size_t i = 0; i < reapers.size();) { // join tab tear-downs that have finished
            if (reapers[i].done->load()) {
                reapers[i].t.join();
                reapers.erase(reapers.begin() + static_cast<std::ptrdiff_t>(i));
            } else i++;
        }

        // Every tab keeps loading in the background; the active one decides redraws, the strip shows the rest.
        for (size_t i = 0; i < tabs.size(); i++)
            if (tabs[i].br->poll() && static_cast<int>(i) == active) dirty = true;

        Browser&    br = cur();
        std::string bu = br.url_for_bar();
        if (bu != last_bar_url) {
            last_bar_url = bu;
            if (!bar.focused) {
                bar.text   = bu == "about:blank" ? std::string() : bu;
                bar.cursor = bar.text.size();
            }
            dirty = true;
        }
        std::string title = br.title();
        if (title != last_title) {
            last_title = title;
            std::string wt = title.empty() ? "Quill" : title + " - Quill";
            SDL_SetWindowTitle(g.win, wt.c_str());
        }
        std::string st = br.status();
        if (st != status) {
            status = st;
            dirty  = true;
        }
        std::string sig;
        for (auto& t : tabs)
            sig += tab_label(*t.br) + (t.br->loading() ? "\x01" : "") + std::to_string(reinterpret_cast<uintptr_t>(t.br->favicon().get())) + '\x1f';
        sig += std::to_string(active);
        if (sig != last_tabs_sig) {
            last_tabs_sig = sig;
            dirty         = true;
        }

        if (dirty) {
            g.fb.clear(Color(255, 255, 255));
            br.draw(g.fb, IRect(0, kTop, g.w, g.h - kTop - kStatusH));
            std::vector<ui::TabInfo> ti;
            for (auto& t : tabs) ti.push_back(tab_info(*t.br));
            ui::draw_tabs(g.fb, ti, active, tab_hover, tab_hover_close, tab_hover_plus);
            ui::draw_toolbar(g.fb, toolbar_state());
            ui::draw_status(g.fb, IRect(0, kTop, g.w, g.h - kTop), status);
            if (info_open) ui::draw_site_info(g.fb, site_info());
            if (menu.open) ui::draw_menu(g.fb, menu);
            SDL_UpdateTexture(g.tex, nullptr, g.fb.px.data(), g.w * static_cast<int>(sizeof(uint32_t)));
            SDL_RenderClear(g.ren);
            SDL_RenderCopy(g.ren, g.tex, nullptr, nullptr);
            SDL_RenderPresent(g.ren);
            dirty = false;
        }
    }

    tabs.clear(); // destroys the remaining browsers (joins their workers)
    for (auto& r : reapers) r.t.join();
    SDL_FreeCursor(cur_arrow);
    SDL_FreeCursor(cur_hand);
    SDL_FreeCursor(cur_ibeam);
    if (g.tex) SDL_DestroyTexture(g.tex);
    SDL_DestroyRenderer(g.ren);
    SDL_DestroyWindow(g.win);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string config = default_config_path();
    std::string dump_path, target;
    int         width = 1000, height = 700;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dump_path = argv[++i];
        else if (a == "--width" && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) height = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: quill [--config FILE] [URL or search terms]\n"
                        "       quill --dump out.png [--width N] [--height N] URL\n"
                        "config: %s\n", default_config_path().c_str());
            return 0;
        } else {
            if (!target.empty()) target += ' ';
            target += a;
        }
    }

    std::string err;
    Settings    s = load_settings(config, &err);
    if (!err.empty()) std::fprintf(stderr, "quill: config warning: %s\n", err.c_str());

    if (!dump_path.empty()) return run_dump(s, target, dump_path, std::max(200, width), std::max(100, height));
    return run_window(s, target);
}
