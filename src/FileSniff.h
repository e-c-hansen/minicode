// FileSniff.h — pure C++. What a file is, for choosing how to show it:
// by its first bytes where they say (a NUL byte means binary, SQLite's
// header), else by its extension. No I/O: the caller reads the head.
#pragma once
#include <cstddef>
#include <string>

namespace FileSniff {

// How many bytes of a file's head the checks look at.
constexpr size_t kHeadBytes = 8192;

// A NUL byte in the first kHeadBytes: no text file has one, but for
// UTF-16 (isUtf16Text, checked first).
bool looksBinary(const char *data, size_t len);
inline bool looksBinary(const std::string &head) { return looksBinary(head.data(), head.size()); }

// The head starts with a UTF-16 byte order mark (FF FE or FE FF) and its
// code units pair up: no lone surrogate, and an even length unless the
// head is cut at kHeadBytes (where half a unit or a pair may end it). Such
// a file is text, read and written in its own encoding (FolderSearch::
// decodeFileText); one with a lone surrogate further on is caught when it
// is read. UTF-16 without a byte order mark stays binary.
bool isUtf16Text(const char *data, size_t len);
inline bool isUtf16Text(const std::string &head) { return isUtf16Text(head.data(), head.size()); }

// SQLite's 16-byte header, "SQLite format 3" and a NUL.
bool isSqlite(const char *data, size_t len);
inline bool isSqlite(const std::string &head) { return isSqlite(head.data(), head.size()); }

enum class Kind {
    Text,        // the editor, as before (Markdown, LaTeX and the rest included)
    Csv,         // a table, comma (or ; or |) separated; raw text a toggle away
    Tsv,         // a table, tab separated
    Json,        // the text, with an outline beside it
    Notebook,    // a Jupyter notebook, rendered read-only
    Sqlite,      // a database, read-only
    QuickLook,   // binary, or a document only Quick Look shows (Office, iWork)
};

// The extension in lower case, without the dot ("" for none).
std::string extensionOf(const std::string &path);

// Documents with no viewer of minicode's own that Quick Look previews:
// Office, iWork, OpenDocument, ePub, Rich Text Format Directory and the like.
bool isQuickLookDocument(const std::string &ext);

// The viewer for a file from its path and its head. Images, PDFs, video
// and audio are the caller's to route first (they have viewers of their
// own); what reaches here as binary goes to Quick Look.
Kind classify(const std::string &path, const char *head, size_t len);
inline Kind classify(const std::string &path, const std::string &head) {
    return classify(path, head.data(), head.size());
}

const char *kindName(Kind k);   // "text", "csv", ... for logs and tests

}  // namespace FileSniff
