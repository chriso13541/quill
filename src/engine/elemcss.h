// Per-element cascade for the parts libcss cannot do.
//
//  * Custom properties scoped to arbitrary selectors (.btn-primary { --bg: blue }, [data-theme=dark] { ... }):
//    the cascade and inheritance of custom properties is evaluated per element here, and every declaration
//    that uses such a variable is resolved for each element it applies to. Resolved declarations are fed to
//    libcss as generated rules keyed on marker classes (qvx<N>) added to the elements.
//  * border-radius and box-shadow, which libcss does not know: cascaded here (selectors, specificity,
//    !important, style="") and handed to the box builder as Extras.
//
// Needs its own selector matcher (complex selectors, attribute operators, structural pseudo-classes, :not/:is).
// Free of libdom/libcss types: document.cpp describes the element tree with ENode.
#pragma once

#include "cssvars.h"
#include "style.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace quill::engine {

struct ENode {
    std::string tag; // lower-case
    std::vector<std::pair<std::string, std::string>> attrs;
    int parent = -1; // index of the parent element, -1 for the root. Nodes are in document order.
    bool hover = false, active = false, focus = false, focus_within = false; // :hover :active :focus :focus-within
};

struct Extras {
    bool                   has_radius = false;
    Radius                 radius[4];
    bool                   has_shadow = false;
    std::vector<BoxShadow> shadows;
    bool                   has_mask = false;
    Mask                   mask;
    bool                   has_bgsize = false;
    SizeSpec               bg_size;
    std::vector<std::pair<std::string, std::string>> grid; // cascaded grid / alignment declarations (name, value), later wins
    bool                   has_transform = false; // translate()/translateX/Y only; px, em and % parts
    float                  tx_px = 0, tx_em = 0, tx_pct = 0, ty_px = 0, ty_em = 0, ty_pct = 0;
    std::string            svg_style; // cascaded SVG presentation properties (fill, stroke, ...) as "name:value;..."
    bool                   has_appearance = false, appearance_none = false; // appearance / -webkit-appearance: none
    // gradients in background-image / the background shorthand (libcss drops those declarations entirely)
    bool                   has_gradients = false, gradient_over_image = false, gradient_hides_url = false;
    std::vector<Gradient>  gradients;
    bool                   has_bg_color = false, bg_color_current = false; // the colour of a gradient shorthand
    uint32_t               bg_color = 0;
    // transform beyond translate: linear part (x' = a x + c y, y' = b x + d y); matrix() / rotate / scale / skew
    bool                   has_xform = false;
    float                  xf[4] = {1, 0, 0, 1};
    bool                   has_origin = false;
    Length                 origin_x = Length::pct(50), origin_y = Length::pct(50);
    // the individual rotate / scale properties (CSS Transforms 2), applied before transform
    bool                   has_rotate = false, has_scale = false;
    float                  rotate_deg = 0, scale_x = 1, scale_y = 1;
};

class ElementCascade {
public:
    ElementCascade(const CssVars& vars, std::vector<ENode> nodes);
    ~ElementCascade();

    void add_sheet(const std::string& css, const std::string& base = std::string()); // original text, document order (imports first); base resolves relative url()s
    void run();

    // Interaction state (:hover :active :focus :focus-within): replaces the per-node flags (document order, like the nodes;
    // bits 1 hover, 2 active, 4 focus, 8 focus-within) and reports whether any selector that mentions one of them now
    // matches a different set of elements (so styles must be recomputed).
    // Returns bit 0: some selector now matches a different set of elements; bit 1: such a selector belongs to a rule with
    // var()/box-shadow/radius/... declarations. `affected` receives the (cascade-order) indices of the elements whose match changed.
    int  set_state_flags(const std::vector<unsigned char>& next, std::vector<int>& affected);
    // True if a custom-property / box-shadow / radius / ... rule depends on an interaction state (then the generated
    // per-element sheets must be rebuilt when the state changes, not only the styles).
    bool state_dependent_rules() const;
    // Recomputes marks / extras / environments for `nodes` and all their descendants (after a state change); returns that
    // full list of indices in document order.
    std::vector<int> recompute(const std::vector<int>& nodes);

    // CSS for the generated per-element rules (append after the author sheets).
    std::string variants_css() const;
    // Variant ids: node `n` gets the class qvx<id> for each.
    const std::vector<int>& marks(size_t n) const;
    const Extras*           extras(size_t n, int pseudo = 0) const; // pseudo: 0 element, 1 ::before, 2 ::after

    size_t rules = 0, variants = 0, deferred_applied = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Colour / length helpers (also used by tests).
bool parse_css_color(const std::string& text, uint32_t& argb, bool& current);

} // namespace quill::engine
