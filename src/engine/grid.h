// CSS grid declarations -> Style::grid / Style::gitem. Input is the cascaded (name, value) list from elemcss
// with var() already substituted; later declarations win.
#pragma once
#include "style.h"

#include <string>
#include <utility>
#include <vector>

namespace quill::engine {

void apply_grid_props(Style& st, const std::vector<std::pair<std::string, std::string>>& props);

// Test hooks
bool parse_grid_track_list(const std::string& v, GTrackList& out);

} // namespace quill::engine
