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
    // `encoding` and `bom` are the renderer's own (an app's tab, which read
    // the file and knows how it writes it): kept whatever the file holds on
    // disk now, never the guess from reading it. With `overwrite` too (an
    // app asks its own questions about the disk first) the file is not read.
    bool keepEncoding = false;
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
// DocSaveDone's json: {path, ok, reason, why, note, encoding (its label),
// and once written encodingName (FolderSearch::encodingName) and stamp
// (stampJson), created}.
Json saveDoneJson(const std::string& path, const SaveDone& done);
// A file's stamp as JSON, exactly ({exists, regular, sec, nsec, dev, ino,
// size}; the device and inode as decimal strings, which a double would
// round), and back (a stamp that does not exist for anything else).
Json stampJson(const FolderSearch::FileStamp& s);
FolderSearch::FileStamp stampFromJson(const Json& j);

// ---- following the disk (the host watches it: SessionHost.h, "The disk")
// A document's file looked at again after the folder holding it changed.
// The rules are the Mac's for a tab (docs/notes/sessions.md, "The disk"):
// a clean document follows its file, one with unsaved changes is never
// written over (it is marked changed on disk, and a save asks first), a
// file deleted leaves the document unsaved and gone until it returns, and
// a document an app's window has open is that app's to follow (it reads
// the file itself and tells the host, DocSaved).
struct DiskLook {
    bool appHasIt = false;     // an app's window has it open
    bool hadFile = false;      // it was read from a file or written to one
    bool exists = false;       // something is at its path now
    bool text = false;         // a regular file that reads as text (not too large)
    bool sameAsSaved = false;  // and holds the text the host counts saved
    bool edited = false;       // the document's text is not its saved text
    bool wasGone = false;      // marked gone already
};
enum class DiskAction {
    Nothing,   // leave it as it is
    Same,      // the file holds the saved text: its stamp taken, gone no longer
    Follow,    // the file's text in place of the document's (one step, "Reload from Disk")
    Changed,   // unsaved changes kept, marked changed on disk
    Gone,      // deleted: kept, unsaved, marked gone
};
DiskAction diskAction(const DiskLook& look);

// The most bytes of a file the host reads to follow it: past it, a clean
// document is marked changed rather than followed.
constexpr std::uintmax_t kMaxFollowed = std::uintmax_t(64) << 20;
// A file read as the host follows it: the stamp taken before reading
// (readFileStable), and its text when it is a regular file that reads as
// text (decodeFileText) within kMaxFollowed.
struct DiskText {
    FolderSearch::FileStamp stamp;
    bool exists = false, regular = false, text = false;
    std::u16string body;
    FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
    bool bom = false;
};
DiskText readDiskText(const std::string& path);

}  // namespace Shared
