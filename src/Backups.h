// Backups.h — unsaved buffers kept on disk while they are edited, so a crash,
// a force quit, a power loss or Request a Change's restart loses none of them
// (VS Code calls this hot exit). Pure C++17, no I/O of its own: the file
// operations go through an `Fs` of callbacks (posixFs() is the real one), so
// the tests run on a scratch tree or a fake.
//
// One file per tab, `<id>.mcbackup`: a short header, which is that backup's
// index entry, then the buffer's text exactly as it is. There is no shared
// index file, so two windows, or two minicode processes (`minicode -n`), never
// race over one; a backup is written whole to a temporary file and renamed
// into place, so a reader sees the old one or the new one, never half.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Backups {

struct Entry {
    std::string id;             // letters, digits and '-' (validId)
    std::string path;           // the file the buffer belongs to
    std::string root;           // the window's folder, symlinks resolved
    // The disk file the buffer was based on, when the backup was taken: its
    // modification time and inode as the tab last read or saved it.
    // diskExisted is false for a file that was never on disk as far as the
    // tab knew.
    double diskMtime = 0;
    uint64_t diskInode = 0;
    bool diskExisted = false;
    std::string encoding = "utf-8";
    std::string lineEndings = "none";   // lineEndingsOf(text)
    double saved = 0;           // when the backup was written (Unix seconds)
    // The process holding the buffer: a backup whose owner still runs is
    // live and never restored or deleted by another window or process.
    int64_t ownerPid = 0;
    double ownerStart = 0;      // the owner's start time, against pid reuse
    uint64_t bytes = 0;         // the text's length, checked when it is read
};

// "lf", "crlf", "cr", "mixed" or "none".
std::string lineEndingsOf(const std::string &text);

bool validId(const std::string &id);
std::string fileName(const std::string &id);     // "<id>.mcbackup"

// The file's bytes: header, a blank line, the text.
std::string encode(const Entry &e, const std::string &text);
// The header alone (enough of `data` to hold it is all that is needed).
// `textStart`, if given, is where the text begins.
bool decodeHeader(const std::string &data, Entry *e, size_t *textStart = nullptr);
// Header and text; false when the text is not the length the header says
// (a file cut short).
bool decode(const std::string &data, Entry *e, std::string *text);

// ------------------------------------------------------------------ files
struct Stat {
    bool exists = false;
    bool regular = false;
    double mtime = 0;
    uint64_t inode = 0;
};

struct Fs {
    std::function<std::vector<std::string>(const std::string &dir)> list;   // names
    // Up to maxBytes (0: all). False when it cannot be read.
    std::function<bool(const std::string &path, std::string *out, size_t maxBytes)> read;
    // Written whole or not at all; the file is readable by its owner only.
    std::function<bool(const std::string &path, const std::string &data)> writeAtomic;
    std::function<bool(const std::string &path)> remove;
    std::function<Stat(const std::string &path)> stat;
};

// POSIX: files 0600, written to a temporary name beside them, fsync'd and
// renamed; the folder is created 0700 when missing.
Fs posixFs();

// How the disk file stands against what the buffer was based on.
enum class Disk {
    Same,        // unchanged since
    Changed,     // modified or replaced
    Gone,        // was there, is not
    NeverSaved,  // was never there, and still is not
    Appeared,    // was never there, and now is
};
Disk compare(const Entry &e, const Stat &now);

// Backups in one folder.
struct Store {
    std::string dir;
    Fs fs;
    std::string pathFor(const std::string &id) const;
    bool save(const Entry &e, const std::string &text) const;
    bool discard(const std::string &id) const;
    // Every backup whose header reads, headers only.
    std::vector<Entry> list() const;
    bool load(const std::string &id, Entry *e, std::string *text) const;
    // Temporary files a crash left behind mid-write, older than maxAge
    // seconds, and backups whose header does not read, older than
    // badAge seconds; returns how many went.
    int sweepDebris(double now, double maxAge, double badAge) const;
};

using IsLive = std::function<bool(const Entry &)>;

// What a window opening `root` brings back. The session's files (in its
// order) that have a backup, then backups of other files in that folder
// (newest first): a crash can lose the session's last save, but never the
// buffers. Live backups are left alone. When one file has several backups,
// the newest comes back and the others are listed to discard.
struct Restore {
    struct Item {
        std::string path;
        std::string id;
        bool inSession = false;
    };
    std::vector<Item> items;
    std::vector<std::string> discard;
};
Restore plan(const std::vector<Entry> &all, const std::string &root,
             const std::vector<std::string> &sessionFiles, const IsLive &live);

// Folders with backups no running window holds, newest first, leaving out
// `openRoots`: at launch each gets a window, so its buffers come back.
std::vector<std::string> foldersToReopen(const std::vector<Entry> &all,
                                         const std::vector<std::string> &openRoots,
                                         const IsLive &live);

// Backups nobody holds and nobody brought back for maxAge seconds.
std::vector<std::string> expired(const std::vector<Entry> &all, double now,
                                 double maxAge, const IsLive &live);

constexpr double kMaxAge = 30 * 24 * 3600.0;   // orphans go after 30 days

}  // namespace Backups
