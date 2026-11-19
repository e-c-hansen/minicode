// ChangeReviewRead.cpp — see ChangeReviewRead.h.
#include "ChangeReviewRead.h"

#include <cctype>
#include <map>

#include <algorithm>

#include "AgentView.h"
#include "GitStatus.h"

namespace ChangeReview {

namespace {

// The encoding a blob is in, from its own first bytes: UTF-16 with its
// mark, UTF-8 with or without one, or Windows-1252 (not UTF-8, no NUL).
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
    } else if (FolderSearch::readsAsWindows1252(b)) {
        *enc = FolderSearch::TextEncoding::Windows1252;
    } else if (FolderSearch::readsAsUtf8WithStrayBytes(b)) {
        *enc = FolderSearch::TextEncoding::Utf8Raw;
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

bool seeImages(File& f, Extra& x, const std::string& oldBytes, const std::string& newBytes,
               const ImageReading& images) {
    if (!images.size || !f.binary || !imagePath(f.path)) return false;
    auto read = [&](bool exists, const std::string& bytes) {
        ImageSide s;
        if (!exists) return s;
        if (!bytes.empty()) s = images.size(bytes);
        s.bytes = bytes.size();
        return s;
    };
    if (!showAsImages(f, read(f.oldExists, oldBytes), read(f.newExists, newBytes), images.rowHeight))
        return false;
    if (f.oldExists) x.oldImage = oldBytes;
    if (f.newExists) x.newImage = newBytes;
    return true;
}

Read readUncommitted(const std::string& root, const GitRunner& run, const ImageReading& images) {
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
        seeImages(f, x, oldBytes, newBytes, images);
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

// ------------------------------------------------ a worktree against main
WorktreeRead readWorktree(const std::string& path, const std::string& mainRef,
                          const GitRunner& run, const ImageReading& images) {
    WorktreeRead w;
    Read& data = w.read;
    GitOut r = run(path, GitUi::topLevelArgs(), "");
    std::string top, gitDir;
    if (!r.ok || !GitUi::parseTopLevel(r.out, top, gitDir)) {
        data.error = "This worktree is not a git checkout any more.";
        return w;
    }
    data.top = top;
    if (mainRef.empty()) {
        data.error = "There is no main branch to compare with.";
        return w;
    }
    GitOut mb = run(top, {"merge-base", mainRef, "HEAD"}, "");
    const AgentView::CompareBase base = AgentView::compareBase(mainRef, mb.ok ? mb.out : std::string());
    w.baseRef = base.ref;
    w.baseLabel = base.label;
    GitOut wl = run(top, worktreeListArgs(), "");
    if (wl.ok) w.mainPath = mainWorktreePath(wl.out);
    GitOut dr = run(top, worktreeRawArgs(base.ref), "");
    if (!dr.ok) {
        data.error = dr.err.empty() ? "git diff failed." : dr.err;
        return w;
    }
    std::vector<RawChange> changes = parseRawDiff(dr.out);
    GitOut un = run(top, GitUi::untrackedArgs(), "");
    if (un.ok)
        for (const std::string& p : nulPaths(un.out)) {
            RawChange c;
            c.letter = '?';
            c.oldMode = "000000";
            c.newMode = "100644";
            c.path = p;
            changes.push_back(c);
        }
    std::sort(changes.begin(), changes.end(),
              [](const RawChange& a, const RawChange& b) { return a.path < b.path; });
    std::string want;
    for (const RawChange& c : changes)
        if (c.oldMode != "000000" && c.oldMode != "160000" && !nullObject(c.oldObject))
            want += c.oldObject + "\n";
    std::map<std::string, std::string> blobs;
    if (!want.empty()) {
        GitOut b = run(top, batchArgs(), want);
        if (b.ok) blobs = parseBatch(b.out);
    }
    size_t total = 0;
    for (const RawChange& c : changes) {
        File f;
        Extra x;
        f.path = c.path;
        f.origPath = c.origPath;
        f.section = Section::Plain;
        f.letter = c.letter;
        f.untracked = c.letter == '?';
        f.oldExists = c.oldMode != "000000";
        f.submodule = c.oldMode == "160000" || c.newMode == "160000";
        f.symlink = c.oldMode == "120000" || c.newMode == "120000";
        x.row.path = c.path;
        x.row.origPath = c.origPath;
        x.row.letter = c.letter;
        x.row.untracked = f.untracked;
        std::string newBytes;
        f.newExists = false;
        if (c.newMode != "000000" && !f.submodule) {
            FolderSearch::FileStamp stamp;
            const FolderSearch::ReadStatus rs =
                FolderSearch::readFileStable(top + "/" + c.path, kMaxSideBytes, &newBytes, &stamp);
            f.newExists = rs != FolderSearch::ReadStatus::Missing;
            if (rs == FolderSearch::ReadStatus::TooLarge) f.tooLarge = true;
            else if (rs != FolderSearch::ReadStatus::Ok) newBytes.clear();
        }
        if (!f.submodule && !f.symlink) {
            auto it = blobs.find(c.oldObject);
            const std::string oldBytes = f.oldExists && it != blobs.end() ? it->second : std::string();
            total += oldBytes.size() + newBytes.size();
            if (oldBytes.size() > kMaxSideBytes || total > kMaxTotalBytes) f.tooLarge = true;
            else if (looksBinary(oldBytes) || looksBinary(newBytes)) f.binary = true;
            seeImages(f, x, oldBytes, newBytes, images);
            if (!f.tooLarge && !f.binary) {
                FolderSearch::TextEncoding enc = FolderSearch::TextEncoding::Utf8;
                bool bom = false;
                std::string work = newBytes;
                if (FolderSearch::decodeFileText(&work, &enc, &bom)) {
                    f.newText = QuickDiff::fromUtf8(work);
                    f.oldText = QuickDiff::readOriginal(oldBytes, enc, bom).text;
                } else {
                    f.binary = true;
                }
            }
        }
        data.extras.push_back(x);
        data.diffs.push_back(diffFile(f, extensionOf(f.path)));
    }
    return w;
}

ApplyFiles applyFiles(const WorktreeRead& w) {
    ApplyFiles out;
    for (const FileDiff& d : w.read.diffs) {
        out.paths.push_back(d.file.path);
        if (!d.file.origPath.empty()) out.paths.push_back(d.file.origPath);
        if (d.file.untracked) out.untracked.push_back(d.file.path);
    }
    return out;
}

GitUi::ApplyPlan planApplyToMain(const WorktreeRead& w, const std::string& mainName,
                                 const std::string& worktreeName, const GitRunner& run) {
    GitUi::ApplyPlan plan;
    if (w.mainPath.empty() || w.read.diffs.empty()) {
        plan.refusal = "There is nothing to bring into the main checkout.";
        return plan;
    }
    const ApplyFiles files = applyFiles(w);
    GitOut st = run(w.mainPath, GitUi::statusArgs(), "");
    if (!st.ok)
        plan.refusal = GitUi::applyFailureText(st.status < 0 ? 1 : st.status, st.out, st.err);
    else
        plan = GitUi::applyToMainPlan(files.paths, Git::parseStatus(st.out), mainName, worktreeName);
    return plan;
}

ApplySource applySource(const WorktreeRead& w) {
    ApplySource s;
    s.top = w.read.top;
    s.baseRef = w.baseRef;
    s.mainPath = w.mainPath;
    s.files = applyFiles(w);
    s.fileCount = w.read.diffs.size();
    return s;
}

ApplyNow applyNow(const ApplyFiles& asked, const std::string& rawDiff, const std::string& untrackedOut) {
    ApplyNow now;
    auto isAsked = [&](const std::string& p) {
        return std::find(asked.paths.begin(), asked.paths.end(), p) != asked.paths.end();
    };
    auto extra = [&](const std::string& p) {
        if (!p.empty() && !isAsked(p) && std::find(now.extra.begin(), now.extra.end(), p) == now.extra.end())
            now.extra.push_back(p);
    };
    for (const RawChange& c : parseRawDiff(rawDiff)) {
        extra(c.path);
        extra(c.origPath);
    }
    for (const std::string& p : nulPaths(untrackedOut)) {
        if (isAsked(p)) now.untracked.push_back(p);
        else extra(p);
    }
    std::sort(now.extra.begin(), now.extra.end());
    return now;
}

ApplyResult applyToMain(const ApplySource& s, const std::string& mainName, const GitRunner& run) {
    ApplyResult res;
    auto failed = [&](const GitOut& o) {
        return GitUi::applyFailureText(o.status < 0 ? 1 : o.status, o.out, o.err);
    };
    std::string failure, patch;
    // The worktree again: what it changed since the read is not what the
    // user was asked about, so nothing comes in (round 10's defect 6: the
    // patch was the whole of a fresh diff, checked against the old list).
    ApplyNow now;
    GitOut raw = run(s.top, worktreeRawArgs(s.baseRef), "");
    GitOut un = raw.ok ? run(s.top, GitUi::untrackedArgs(), "") : GitOut();
    if (!raw.ok) failure = failed(raw);
    else if (!un.ok) failure = failed(un);
    else {
        now = applyNow(s.files, raw.out, un.out);
        if (!now.extra.empty()) failure = GitUi::applyStaleText(now.extra);
    }
    // Main again: the user may have changed one of the files while asked.
    if (failure.empty()) {
        GitOut st = run(s.mainPath, GitUi::statusArgs(), "");
        if (!st.ok) failure = failed(st);
        else
            failure = GitUi::applyToMainPlan(s.files.paths, Git::parseStatus(st.out), mainName, "The worktree")
                          .refusal;
    }
    if (failure.empty()) {
        GitOut d = run(s.top, GitUi::worktreeDiffArgs(s.baseRef, s.files.paths), "");
        if (!d.ok) failure = failed(d);
        else patch = d.out;
    }
    for (size_t i = 0; failure.empty() && i < now.untracked.size(); i++) {
        GitOut n = run(s.top, GitUi::newFilePatchArgs(now.untracked[i]), "");
        if (n.ok || n.status == 1) patch += n.out;
        else failure = failed(n);
    }
    if (failure.empty() && patch.empty()) failure = "There was nothing to bring into the main checkout.";
    if (failure.empty()) {
        GitOut plain = run(s.mainPath, GitUi::applyToMainArgs(false), patch);
        if (!plain.ok) {
            GitOut three = run(s.mainPath, GitUi::applyToMainArgs(true), patch);
            GitOut st = run(s.mainPath, GitUi::statusArgs(), "");
            std::vector<std::string> staged;
            auto isAsked = [&](const std::string& p) {
                return std::find(s.files.paths.begin(), s.files.paths.end(), p) != s.files.paths.end();
            };
            if (st.ok)
                for (const Git::Entry& e : Git::parseStatus(st.out).entries) {
                    if (!isAsked(e.path)) continue;
                    if (e.unmerged) res.conflicts.push_back(e.path);
                    else if (e.staged != '.') {
                        staged.push_back(e.path);
                        // A rename's staged deletion of its source, or it
                        // stays staged when only the new path is restored.
                        if (!e.origPath.empty()) staged.push_back(e.origPath);
                    }
                }
            if (!three.ok && res.conflicts.empty()) failure = failed(three);
            else if (!staged.empty()) run(s.mainPath, GitUi::unstageArgs(staged), "");
        }
    }
    res.ok = failure.empty();
    res.text = res.ok ? GitUi::applyDoneText(s.fileCount, res.conflicts, mainName) : failure;
    return res;
}

ApplyResult applyToMain(const WorktreeRead& w, const std::string& mainName, const GitRunner& run) {
    return applyToMain(applySource(w), mainName, run);
}

}  // namespace ChangeReview
