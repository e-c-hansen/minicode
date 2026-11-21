// SessionFiles.h — the session host's reads of the workspace
// (SESSION-DESIGN.md, stage 7, "workspace I/O in the host"). A renderer asks
// with FsRead {req, window, json: the request} and the host answers FsData
// {req, json: the answer, bytes} (SessionProtocol.h). The reads are a
// folder's listing, one level at a time, for the browser's tree, and a
// piece of a file's bytes, for the browser's viewers (SessionWeb.h: R3);
// stage 7 adds reads by giving the request another "op".
//
// Every read is confined to the session's folder: the headless host's own
// (Host::setFolder), else the folder of the window the request names (its
// "root", as the window's renderer reported it). A path is '/'-separated
// names from that folder, "" for the folder itself; an absolute path, an
// empty, "." or ".." name, and a path whose real place (symlinks followed)
// is outside the folder's real place are refused.
//
// A listing follows the apps' trees (the Mac's MCTreeList, GTK's FileTree):
// dotfiles only when asked for (Show Hidden Files), folders first, then
// names as TreeSort orders them. What the folder's .gitignore and .ignore
// files ignore (FolderSearch.h's Ignore, as Find in Folder reads them) is
// listed and marked, as the apps' trees show it in tree.git-ignored's
// color rather than leaving it out.
//
// And the first write, saving (saveDocument): the host writes a document's
// text to its file with the rules the apps' saves keep (docs/notes/
// sessions.md, "One writer" and "The disk"): atomically
// (FolderSearch::writeFileAtomically: a link stays a link, hard links and
// other owners written in place, fsync'd), in the file's own encoding with
// its byte order mark (UTF-16, Windows-1252, UTF-8 with stray bytes; the
// text's line endings are its own, never changed), and never over a file
// that changed on disk since the text the host counts saved was read or
// written, unless asked to (`overwrite`, a person's answer). Pure C++17 and
// POSIX.
#pragma once

#include <cstddef>
#include <string>

#include "FolderSearch.h"
#include "Json.h"

namespace Shared {

// The most entries one listing names; the rest are counted ("more").
constexpr std::size_t kMaxListed = 10000;
// The most bytes one "read" gives: a larger file is read a piece at a time.
constexpr std::size_t kMaxReadBytes = std::size_t(4) << 20;

// `rel` held inside `root`: true with *abs = root + "/" + rel (spelled
// under `root`, as the windows spell their tabs' paths; `root` itself for
// ""), false with *why for a path that is not one of the folder's own.
bool confinedPath(const std::string& root, const std::string& rel, std::string* abs,
                  std::string* why);

// One read (FsRead's json) answered (FsData's json), in `root`:
//   {"op": "list", "path": rel, "hidden": bool}
//     -> {"op": "list", "path": rel, "root": root, "entries": [{"name",
//        "dir", "link", "ignored", "outside", "kind"}, ...], "more": n}
// where "dir", "link" (a symlink), "ignored" (by the ignore files) and
// "outside" (a symlink leading out of the folder: never listed or opened)
// are there only when true, "kind" is a file's FileIcon kind ("text",
// "code", "image", ...), and "more" counts entries past kMaxListed. A read
// that fails answers {"op", "path", "error": why}; an unknown op too.
//   {"op": "read", "path": rel, "offset": n, "length": n, "hidden": bool}
//     -> {"op": "read", "path": rel, "size": the file's, "offset": n,
//        "length": bytes given}, and the bytes in *bytes
// for a regular file held inside the folder as a listing holds it: a name
// starting with '.' anywhere in the path only with "hidden" (the tree shows
// dotfiles only then), never a FIFO, device or folder; at most
// kMaxReadBytes from `offset` (none past the end; "length" 0 gives the
// size alone). The file is opened at its real place, not following a
// symlink there, so a link swapped in after the check is refused.
Json readWorkspace(const std::string& root, const Json& request, std::string* bytes = nullptr);

// ---- saving (the host's DocSave; SessionHost.h)
struct SaveAsk {
    std::string path;           // the document's file, absolute ("" for none yet)
    // When not empty, the file must be inside this folder (a browser's
    // save): its folder's real place, and its own when it is there (a
    // symlink followed), inside the folder's real place.
    std::string root;
    std::u16string text;        // what to write
    std::u16string saved;       // the text the host counts saved (as read, or last written)
    // The file as the host last wrote it (exists false when it never has):
    // a file whose stamp is still that is unchanged without being read,
    // and is written in `encoding` with `bom`.
    FolderSearch::FileStamp stamp;
    FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
    bool bom = false;
    bool overwrite = false;     // write even when it changed on disk, or went
};
struct SaveDone {
    bool ok = false;
    // Why not: "nofile" (no path yet), "outside" (not in `root`), "changed"
    // (on disk since `saved`), "gone" (deleted since), "notfile" (a folder,
    // a FIFO, a link to nothing), "unreadable" (it could not be read to
    // compare), "encoding" (the text holds a lone surrogate), "write" (the
    // write failed). "changed" and "gone" are the two a person may answer
    // with `overwrite`.
    std::string reason;
    std::string why;            // a sentence for the person
    std::string note;           // a sentence when it worked but differs (the encoding)
    FolderSearch::FileStamp written;   // the file as written
    FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
    bool bom = false;
    bool created = false;       // there was no file before
};
// Whether the file is unchanged is decided as the apps decide it, by the
// stamp the host wrote it with; when the host has none (an app read the
// file, or saved it last), by reading the file and comparing its text with
// `saved`, which is the text it was read or written with. The encoding is
// the one the file is in on disk (or that the host last wrote; written
// over with `overwrite`, the one the host last wrote, else the file's
// own as it is now), except
// that a Windows-1252 file whose text now holds a character Windows-1252
// lacks is written as UTF-8, with `note` saying so, as the Mac's save does.
SaveDone saveDocument(const SaveAsk& ask);
// DocSaveDone's json: {path, ok, reason, why, note, encoding (its label)}.
Json saveDoneJson(const std::string& path, const SaveDone& done);

}  // namespace Shared
