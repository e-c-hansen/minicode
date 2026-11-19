// ThemePicker.cpp — see ThemePicker.h.
#include "ThemePicker.h"

#include "Settings.h"

#include <algorithm>
#include <cctype>

namespace ThemePicker {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

}  // namespace

std::vector<Row> rows() {
    std::vector<Row> out;
    for (const Theme& t : Settings::themes()) {
        Row r;
        r.id = t.id;
        r.title = t.title;
        r.dark = t.dark;
        out.push_back(r);
    }
    std::stable_sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
        if (a.dark != b.dark) return a.dark;
        return lower(a.title) < lower(b.title);
    });
    for (size_t i = 0; i < out.size(); i++)
        if (i == 0 || out[i].dark != out[i - 1].dark)
            out[i].group = out[i].dark ? "dark themes" : "light themes";
    return out;
}

int rowOf(const std::vector<Row>& rows, const std::string& id) {
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].id == id) return (int)i;
    return -1;
}

Session::Session(const std::string& setTheme)
    : rows_(ThemePicker::rows()), set_(setTheme), showing_(setTheme) {}

int Session::initialRow() const {
    const int r = rowOf(rows_, set_);
    return r >= 0 ? r : (rows_.empty() ? -1 : 0);
}

std::string Session::preview(int row) {
    if (!open_ || row < 0 || row >= (int)rows_.size()) return "";
    if (rows_[(size_t)row].id == showing_) return "";
    showing_ = rows_[(size_t)row].id;
    return showing_;
}

std::string Session::choose(int row) {
    if (!open_ || row < 0 || row >= (int)rows_.size()) return "";
    open_ = false;
    showing_ = rows_[(size_t)row].id;
    return showing_ == set_ ? "" : showing_;
}

std::string Session::cancel() {
    if (!open_) return "";
    open_ = false;
    const bool was = showing_ != set_;
    showing_ = set_;
    return was ? set_ : "";
}

}  // namespace ThemePicker
