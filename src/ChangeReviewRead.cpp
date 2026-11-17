// ChangeReviewRead.cpp — see ChangeReviewRead.h.
#include "ChangeReviewRead.h"

#include <cctype>
#include <map>

#include "GitStatus.h"

namespace ChangeReview {

namespace {

// The encoding a blob is in, from its own first bytes: UTF-16 with its
// mark, UTF-8 with or without one.
void encodingOfBlob(const std::string& b, FolderSearch::TextEncoding* enc, bool* bom) {
    *enc = FolderSearch::TextEncoding::Utf8;
    *bom = false;
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) {
        *enc = FolderSearch::TextEncoding::Utf16LE;
        *bom = true;
    } else if (b.size() >= 2 && (unsigned char)b[0] == 0xFE && (unsigned char)b[1] == 0xFF) {
        *enc = FolderSearch::TextEncoding::Utf16BE;
        *bom = true;
    } else if (b.size() >= 3 && b.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        *bom = true;
    }
}

}  // namespace

std::string extensionOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path.substr(slash == std::string::npos ? 0 : slash + 1);
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

Read readUncommitted(const std::string& root, const GitRunner& run) {
    Read data;
    GitOut r = run(root, GitUi::topLevelArgs(), "");
    std::string top, gitDir;
    if (!r.ok || !GitUi::parseTopLevel(r.out, top, gitDir)) {
        data.error = "This folder is not in a git repository.";
        return data;
    }
    data.top = top;
    GitOut st = run(top, GitUi::statusArgs(), "");
    if (!st.ok) {
        data.error = st.err.empty() ? "git status failed." : st.err;
        return data;
    }
    GitUi::Snapshot snap;
    GitUi::applyStatus(snap, Git::parseStatus(st.out));
    data.initial = snap.initial;
    std::vector<GitUi::Row> rows;
    std::vector<std::string> paths;
    for (const GitUi::Row& row : snap.rows) {
        if (row.header) continue;
        rows.push_back(row);
        paths.push_back(row.path);
        if (!row.origPath.empty()) paths.push_back(row.origPath);
    }
    if (rows.empty()) return data;
    std::map<std::string, Entry> index, head;
    GitOut ls = run(top, lsFilesArgs(paths), "");
    if (ls.ok) index = parseLsFiles(ls.out);
    if (!snap.initial) {
        GitOut lt = run(top, lsTreeArgs("HEAD", paths), "");
        if (lt.ok) head = parseLsTree(lt.out);
    }
    // Every blob wanted, in one cat-file --batch.
    std::string want;
    auto ask = [&](const std::map<std::string, Entry>& m, const std::string& p) {
        auto it = m.find(p);
        if (it != m.end() && it->second.mode != "160000") want += it->second.object + "\n";
    };
    for (const GitUi::Row& row : rows) {
        if (row.unmerged || row.submodule) continue;
        if (row.staged) {
            ask(head, row.origPath.empty() ? row.path : row.origPath);
            ask(index, row.path);
        } else if (!row.untracked) {
            ask(index, row.path);
        }
    }
    std::map<std::string, std::string> blobs;
    if (!want.empty()) {
        GitOut b = run(top, batchArgs(), want);
        if (b.ok) blobs = parseBatch(b.out);
    }
    size_t total = 0;
    for (const GitUi::Row& row : rows) {
        File f;
        Extra x;
        x.row = row;
        f.path = row.path;
        f.origPath = row.origPath;
        f.section = row.staged ? Section::Staged : Section::Changes;
        f.letter = row.letter;
        f.untracked = row.untracked;
        f.conflict = row.unmerged;
        f.submodule = row.submodule;
        auto idx = index.find(row.path);
        if (idx != index.end()) {
            x.indexObject = idx->second.object;
            if (idx->second.mode == "160000") f.submodule = true;
            if (idx->second.mode == "120000") f.symlink = true;
        }
        auto inHead = head.find(row.origPath.empty() ? row.path : row.origPath);
        if (inHead != head.end() && inHead->second.mode == "120000") f.symlink = true;
        if (f.conflict && !f.submodule && !f.symlink) {
            // A conflict: the work tree's text, whose blocks the page shows.
            std::string bytes;
            FolderSearch::FileStamp stamp;
            const FolderSearch::ReadStatus rs =
                FolderSearch::readFileStable(top + "/" + row.path, kMaxSideBytes, &bytes, &stamp);
            f.newExists = rs != FolderSearch::ReadStatus::Missing;
            if (rs == FolderSearch::ReadStatus::TooLarge) f.tooLarge = true;
            x.workRead = rs == FolderSearch::ReadStatus::Ok;
            x.workBytes = bytes;
            total += bytes.size();
            if (total > kMaxTotalBytes) f.tooLarge = true;
            if (x.workRead && !f.tooLarge) {
                if (looksBinary(bytes)) {
                    f.binary = true;
                } else {
                    FolderSearch::TextEncoding enc = FolderSearch::TextEncoding::Utf8;
                    bool bom = false;
                    std::string work = bytes;
                    if (FolderSearch::decodeFileText(&work, &enc, &bom)) {
                        x.workEnc = enc;
                        x.workBom = bom;
                        f.newText = QuickDiff::fromUtf8(work);
                    } else {
                        f.binary = true;
                    }
                }
            } else if (!x.workRead && f.newExists && !f.tooLarge) {
                f.binary = true;   // unreadable: left whole, as a binary file
            }
            data.extras.push_back(x);
            data.diffs.push_back(conflictFile(f, extensionOf(f.path)));
            continue;
        }
        if (f.conflict || f.submodule || f.symlink) {
            data.extras.push_back(x);
            data.diffs.push_back(diffFile(f, ""));
            continue;
        }
        auto blobOf = [&](const std::map<std::string, Entry>& m, const std::string& p,
                          bool* exists) -> std::string {
            auto it = m.find(p);
            if (it == m.end()) {
                *exists = false;
                return "";
            }
            auto b = blobs.find(it->second.object);
            *exists = true;
            return b == blobs.end() ? std::string() : b->second;
        };
        std::string oldBytes, newBytes;
        bool oldExists = false, newExists = false;
        if (f.section == Section::Staged) {
            oldBytes = row.letter == 'A' ? std::string()
                       : blobOf(head, row.origPath.empty() ? row.path : row.origPath, &oldExists);
            newBytes = row.letter == 'D' ? std::string() : blobOf(index, row.path, &newExists);
        } else {
            if (!row.untracked) oldBytes = blobOf(index, row.path, &oldExists);
            if (row.letter != 'D') {
                FolderSearch::FileStamp stamp;
                const FolderSearch::ReadStatus rs = FolderSearch::readFileStable(
                    top + "/" + row.path, kMaxSideBytes, &newBytes, &stamp);
                newExists = rs != FolderSearch::ReadStatus::Missing;
                if (rs == FolderSearch::ReadStatus::TooLarge) f.tooLarge = true;
                else if (rs != FolderSearch::ReadStatus::Ok) newBytes.clear();
                x.workRead = rs == FolderSearch::ReadStatus::Ok;
                x.workBytes = newBytes;
            }
        }
        f.oldExists = oldExists;
        f.newExists = newExists;
        total += oldBytes.size() + newBytes.size();
        if (oldBytes.size() > kMaxSideBytes || newBytes.size() > kMaxSideBytes ||
            total > kMaxTotalBytes)
            f.tooLarge = true;
        if (looksBinary(oldBytes) || looksBinary(newBytes)) f.binary = true;
        if (!f.tooLarge && !f.binary) {
            // The work tree's encoding decides how the index is read, as the
            // change bars read it; in the staged list the index's own bytes.
            FolderSearch::TextEncoding enc = FolderSearch::TextEncoding::Utf8;
            bool bom = false;
            if (f.section == Section::Changes && x.workRead) {
                std::string work = newBytes;
                if (FolderSearch::decodeFileText(&work, &enc, &bom)) {
                    x.workEnc = enc;
                    x.workBom = bom;
                    f.newText = QuickDiff::fromUtf8(work);
                } else {
                    f.binary = true;
                }
                x.indexHow = QuickDiff::readOriginal(oldBytes, enc, bom);
                f.oldText = x.indexHow.text;
            } else if (f.section == Section::Staged) {
                encodingOfBlob(newExists ? newBytes : oldBytes, &enc, &bom);
                x.indexHow = QuickDiff::readOriginal(newBytes, enc, bom);
                f.newText = x.indexHow.text;
                f.oldText = QuickDiff::readOriginal(oldBytes, enc, bom).text;
            } else {
                x.indexHow = QuickDiff::readOriginal(oldBytes, enc, bom);
                f.oldText = x.indexHow.text;
            }
        }
        data.extras.push_back(x);
        data.diffs.push_back(diffFile(f, extensionOf(f.path)));
    }
    return data;
}

std::string stageText(const Read& r, size_t fi, const std::u16string& text, const GitRunner& run) {
    if (fi >= r.diffs.size() || fi >= r.extras.size()) return "That file is no longer in the view.";
    const Extra& x = r.extras[fi];
    const std::string rel = x.row.path;
    const std::string blob = QuickDiff::stagedBlob(text, x.indexHow);
    GitOut ls = run(r.top, GitUi::indexEntryArgs(rel), "");
    const GitUi::IndexEntry now = GitUi::parseIndexEntry(ls.out);
    if (!ls.ok || !now.ok || now.object != x.indexObject) return staleText(rel, true);
    GitOut h = run(r.top, GitUi::hashObjectArgs(rel), blob);
    if (!h.ok) return h.err.empty() ? "git hash-object failed." : h.err;
    GitOut u = run(r.top, GitUi::updateIndexArgs(now, h.out, rel), "");
    if (!u.ok) return u.err.empty() ? "git update-index failed." : u.err;
    return "";
}

std::string discardText(const Read& r, size_t fi, const std::u16string& text, const GitRunner& run,
                        const std::function<bool(std::string* where, std::string* why)>& trashCopy,
                        std::string* trashed) {
    if (trashed) trashed->clear();
    if (fi >= r.diffs.size() || fi >= r.extras.size()) return "That file is no longer in the view.";
    const Extra& x = r.extras[fi];
    const std::string rel = x.row.path;
    const std::string path = r.top + "/" + rel;
    const std::string bytes =
        FolderSearch::encodeFileText(QuickDiff::toUtf8(text), x.workEnc, x.workBom);
    GitOut ls = run(r.top, GitUi::indexEntryArgs(rel), "");
    const GitUi::IndexEntry entry = GitUi::parseIndexEntry(ls.out);
    std::string now;
    FolderSearch::FileStamp stamp;
    if (FolderSearch::readFileStable(path, kMaxSideBytes, &now, &stamp) !=
            FolderSearch::ReadStatus::Ok ||
        now != x.workBytes)
        return staleText(rel, false);
    if (!ls.ok || !entry.ok || entry.object != x.indexObject) return staleText(rel, true);
    std::string where, why;
    if (!trashCopy(&where, &why)) return why;
    std::string error;
    if (!FolderSearch::writeFileAtomically(path, bytes, &error)) {
        // The copy is not needed: the caller drops it (nothing was changed).
        if (trashed) *trashed = "";
        return rel.substr(rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/') + 1) +
               " could not be written (" + error + "), so nothing was discarded.";
    }
    if (trashed) *trashed = where;
    return "";
}

}  // namespace ChangeReview
