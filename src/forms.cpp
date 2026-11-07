#include "forms.h"

#include "font.h"
#include "url.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace quill {

namespace {

Color from_argb(uint32_t c) { return Color((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, c >> 24); }

} // namespace

FormControl::FormControl(const engine::ControlInfo& ci) {
    autofocus = ci.autofocus;
    node          = ci.node;
    form          = ci.form;
    tag_name      = ci.tag;
    name          = ci.name;
    id            = ci.id;
    placeholder   = ci.placeholder;
    disabled      = ci.disabled;
    readonly      = ci.readonly;
    size_attr     = ci.size;
    rows          = ci.rows;
    cols          = ci.cols;
    maxlength     = ci.maxlength;
    initial_value = ci.value;
    has_value_attr = ci.has_value;
    initially_checked = ci.checked;
    const std::string& type = ci.type;
    if (ci.tag == "textarea") {
        kind           = Kind::Textarea;
        edit.multiline = true;
        edit.set(ci.text);
    } else if (ci.tag == "select") {
        kind = Kind::Select;
        for (auto& o : ci.options) {
            Option op;
            op.label    = o.label;
            op.value    = o.value;
            op.selected = o.selected;
            options.push_back(std::move(op));
        }
        multiple  = ci.multiple;
        listbox   = ci.multiple || ci.list_size > 1;
        list_rows = ci.list_size >= 1 ? std::min(ci.list_size, 64) : (listbox ? 4 : 1);
        reset();
    } else if (ci.tag == "button") {
        is_button_element = true;
        kind = type == "button" ? Kind::Button : type == "reset" ? Kind::Reset : Kind::Submit;
    } else { // input
        if (type == "password") kind = Kind::Password;
        else if (type == "checkbox") kind = Kind::Checkbox;
        else if (type == "radio") kind = Kind::Radio;
        else if (type == "submit" || type == "image") kind = Kind::Submit;
        else if (type == "reset") kind = Kind::Reset;
        else if (type == "button") kind = Kind::Button;
        else if (type == "hidden") kind = Kind::Hidden;
        else if (type == "file") { kind = Kind::File; multiple = ci.multiple; accept = ci.accept; }
        else kind = Kind::Text; // text, search, email, url, tel, number, ... and unknown types
    }
    if (kind == Kind::Text || kind == Kind::Password) edit.set(initial_value);
    checked = initially_checked;
}

std::string FormControl::submit_value() const {
    switch (kind) {
    case Kind::Text:
    case Kind::Password:
    case Kind::Textarea: return edit.text;
    case Kind::Hidden: return initial_value;
    case Kind::Checkbox:
    case Kind::Radio: return has_value_attr ? initial_value : "on";
    case Kind::Select: return selected >= 0 && selected < static_cast<int>(options.size()) ? options[static_cast<size_t>(selected)].value : "";
    default: return initial_value;
    }
}

void FormControl::reset() {
    if (kind == Kind::Text || kind == Kind::Password) edit.set(initial_value);
    else if (kind == Kind::Checkbox || kind == Kind::Radio) checked = initially_checked;
    else if (kind == Kind::Select) {
        int sel = -1;
        for (size_t i = 0; i < options.size(); i++) {
            options[i].on = options[i].selected;
            if (options[i].selected) sel = static_cast<int>(i);
        }
        if (!multiple) // one selection at most: the last `selected` wins; a dropdown (not a list box) falls back to the first
            for (size_t i = 0; i < options.size(); i++) options[i].on = static_cast<int>(i) == sel;
        selected = options.empty() ? -1 : (sel >= 0 ? sel : (listbox ? -1 : 0));
        if (!listbox && selected >= 0) options[static_cast<size_t>(selected)].on = true;
        anchor   = selected;
        list_top = 0;
        if (listbox && selected >= list_rows) list_top = selected - list_rows + 1;
    } else if (kind == Kind::File) files.clear();
    // textarea: the original text is not kept; resetting leaves it as typed (rare)
}

void FormControl::select_option(int i) {
    if (i < 0 || i >= static_cast<int>(options.size())) return;
    selected = i;
    for (size_t k = 0; k < options.size(); k++) options[k].on = static_cast<int>(k) == i;
    anchor = i;
    if (listbox) listbox_reveal(i);
}

std::vector<std::string> FormControl::submit_values() const {
    std::vector<std::string> out;
    if (kind == Kind::Select) {
        for (auto& o : options)
            if (o.on) out.push_back(o.value);
        return out;
    }
    out.push_back(submit_value());
    return out;
}

int FormControl::listbox_option_at(float doc_y) const {
    float cx, cy, cw, ch;
    if (!listbox || !content_box(cx, cy, cw, ch) || ch <= 0) return -1;
    float rh = ch / static_cast<float>(std::max(1, list_rows));
    if (doc_y < cy || doc_y >= cy + ch) return -1;
    int i = list_top + static_cast<int>((doc_y - cy) / rh);
    return i < static_cast<int>(options.size()) ? i : -1;
}

void FormControl::listbox_click(int i, bool ctrl, bool shift) {
    int n = static_cast<int>(options.size());
    if (i < 0 || i >= n || disabled) return;
    if (!multiple || (!ctrl && !shift)) { select_option(i); return; }
    if (shift && anchor >= 0) {
        int a = std::min(anchor, i), b = std::max(anchor, i);
        if (!ctrl)
            for (auto& o : options) o.on = false;
        for (int k = a; k <= b; k++) options[static_cast<size_t>(k)].on = true;
    } else {
        options[static_cast<size_t>(i)].on = !options[static_cast<size_t>(i)].on;
        anchor = i;
    }
    selected = i;
    listbox_reveal(i);
}

bool FormControl::listbox_scroll(int d) {
    int mx = std::max(0, static_cast<int>(options.size()) - list_rows);
    int t  = std::max(0, std::min(mx, list_top + d));
    if (t == list_top) return false;
    list_top = t;
    return true;
}

void FormControl::listbox_reveal(int i) {
    if (i < list_top) list_top = i;
    else if (i >= list_top + list_rows) list_top = i - list_rows + 1;
    listbox_scroll(0);
}

std::string FormControl::file_label() const {
    if (files.empty()) return multiple ? "No files selected." : "No file selected.";
    if (files.size() > 1) return std::to_string(files.size()) + " files";
    std::string f = files[0];
    size_t      s = f.find_last_of("/\\");
    return s == std::string::npos ? f : f.substr(s + 1);
}

float FormControl::fsize() const {
    float v = box && box->style ? box->style->font_size : 13.0f;
    return v > 0 ? v : 13.0f;
}

bool FormControl::border_box(float& x, float& y, float& w, float& h) const {
    if (!box || kind == Kind::Hidden || box->skipped) return false;
    x = box->x;
    y = box->y;
    w = box->w;
    h = box->h;
    return w > 0 && h > 0;
}

bool FormControl::content_box(float& x, float& y, float& w, float& h) const {
    if (!box) return false;
    x = box->x + box->border.left + box->padding.left;
    y = box->y + box->border.top + box->padding.top;
    w = box->w - box->border.left - box->border.right - box->padding.left - box->padding.right;
    h = box->h - box->border.top - box->border.bottom - box->padding.top - box->padding.bottom;
    return true;
}

bool FormControl::paint(Framebuffer& fb, int ox, int oy, bool focused) const {
    if (!box || !box->style) return false;
    if (kind != Kind::Text && kind != Kind::Password && kind != Kind::Textarea && kind != Kind::Select && kind != Kind::File) return false;
    if (box->style->visibility != engine::Visibility::Visible) return true;
    float cx, cy, cw, ch;
    if (!content_box(cx, cy, cw, ch)) return false;
    IRect content(ox + static_cast<int>(std::lround(cx)), oy + static_cast<int>(std::lround(cy)), static_cast<int>(std::lround(cw)),
                  static_cast<int>(std::lround(ch)));
    IRect bb(ox + static_cast<int>(std::lround(box->x)), oy + static_cast<int>(std::lround(box->y)), static_cast<int>(std::lround(box->w)),
             static_cast<int>(std::lround(box->h)));
    Color col = from_argb(box->style->color);
    if (disabled) col = Color(120, 120, 120);
    Font* f = FontManager::instance().get("sans-serif", fsize(), 400, false);
    if (!f) return true;

    switch (kind) {
    case Kind::Text:
    case Kind::Password: {
        std::string shown = shown_text_range(0, edit.text.size());
        fb.push_clip(content);
        int base = content.y + (content.h + f->ascent - f->descent) / 2;
        if (shown.empty() && !focused && !placeholder.empty()) {
            f->draw(fb, placeholder, content.x, base, Color(140, 140, 140));
        } else {
            std::string before  = shown_text_range(0, std::min(edit.cursor, edit.text.size()));
            int         caret_x = static_cast<int>(f->text_width(before));
            int         off     = focused && caret_x > content.w - 2 ? caret_x - (content.w - 2) : 0;
            if (focused && edit.select_all && !shown.empty())
                fb.fill_rect(IRect(content.x - off, content.y, static_cast<int>(f->text_width(shown)), content.h), Color(180, 210, 255));
            f->draw(fb, shown, content.x - off, base, col);
            if (focused && !edit.select_all) fb.fill_rect(IRect(content.x - off + caret_x, content.y + 1, 1, std::max(1, content.h - 2)), col);
        }
        fb.pop_clip();
        break;
    }
    case Kind::Textarea: {
        fb.push_clip(content);
        int    lh = std::max(1, f->line_height);
        size_t cl = 0;
        for (size_t i = 0; i < edit.cursor && i < edit.text.size(); i++)
            if (edit.text[i] == '\n') cl++;
        int vis   = std::max(1, content.h / lh);
        int first = focused && static_cast<int>(cl) >= vis ? static_cast<int>(cl) - vis + 1 : 0;
        if (edit.text.empty() && !focused && !placeholder.empty()) {
            f->draw(fb, placeholder, content.x, content.y + f->ascent, Color(140, 140, 140));
        } else {
            size_t ls = 0;
            int    li = 0;
            while (ls <= edit.text.size()) {
                size_t le = edit.text.find('\n', ls);
                if (le == std::string::npos) le = edit.text.size();
                if (li >= first && li < first + vis + 1) {
                    int y = content.y + (li - first) * lh;
                    if (focused && edit.select_all)
                        fb.fill_rect(IRect(content.x, y, static_cast<int>(f->text_width(edit.text.substr(ls, le - ls))), lh), Color(180, 210, 255));
                    f->draw(fb, edit.text.substr(ls, le - ls), content.x, y + f->ascent, col);
                    if (focused && !edit.select_all && edit.cursor >= ls && edit.cursor <= le && static_cast<size_t>(li) == cl) {
                        int cxp = static_cast<int>(f->text_width(edit.text.substr(ls, edit.cursor - ls)));
                        fb.fill_rect(IRect(content.x + cxp, y, 1, lh), col);
                    }
                }
                li++;
                if (le >= edit.text.size()) break;
                ls = le + 1;
            }
        }
        fb.pop_clip();
        break;
    }
    case Kind::Select:
        if (listbox) {
            fb.push_clip(content);
            int  n  = static_cast<int>(options.size());
            bool sb = n > list_rows; // a scroll bar along the right edge
            int  rw = content.w - (sb ? 10 : 0);
            float rh = static_cast<float>(content.h) / static_cast<float>(std::max(1, list_rows));
            for (int r = 0; r < list_rows && list_top + r < n; r++) {
                const Option& o  = options[static_cast<size_t>(list_top + r)];
                int           y0 = content.y + static_cast<int>(std::lround(r * rh)), y1 = content.y + static_cast<int>(std::lround((r + 1) * rh));
                Color         tc = col;
                if (o.on) {
                    fb.fill_rect(IRect(content.x, y0, rw, y1 - y0), focused ? Color(30, 100, 220) : Color(200, 200, 200));
                    tc = focused ? Color(255, 255, 255) : Color(0, 0, 0);
                }
                f->draw(fb, o.label, content.x + 2, y0 + (y1 - y0 + f->ascent - f->descent) / 2, tc);
            }
            if (sb) {
                IRect track(content.right() - 10, content.y, 10, content.h);
                fb.fill_rect(track, Color(235, 235, 235));
                int th = std::max(12, content.h * list_rows / n);
                int ty = content.y + (content.h - th) * list_top / std::max(1, n - list_rows);
                fb.fill_rect(IRect(track.x + 2, ty + 1, 6, th - 2), Color(160, 160, 160));
            }
            fb.pop_clip();
            break;
        } else {
        fb.push_clip(bb);
        std::string lab = selected >= 0 && selected < static_cast<int>(options.size()) ? options[static_cast<size_t>(selected)].label : "";
        f->draw(fb, lab, content.x, content.y + (content.h + f->ascent - f->descent) / 2, col);
        if (!(box && box->style && box->style->appearance_none)) { // appearance:none pages draw their own arrow
            int ax = bb.right() - 14, ay = bb.y + bb.h / 2 - 2;
            for (int i = 0; i < 4; i++) fb.fill_rect(IRect(ax + i, ay + i, 9 - 2 * i, 1), col); // little down arrow
        }
        fb.pop_clip();
        break;
        }
    case Kind::File: {
        // [ Browse… ] name-of-file.txt
        fb.push_clip(bb);
        int bw = static_cast<int>(f->text_width("Browse\xe2\x80\xa6")) + 16;
        IRect btn(bb.x, bb.y, std::min(bw, bb.w), bb.h);
        fb.fill_rect(btn, disabled ? Color(235, 235, 235) : Color(225, 225, 228));
        fb.fill_rect(IRect(btn.x, btn.y, btn.w, 1), Color(150, 150, 150));
        fb.fill_rect(IRect(btn.x, btn.bottom() - 1, btn.w, 1), Color(150, 150, 150));
        fb.fill_rect(IRect(btn.x, btn.y, 1, btn.h), Color(150, 150, 150));
        fb.fill_rect(IRect(btn.right() - 1, btn.y, 1, btn.h), Color(150, 150, 150));
        if (focused) fb.fill_rect(IRect(btn.x + 2, btn.bottom() - 3, btn.w - 4, 1), Color(30, 100, 220));
        int base = bb.y + (bb.h + f->ascent - f->descent) / 2;
        f->draw(fb, "Browse\xe2\x80\xa6", btn.x + 8, base, col);
        f->draw(fb, file_label(), btn.right() + 6, base, col);
        fb.pop_clip();
        break;
    }
    default: break;
    }
    return true;
}

void FormControl::set_caret_from_point(float px, float py) {
    if (kind != Kind::Text && kind != Kind::Password && kind != Kind::Textarea) return;
    Font* f = FontManager::instance().get("sans-serif", fsize(), 400, false);
    if (!f) return;
    edit.select_all   = false;
    size_t line_begin = 0, line_stop = edit.text.size();
    if (kind == Kind::Textarea) {
        int    lh   = std::max(1, f->line_height);
        int    want = std::max(0, static_cast<int>(py) / lh);
        size_t ls   = 0;
        int    li   = 0;
        while (li < want) {
            size_t le = edit.text.find('\n', ls);
            if (le == std::string::npos) break;
            ls = le + 1;
            li++;
        }
        line_begin = ls;
        line_stop  = edit.line_end(ls);
    }
    size_t i = line_begin;
    while (i < line_stop) {
        size_t      n = edit.next_boundary(i);
        std::string a = shown_text_range(line_begin, i), b = shown_text_range(line_begin, n);
        float       w0 = f->text_width(a), w1 = f->text_width(b);
        if (px < (w0 + w1) / 2) break;
        i = n;
    }
    edit.cursor = i;
}

std::string FormControl::shown_text_range(size_t from, size_t to) const {
    if (kind != Kind::Password) return edit.text.substr(from, to - from);
    std::string out;
    size_t      i = from;
    while (i < to) {
        i = edit.next_boundary(i);
        out += "\xE2\x80\xA2";
    }
    return out;
}

std::string form_urlencode(const FormFields& f) {
    std::string out;
    for (auto& kv : f) {
        std::string v;
        for (size_t i = 0; i < kv.second.size(); i++) { // normalise newlines to CRLF
            char c = kv.second[i];
            if (c == '\r') {
                if (i + 1 < kv.second.size() && kv.second[i + 1] == '\n') i++;
                v += "\r\n";
            } else if (c == '\n') v += "\r\n";
            else v += c;
        }
        if (!out.empty()) out += '&';
        out += percent_encode(kv.first) + "=" + percent_encode(v);
    }
    return out;
}

std::string mime_for_filename(const std::string& name) {
    size_t dot = name.find_last_of('.');
    size_t sl  = name.find_last_of("/\\");
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return "application/octet-stream";
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, const char*> k[] = {
        {"txt", "text/plain"},        {"html", "text/html"},       {"htm", "text/html"},        {"css", "text/css"},
        {"csv", "text/csv"},          {"md", "text/markdown"},     {"xml", "application/xml"},  {"json", "application/json"},
        {"js", "text/javascript"},    {"png", "image/png"},        {"jpg", "image/jpeg"},       {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},         {"webp", "image/webp"},      {"svg", "image/svg+xml"},    {"bmp", "image/bmp"},
        {"ico", "image/x-icon"},      {"pdf", "application/pdf"},  {"zip", "application/zip"},  {"gz", "application/gzip"},
        {"tar", "application/x-tar"}, {"mp3", "audio/mpeg"},       {"ogg", "audio/ogg"},        {"wav", "audio/wav"},
        {"mp4", "video/mp4"},         {"webm", "video/webm"},      {"doc", "application/msword"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"odt", "application/vnd.oasis.opendocument.text"},       {"woff2", "font/woff2"},     {"ttf", "font/ttf"}};
    for (auto& p : k)
        if (e == p.first) return p.second;
    return "application/octet-stream";
}

namespace {
// Names and file names in Content-Disposition: quote-safe, as browsers do (" → %22, CR/LF → %0D/%0A).
std::string disp_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"') o += "%22";
        else if (c == '\r') o += "%0D";
        else if (c == '\n') o += "%0A";
        else o += c;
    }
    return o;
}
std::string crlf(const std::string& s) { // values: lone CR or LF become CRLF
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r') { o += "\r\n"; if (i + 1 < s.size() && s[i + 1] == '\n') i++; }
        else if (s[i] == '\n') o += "\r\n";
        else o += s[i];
    }
    return o;
}
} // namespace

bool form_multipart(const FormFields& fields, const std::vector<FormFile>& files, std::string& body, std::string& content_type,
                    std::string* err, const std::string& boundary_seed) {
    const size_t kCap = 100u << 20;
    // read the files first: the boundary must not occur in any of them
    std::vector<std::string> contents(files.size());
    size_t                   total = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (files[i].path.empty()) continue;
        std::ifstream in(files[i].path, std::ios::binary);
        if (!in) { if (err) *err = "Couldn't read " + files[i].path; return false; }
        in.seekg(0, std::ios::end);
        std::streamoff n = in.tellg();
        in.seekg(0);
        if (n < 0 || total + static_cast<size_t>(n) > kCap) { if (err) *err = "Files too big to upload (100 MB at most)"; return false; }
        contents[i].resize(static_cast<size_t>(n));
        if (n > 0 && !in.read(&contents[i][0], n)) { if (err) *err = "Couldn't read " + files[i].path; return false; }
        total += static_cast<size_t>(n);
    }
    std::string boundary;
    std::mt19937_64 rng(boundary_seed.empty() ? std::random_device{}() : std::hash<std::string>()(boundary_seed));
    for (int tries = 0;; tries++) {
        static const char* al = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        boundary = "----QuillFormBoundary";
        for (int i = 0; i < 16; i++) boundary += al[rng() % 62];
        bool clash = false;
        for (auto& c : contents)
            if (c.find(boundary) != std::string::npos) clash = true;
        for (auto& f : fields)
            if (f.second.find(boundary) != std::string::npos) clash = true;
        if (!clash || tries > 16) break;
    }
    body.clear();
    auto field = [&](const FormFields::value_type& f) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + disp_escape(crlf(f.first)) + "\"\r\n\r\n" + crlf(f.second) + "\r\n";
    };
    auto file = [&](size_t i) {
        const FormFile& f = files[i];
        size_t          sl = f.path.find_last_of("/\\");
        std::string     fn = sl == std::string::npos ? f.path : f.path.substr(sl + 1);
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + disp_escape(crlf(f.name)) + "\"; filename=\"" + disp_escape(fn) +
                "\"\r\nContent-Type: " + (f.path.empty() ? std::string("application/octet-stream") : mime_for_filename(fn)) + "\r\n\r\n";
        body += contents[i];
        body += "\r\n";
    };
    size_t fi = 0;
    for (size_t i = 0; i <= fields.size(); i++) {
        for (; fi < files.size() && files[fi].at == i; fi++) file(fi);
        if (i < fields.size()) field(fields[i]);
    }
    for (; fi < files.size(); fi++) file(fi);
    body += "--" + boundary + "--\r\n";
    content_type = "multipart/form-data; boundary=" + boundary;
    return true;
}

} // namespace quill
