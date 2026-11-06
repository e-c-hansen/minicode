// FileSniff.cpp — see FileSniff.h.
#include "FileSniff.h"

#include <cstring>
#include <set>

namespace FileSniff {

bool looksBinary(const char *data, size_t len) {
    if (!data) return false;
    if (len > kHeadBytes) len = kHeadBytes;
    return std::memchr(data, 0, len) != nullptr;
}

bool isSqlite(const char *data, size_t len) {
    static const char magic[16] = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
                                   'o', 'r', 'm', 'a', 't', ' ', '3', '\0'};
    return data && len >= 16 && std::memcmp(data, magic, 16) == 0;
}

std::string extensionOf(const std::string &path) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    // A dotfile's name is not an extension (".env" has none).
    if (dot == (slash == std::string::npos ? 0 : slash + 1)) return "";
    std::string ext = path.substr(dot + 1);
    for (char &c : ext)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return ext;
}

bool isQuickLookDocument(const std::string &ext) {
    static const std::set<std::string> exts = {
        "doc", "docx", "docm", "dot", "dotx",               // Word
        "xls", "xlsx", "xlsm", "xlsb", "xlt", "xltx",       // Excel
        "ppt", "pptx", "pptm", "pps", "ppsx", "pot", "potx",// PowerPoint
        "pages", "numbers", "key",                           // iWork
        "odt", "ods", "odp", "odg",                          // OpenDocument
        "epub", "rtfd", "webarchive", "usdz", "reality",
        "sketch", "psd", "ai", "eps", "dmg", "pkg", "zip",
        "ttf", "otf", "ttc", "woff", "woff2",
    };
    return exts.count(ext) > 0;
}

Kind classify(const std::string &path, const char *head, size_t len) {
    const std::string ext = extensionOf(path);
    if (isSqlite(head, len)) return Kind::Sqlite;
    if (isQuickLookDocument(ext)) return Kind::QuickLook;
    if (looksBinary(head, len)) return Kind::QuickLook;
    if (ext == "csv") return Kind::Csv;
    if (ext == "tsv" || ext == "tab") return Kind::Tsv;
    if (ext == "json" || ext == "geojson" || ext == "jsonc" || ext == "har" ||
        ext == "webmanifest")
        return Kind::Json;
    if (ext == "ipynb") return Kind::Notebook;
    // An empty database file has no header yet; by its name it is still one.
    if ((ext == "sqlite" || ext == "sqlite3" || ext == "db3") && len == 0) return Kind::Sqlite;
    return Kind::Text;
}

const char *kindName(Kind k) {
    switch (k) {
    case Kind::Text: return "text";
    case Kind::Csv: return "csv";
    case Kind::Tsv: return "tsv";
    case Kind::Json: return "json";
    case Kind::Notebook: return "notebook";
    case Kind::Sqlite: return "sqlite";
    case Kind::QuickLook: return "quicklook";
    }
    return "text";
}

}  // namespace FileSniff
