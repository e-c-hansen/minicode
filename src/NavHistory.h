// NavHistory.h — pure C++. Go Back and Go Forward (Ctrl+- and Ctrl+Shift+-,
// VS Code's Mac keys): a window's list of places it jumped between.
//
// A jump is recorded as where it left and where it landed: go to
// definition, a reference, a symbol, a line, a file from Quick Open or the
// tree, a problem, a find result, a terminal link, a tab switch, and a
// caret that moved more than kFarLines lines at once. Places close together
// (the same file, within kFarLines lines) are one entry, the later one kept,
// so the list holds where the user really was. A new jump drops what lay
// ahead of the current entry, as a browser's history does. Back from a
// place far from the current entry first records it, so Forward comes back
// to it. Places follow edits to their file's text (edited), so Back after
// typing above a place lands where that text went.
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Nav {

// A caret move of more than this many lines counts as a jump; places within
// it of each other in one file are one entry.
constexpr int kFarLines = 10;

struct Place {
    std::string path;
    // The selection, in UTF-16 units of the file's text, and the line it
    // starts on (0-based). hasText is false for a file shown without text
    // to place a caret in (a picture, a PDF, a rendered preview).
    size_t location = 0, length = 0;
    int line = 0;
    bool hasText = true;
    // An edit before the place may have moved its line (edited); fixLines
    // counts it again.
    bool lineStale = false;
};

// The same file, and within kFarLines lines (or either has no text place).
bool near(const Place& a, const Place& b);

class History {
public:
    explicit History(size_t cap = 50) : cap_(cap) {}

    // A jump from `from` to `to`. Either merges into the entry before it
    // when near it.
    void jump(const Place& from, const Place& to);
    // Where Back goes from `here`, or false. `here` replaces the current
    // entry when near it, else is added as one, so Forward returns to it.
    bool back(const Place& here, Place* out);
    bool forward(const Place& here, Place* out);
    bool canGoBack(const Place& here) const;
    bool canGoForward() const;

    // The text of `path` changed: the units [pos, pos + oldLen) became
    // newLen units. Places after the edit move with their text, places
    // inside it go to where its new text is (as far into it as they were,
    // at most its end), and every place past pos has its line marked stale.
    void edited(const std::string& path, size_t pos, size_t oldLen, size_t newLen);
    // Whether any place of `path` waits for fixLines.
    bool hasStaleLines(const std::string& path) const;
    // Counts the stale lines of `path`'s places again: `lines` gets their
    // offsets, ascending, and gives back each one's 0-based line.
    void fixLines(const std::string& path,
                  const std::function<std::vector<int>(const std::vector<size_t>& offsets)>& lines);

    // A file or folder moved: entries for it, or below it, follow.
    void moved(const std::string& from, const std::string& to);
    // Drop the entries of files for which gone(path) is true (deleted, or no
    // longer openable); neighbours that become near are merged.
    void removeIf(const std::function<bool(const std::string& path)>& gone);

    const std::vector<Place>& entries() const { return list_; }
    size_t index() const { return cur_; }   // the current entry (0 when empty)
    void clear() { list_.clear(); cur_ = 0; }

private:
    void push(const Place& p);   // after cur_, dropping what was ahead
    void trim();
    std::vector<Place> list_;
    size_t cur_ = 0;
    size_t cap_;
};

}  // namespace Nav
