// The native "choose a file" dialog for <input type=file>. Linux / BSD: zenity or kdialog (whichever is installed;
// both are run without a shell, with arguments Quill built, and only their stdout is read). Windows: the common
// GetOpenFileNameW dialog. Blocks until the dialog closes.
#pragma once

#include <string>
#include <vector>

namespace quill {

// `accept` is the input's accept attribute ("image/*,.pdf"); it becomes the dialog's filter (with "All files" too).
// Returns the chosen paths: empty if cancelled or if no dialog program could be run (then `err` says why).
std::vector<std::string> pick_files(bool multiple, const std::string& accept, std::string* err = nullptr);

// The glob patterns ("*.png") an accept attribute stands for. Exposed for tests.
std::vector<std::string> accept_patterns(const std::string& accept);

} // namespace quill
