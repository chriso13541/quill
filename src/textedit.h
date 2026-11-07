// Minimal UTF-8 text editing state shared by the address bar and page form fields.
#pragma once
#include <algorithm>
#include <string>

namespace quill {

struct TextEdit {
    std::string text;
    size_t      cursor     = 0;
    bool        select_all = false; // whole text selected: the next edit replaces it
    bool        multiline  = false; // textarea: newlines allowed, Up/Down move between lines

    void set(const std::string& t) {
        text       = t;
        cursor     = text.size();
        select_all = false;
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
    void clear_selection_for_edit() {
        if (select_all) {
            text.clear();
            cursor     = 0;
            select_all = false;
        }
    }
    void insert(std::string s) {
        clear_selection_for_edit();
        for (auto& c : s) {
            if (c == '\r') c = '\n';
            if (!multiline && (c == '\n' || c == '\t')) c = ' ';
        }
        text.insert(cursor, s);
        cursor += s.size();
    }
    void backspace() {
        if (select_all) {
            clear_selection_for_edit();
            return;
        }
        size_t p = prev_boundary(cursor);
        text.erase(p, cursor - p);
        cursor = p;
    }
    void del() {
        if (select_all) {
            clear_selection_for_edit();
            return;
        }
        size_t n = next_boundary(cursor);
        text.erase(cursor, n - cursor);
    }
    void left() {
        cursor     = select_all ? 0 : prev_boundary(cursor);
        select_all = false;
    }
    void right() {
        cursor     = select_all ? text.size() : next_boundary(cursor);
        select_all = false;
    }
    size_t line_start(size_t pos) const {
        size_t p = text.rfind('\n', pos == 0 ? 0 : pos - 1);
        if (pos == 0 || p == std::string::npos) return 0;
        return p + 1;
    }
    size_t line_end(size_t pos) const {
        size_t p = text.find('\n', pos);
        return p == std::string::npos ? text.size() : p;
    }
    void home() {
        select_all = false;
        cursor     = multiline ? line_start(cursor) : 0;
    }
    void end() {
        select_all = false;
        cursor     = multiline ? line_end(cursor) : text.size();
    }
    // Moves between lines of a multi-line field, keeping the column (in bytes) where possible.
    void up() {
        select_all = false;
        size_t ls  = line_start(cursor);
        if (ls == 0) {
            cursor = 0;
            return;
        }
        size_t col = cursor - ls, ps = line_start(ls - 1);
        cursor = std::min(ps + col, ls - 1);
        while (cursor > ps && (static_cast<unsigned char>(text[cursor]) & 0xC0) == 0x80) cursor--;
    }
    void down() {
        select_all = false;
        size_t le  = line_end(cursor);
        if (le >= text.size()) {
            cursor = text.size();
            return;
        }
        size_t col = cursor - line_start(cursor), ns = le + 1, ne = line_end(ns);
        cursor = std::min(ns + col, ne);
        while (cursor < ne && (static_cast<unsigned char>(text[cursor]) & 0xC0) == 0x80) cursor++;
    }
};

} // namespace quill
