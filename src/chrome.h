// Quill's window chrome: tab strip, toolbar (back / forward / reload / home / address bar with the security
// indicator), the floating status bubble, the context menu and the site-information panel. Pure drawing on a
// Framebuffer plus the geometry for hit-testing, no SDL, so it can be rendered and tested headless.
#pragma once

#include "framebuffer.h"
#include "image.h"

#include <memory>

#include <string>
#include <vector>

namespace quill::ui {

constexpr int kTabH     = 38; // tab strip
constexpr int kToolbarH = 44; // buttons + address bar
constexpr int kTop      = kTabH + kToolbarH;
constexpr int kMaxTabs  = 40;

// ---------------------------------------------------------------- icons
// Monochrome icons drawn as tinted masks. Built in: Google's Material Icons (Apache License 2.0). An `icon_dir` with
// <name>.svg files (Material Symbols, Noto, ...) overrides them by name.
void set_icon_dir(const std::string& dir);
void draw_icon(Framebuffer& fb, const std::string& name, int x, int y, int size, Color c);
bool have_icon(const std::string& name);

// ---------------------------------------------------------------- tabs
struct TabInfo {
    std::string title;
    std::string host;    // for the letter badge ("" for Quill's own pages)
    bool        loading = false;
    std::shared_ptr<const Image> icon; // the site's icon (favicon), else the letter badge
};
struct TabHit {
    int  index = -1;   // tab index, or -1
    bool close = false; // on the close button
    bool plus  = false; // on the new-tab button
};
IRect  tab_rect(int i, int n, int W);
TabHit tab_hit(int x, int y, int n, int W, int active);
// `hover`: tab under the mouse (-1 none), `hover_close` / `hover_plus` for the small buttons
void draw_tabs(Framebuffer& fb, const std::vector<TabInfo>& tabs, int active, int hover, bool hover_close, bool hover_plus);

// ---------------------------------------------------------------- toolbar
enum class Security { None, Internal, Secure, TrustedKey, LocalHttp, Insecure };
enum class Button { None, Back, Forward, Reload, Home, Identity, Zoom, Field };
struct ToolbarLayout {
    IRect back, forward, reload, home, field, identity, text; // `text`: where the address text goes
    IRect zoom;                                                // the zoom badge (empty at the default zoom)
};
// `zoom_pct` != `default_pct`: a "110%" badge at the right end of the address field (click: reset).
ToolbarLayout toolbar_layout(int W, int zoom_pct = 100, int default_pct = 100);
Button        toolbar_hit(const ToolbarLayout& l, int x, int y);

struct ToolbarState {
    bool        can_back = false, can_forward = false, loading = false;
    Button      hover    = Button::None;
    Security    security = Security::None;
    std::string url;              // what the page is (shown dimmed / with the host emphasised when not editing)
    bool        editing = false;  // the address bar has focus: `text` is shown raw with a caret
    std::string text;
    size_t      cursor     = 0;
    bool        select_all = false;
    bool        info_open  = false; // site-information panel showing
    int         zoom = 100, default_zoom = 100; // page zoom, percent
};
void draw_toolbar(Framebuffer& fb, const ToolbarState& s);
// Byte offset in `text` nearest to pixel `px` (relative to the start of the text) while editing.
size_t text_index_at(const std::string& text, int px);
int    text_scroll(const ToolbarState& s, int avail); // horizontal scroll keeping the caret visible

// ---------------------------------------------------------------- status bubble (link previews, loading)
void draw_status(Framebuffer& fb, const IRect& content, const std::string& text);

// ---------------------------------------------------------------- site information panel
struct SiteInfo {
    Security    security = Security::None;
    std::string host;
    bool        hsts = false;
};
IRect site_info_rect(int W, const SiteInfo& info);
void  draw_site_info(Framebuffer& fb, const SiteInfo& info);

// ---------------------------------------------------------------- context menu
struct MenuItem {
    std::string label;
    int         id      = 0; // 0 = separator
    bool        enabled = true;
};
struct Menu {
    bool                  open = false;
    int                   x = 0, y = 0, w = 0, hover = -1;
    std::vector<MenuItem> items;
    static constexpr int  kRowH = 28, kSepH = 9, kPad = 5;
    int row_y(size_t i) const;
    int height() const;
    int item_at(int mx, int my) const;
    // Sizes and places the menu at (mx, my), kept inside a W x H window.
    void place(int mx, int my, int W, int H);
};
void draw_menu(Framebuffer& fb, const Menu& m);

// Cuts `s` to `maxw` pixels with an ellipsis (UTF-8 aware).
std::string ellipsize(const std::string& s, float px, int maxw, int weight = 400);

} // namespace quill::ui
