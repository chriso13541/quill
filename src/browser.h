// Browser core: navigation, history, page loading, layout and drawing.
// Independent of SDL; the window front-end (main.cpp) and the headless
// screenshot mode both drive it through this class.
//
// Threading: all public methods are for the UI thread. Loading happens on a
// worker pool and signals completion through the `wake` callback, after which
// the UI thread calls poll().
#pragma once
#include "engine/document.h"
#include "engine/paint.h"
#include "forms.h"
#include "framebuffer.h"
#include "net.h"
#include "page.h"
#include "engine/selection.h"
#include "settings.h"
#include "threadpool.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace quill {

// Keys the window front-end forwards to a focused page form control (SDL-independent).
enum class FormKey { Backspace, Delete, Left, Right, Up, Down, Home, End, Enter, Tab, ShiftTab, Escape, SelectAll, Space };

class Browser {
public:
    Browser(const Settings& s, std::function<void()> wake, std::shared_ptr<RequestFilter> filter = nullptr);
    ~Browser();

    // Navigation. `input` is whatever the user typed (URL, host, or search terms).
    void navigate(const std::string& input);
    // Navigate to an absolute URL that came from a page (link clicked or opened in a new tab).
    // Unlike navigate(), the text is never reinterpreted as a search, and quill: commands are refused.
    // `source_page`: the page the link was on, when it was opened from another tab (for the http warning rule).
    void navigate_link(const std::string& url, const std::string& source_page = "");
    // URL of the page a link under the mouse belongs to (the hovered frame's page in a frameset), or "".
    std::string link_source() const;
    void reload();
    void back();
    void forward();
    bool can_back() const { return idx_ > 0; }
    bool can_forward() const { return idx_ + 1 < static_cast<int>(hist_.size()); }

    // Call on the UI thread after wake() fired (or periodically). True if a redraw is needed.
    bool poll();
    // Milliseconds until poll() should be called again even without a wake, or -1.
    int next_wake_ms() const;

    void set_viewport(int w, int h);
    void draw(Framebuffer& fb, const IRect& area);

    // In a frameset these act on the frame under the mouse (else the last clicked / largest frame).
    void scroll_by(int dy);
    // Sideways (shift+wheel, a tilting wheel): scroll containers that can move that way. The page itself doesn't.
    void scroll_by_x(int dx);
    void scroll_to(int y);
    int  scroll_y() const;
    int  content_height() const;
    int  viewport_h() const;

    // ---- page form controls (text fields, checkboxes, buttons, ...)
    bool        form_focused() const;                                        // a control has keyboard focus
    bool        form_wants_text() const;                                     // ... and accepts typed text
    bool        form_text(const std::string& utf8);                          // typed or pasted text
    bool        form_key(FormKey k);                                         // true if the key was used
    bool        form_key_core(FormKey k);
    std::string form_selected_text() const;                                  // for Ctrl+C
    void        form_blur();
    void        form_blur_core();

    // Coordinates are relative to the top-left of the content area. Return true if a redraw is needed.
    bool mouse_move(int x, int y);
    bool mouse_down(int x, int y);
    bool mouse_up(int x, int y);
    bool mouse_down_core(int x, int y);
    bool mouse_up_core(int x, int y);
    bool mouse_leave();

    // What a right-click at (x,y) (content-area coordinates) is on.
    struct ContextInfo {
        std::string link;           // absolute URL of the enclosing link, or empty
        std::string image;          // absolute URL of the image under the pointer, or empty
        bool        editable = false; // an editable text control
        bool        control  = false; // any form control
    };
    ContextInfo context_at(int x, int y) const;

    std::string title() const;
    // The site icon of the page in this tab (32x32 at most), or null (the tab strip then shows a letter).
    std::shared_ptr<const Image> favicon() const;
    std::string url_for_bar() const;  // what the address bar should show
    std::string status() const;
    // What the address bar's security indicator shows for the page in this tab.
    struct Security {
        enum class Level { None, Internal, Secure, TrustedKey, LocalHttp, Insecure } level = Level::None;
        std::string host;
        bool        hsts = false; // the site is HTTPS-only (HSTS / preload list)
    };
    Security security() const;       // hovered link, or "Loading ..."
    std::string cursor() const;       // "pointer", "text", "auto", ... (page CSS, or the control under the mouse)
    std::string hover_link() const;   // absolute URL of the link under the mouse, or empty
    bool        loading() const;
    void        stop(); // abandon the page that is loading (the current one stays)
    bool        idle() const;         // nothing loading and no images pending

    // A link or form asked for a new window (target=_blank, or a window name no frame has). The front-end opens a
    // tab and hands the request to the new tab's Browser with open_request(). Without a handler the tab navigates.
    struct NewTab {
        std::string url, method = "GET", body, content_type;
        std::string source_page, initiator;
        bool        form_submit = false;
        std::string referrer, referrer_policy; // the page it came from, for Referer
        bool        no_referrer = false;
    };
    void set_open_tab(std::function<void(const NewTab&)> f) { open_tab_ = std::move(f); }
    void open_request(const NewTab& r); // load a NewTab request in this (fresh) tab

    // <input type=file>: the front-end's file chooser. Gets (multiple, accept) and returns the chosen paths (none if
    // cancelled). Process-wide so every frame's Browser uses it. Without one, file inputs can't be filled in.
    using FilePicker = std::function<std::vector<std::string>(bool multiple, const std::string& accept)>;
    static void set_file_picker(FilePicker f);
    // Keyboard modifiers held during the next mouse_down (ctrl / shift + click in a multiple list box).
    static void set_click_modifiers(bool ctrl, bool shift);
    // Page zoom, per site (like browsers: every tab on that site follows): steps 30% .. 300%; reset = the `zoom` setting.
    void zoom_in();
    void zoom_out();
    void zoom_reset();
    int  zoom_percent() const { return static_cast<int>(zoom_.load() * 100 + 0.5f); }
    // Picks up a zoom change made for this site in another tab (call when the tab is shown).
    void refresh_zoom();
    // Page text selection (drag, double-click a word, triple-click a block, shift+click to extend, Ctrl+A).
    bool        has_selection() const;
    std::string selected_text() const; // this page's or a frame's; "" if none
    void        select_all_text();
    void        clear_selection();
    // Clicks in a row so far (2 = double-click), for the next mouse_down.
    static void set_click_count(int n);
    // Number of full layouts this Browser has run (hover effects that only repaint don't count).
    int layout_count() const { return layout_count_; }
    // The current page's laid-out box tree (null while there is none). For tests and debugging.
    const engine::Box* debug_root() const { return current_ && current_->doc ? current_->doc->box_root() : nullptr; }

    Network& network() { return net_; }
    const Settings& settings() const { return settings_; }

private:
    struct LoadSpec;
    struct Page {
        ~Page();
        std::shared_ptr<PageState>                state;
        std::unique_ptr<ImageLoader>              images;
        engine::FontTextMeasurer                  measurer; // before doc: the document points at it
        std::unique_ptr<engine::Document>         doc;
        double                                    layout_ms = 0; // the worker's layout of it
        std::vector<std::shared_ptr<FormControl>> controls;
        std::vector<engine::FormInfo>             forms;
        std::unordered_map<const void*, FormControl*> by_node; // DOM element -> control
        std::string                               display_url;
        std::string                               requested_url;
        std::string                               base_url;
        std::string                               title;
        int                                       laid_w = 0, laid_h = 0; // viewport the layout was made for
        float                                     zoom = 1.f;              // the page zoom it was styled with
        bool                                      internal = false; // generated by Quill (errors, interstitials, about:)
        bool                                      insecure = false; // public page loaded over plain http
        // the sign-in prompt for HTTP authentication: what to load again once the user has given a username / password
        std::shared_ptr<LoadSpec>                 auth_retry;
        std::string                               auth_origin, auth_scheme, auth_realm;
        uint64_t                                  visited_gen = 0;  // VisitedLinks generation the styles were resolved with
        std::string                               base_target;      // <base target=...>
        double                                    refresh_delay = -1; // <meta http-equiv=refresh>: seconds (-1 = none)
        std::string                               refresh_url;        // ... and where to (absolute)
        std::shared_ptr<engine::FrameNode>        frameset;         // set for <frameset> documents
        std::vector<std::pair<std::string, std::vector<engine::MapArea>>> maps; // client-side image maps by name
        uint64_t                                  font_scope = 0;   // this page's web fonts (FontManager scope)
        std::string                               icon_url;         // <link rel=icon> (or /favicon.ico), "" for none
        engine::BoxScroll                         box_scroll;       // scroll positions of the page's scroll containers
    };
    struct Hit {
        FormControl*       ctl  = nullptr;
        const std::string* link = nullptr; // href attribute of the enclosing <a>
        int                cursor = 0;     // Style::cursor
        FormControl*       label_ctl = nullptr; // control a click on the enclosing <label> would activate
        const std::string* target = nullptr;    // target attribute of the enclosing <a>
        const std::string* rel = nullptr;       // rel attribute of the enclosing <a>
        const void*        node = nullptr;      // DOM element under the pointer (for :hover)
        int                ismap_x = -1, ismap_y = -1; // a server-side image map (<a><img ismap>): where in the image
    };
    struct LoadResult {
        int                   id = 0;
        std::shared_ptr<Page> page;
        bool                  final_post = false; // the displayed page is the response to a POST (no redirect to GET)
    };
    // Everything needed to (re)issue a navigation.
    struct LoadSpec {
        std::string url;
        std::string method = "GET";   // "GET" or "POST"
        std::string body, content_type;
        std::string initiator;        // page the form was on ("" for typed / clicked-link navigation)
        std::string source_page;      // page whose link was followed ("" if typed); relaxes only the http warning
        bool        no_auth_prompt = false; // a 401 shows the site's own page (the user cancelled the sign-in)
        bool        form_submit = false;
        bool        guessed_scheme = false; // typed bare LAN address: retry https if plain http is refused
        bool        confirm_resend = false; // show the "resend form data?" page instead of sending a POST
        bool        embedded = false;       // an <iframe>'s page: fetched as a subresource of `initiator` (private
                                            // network / mixed content rules) and only shown if the site allows framing
        std::string srcdoc;                 // <iframe srcdoc>: the page's HTML (url is about:srcdoc)
        std::string referrer, referrer_policy; // the page this navigation comes from (Referer, Sec-Fetch-Site)
        bool        no_referrer = false;       // <a rel=noreferrer>
        bool        user_activated = true;     // false: a <meta refresh> (no Sec-Fetch-User)
    };
    struct HistEntry {
        std::string url;
        std::vector<std::string> frames; // frameset page: what each <frame> showed (in frame order)
        int         scroll = 0;
        bool        post   = false; // the page came from a form POST
        std::string body, content_type;
        std::string referrer, referrer_policy; // sent again on reload / back
        bool        no_referrer = false;
    };

    void navigate_url(const std::string& url, bool from_page);
    void navigate_spec(const LoadSpec& spec);                 // new history entry + load
    void load_entry(int idx, int restore_scroll);             // history entry -> load (POST entries ask first)
    void start_load(const LoadSpec& spec, int restore_scroll);
    void load_job(std::shared_ptr<PageState> st, LoadSpec spec, int id, int vw, int vh);
    // @font-face fonts the page uses: fetched (or taken from the caches) in parallel; the ones that arrive within a
    // short wait are in place before the first layout, later ones relayout the page when they come.
    void load_web_fonts(uint64_t scope, const std::vector<engine::FontFaceRule>& faces, const std::string& page_url,
                        const std::shared_ptr<PageState>& st);
    void install(const std::shared_ptr<LoadResult>& lr);
    bool push_state(); // hover / focus / active into the document (false: unchanged, or deferred while the engine is busy)
    bool relayout(bool restyle, bool blocking, bool layout = true); // false if the engine was busy (non-blocking only)
    void map_boxes(const engine::Box& b);
    void scroll_to_fragment(const std::string& frag);
    void set_scroll(int y);                       // this document's own scroll position (never routed to a frame)
    void apply_viewport(int w, int h);
    Hit  hit_at(int x, int y) const;
    std::string resolve_href(const std::string& href) const;
    int  max_scroll() const;

    Settings                        settings_;
    std::function<void()>           wake_;
    std::shared_ptr<Network>        net_owner_; // shared with child frames
    Network&                        net_;
    std::unique_ptr<ThreadPool>     pool_;     // images, fonts, favicons
    std::unique_ptr<ThreadPool>     doc_pool_; // page loads (and freeing the page left behind): never queued behind images

    std::mutex                      mu_;
    std::shared_ptr<LoadResult>     ready_;

    std::shared_ptr<Page>           current_;
    std::shared_ptr<PageState>      loading_state_;
    bool                            loading_ = false;
    std::string                     loading_url_;
    int                             load_id_ = 0;
    int                             restore_scroll_ = 0;

    std::vector<HistEntry>          hist_;
    int                             idx_ = -1;
    std::string                     pending_nav_, hover_url_, cursor_ = "auto", down_link_;

    // form interaction (UI thread only)
    std::shared_ptr<FormControl>    focus_, pressed_, popup_;
    bool                            press_consumed_ = false, hover_pending_ = false;
    const void*                     active_node_ = nullptr; // element under the pressed mouse button (:active)
    const void*                     down_node_ = nullptr;   // element the mouse button went down on (<summary> clicks)
    bool                            restate();               // pushes focus / :active into the document; true if redraw needed
    int                             popup_first_ = 0, popup_hover_ = -1;
    int                             mouse_x_ = -1, mouse_y_ = -1; // content-area coordinates
    std::vector<std::shared_ptr<FormControl>> live_controls() const;
    std::shared_ptr<FormControl>    control_at(float dx, float dy) const;
    void focus_control(const std::shared_ptr<FormControl>& c, bool scroll_into_view);
    void focus_next(bool back);
    void activate_control(const std::shared_ptr<FormControl>& c);
    void submit_form(const std::shared_ptr<FormControl>& ctl, const std::shared_ptr<FormControl>& button);
    bool popup_geometry(IRect& box, int& rows, int& row_h) const; // popup box in content coordinates
    void close_popup() { popup_.reset(); popup_hover_ = -1; }

    int  vw_ = 1000, vh_ = 700;
    int  scroll_ = 0, doc_h_ = 0;
    std::chrono::steady_clock::time_point last_layout_;
    // animated images (GIF): the clock starts when a page is installed; draw() records when the next frame is due
    std::chrono::steady_clock::time_point anim_start_, anim_due_;
    long long anim_clock_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - anim_start_).count();
    }
    bool                            anim_pending_ = false;
    // <meta http-equiv="refresh">: armed when such a page is installed, cancelled by any other navigation
    std::chrono::steady_clock::time_point refresh_due_;
    bool                            refresh_pending_ = false;

    // ---- rendered-page cache: the page painted into 256 px tall strips (document coordinates, without fixed boxes).
    // Scrolling copies strips instead of painting; idle time paints the strips around the screen ahead of time.
    struct Tile {
        std::unique_ptr<Framebuffer>          fb;
        uint64_t                              gen      = 0;     // == paint_gen_ while current
        bool                                  animated = false; // shows an animated GIF: repainted when its frame changes
        std::chrono::steady_clock::time_point anim_due;
    };
    static constexpr int                  kTileH = 256;
    std::unordered_map<int, Tile>         tiles_;          // by strip index (y / kTileH)
    uint64_t                              paint_gen_ = 1;   // bumped whenever the page's pixels may have changed
    int                                   tile_w_    = 0;
    const void*                           tile_page_ = nullptr;
    bool                                  has_fixed_ = false; // page has position:fixed boxes (painted per frame)
    bool                                  prerender_pending_ = false;
    std::chrono::steady_clock::time_point last_draw_;
    void                                  invalidate_tiles() { paint_gen_++; }
    bool                                  use_tiles() const { return settings_.render_cache_mb > 0; }
    size_t                                tile_budget() const;
    engine::PaintEnv                      make_env(Page* pg);
    Tile&                                 tile(int ty, Page* pg, std::chrono::steady_clock::time_point now);
    void                                  draw_tiles(Framebuffer& fb, const IRect& area, Page* pg,
                                                     std::chrono::steady_clock::time_point now);
    int                                   next_prerender_tile() const; // -1 = nothing left to paint ahead
    void                                  evict_tiles();

    // ---- frames. A <frameset> page shows each <frame> as a child Browser that shares this one's Network (http
    // allowances, LAN certificate trust) but has its own small thread pool, so destroying a frame joins only its own
    // workers. The child does its own loading, layout, painting and forms; this Browser routes input to it.
    Browser(Browser* parent, const std::string& name, int scrolling);
    struct Frame {
        std::string              name, src;
        IRect                    rect;   // in this browser's content-area coordinates
        std::unique_ptr<Browser> br;
    };
    Browser*                    parent_ = nullptr;
    std::string                 frame_name_;
    bool                        no_scroll_ = false; // <frame scrolling=no>
    std::vector<Frame>          frames_;
    std::vector<IRect>          frame_gaps_;  // the borders between frames
    int                         hover_frame_ = -1, active_frame_ = -1, press_frame_ = -1;

    Browser*       top() { Browser* b = this; while (b->parent_) b = b->parent_; return b; }
    Browser*       find_frame(const std::string& name);
    Browser*       resolve_target(const std::string& target); // where a link / form with this target loads
    void           follow_link(const std::string& url, const std::string& target, bool no_referrer = false);
    void           set_referrer(LoadSpec& sp) const; // the page on screen refers this navigation
    void           preconnect_link(const std::string& url);
    void           navigate_target(const std::string& target, const LoadSpec& sp);
    void           build_frames();
    void           layout_frames();
    void           walk_frames(const engine::FrameNode& n, IRect r, int border, bool show_border, size_t& idx, bool create);
    int            frame_at(int x, int y) const;
    Browser*       scroll_frame() const;    // frame that receives scrolling
    Browser*       key_frame() const;       // frame that receives keys / typed text
    std::string    frame_url() const;       // what this (frame) browser shows or is loading
    void           load_frame(const std::string& url, const std::string& source_page); // no history entry upstream
    void           frame_navigated();       // a child frame navigated: becomes a history entry here
    std::function<void(const NewTab&)> open_tab_;
    int                                layout_count_ = 0;
    const void*                        hover_want_    = nullptr; // the node under the mouse, for :hover
    bool                               state_pending_ = false;   // hover / focus state waiting for the engine lock
    double                             last_layout_ms_ = 0; // how long the last layout took (spaces out image relayouts)
    // text selection
    engine::TextIndex                  tindex_;
    const void*                        tindex_page_ = nullptr;
    int                                tindex_layout_ = -1, tindex_scroll_ = -1;
    uint64_t                           tindex_gen_ = ~0ull;
    uint64_t                           box_scroll_gen_ = 0, tindex_bs_ = ~0ull;
    engine::TextPoint                  sel_a_, sel_b_; // anchor and focus
    size_t                             sel_ord_a_ = 0, sel_ord_b_ = 0, sel_boxes_ = 0; // the same, by text box order
    uint64_t                           sel_gen_ = 0;       // the box tree they point into
    bool                               sel_fresh_ = false; // just set from the current tree
    bool                               selecting_ = false;
    engine::TextSelection              sel_map_;
    const engine::TextIndex& text_index();
    void                     refresh_selection();
    std::atomic<float>                 zoom_{1.f}; // the shown page's zoom (load jobs read it from other threads)
    float zoom_for(const std::string& url) const; // the zoom a page at this address gets
    void  apply_zoom(float z);                    // lays the page (and its frames) out again at z
    void  step_zoom(int dir);
    // Dragging a scroll bar: the page's (node null) or a scroll container's
    struct ScrollDrag {
        bool        on = false;
        const void* node = nullptr;
        int         start_y = 0;
        float       start_scroll = 0, per_px = 0; // scroll moved per pixel of mouse movement
    } drag_;
    bool scroll_drag_start(int x, int y);
    bool scroll_drag_move(int y);
    // Moves the innermost scroll container under the mouse that can go that way. True if one moved.
    bool scroll_box(float dx, float dy);
    std::string                        notice_; // shown in the status bar when nothing else is (a form that couldn't be sent)

    // ---- site icon: fetched on a worker after a page is installed, cached in memory (all tabs) and on disk
    struct IconSlot {
        std::mutex                   mu;
        std::string                  url; // the icon wanted now
        std::shared_ptr<const Image> img;
        bool                         fresh = false; // arrived since the last poll()
    };
    std::shared_ptr<IconSlot> icon_ = std::make_shared<IconSlot>();
    void                      start_favicon(const std::string& icon_url, const std::string& page_url);

    // ---- inline frames. Each <iframe> box shows a child Browser (like a <frame>), painted into the box by the page
    // painter (so it scrolls, clips and stacks with the page). The child paints directly (no tile cache of its own);
    // this Browser's tiles hold the result and are repainted where a frame changed.
    struct InlineFrame {
        const void*              node = nullptr; // the <iframe> element
        std::string              src;            // resolved URL ("about:srcdoc" for srcdoc)
        IRect                    rect;           // content box, document coordinates
        std::unique_ptr<Browser> br;
    };
    std::vector<InlineFrame>    iframes_;
    int                         hover_iframe_ = -1, active_iframe_ = -1, press_iframe_ = -1;
    bool                        is_iframe_ = false;
    void           sync_iframes();                       // after a layout: create / move / drop the frames
    int            iframe_at(int x, int y) const;        // content-area point -> iframe index, or -1
    void           invalidate_doc_rect(const IRect& r);  // repaint the cached strips under a document rectangle
    Browser*       hover_child() const { return hover_iframe_ >= 0 ? iframes_[static_cast<size_t>(hover_iframe_)].br.get() : nullptr; }
    bool           go_to_entry(int from, int to); // back/forward within a frameset: reload only the frames that differ
};

} // namespace quill
