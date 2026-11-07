#include "filepick.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace quill {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
bool safe_ext(const std::string& e) { // what goes into a pattern: letters, digits, - and +
    if (e.empty() || e.size() > 16) return false;
    for (char c : e)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '+' && c != '_') return false;
    return true;
}
} // namespace

std::vector<std::string> accept_patterns(const std::string& accept) {
    static const std::pair<const char*, const char*> kTypes[] = {
        {"image/*", "png jpg jpeg gif webp bmp svg ico avif"},
        {"audio/*", "mp3 ogg oga wav flac m4a opus"},
        {"video/*", "mp4 webm mkv mov avi ogv"},
        {"text/*", "txt html htm css csv md"},
        {"image/png", "png"}, {"image/jpeg", "jpg jpeg"}, {"image/gif", "gif"}, {"image/webp", "webp"},
        {"image/svg+xml", "svg"}, {"application/pdf", "pdf"}, {"text/plain", "txt"}, {"text/csv", "csv"},
        {"text/html", "html htm"}, {"application/json", "json"}, {"application/zip", "zip"},
    };
    std::vector<std::string> out;
    auto add = [&](const std::string& e) {
        std::string p = "*." + e;
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    size_t i = 0;
    while (i <= accept.size()) {
        size_t      j = accept.find(',', i);
        if (j == std::string::npos) j = accept.size();
        std::string t = lower(trim(accept.substr(i, j - i)));
        i = j + 1;
        if (t.empty()) continue;
        if (t[0] == '.') {
            if (safe_ext(t.substr(1))) add(t.substr(1));
            continue;
        }
        for (auto& k : kTypes)
            if (t == k.first) {
                std::string list = k.second;
                size_t      s = 0;
                while (s < list.size()) {
                    size_t e = list.find(' ', s);
                    if (e == std::string::npos) e = list.size();
                    add(list.substr(s, e - s));
                    s = e + 1;
                }
            }
    }
    return out;
}

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}
} // namespace

std::vector<std::string> pick_files(bool multiple, const std::string& accept, std::string* err) {
    std::vector<std::string> pats = accept_patterns(accept);
    std::wstring             filter;
    if (!pats.empty()) {
        std::string joined;
        for (auto& p : pats) joined += (joined.empty() ? "" : ";") + p;
        filter += widen("Accepted files (" + joined + ")");
        filter += L'\0';
        filter += widen(joined);
        filter += L'\0';
    }
    filter += L"All files";
    filter += L'\0';
    filter += L"*.*";
    filter += L'\0';
    filter += L'\0';
    std::vector<wchar_t> buf(1 << 16, L'\0');
    OPENFILENAMEW        ofn;
    std::memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile   = buf.data();
    ofn.nMaxFile    = static_cast<DWORD>(buf.size());
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | (multiple ? OFN_ALLOWMULTISELECT : 0);
    std::vector<std::string> out;
    if (!GetOpenFileNameW(&ofn)) {
        if (err && CommDlgExtendedError() != 0) *err = "The file dialog could not be opened";
        return out;
    }
    // one file: the full path. Several: the folder, then each name, each NUL-terminated, then an empty one
    std::wstring first(buf.data());
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) {
        out.push_back(narrow(first));
        return out;
    }
    while (*p) {
        std::wstring name(p);
        out.push_back(narrow(first + L"\\" + name));
        p += name.size() + 1;
    }
    return out;
}
#else
namespace {
// Runs argv (no shell) and returns its stdout and exit status. False if it couldn't be started.
bool run(const std::vector<std::string>& argv, std::string& out, int& status) {
    std::vector<char*> a; // built before fork(): the child only calls async-signal-safe functions
    for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    int fd[2];
    if (pipe(fd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        execvp(a[0], a.data());
        _exit(127);
    }
    close(fd[1]);
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd[0], buf, sizeof buf);
        if (n > 0) { out.append(buf, static_cast<size_t>(n)); if (out.size() > (1u << 20)) break; }
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return true;
}
std::vector<std::string> split_lines(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t                   i = 0;
    while (i < s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        std::string l = s.substr(i, j - i);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (!l.empty() && l[0] == '/') v.push_back(l); // absolute paths only
        i = j + 1;
    }
    return v;
}
} // namespace

std::vector<std::string> pick_files(bool multiple, const std::string& accept, std::string* err) {
    std::vector<std::string> pats = accept_patterns(accept);
    std::string              joined;
    for (auto& p : pats) joined += (joined.empty() ? "" : " ") + p;
    std::string out;
    int         status = 0;
    // zenity (GTK desktops) — the separator is a newline, which paths almost never contain
    std::vector<std::string> z = {"zenity", "--file-selection", "--title=Choose a file", "--separator=\n"};
    if (multiple) z.push_back("--multiple");
    if (!pats.empty()) z.push_back("--file-filter=Accepted files | " + joined);
    if (!pats.empty()) z.push_back("--file-filter=All files | *");
    if (run(z, out, status) && status != 127) return status == 0 ? split_lines(out, '\n') : std::vector<std::string>{};
    // kdialog (KDE)
    out.clear();
    std::vector<std::string> k = {"kdialog", "--title", "Choose a file", "--getopenfilename", ".",
                                  pats.empty() ? std::string("All files (*)") : "Accepted files (" + joined + ")\nAll files (*)"};
    if (multiple) {
        k.insert(k.begin() + 3, "--multiple");
        k.insert(k.begin() + 4, "--separate-output");
    }
    if (run(k, out, status) && status != 127) return status == 0 ? split_lines(out, '\n') : std::vector<std::string>{};
    if (err) *err = "Choosing files needs zenity or kdialog installed";
    return {};
}
#endif

} // namespace quill
