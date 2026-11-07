// HTML form controls on top of the engine.
//
// The engine lays out <input>/<textarea>/<select>/<button> boxes (intrinsic sizes come from the UA
// sheet and layout). FormControl keeps a control's live state (text, checked, selected option) and
// paints its content over the box through PaintEnv::control; Browser does focus, typing and submission.
#pragma once
#include "engine/box.h"
#include "engine/document.h"
#include "framebuffer.h"
#include "textedit.h"

#include <string>
#include <utility>
#include <vector>

namespace quill {

class FormControl {
public:
    enum class Kind { Text, Password, Checkbox, Radio, Submit, Reset, Button, Hidden, File, Select, Textarea };
    struct Option {
        std::string label, value;
        bool        selected = false; // the markup's `selected` (what reset() goes back to)
        bool        on = false;       // list box: currently selected
    };

    explicit FormControl(const engine::ControlInfo& ci);

    // ---- identity
    const void*  node = nullptr;   // DOM element
    const void*  form = nullptr;   // owning <form> element or null
    const engine::Box* box = nullptr; // current box (null if display:none); refreshed after every layout

    // ---- state
    Kind        kind = Kind::Text;
    std::string tag_name, name, id, placeholder, initial_value;
    bool        disabled = false, readonly = false, is_button_element = false, initially_checked = false, has_value_attr = false;
    int         size_attr = 20, rows = 2, cols = 20, maxlength = 0;
    TextEdit    edit;              // Text / Password / Textarea content
    bool        checked = false;   // Checkbox / Radio
    std::vector<Option> options;   // Select
    int         selected = -1;     // Select: index into options (a multiple list box: the last one clicked)
    bool        listbox = false;   // Select shown as a list (size > 1 or multiple) rather than a dropdown
    bool        multiple = false;  // Select / File: more than one may be chosen
    bool        autofocus = false; // focused when the page is shown
    int         list_rows = 1;     // list box: visible rows
    int         list_top = 0;      // list box: first visible option
    int         anchor = -1;       // list box: where shift+click ranges start
    std::string accept;            // File: accept attribute
    std::vector<std::string> files; // File: chosen paths

    bool editable() const { return !disabled && !readonly && (kind == Kind::Text || kind == Kind::Password || kind == Kind::Textarea); }
    bool focusable() const { return !disabled && kind != Kind::Hidden; }
    // The value submitted for this control.
    std::string submit_value() const;
    void        reset();
    void        select_option(int i);
    // The values submitted (a multiple list box can send several; others send submit_value() once).
    std::vector<std::string> submit_values() const;
    // List box: option under a document y coordinate (-1 outside), a click there, scrolling.
    int  listbox_option_at(float doc_y) const;
    void listbox_click(int i, bool ctrl, bool shift);
    bool listbox_scroll(int rows); // true if it moved
    void listbox_reveal(int i);
    // File: the label shown next to the button.
    std::string file_label() const;

    // Border box in document coordinates. False if the control is not displayed.
    bool border_box(float& x, float& y, float& w, float& h) const;
    // Paints the content of text fields, textareas and selects. (ox, oy) is where document (0,0) lands on the framebuffer.
    bool paint(Framebuffer& fb, int ox, int oy, bool focused) const;
    // Caret position from a click: `px`/`py` relative to the content box.
    void set_caret_from_point(float px, float py);
    // Content box (document coordinates).
    bool content_box(float& x, float& y, float& w, float& h) const;

private:
    std::string shown_text_range(size_t from, size_t to) const;
    float       fsize() const;
};

using FormFields = std::vector<std::pair<std::string, std::string>>;
// application/x-www-form-urlencoded (newlines become CRLF as browsers do).
std::string form_urlencode(const FormFields& f);

// A file input's entry in a submission: `path` "" for an input with nothing chosen. Goes before fields[at].
struct FormFile {
    std::string name, path;
    size_t      at = 0;
};
// multipart/form-data: fields and files in document order. Files are read now (100 MB in all at most). Sets the body
// and the Content-Type (with its boundary). False with a reason if a file can't be read or the total is too big.
bool form_multipart(const FormFields& fields, const std::vector<FormFile>& files, std::string& body, std::string& content_type,
                    std::string* err = nullptr, const std::string& boundary_seed = std::string());
// A MIME type from a file name's extension ("application/octet-stream" if unknown).
std::string mime_for_filename(const std::string& name);

} // namespace quill
