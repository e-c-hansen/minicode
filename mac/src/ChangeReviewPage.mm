// ChangeReviewPage.mm — see ChangeReviewPage.h.
#import "ChangeReviewPage.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "CompareView.h"
#import "EditorFont.h"
#import "GitPanel.h"
#include "ChangeReview.h"
#include "FolderSearch.h"
#include "GitModel.h"
#include "GitGraph.h"
#include "GitStatus.h"
#include "QuickDiff.h"
#include "AgentView.h"
#include "SyntaxHighlighter.h"
#include <algorithm>
#include <memory>
#include <string>
#include <time.h>
#include <vector>

using ChangeReview::FileDiff;
using ChangeReview::Op;
using ChangeReview::Page;
using ChangeReview::PageRow;
using ChangeReview::RowKind;
using ChangeReview::Section;

namespace {

NSColor *RHex(unsigned rgb, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}

NSString *NS8(const std::string &s) {
    NSString *out = [[NSString alloc] initWithBytes:s.data() length:s.size()
                                           encoding:NSUTF8StringEncoding];
    return out ?: [[NSString alloc] initWithBytes:s.data() length:s.size()
                                         encoding:NSISOLatin1StringEncoding];
}

NSString *NS16(const std::u16string &s) {
    return [NSString stringWithCharacters:(const unichar *)s.data() length:s.size()];
}

std::string Std(NSString *s) { return s.UTF8String ?: ""; }

std::string Bytes(NSData *d) {
    return d.length ? std::string((const char *)d.bytes, d.length) : std::string();
}

NSArray<NSString *> *Args(const std::vector<std::string> &v) {
    NSMutableArray *a = [NSMutableArray arrayWithCapacity:v.size()];
    for (const std::string &s : v) [a addObject:NS8(s)];
    return a;
}

double ThreadCpuMs() { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e6; }

// Past this a side is not read (its header says so, Side by Side shows it).
const size_t kMaxSideBytes = 4u << 20;
// Past this in all, the rest of the files are not read either.
const size_t kMaxTotalBytes = 64u << 20;

// What the page keeps beside each file to act on it: its row in the panel,
// the index entry the view was drawn from, how the index's text was read
// (so a staged text is written the same way), and the work tree's bytes
// and encoding (so a discard checks nothing changed, and writes it back
// as it was written).
struct Extra {
    GitUi::Row row;
    std::string indexObject;       // empty when the index has none
    QuickDiff::Original indexHow;  // its encoding and mark
    FolderSearch::TextEncoding workEnc = FolderSearch::TextEncoding::Utf8;
    bool workBom = false;
    bool workRead = false;         // the work tree's file was read
    std::string workBytes;
};

// One read: every file and its diff, and the page laid out from them.
struct ReviewData {
    std::string top;
    bool initial = false;
    std::string error;
    Git::CommitDetail commit;      // a commit's page: its header
    std::string baseRef, baseLabel;   // a worktree's page: what it is compared with
    std::string mainPath;             // ...and the main checkout
    std::vector<FileDiff> diffs;
    std::vector<Extra> extras;
    Page page;
    size_t fingerprint = 0;           // ChangeReview::fingerprint of the diffs
    std::string head;                 // HEAD's commit as status read it (uncommitted changes only)
};

std::string ExtOf(const std::string &path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        // Makefile, Dockerfile: the highlighter knows some names.
        return path.substr(slash == std::string::npos ? 0 : slash + 1);
    }
    std::string e = path.substr(dot + 1);
    for (char &c : e) c = (char)tolower((unsigned char)c);
    return e;
}

// The encoding a blob is in, from its own first bytes: UTF-16 with its
// mark, UTF-8 with or without one, or Windows-1252 (not UTF-8, no NUL).
void EncodingOfBlob(const std::string &b, FolderSearch::TextEncoding *enc, bool *bom) {
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
    }
}

// Everything the page shows, read off the main thread.
std::shared_ptr<ReviewData> ReadReview(NSString *root, const ChangeReview::Opened &opened) {
    auto data = std::make_shared<ReviewData>();
    if (!MCGitExecutable()) {
        data->error = "git was not found, so there is nothing to review.";
        return data;
    }
    MCGitResult *r = MCGitRunSync(root, Args(GitUi::topLevelArgs()));
    std::string top, gitDir;
    if (!r.ok || !GitUi::parseTopLevel(Bytes(r.output), top, gitDir)) {
        data->error = "This folder is not in a git repository.";
        return data;
    }
    data->top = top;
    NSString *topNS = NS8(top);
    MCGitResult *st = MCGitRunSync(topNS, Args(GitUi::statusArgs()));
    if (!st.ok) {
        data->error = Std(st.errorText.length ? st.errorText : @"git status failed.");
        return data;
    }
    GitUi::Snapshot snap;
    GitUi::applyStatus(snap, Git::parseStatus(Bytes(st.output)));
    data->initial = snap.initial;
    data->head = snap.headOid;
    std::vector<GitUi::Row> rows;
    std::vector<std::string> paths;
    for (const GitUi::Row &row : snap.rows) {
        if (row.header) continue;
        rows.push_back(row);
        paths.push_back(row.path);
        if (!row.origPath.empty()) paths.push_back(row.origPath);
    }
    if (rows.empty()) return data;
    std::map<std::string, ChangeReview::Entry> index, head;
    MCGitResult *ls = MCGitRunSync(topNS, Args(ChangeReview::lsFilesArgs(paths)));
    if (ls.ok) index = ChangeReview::parseLsFiles(Bytes(ls.output));
    if (!snap.initial) {
        MCGitResult *lt = MCGitRunSync(topNS, Args(ChangeReview::lsTreeArgs("HEAD", paths)));
        if (lt.ok) head = ChangeReview::parseLsTree(Bytes(lt.output));
    }
    // Every blob wanted, in one cat-file --batch.
    std::string want;
    auto ask = [&](const std::map<std::string, ChangeReview::Entry> &m, const std::string &p) {
        auto it = m.find(p);
        if (it != m.end() && it->second.mode != "160000") want += it->second.object + "\n";
    };
    for (const GitUi::Row &row : rows) {
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
        MCGitResult *b = MCGitRunSyncWithInput(topNS, Args(ChangeReview::batchArgs()),
                                               [NSData dataWithBytes:want.data() length:want.size()]);
        if (b.ok) blobs = ChangeReview::parseBatch(Bytes(b.output));
    }
    size_t total = 0;
    for (const GitUi::Row &row : rows) {
        ChangeReview::File f;
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
            // A conflict: the work tree's text, whose blocks the page shows
            // with Keep Current, Keep Incoming and Keep Both.
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
                if (ChangeReview::looksBinary(bytes)) {
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
            data->extras.push_back(x);
            data->diffs.push_back(ChangeReview::conflictFile(f, ExtOf(f.path)));
            continue;
        }
        if (f.conflict || f.submodule || f.symlink) {
            data->extras.push_back(x);
            data->diffs.push_back(ChangeReview::diffFile(f, ""));
            continue;
        }
        auto blobOf = [&](const std::map<std::string, ChangeReview::Entry> &m, const std::string &p,
                          bool *exists) -> std::string {
            auto it = m.find(p);
            if (it == m.end()) { *exists = false; return ""; }
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
                const std::string path = top + "/" + row.path;
                FolderSearch::FileStamp stamp;
                const FolderSearch::ReadStatus rs =
                    FolderSearch::readFileStable(path, kMaxSideBytes, &newBytes, &stamp);
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
        if (ChangeReview::looksBinary(oldBytes) || ChangeReview::looksBinary(newBytes))
            f.binary = true;
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
                EncodingOfBlob(newExists ? newBytes : oldBytes, &enc, &bom);
                x.indexHow = QuickDiff::readOriginal(newBytes, enc, bom);
                f.newText = x.indexHow.text;
                f.oldText = QuickDiff::readOriginal(oldBytes, enc, bom).text;
            } else {
                x.indexHow = QuickDiff::readOriginal(oldBytes, enc, bom);
                f.oldText = x.indexHow.text;
            }
        }
        data->extras.push_back(x);
        data->diffs.push_back(ChangeReview::diffFile(f, ExtOf(f.path)));
    }
    data->page = ChangeReview::layout(data->diffs, opened);
    return data;
}

// A commit and what it changed against its first parent, read off the
// main thread: its header, diff-tree's records, and their blobs in one
// cat-file --batch.
std::shared_ptr<ReviewData> ReadCommit(NSString *root, NSString *hash,
                                       const ChangeReview::Opened &opened) {
    auto data = std::make_shared<ReviewData>();
    if (!MCGitExecutable()) {
        data->error = "git was not found, so the commit cannot be shown.";
        return data;
    }
    MCGitResult *r = MCGitRunSync(root, Args(GitUi::topLevelArgs()));
    std::string top, gitDir;
    if (!r.ok || !GitUi::parseTopLevel(Bytes(r.output), top, gitDir)) {
        data->error = "This folder is not in a git repository.";
        return data;
    }
    data->top = top;
    NSString *topNS = NS8(top);
    MCGitResult *h = MCGitRunSync(topNS, Args(ChangeReview::commitHeaderArgs(Std(hash))));
    if (!h.ok || !Git::parseShow(Bytes(h.output), data->commit)) {
        data->error = Std(h.errorText.length ? h.errorText : @"git show failed.");
        return data;
    }
    const std::string parent = data->commit.parents.empty() ? "" : data->commit.parents[0];
    MCGitResult *dt = MCGitRunSync(topNS, Args(ChangeReview::commitDiffArgs(data->commit.hash, parent)));
    if (!dt.ok) {
        data->error = Std(dt.errorText.length ? dt.errorText : @"git diff-tree failed.");
        return data;
    }
    const std::vector<ChangeReview::RawChange> changes = ChangeReview::parseRawDiff(Bytes(dt.output));
    std::string want;
    auto wanted = [](const std::string &mode, const std::string &object) {
        return mode != "000000" && mode != "160000" && !ChangeReview::nullObject(object);
    };
    for (const ChangeReview::RawChange &c : changes) {
        if (wanted(c.oldMode, c.oldObject)) want += c.oldObject + "\n";
        if (wanted(c.newMode, c.newObject)) want += c.newObject + "\n";
    }
    std::map<std::string, std::string> blobs;
    if (!want.empty()) {
        MCGitResult *b = MCGitRunSyncWithInput(topNS, Args(ChangeReview::batchArgs()),
                                               [NSData dataWithBytes:want.data() length:want.size()]);
        if (b.ok) blobs = ChangeReview::parseBatch(Bytes(b.output));
    }
    size_t total = 0;
    for (const ChangeReview::RawChange &c : changes) {
        ChangeReview::File f;
        Extra x;
        f.path = c.path;
        f.origPath = c.origPath;
        f.section = Section::Plain;
        f.letter = c.letter;
        f.oldExists = c.oldMode != "000000";
        f.newExists = c.newMode != "000000";
        f.submodule = c.oldMode == "160000" || c.newMode == "160000";
        f.symlink = c.oldMode == "120000" || c.newMode == "120000";
        x.row.path = c.path;
        x.row.origPath = c.origPath;
        x.row.letter = c.letter;
        if (!f.submodule && !f.symlink) {
            auto blob = [&](const std::string &object) {
                auto it = blobs.find(object);
                return it == blobs.end() ? std::string() : it->second;
            };
            const std::string oldBytes = f.oldExists ? blob(c.oldObject) : std::string();
            const std::string newBytes = f.newExists ? blob(c.newObject) : std::string();
            total += oldBytes.size() + newBytes.size();
            if (oldBytes.size() > kMaxSideBytes || newBytes.size() > kMaxSideBytes ||
                total > kMaxTotalBytes)
                f.tooLarge = true;
            else if (ChangeReview::looksBinary(oldBytes) || ChangeReview::looksBinary(newBytes))
                f.binary = true;
            else {
                FolderSearch::TextEncoding enc;
                bool bom = false;
                EncodingOfBlob(f.newExists ? newBytes : oldBytes, &enc, &bom);
                f.oldText = QuickDiff::readOriginal(oldBytes, enc, bom).text;
                f.newText = QuickDiff::readOriginal(newBytes, enc, bom).text;
            }
        }
        data->extras.push_back(x);
        data->diffs.push_back(ChangeReview::diffFile(f, ExtOf(f.path)));
    }
    data->page = ChangeReview::layout(data->diffs, opened);
    return data;
}

// A worktree against where its branch left main, read off the main
// thread: the merge base, `diff --raw` against it (the new side read from
// the worktree's disk), its untracked files, and the old blobs in one
// cat-file --batch.
std::shared_ptr<ReviewData> ReadWorktree(NSString *path, NSString *mainRef,
                                         const ChangeReview::Opened &opened) {
    auto data = std::make_shared<ReviewData>();
    if (!MCGitExecutable()) {
        data->error = "git was not found, so the worktree cannot be compared.";
        return data;
    }
    MCGitResult *r = MCGitRunSync(path, Args(GitUi::topLevelArgs()));
    std::string top, gitDir;
    if (!r.ok || !GitUi::parseTopLevel(Bytes(r.output), top, gitDir)) {
        data->error = "This worktree is not a git checkout any more.";
        return data;
    }
    data->top = top;
    NSString *topNS = NS8(top);
    if (!mainRef.length) {
        data->error = "There is no main branch to compare with.";
        return data;
    }
    MCGitResult *mb = MCGitRunSync(topNS, @[@"merge-base", mainRef, @"HEAD"]);
    const AgentView::CompareBase base =
        AgentView::compareBase(Std(mainRef), mb.ok ? Bytes(mb.output) : std::string());
    data->baseRef = base.ref;
    data->baseLabel = base.label;
    MCGitResult *wl = MCGitRunSync(topNS, Args(ChangeReview::worktreeListArgs()));
    if (wl.ok) data->mainPath = ChangeReview::mainWorktreePath(Bytes(wl.output));
    MCGitResult *dr = MCGitRunSync(topNS, Args(ChangeReview::worktreeRawArgs(base.ref)));
    if (!dr.ok) {
        data->error = Std(dr.errorText.length ? dr.errorText : @"git diff failed.");
        return data;
    }
    std::vector<ChangeReview::RawChange> changes = ChangeReview::parseRawDiff(Bytes(dr.output));
    MCGitResult *un = MCGitRunSync(topNS, Args(GitUi::untrackedArgs()));
    if (un.ok)
        for (const std::string &p : ChangeReview::nulPaths(Bytes(un.output))) {
            ChangeReview::RawChange c;
            c.letter = '?';
            c.oldMode = "000000";
            c.newMode = "100644";
            c.path = p;
            changes.push_back(c);
        }
    std::sort(changes.begin(), changes.end(),
              [](const ChangeReview::RawChange &a, const ChangeReview::RawChange &b) { return a.path < b.path; });
    std::string want;
    for (const ChangeReview::RawChange &c : changes)
        if (c.oldMode != "000000" && c.oldMode != "160000" && !ChangeReview::nullObject(c.oldObject))
            want += c.oldObject + "\n";
    std::map<std::string, std::string> blobs;
    if (!want.empty()) {
        MCGitResult *b = MCGitRunSyncWithInput(topNS, Args(ChangeReview::batchArgs()),
                                               [NSData dataWithBytes:want.data() length:want.size()]);
        if (b.ok) blobs = ChangeReview::parseBatch(Bytes(b.output));
    }
    size_t total = 0;
    for (const ChangeReview::RawChange &c : changes) {
        ChangeReview::File f;
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
            else if (ChangeReview::looksBinary(oldBytes) || ChangeReview::looksBinary(newBytes))
                f.binary = true;
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
        data->extras.push_back(x);
        data->diffs.push_back(ChangeReview::diffFile(f, ExtOf(f.path)));
    }
    data->page = ChangeReview::layout(data->diffs, opened);
    return data;
}

// The rows' font and height: one height for every row, so the page's size
// and a row's place are arithmetic.
NSFont *ReviewFont() { return MCMonoFont(12.5, NSFontWeightRegular); }
CGFloat ReviewRowHeight() {
    static CGFloat h;
    if (!h) {
        NSLayoutManager *lm = [NSLayoutManager new];
        h = ceil([lm defaultLineHeightForFont:ReviewFont()]) + 2;
    }
    return h;
}

const CGFloat kReviewGutter = 80;   // two columns of line numbers
const CGFloat kTopBarHeight = 34;

enum Act { ActStage, ActUnstage, ActDiscard, ActSide, ActOpen, ActDiscardFile, ActStageFile,
           ActUnstageFile, ActKeepCurrent, ActKeepIncoming, ActKeepBoth, ActMarkResolved };

MergeConflicts::Keep KeepOf(Act a) {
    return a == ActKeepIncoming ? MergeConflicts::Keep::Incoming
         : a == ActKeepBoth     ? MergeConflicts::Keep::Both
                                : MergeConflicts::Keep::Current;
}

struct Button {
    NSString *title;
    Act act;
    NSRect rect;
};

NSString *TitleOf(Act a) {
    switch (a) {
    case ActStage: return @"Stage";
    case ActUnstage: return @"Unstage";
    case ActDiscard: return @"Discard";
    case ActSide: return @"Side by Side";
    case ActOpen: return @"Open";
    case ActDiscardFile: return @"Discard File";
    case ActStageFile: return @"Stage File";
    case ActUnstageFile: return @"Unstage File";
    case ActKeepCurrent:
    case ActKeepIncoming:
    case ActKeepBoth: return NS8(MergeConflicts::buttonTitle(KeepOf(a)));
    case ActMarkResolved: return @"Mark Resolved";
    }
    return @"";
}

// A conflict's markers and the Keep buttons: its side's color more
// strongly, as the editor draws them (EditorConflicts.mm).
NSColor *ConflictHeader(NSColor *c) {
    return [c colorWithAlphaComponent:MIN(1.0, MAX(0.25, c.alphaComponent * 2.5))];
}

NSColor *ConflictTint(ChangeReview::ConflictPart part, NSColor *current, NSColor *incoming,
                      NSColor *base) {
    using ChangeReview::ConflictPart;
    switch (part) {
    case ConflictPart::CurrentMarker: return ConflictHeader(current);
    case ConflictPart::Current: return current;
    case ConflictPart::BaseMarker: return ConflictHeader(base);
    case ConflictPart::Base: return base;
    case ConflictPart::Incoming: return incoming;
    case ConflictPart::IncomingMarker: return ConflictHeader(incoming);
    case ConflictPart::Separator:
    case ConflictPart::None: break;
    }
    return nil;
}

NSDictionary *ButtonAttrs(NSColor *color) {
    return @{NSFontAttributeName: [NSFont systemFontOfSize:11 weight:NSFontWeightMedium],
             NSForegroundColorAttributeName: color ?: NSColor.whiteColor};
}

}  // namespace

@class MCReviewTextView;

@interface MCChangeReviewPage ()
- (void)reviewView:(MCReviewTextView *)view pressedButton:(Act)act row:(size_t)row;
- (void)openGapAtRow:(size_t)row;
- (void)openLineAtRow:(size_t)row;
- (NSMenu *)menuForRow:(long)row;
- (void)stepBlock:(BOOL)forward;
- (std::shared_ptr<ReviewData>)data;
- (BOOL)plainFilesOpen;   // a worktree's files are on disk: Open is offered
@end

// ------------------------------------------------------------- the text
// A read-only text view over the page's rows: each row's tint and line
// numbers are drawn behind the text, the buttons over it at the right edge
// of what is on screen, and a click on a button or a folded stretch acts.
@interface MCReviewTextView : NSTextView
@property(nonatomic, weak) MCChangeReviewPage *owner;
@property(nonatomic, strong) NSColor *numberColor;
- (NSRect)rectOfRow:(size_t)row;
- (long)rowAtY:(CGFloat)y;
- (std::vector<Button>)buttonsForRow:(size_t)row;
@end

@implementation MCReviewTextView {
    BOOL _overButton;
}

- (std::shared_ptr<ReviewData>)reviewData { return [self.owner data]; }

- (NSRect)rectOfRow:(size_t)row {
    const CGFloat h = ReviewRowHeight();
    return NSMakeRect(0, self.textContainerOrigin.y + (CGFloat)row * h, NSWidth(self.bounds), h);
}

- (long)rowAtY:(CGFloat)y {
    auto d = [self reviewData];
    if (!d || d->page.rows.empty()) return -1;
    const long r = (long)floor((y - self.textContainerOrigin.y) / ReviewRowHeight());
    return r < 0 || r >= (long)d->page.rows.size() ? -1 : r;
}

- (std::vector<Button>)buttonsForRow:(size_t)row {
    std::vector<Button> out;
    auto d = [self reviewData];
    if (!d || row >= d->page.rows.size()) return out;
    const PageRow &pr = d->page.rows[row];
    if (pr.file >= d->diffs.size()) return out;
    const ChangeReview::File &f = d->diffs[pr.file].file;
    std::vector<Act> acts;
    const FileDiff &fd = d->diffs[pr.file];
    if (pr.kind == RowKind::File && f.conflict && f.section == Section::Changes &&
        (ChangeReview::canMarkResolved(fd) || !fd.conflicts.empty())) {
        // A conflict read as text: its blocks have the choices, and once
        // none is left, Mark Resolved stages it.
        if (!fd.conflicts.empty()) acts.push_back(ActSide);
        acts.push_back(ActOpen);
        if (ChangeReview::canMarkResolved(fd)) acts.push_back(ActMarkResolved);
    } else if (pr.kind == RowKind::Conflict) {
        if (pr.blockStart && f.section == Section::Changes) {
            acts.push_back(ActKeepCurrent);
            acts.push_back(ActKeepIncoming);
            acts.push_back(ActKeepBoth);
        }
    } else if (pr.kind == RowKind::File) {
        const bool shows = !f.binary && !f.submodule && !f.conflict && !f.symlink;
        if (shows) acts.push_back(ActSide);
        // A commit's page (Plain) acts on nothing: only Side by Side, a way
        // of looking.
        if (f.section == Section::Changes || (f.section == Section::Staged && f.newExists) ||
            (f.section == Section::Plain && f.newExists && [self.owner plainFilesOpen]))
            acts.push_back(ActOpen);
        if (f.section == Section::Changes) {
            if (!f.conflict && !f.submodule) acts.push_back(ActDiscardFile);
            acts.push_back(ActStageFile);
        } else if (f.section == Section::Staged) {
            acts.push_back(ActUnstageFile);
        }
    } else if (pr.blockStart && ChangeReview::wholeOnlyReason(f).empty()) {
        if (f.section == Section::Changes) {
            acts.push_back(ActStage);
            acts.push_back(ActDiscard);
        } else if (f.section == Section::Staged) {
            acts.push_back(ActUnstage);
        }
    }
    if (acts.empty()) return out;
    const NSRect line = [self rectOfRow:row];
    NSDictionary *attrs = ButtonAttrs(nil);
    const CGFloat h = NSHeight(line) - 4, gap = 6, padX = 8;
    std::vector<CGFloat> widths;
    CGFloat total = 0;
    for (Act a : acts) {
        const CGFloat w = ceil([TitleOf(a) sizeWithAttributes:attrs].width) + 2 * padX;
        widths.push_back(w);
        total += w + gap;
    }
    // Pinned to the right edge of what is on screen, so they stay in reach
    // however wide the lines are.
    CGFloat x = NSMaxX(self.visibleRect) - 10 - total + gap;
    for (size_t i = 0; i < acts.size(); i++) {
        out.push_back({TitleOf(acts[i]), acts[i], NSMakeRect(x, NSMinY(line) + 2, widths[i], h)});
        x += widths[i] + gap;
    }
    return out;
}

- (void)drawViewBackgroundInRect:(NSRect)rect {
    [super drawViewBackgroundInRect:rect];
    auto d = [self reviewData];
    if (!d || d->page.rows.empty()) return;
    const Page &p = d->page;
    AppSettings *cfg = [AppSettings shared];
    NSColor *bg = [cfg background:Surface::Editor];
    NSColor *removed = RHex(0xF14C4C, 0.16), *added = RHex(0x73C991, 0.16);
    NSColor *header = [bg blendedColorWithFraction:0.10 ofColor:NSColor.grayColor];
    NSColor *gap = [NSColor colorWithWhite:0.5 alpha:0.08];
    NSColor *rule = RHex(0x333333, 1);
    NSColor *bar = RHex(0x4EA1F7, 0.9);
    const Settings &st = [cfg settings];
    NSColor *current = MCColor(st.mergeCurrentColor()), *incoming = MCColor(st.mergeIncomingColor());
    NSColor *base = [NSColor colorWithWhite:0x60 / 255.0 alpha:0x29 / 255.0];
    NSDictionary *num = @{
        NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:10.5 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: self.numberColor ?: NSColor.grayColor};
    const CGFloat h = ReviewRowHeight(), top = self.textContainerOrigin.y;
    const long first = std::max(0L, (long)floor((NSMinY(rect) - top) / h));
    const long last = std::min((long)p.rows.size() - 1, (long)floor((NSMaxY(rect) - top) / h));
    for (long r = first; r <= last; r++) {
        const PageRow &pr = p.rows[(size_t)r];
        const NSRect line = [self rectOfRow:(size_t)r];
        NSColor *tint = nil;
        switch (pr.kind) {
        case RowKind::Removed: tint = removed; break;
        case RowKind::Added: tint = added; break;
        case RowKind::File: tint = header; break;
        case RowKind::Gap: tint = gap; break;
        case RowKind::Conflict: tint = ConflictTint(pr.part, current, incoming, base); break;
        default: break;
        }
        if (tint) {
            [tint set];
            NSRectFillUsingOperation(line, NSCompositingOperationSourceOver);
        }
        if (pr.kind == RowKind::File) {
            [rule set];
            NSRectFill(NSMakeRect(NSMinX(line), NSMinY(line), NSWidth(line), 1));
        }
        // A block's first row: a mark in the gutter, where the eye looks
        // for the next change.
        if (pr.blockStart) {
            [bar set];
            NSRectFill(NSMakeRect(2, NSMinY(line) + 2, 3, NSHeight(line) - 4));
        }
        auto number = [&](int n, CGFloat right) {
            if (n < 0) return;
            NSString *s = [NSString stringWithFormat:@"%d", n + 1];
            const NSSize sz = [s sizeWithAttributes:num];
            [s drawAtPoint:NSMakePoint(right - sz.width, NSMinY(line) + (h - sz.height) / 2)
            withAttributes:num];
        };
        if (pr.kind == RowKind::Context && d->diffs[pr.file].file.conflict) {
            number(pr.newLine, 72);   // one text: one number
        } else if (pr.kind == RowKind::Conflict) {
            number(pr.newLine, 72);
        } else if (pr.kind == RowKind::Context || pr.kind == RowKind::Removed ||
                   pr.kind == RowKind::Added) {
            number(pr.oldLine, 38);
            number(pr.newLine, 72);
        }
    }
}

- (void)drawRect:(NSRect)rect {
    [super drawRect:rect];
    auto d = [self reviewData];
    if (!d || d->page.rows.empty()) return;
    AppSettings *cfg = [AppSettings shared];
    NSColor *bg = [cfg background:Surface::Editor];
    NSColor *text = [cfg text:Surface::Editor];
    const CGFloat h = ReviewRowHeight(), top = self.textContainerOrigin.y;
    const long first = std::max(0L, (long)floor((NSMinY(rect) - top) / h));
    const long last = std::min((long)d->page.rows.size() - 1, (long)floor((NSMaxY(rect) - top) / h));
    [self removeAllToolTips];
    for (long r = first; r <= last; r++) {
        std::vector<Button> bs = [self buttonsForRow:(size_t)r];
        if (bs.empty()) continue;
        // A backing behind the group, so the line under it does not show
        // through.
        NSRect group = NSUnionRect(bs.front().rect, bs.back().rect);
        group = NSInsetRect(group, -6, -2);
        const PageRow &pr = d->page.rows[(size_t)r];
        NSColor *under = pr.kind == RowKind::File
                             ? [bg blendedColorWithFraction:0.10 ofColor:NSColor.grayColor]
                         : pr.kind == RowKind::Removed
                             ? [bg blendedColorWithFraction:0.16 ofColor:RHex(0xF14C4C, 1)]
                         : pr.kind == RowKind::Added
                             ? [bg blendedColorWithFraction:0.16 ofColor:RHex(0x73C991, 1)]
                         : pr.kind == RowKind::Conflict
                             ? [bg blendedColorWithFraction:0.4 ofColor:MCColor([[AppSettings shared] settings].mergeCurrentColor())]
                             : bg;
        [under set];
        NSRectFill(group);
        for (const Button &b : bs) {
            NSColor *fill, *ink;
            switch (b.act) {
            case ActStage:
            case ActStageFile: fill = RHex(0x2E7D4F, 1); ink = NSColor.whiteColor; break;
            case ActDiscard:
            case ActDiscardFile: fill = RHex(0xB3363A, 1); ink = NSColor.whiteColor; break;
            case ActMarkResolved: fill = RHex(0x2E7D4F, 1); ink = NSColor.whiteColor; break;
            case ActKeepCurrent:
                fill = ConflictHeader(MCColor([cfg settings].mergeCurrentColor()));
                ink = text;
                break;
            case ActKeepIncoming:
                fill = ConflictHeader(MCColor([cfg settings].mergeIncomingColor()));
                ink = text;
                break;
            default:
                fill = [bg blendedColorWithFraction:0.25 ofColor:NSColor.grayColor];
                ink = text;
                break;
            }
            [fill set];
            [[NSBezierPath bezierPathWithRoundedRect:b.rect xRadius:4 yRadius:4] fill];
            NSDictionary *attrs = ButtonAttrs(ink);
            const NSSize sz = [b.title sizeWithAttributes:attrs];
            [b.title drawAtPoint:NSMakePoint(NSMidX(b.rect) - sz.width / 2,
                                             NSMidY(b.rect) - sz.height / 2)
                  withAttributes:attrs];
            NSString *tip = nil;
            switch (b.act) {
            case ActStage: tip = @"Stage this change"; break;
            case ActUnstage: tip = @"Unstage this change"; break;
            case ActDiscard: tip = @"Discard this change (asks first; Undo Discard puts it back)"; break;
            case ActSide: tip = @"Show this file side by side"; break;
            case ActOpen: tip = @"Open the file"; break;
            case ActDiscardFile: tip = @"Discard every change to this file (asks first)"; break;
            case ActStageFile: tip = @"Stage the whole file"; break;
            case ActUnstageFile: tip = @"Unstage the whole file"; break;
            case ActKeepCurrent:
            case ActKeepIncoming:
            case ActKeepBoth: {
                const ChangeReview::FileDiff &cd = d->diffs[pr.file];
                const size_t bi = pr.block >= 0 ? (size_t)pr.block : 0;
                tip = bi < cd.conflicts.size()
                          ? NS8(MergeConflicts::tooltip(KeepOf(b.act), cd.conflicts[bi]))
                          : b.title;
                break;
            }
            case ActMarkResolved: tip = @"Stage the file, which tells git its conflicts are resolved"; break;
            }
            [self addToolTipRect:b.rect owner:tip userData:NULL];
        }
    }
}

// The button under a point, if any.
- (BOOL)buttonAt:(NSPoint)p act:(Act *)act row:(long *)row {
    const long r = [self rowAtY:p.y];
    if (r < 0) return NO;
    for (const Button &b : [self buttonsForRow:(size_t)r])
        if (NSPointInRect(p, b.rect)) {
            *act = b.act;
            *row = r;
            return YES;
        }
    return NO;
}

- (BOOL)pressAt:(NSPoint)p clicks:(NSInteger)clicks {
    Act act;
    long row;
    if ([self buttonAt:p act:&act row:&row]) {
        [self.owner reviewView:self pressedButton:act row:(size_t)row];
        return YES;
    }
    const long r = [self rowAtY:p.y];
    auto d = [self reviewData];
    if (r < 0 || !d) return NO;
    const RowKind k = d->page.rows[(size_t)r].kind;
    if (k == RowKind::Gap) {
        [self.owner openGapAtRow:(size_t)r];
        return YES;
    }
    if (clicks == 2 && (k == RowKind::Context || k == RowKind::Added || k == RowKind::Removed)) {
        [self.owner openLineAtRow:(size_t)r];
        return YES;
    }
    return NO;
}

- (void)mouseDown:(NSEvent *)e {
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if ([self pressAt:p clicks:e.clickCount]) return;
    [super mouseDown:e];
}

- (void)mouseMoved:(NSEvent *)e {
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    Act act;
    long row;
    BOOL over = [self buttonAt:p act:&act row:&row];
    if (!over) {
        const long r = [self rowAtY:p.y];
        auto d = [self reviewData];
        over = r >= 0 && d && d->page.rows[(size_t)r].kind == RowKind::Gap;
    }
    if (over) {
        [[NSCursor pointingHandCursor] set];
        _overButton = YES;
        return;
    }
    if (_overButton) [[NSCursor IBeamCursor] set];
    _overButton = NO;
    [super mouseMoved:e];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *a in self.trackingAreas)
        if (a.owner == self && (a.options & NSTrackingMouseMoved) && a.userInfo[@"review"]) return;
    [self addTrackingArea:[[NSTrackingArea alloc]
                              initWithRect:NSZeroRect
                                   options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                                           NSTrackingInVisibleRect
                                     owner:self
                                  userInfo:@{@"review": @YES}]];
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSMenu *menu = [self.owner menuForRow:[self rowAtY:p.y]];
    if (!menu) menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *copy = [menu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@""];
    copy.target = self;
    MCAddAdjustSettingsItem(menu, @"editor", self);
    return menu;
}

- (IBAction)goToNextDifference:(id)sender {
    (void)sender;
    [self.owner stepBlock:YES];
}
- (IBAction)goToPreviousDifference:(id)sender {
    (void)sender;
    [self.owner stepBlock:NO];
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(goToNextDifference:) ||
        item.action == @selector(goToPreviousDifference:)) {
        auto d = [self reviewData];
        return d && !d->page.blockRows.empty();
    }
    return [super validateMenuItem:item];
}
@end

// -------------------------------------------------------- the container
@interface MCReviewContainer : NSView
@property(nonatomic, weak) MCChangeReviewPage *owner;
@end
@implementation MCReviewContainer
- (BOOL)isFlipped { return YES; }
- (void)viewDidUnhide {
    [super viewDidUnhide];
    [self.owner reload];
}
- (void)viewWillMoveToWindow:(NSWindow *)w {
    [super viewWillMoveToWindow:w];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    if (self.window) [nc removeObserver:self name:NSWindowWillCloseNotification object:self.window];
    if (w) [nc addObserver:self selector:@selector(windowClosing:) name:NSWindowWillCloseNotification object:w];
}
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) [self.owner reload];
}
// A closed window keeps its views (releasedWhenClosed is NO), so the page
// would go on answering settings changes with a page it no longer shows:
// it lets go of everything, as when its tab closes.
- (void)windowClosing:(NSNotification *)note {
    (void)note;
    [[NSNotificationCenter defaultCenter] removeObserver:self name:NSWindowWillCloseNotification object:nil];
    [self.owner pageClosed];
}
- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }
@end

// ------------------------------------------------------------- the page
@implementation MCChangeReviewPage {
    __weak MCGitPanel *_panel;
    dispatch_queue_t _queue;
    std::shared_ptr<ReviewData> _data;
    ChangeReview::Opened _opened;
    MCReviewContainer *_view;
    NSScrollView *_scroll;
    MCReviewTextView *_text;
    NSTextField *_titleLabel, *_summaryLabel, *_messageLabel;
    NSButton *_undoButton;
    BOOL _loading, _again;
    NSString *_revealKey;     // a file to scroll to once read
    BOOL _revealTop;
    long _currentBlockRow;
    CGFloat _lastX;
    BOOL _stale;              // something changed while out of sight
    BOOL _closed;             // its tab closed
    size_t _columns;          // the longest row, a tab as 8
    BOOL _commitMode;         // a commit's page, not the uncommitted changes
    NSString *_worktreeLabel, *_worktreeBranch, *_mainRef;   // a worktree's page
    NSButton *_applyButton;   // its Apply to Main…
    NSButton *_sourceButton;  // after applying: Show in Source Control
    BOOL _applying;
    BOOL _applied;            // the changes on show were brought into main
    size_t _appliedPrint;     // ...as this read showed them
    NSString *_appliedMain;   // the main checkout they went into
    // The note at the top is about the last action and the files it
    // touched: it goes once the page shows those files otherwise than the
    // action left them, or HEAD moved (ChangeReview::ActionNote), so
    // "Staged a change" never outlives the staged change.
    ChangeReview::ActionNote _note;
    BOOL _messageSettled;     // _note.settled, for the checks
    std::string _actionHead;  // HEAD as the last action ran, read on the queue
    BOOL _actionHeadKnown;
    size_t _lastConflicts;
    NSString *_commitTitle;   // "a1b2c3d Subject", the tab's title
    NSTextField *_bodyLabel;  // the rest of a commit's message
}

- (instancetype)initWithRoot:(NSString *)root panel:(MCGitPanel *)panel {
    if ((self = [super init])) {
        _root = [root copy];
        _panel = panel;
        _queue = dispatch_queue_create("mutible.review", DISPATCH_QUEUE_SERIAL);
        _currentBlockRow = -1;
        [self build];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(panelRefreshed:)
                                                     name:MCGitPanelDidRefreshNotification
                                                   object:panel];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(pageApplySettings)
                                                     name:MCSettingsDidChangeNotification
                                                   object:nil];
    }
    return self;
}

- (instancetype)initForCommitsWithRoot:(NSString *)root panel:(MCGitPanel *)panel {
    if ((self = [self initWithRoot:root panel:panel])) {
        _commitMode = YES;
        // A commit does not change: the panel's refreshes are not for it.
        [[NSNotificationCenter defaultCenter] removeObserver:self
                                                        name:MCGitPanelDidRefreshNotification
                                                      object:panel];
        _titleLabel.stringValue = @"";
    }
    return self;
}

- (instancetype)initForWorktree:(NSString *)path label:(NSString *)label
                         branch:(NSString *)branch mainRef:(NSString *)mainRef {
    if ((self = [self initWithRoot:path panel:nil])) {
        _worktree = [path copy];
        _worktreeLabel = [label copy];
        _worktreeBranch = [branch copy];
        _mainRef = [mainRef copy];
        _titleLabel.stringValue = [NSString stringWithFormat:@"%@ compared with %@",
                                   label.length ? label : path.lastPathComponent, mainRef ?: @"main"];
        _titleLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
        _applyButton = [NSButton buttonWithTitle:@"Apply to Main…" target:self
                                          action:@selector(applyFromButton:)];
        _applyButton.bezelStyle = NSBezelStyleRounded;
        _applyButton.controlSize = NSControlSizeSmall;
        _applyButton.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        _applyButton.toolTip = @"Bring these changes into the main checkout as uncommitted changes";
        _applyButton.hidden = YES;
        [_view addSubview:_applyButton];
        _sourceButton = [NSButton buttonWithTitle:@"Show in Source Control" target:self
                                           action:@selector(showSourceControl:)];
        _sourceButton.bezelStyle = NSBezelStyleRounded;
        _sourceButton.controlSize = NSControlSizeSmall;
        _sourceButton.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        _sourceButton.toolTip = @"The main checkout's uncommitted changes, with Source Control beside them";
        _sourceButton.hidden = YES;
        [_view addSubview:_sourceButton];
    }
    return self;
}

- (BOOL)plainFilesOpen { return _worktree != nil; }

- (void)showCommit:(NSString *)hash title:(NSString *)title {
    if (!_commitMode || !hash.length) return;
    const BOOL same = [hash isEqualToString:_commit];
    _commit = [hash copy];
    _commitTitle = [title copy];
    if (!same) {
        _opened.clear();
        _data = nullptr;   // a new commit starts at its top
        _revealTop = YES;
    }
    [self reload];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (std::shared_ptr<ReviewData>)data { return _data; }

- (NSTextField *)label:(CGFloat)size weight:(NSFontWeight)w {
    NSTextField *f = [NSTextField labelWithString:@""];
    f.font = [NSFont systemFontOfSize:size weight:w];
    f.lineBreakMode = NSLineBreakByTruncatingTail;
    return f;
}

- (void)build {
    _view = [[MCReviewContainer alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    _view.owner = self;
    _view.wantsLayer = YES;
    _view.postsFrameChangedNotifications = YES;
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(layoutView)
                                                 name:NSViewFrameDidChangeNotification
                                               object:_view];
    _titleLabel = [self label:13 weight:NSFontWeightSemibold];
    _titleLabel.stringValue = @"Uncommitted Changes";
    _summaryLabel = [self label:12 weight:NSFontWeightRegular];
    _messageLabel = [self label:11.5 weight:NSFontWeightRegular];
    _messageLabel.alignment = NSTextAlignmentRight;
    _undoButton = [NSButton buttonWithTitle:@"Undo Discard" target:self action:@selector(undoDiscard:)];
    _undoButton.bezelStyle = NSBezelStyleRounded;
    _undoButton.controlSize = NSControlSizeSmall;
    _undoButton.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _undoButton.hidden = YES;
    _bodyLabel = [NSTextField wrappingLabelWithString:@""];
    _bodyLabel.font = [NSFont systemFontOfSize:12];
    _bodyLabel.maximumNumberOfLines = 12;
    _bodyLabel.selectable = YES;
    _bodyLabel.hidden = YES;
    for (NSView *v in @[_titleLabel, _summaryLabel, _messageLabel, _undoButton, _bodyLabel])
        [_view addSubview:v];

    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, kTopBarHeight, 600, 366)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.drawsBackground = NO;
    _scroll.automaticallyAdjustsContentInsets = NO;
    // TextKit 1, built by hand, laid out only where it is drawn.
    NSTextStorage *storage = [NSTextStorage new];
    NSLayoutManager *lm = [NSLayoutManager new];
    [storage addLayoutManager:lm];
    NSTextContainer *tc = [[NSTextContainer alloc] initWithSize:NSMakeSize(FLT_MAX, FLT_MAX)];
    tc.widthTracksTextView = NO;
    [lm addTextContainer:tc];
    lm.allowsNonContiguousLayout = YES;
    _text = [[MCReviewTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 366) textContainer:tc];
    _text.owner = self;
    _text.editable = NO;
    _text.selectable = YES;
    _text.richText = NO;
    _text.drawsBackground = NO;
    _text.usesFontPanel = NO;
    _text.usesFindBar = YES;
    _text.incrementalSearchingEnabled = YES;
    _text.textContainerInset = NSMakeSize(kReviewGutter, 6);
    _text.minSize = NSMakeSize(0, 0);
    _text.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _text.verticallyResizable = NO;
    _text.horizontallyResizable = NO;
    _text.autoresizingMask = NSViewNotSizable;
    _scroll.documentView = _text;
    _scroll.contentView.postsBoundsChangedNotifications = YES;
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(scrolled:)
                                                 name:NSViewBoundsDidChangeNotification
                                               object:_scroll.contentView];
    [_view addSubview:_scroll];
    [self pageApplySettings];
    [self layoutView];
}

// The buttons stay at the right edge of what shows: a sideways scroll
// draws the rows again rather than moving the old picture.
- (void)scrolled:(NSNotification *)note {
    (void)note;
    const CGFloat x = NSMinX(_scroll.contentView.bounds);
    if (x != _lastX) {
        _lastX = x;
        [_text setNeedsDisplayInRect:_text.visibleRect];
    }
}

- (void)layoutView {
    const NSRect b = _view.bounds;
    const CGFloat W = NSWidth(b);
    [_undoButton sizeToFit];
    CGFloat right = W - 10;
    if (_applyButton && !_applyButton.hidden) {
        [_applyButton sizeToFit];
        const NSSize sz = _applyButton.frame.size;
        right -= sz.width;
        _applyButton.frame = NSMakeRect(right, floor((kTopBarHeight - sz.height) / 2), sz.width, sz.height);
        right -= 8;
    }
    if (!_undoButton.hidden) {
        const NSSize sz = _undoButton.frame.size;
        right -= sz.width;
        _undoButton.frame = NSMakeRect(right, floor((kTopBarHeight - sz.height) / 2), sz.width, sz.height);
        right -= 8;
    }
    CGFloat barH = kTopBarHeight;
    if (_commitMode) {
        // The subject on its line, the author and date under it, then the
        // rest of the message.
        _titleLabel.frame = NSMakeRect(12, 9, MAX(0, W - 24), 17);
        _summaryLabel.frame = NSMakeRect(12, 29, MAX(0, W - 24), 16);
        CGFloat y = 49;
        if (!_bodyLabel.hidden) {
            const CGFloat h = MIN(200.0, ceil([_bodyLabel.cell cellSizeForBounds:
                                                    NSMakeRect(0, 0, MAX(0, W - 24), 400)].height));
            _bodyLabel.frame = NSMakeRect(12, y + 2, MAX(0, W - 24), h);
            y += h + 6;
        }
        _messageLabel.frame = NSZeroRect;
        barH = y + 4;
    } else {
        const CGFloat titleW = MIN(ceil(_titleLabel.fittingSize.width), MAX(0, (right - 12) * 0.6));
        _titleLabel.frame = NSMakeRect(12, 9, titleW, 17);
        const CGFloat sumX = 12 + titleW + 10;
        const CGFloat sumW = MIN(ceil(_summaryLabel.fittingSize.width), MAX(0, right - sumX));
        _summaryLabel.frame = NSMakeRect(sumX, 10, sumW, 16);
        const CGFloat msgX = sumX + sumW + 12;
        const CGFloat need = _message.length ? ceil(_messageLabel.fittingSize.width) : 0;
        const BOOL sourceShown = _sourceButton && !_sourceButton.hidden;
        if (need > 0 && (need > right - msgX || sourceShown)) {
            // No room beside the title (a worktree's long one), or a button
            // to go with it: the note gets a line of its own under the bar,
            // so what an action did is always there to read.
            CGFloat lineRight = W - 10;
            if (sourceShown) {
                [_sourceButton sizeToFit];
                const NSSize sz = _sourceButton.frame.size;
                _sourceButton.frame = NSMakeRect(MIN(12 + need + 10, lineRight - sz.width),
                                                 kTopBarHeight - 4 + floor((24 - sz.height) / 2),
                                                 sz.width, sz.height);
                lineRight = NSMinX(_sourceButton.frame) - 10;
            }
            _messageLabel.alignment = NSTextAlignmentLeft;
            _messageLabel.frame = NSMakeRect(12, kTopBarHeight, MAX(0, lineRight - 12), 16);
            barH = kTopBarHeight + 24;
        } else {
            _messageLabel.alignment = NSTextAlignmentRight;
            _messageLabel.frame = NSMakeRect(msgX, 10, MAX(0, right - msgX), 16);
        }
    }
    _scroll.frame = NSMakeRect(0, barH, W, MAX(0, NSHeight(b) - barH));
    [self fitText];
}

// The text view as wide as its longest row (or the pane) and as tall as
// its rows, from the font's measures, never by laying the text out.
- (void)fitText {
    const size_t rows = _data ? _data->page.rows.size() : 0;
    const size_t cols = _columns;
    const CGFloat advance = [@"0" sizeWithAttributes:@{NSFontAttributeName: ReviewFont()}].width;
    const NSSize clip = _scroll.contentView.bounds.size;
    const CGFloat w = MAX(clip.width, ceil(cols * advance) + kReviewGutter + 40);
    const CGFloat h = MAX(clip.height, (CGFloat)MAX(rows, (size_t)1) * ReviewRowHeight() + 12);
    [_text setFrameSize:NSMakeSize(w, h)];
}

// ------------------------------------------------------- EditorPage
- (NSString *)pageTitle {
    if (_worktree)
        return [NSString stringWithFormat:@"%@ vs %@", _worktree.lastPathComponent, _mainRef ?: @"main"];
    return _commitMode ? (_commitTitle ?: @"Commit") : @"Uncommitted Changes";
}
- (NSString *)pageKind {
    return _worktree ? @"worktree-changes" : _commitMode ? @"commit" : @"review";
}
- (NSString *)pageKey {
    if (_worktree) return [@"worktree:" stringByAppendingString:_worktree];
    return [(_commitMode ? @"commit:" : @"review:") stringByAppendingString:_root ?: @""];
}
- (NSString *)headerTitle { return _titleLabel.stringValue; }
- (NSString *)headerDetail { return _summaryLabel.stringValue; }
- (NSView *)pageView { return _view; }
- (NSView *)pageFocusView { return _text; }
- (NSTextView *)textView { return _text; }
- (NSScrollView *)scrollView { return _scroll; }

// Whether the page is on screen in its tab; a page out of sight reads and
// recolors nothing until it shows again.
- (BOOL)onShow {
    return _view.window != nil && !_view.isHiddenOrHasHiddenAncestor;
}

- (void)pageApplySettings {
    AppSettings *cfg = [AppSettings shared];
    NSColor *bg = [cfg background:Surface::Editor];
    NSColor *text = [cfg text:Surface::Editor];
    _view.layer.backgroundColor = bg.CGColor;
    _titleLabel.textColor = text;
    _summaryLabel.textColor = [text colorWithAlphaComponent:0.6];
    _bodyLabel.textColor = [text colorWithAlphaComponent:0.8];
    _text.numberColor = [text colorWithAlphaComponent:0.4];
    _text.textColor = text;
    _text.insertionPointColor = text;
    if (_data && [self onShow]) [self fillText];
    else if (_data) _stale = YES;
    _text.needsDisplay = YES;
}

- (void)pageClosed {
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    _closed = YES;
    _data = nullptr;
    [_text.textStorage setAttributedString:[NSAttributedString new]];
}

// ------------------------------------------------------------ reading
- (void)panelRefreshed:(NSNotification *)note {
    (void)note;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(reload) object:nil];
    [self performSelector:@selector(reload) withObject:nil afterDelay:0.05];
}

- (void)reload {
    // Out of sight (another tab, a closed window): read when shown again.
    if (_closed) return;
    if (![self onShow]) {
        _stale = YES;
        return;
    }
    _stale = NO;
    if (_loading) {
        _again = YES;
        return;
    }
    if (_commitMode && !_commit) return;
    _loading = YES;
    NSString *root = _root, *commit = _commit;
    const BOOL commitMode = _commitMode;
    NSString *worktree = _worktree, *mainRef = _mainRef;
    const ChangeReview::Opened opened = _opened;
    __weak MCChangeReviewPage *weakSelf = self;
    dispatch_async(_queue, ^{
        std::shared_ptr<ReviewData> d = worktree ? ReadWorktree(worktree, mainRef, opened)
                                        : commitMode ? ReadCommit(root, commit, opened)
                                                     : ReadReview(root, opened);
        d->fingerprint = ChangeReview::fingerprint(d->diffs);
        dispatch_async(dispatch_get_main_queue(), ^{
            MCChangeReviewPage *s = weakSelf;
            if (!s) return;
            s->_loading = NO;
            // Its window closed or its tab went while git was read: a page
            // nobody sees is not built (100,000 rows cost seconds of the
            // main thread, laid out in idle time, after every suite since).
            if (s->_closed) return;
            // A commit shown meanwhile: this read is the old one's.
            if (commitMode && ![commit isEqualToString:s->_commit]) {
                [s reload];
                return;
            }
            [s noteRead:d];
            [s show:d];
            if (s->_again) {
                s->_again = NO;
                [s reload];
            }
        });
    });
}

- (void)revealFile:(NSString *)rel staged:(BOOL)staged {
    if (!rel.length) {
        _revealKey = nil;
        _revealTop = YES;
    } else {
        ChangeReview::File f;
        f.path = Std(rel);
        f.section = staged ? Section::Staged : Section::Changes;
        _revealKey = NS8(f.key());
        _revealTop = NO;
    }
    if (_data && !_loading) [self applyReveal];
    [self reload];
}

- (void)applyReveal {
    if (!_data) return;
    long row = -1;
    if (_revealKey) {
        row = ChangeReview::fileRow(_data->page, _data->diffs, Std(_revealKey));
        // The file is in the other list now (staged meanwhile).
        if (row < 0 && _revealKey.length > 2)
            row = ChangeReview::fileRow(
                _data->page, _data->diffs,
                std::string([_revealKey hasPrefix:@"S:"] ? "W:" : "S:") + Std([_revealKey substringFromIndex:2]));
    }
    if (_revealTop) row = 0;
    if (row < 0) return;
    // A file's section row stays in sight above its first file.
    if (row > 0 && _data->page.rows[(size_t)row - 1].kind == RowKind::Section) row--;
    [self scrollRowToTop:(size_t)row offset:0];
    _revealKey = nil;
    _revealTop = NO;
}

- (void)scrollRowToTop:(size_t)row offset:(CGFloat)offset {
    [self fitText];
    NSClipView *clip = _scroll.contentView;
    NSRect want = clip.bounds;
    // The row's top at the pane's top (the first row with the inset above it).
    want.origin.y = row == 0 ? offset : NSMinY([_text rectOfRow:row]) + offset;
    want.origin.y = MAX(0, MIN(want.origin.y, NSHeight(_text.frame) - NSHeight(want)));
    [clip scrollToPoint:[clip constrainBoundsRect:want].origin];
    [_scroll reflectScrolledClipView:clip];
}

- (NSInteger)topRow {
    if (!_data || _data->page.rows.empty()) return 0;
    const CGFloat y = NSMinY(_scroll.contentView.bounds) - _text.textContainerOrigin.y + 2;
    const long r = (long)floor(y / ReviewRowHeight());
    return (NSInteger)MAX(0L, MIN(r, (long)_data->page.rows.size() - 1));
}

// A read on show, keeping the place the eye was on: the same line of the
// same file, at the same height.
- (void)show:(std::shared_ptr<ReviewData>)d {
    if (_closed) return;
    const double c0 = ThreadCpuMs();
    ChangeReview::Anchor anchor;
    CGFloat offset = 0;
    if (_data && !_data->page.rows.empty()) {
        const NSInteger top = self.topRow;
        anchor = ChangeReview::anchorAt(_data->page, _data->diffs, (size_t)top);
        offset = NSMinY(_scroll.contentView.bounds) -
                 (top == 0 ? 0 : NSMinY([_text rectOfRow:(size_t)top]));
    }
    const BOOL first = _data == nullptr;
    _data = d;
    _currentBlockRow = -1;
    _columns = 0;
    size_t col = 0;
    for (char16_t ch : d->page.text) {
        if (ch == u'\n') col = 0;
        else col += ch == u'\t' ? 8 : 1;
        _columns = std::max(_columns, col);
    }
    [self fillText];
    [self updateSummary];
    if (_revealKey || _revealTop) [self applyReveal];
    else if (anchor.valid)
        [self scrollRowToTop:ChangeReview::rowFor(_data->page, _data->diffs, anchor) offset:offset];
    else if (first) [self scrollRowToTop:0 offset:0];
    _text.needsDisplay = YES;
    _lastShowCpuMilliseconds = ThreadCpuMs() - c0;
    _loaded = YES;
    _loads++;
    if (self.onLoaded) self.onLoaded();
}

// The page's text with its fonts, syntax colors and changed words.
- (void)fillText {
    AppSettings *cfg = [AppSettings shared];
    NSColor *text = [cfg text:Surface::Editor];
    NSColor *muted = [text colorWithAlphaComponent:0.6];
    NSMutableParagraphStyle *rows = [NSMutableParagraphStyle new];
    rows.minimumLineHeight = rows.maximumLineHeight = ReviewRowHeight();
    NSDictionary *base = @{NSFontAttributeName: ReviewFont(),
                           NSForegroundColorAttributeName: text,
                           NSParagraphStyleAttributeName: rows};
    NSMutableAttributedString *s;
    if (!_data->error.empty() || _data->diffs.empty()) {
        NSString *why = !_data->error.empty() ? NS8(_data->error)
                        : _worktree           ? @"No changes against the main branch."
                        : _commitMode         ? @"This commit changes no files."
                                              : @"No uncommitted changes.";
        s = [[NSMutableAttributedString alloc] initWithString:why attributes:base];
        [s addAttribute:NSForegroundColorAttributeName value:muted range:NSMakeRange(0, s.length)];
        [s addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:12.5]
                  range:NSMakeRange(0, s.length)];
        [self setPageText:s];
        [self fitText];
        return;
    }
    const Page &p = _data->page;
    s = [[NSMutableAttributedString alloc] initWithString:NS16(p.text) attributes:base];
    const NSUInteger len = s.length;
    [s beginEditing];
    NSFont *headerFont = [NSFont systemFontOfSize:12.5 weight:NSFontWeightSemibold];
    NSFont *sectionFont = [NSFont systemFontOfSize:11.5 weight:NSFontWeightSemibold];
    NSFont *noteFont = [NSFont systemFontOfSize:11.5];
    for (size_t r = 0; r < p.rows.size(); r++) {
        const RowKind k = p.rows[r].kind;
        if (k == RowKind::Context || k == RowKind::Removed || k == RowKind::Added) continue;
        const size_t from = p.starts[r];
        const size_t to = r + 1 < p.starts.size() ? p.starts[r + 1] - 1 : p.text.size();
        if (to > len || to <= from) continue;
        const NSRange range = NSMakeRange(from, to - from);
        switch (k) {
        case RowKind::File:
            [s addAttribute:NSFontAttributeName value:headerFont range:range];
            break;
        case RowKind::Section:
            [s addAttributes:@{NSFontAttributeName: sectionFont,
                               NSForegroundColorAttributeName: muted} range:range];
            break;
        default:
            [s addAttributes:@{NSFontAttributeName: noteFont,
                               NSForegroundColorAttributeName: muted} range:range];
            break;
        }
    }
    NSColor *colors[kTokenStyleCount];
    for (int i = 0; i < kTokenStyleCount; i++) colors[i] = [cfg syntax:(TokenStyle)i];
    for (const SideBySide::Span &sp : p.syntax) {
        if (sp.start + sp.length > len || sp.style <= 0 || sp.style >= kTokenStyleCount) continue;
        [s addAttribute:NSForegroundColorAttributeName value:colors[sp.style]
                  range:NSMakeRange(sp.start, sp.length)];
    }
    NSColor *removedWord = RHex(0xF14C4C, 0.38), *addedWord = RHex(0x73C991, 0.36);
    for (const SideBySide::Span &sp : p.words) {
        if (sp.start + sp.length > len) continue;
        [s addAttribute:NSBackgroundColorAttributeName value:sp.style ? addedWord : removedWord
                  range:NSMakeRange(sp.start, sp.length)];
    }
    [s endEditing];
    [self setPageText:s];
    [self fitText];
}

// The storage emptied first: replacing a laid-out page of 100,000 rows with
// another in one step cost TextKit about 5 s of the main thread (measured
// headless: 13 ms the first time, 4.8 s for each replacement after), and
// emptying it, then setting the new page, 6 ms.
- (void)setPageText:(NSAttributedString *)s {
    NSTextStorage *storage = _text.textStorage;
    if (storage.length > 100000) [storage setAttributedString:[NSAttributedString new]];
    [storage setAttributedString:s];
}

- (void)updateSummary {
    // Files, not lists: one with staged and unstaged changes is one file.
    size_t files = _data ? ChangeReview::fileCount(_data->diffs) : 0, added = 0, removed = 0;
    if (_data)
        for (const FileDiff &d : _data->diffs) {
            added += d.added;
            removed += d.removed;
        }
    NSString *counts = files == 0 ? @""
                       : [NSString stringWithFormat:@"%zu %@, +%zu -%zu", files,
                                                    files == 1 ? @"file" : @"files", added, removed];
    if (_commitMode && _data && _data->error.empty()) {
        const Git::CommitDetail &c = _data->commit;
        const std::string msg = GitUi::validUtf8(c.message);
        const size_t nl = msg.find('\n');
        _titleLabel.stringValue = NS8(nl == std::string::npos ? msg : msg.substr(0, nl));
        std::string body = nl == std::string::npos ? "" : msg.substr(nl + 1);
        while (!body.empty() && (body[0] == '\n' || body[0] == '\r')) body.erase(0, 1);
        _bodyLabel.stringValue = NS8(body);
        _bodyLabel.hidden = body.empty();
        NSString *by = NS8(ChangeReview::commitByline(c.author, c.time, c.hash, c.parents.size(),
                                                      (long long)NSDate.date.timeIntervalSince1970));
        _summaryLabel.stringValue = counts.length ? [NSString stringWithFormat:@"%@  ·  %@", by, counts] : by;
        _summaryLabel.toolTip = c.committer.empty() ? nil : NS8("Committed by " + c.committer);
        _undoButton.hidden = YES;
        [self layoutView];
        return;
    }
    if (_worktree) {
        NSMutableArray *parts = [NSMutableArray array];
        if (_data && !_data->baseLabel.empty()) [parts addObject:[@"against " stringByAppendingString:NS8(_data->baseLabel)]];
        if (_worktreeBranch.length) [parts addObject:_worktreeBranch];
        if (counts.length) [parts addObject:counts];
        _summaryLabel.stringValue = [parts componentsJoinedByString:@"  ·  "];
        _applyButton.hidden = !_data || _data->diffs.empty() || _data->mainPath.empty();
        // Once applied, the button says so until the worktree's changes
        // are other than those brought in (the agent worked on).
        if (_applied && _data && _data->fingerprint != _appliedPrint) _applied = NO;
        _applyButton.title = _applied ? @"Applied to Main" : @"Apply to Main…";
        _applyButton.enabled = !_applied && !_applying;
        _applyButton.toolTip = _applied
            ? @"These changes are in the main checkout now, uncommitted. The button comes back "
              @"when the worktree's changes differ from what was brought in."
            : @"Bring these changes into the main checkout as uncommitted changes";
        _sourceButton.hidden = !_applied || !self.onShowSourceControl;
        _undoButton.hidden = YES;
        [self layoutView];
        return;
    }
    _summaryLabel.stringValue = counts;
    MCGitPanel *panel = _panel;
    NSString *undo = _commitMode ? nil : panel.undoDiscardTitle;
    _undoButton.hidden = undo == nil;
    _undoButton.toolTip = undo;
    [self layoutView];
}

// Whether a read says which commit HEAD is: only the page of uncommitted
// changes reads it ("" for a repository with no commit yet).
- (BOOL)readKnowsHead:(const std::shared_ptr<ReviewData> &)d {
    return d && d->error.empty() && !_worktree && !_commitMode;
}

// `paths`: the files the note speaks of (every file when empty). `ran`: the
// action went through the page's queue, so the next read shown is its
// result; else nothing changed (a refusal) and the page as it is now is.
- (void)say:(NSString *)text ok:(BOOL)ok about:(const std::vector<std::string> &)paths ran:(BOOL)ran {
    _message = [text copy];
    _note = ChangeReview::ActionNote();
    for (const std::string &p : paths)
        if (!p.empty()) _note.paths.push_back(p);
    if (ran && _actionHeadKnown) {
        _note.headKnown = true;
        _note.head = _actionHead;
    } else if ([self readKnowsHead:_data]) {
        _note.headKnown = true;
        _note.head = _data->head;
    }
    _actionHeadKnown = NO;
    // A worktree's page is what it compares, which no action of its own
    // changes: the result is on show already.
    if ((!ran || _worktree) && _data && _data->error.empty()) _note.settle(_data->diffs);
    _messageSettled = _note.settled;
    _messageLabel.stringValue = text ?: @"";
    _messageLabel.toolTip = text;
    AppSettings *cfg = [AppSettings shared];
    _messageLabel.textColor = ok ? [[cfg text:Surface::Editor] colorWithAlphaComponent:0.7]
                                 : RHex(0xF14C4C, 1);
    [_panel showActionMessage:text ok:ok];
    [self updateSummary];
}

// A read about to go on show: the note at the top stays while the files it
// speaks of are as its action left them, and goes when they are not (a
// commit made in the panel or a terminal, even one landing before the
// first read after the action; an unstage, a discard, an edit or git from
// outside). Reads are delivered in the order started, and an action runs
// on the same queue, so every read shown after the note is its result or
// later.
- (void)noteRead:(const std::shared_ptr<ReviewData> &)d {
    if (!_message.length || _commitMode || !d->error.empty()) return;
    const BOOL goes = _note.goesWith(d->diffs, [self readKnowsHead:d], d->head);
    _messageSettled = _note.settled;
    if (!goes) return;
    _message = nil;
    _messageLabel.stringValue = @"";
    _messageLabel.toolTip = nil;
}

// ------------------------------------------------------------ queries
- (NSUInteger)blockCount { return _data ? _data->page.blockRows.size() : 0; }

- (NSInteger)rowOfBlock:(NSUInteger)block {
    if (!_data || block >= _data->page.blockRows.size()) return -1;
    return (NSInteger)_data->page.blockRows[block];
}

- (NSInteger)rowOfFile:(NSString *)rel staged:(BOOL)staged {
    if (!_data) return -1;
    ChangeReview::File f;
    f.path = Std(rel);
    f.section = staged ? Section::Staged : Section::Changes;
    return ChangeReview::fileRow(_data->page, _data->diffs, f.key());
}

- (NSRect)rectOfRow:(NSInteger)row { return row < 0 ? NSZeroRect : [_text rectOfRow:(size_t)row]; }

- (NSRect)buttonRect:(NSString *)title row:(NSInteger)row {
    if (row < 0) return NSZeroRect;
    for (const Button &b : [_text buttonsForRow:(size_t)row])
        if ([b.title isEqualToString:title]) return b.rect;
    return NSZeroRect;
}

- (BOOL)pressAtPoint:(NSPoint)point { return [_text pressAt:point clicks:1]; }

- (NSArray<NSString *> *)rowDescriptions {
    NSMutableArray *out = [NSMutableArray array];
    if (!_data) return out;
    const Page &p = _data->page;
    for (size_t r = 0; r < p.rows.size(); r++) {
        const size_t to = r + 1 < p.starts.size() ? p.starts[r + 1] - 1 : p.text.size();
        NSString *t = NS16(p.text.substr(p.starts[r], to - p.starts[r]));
        NSString *mark = @"  ";
        switch (p.rows[r].kind) {
        case RowKind::Section: mark = @"§ "; break;
        case RowKind::File: mark = @"F "; break;
        case RowKind::Removed: mark = @"- "; break;
        case RowKind::Added: mark = @"+ "; break;
        case RowKind::Gap: mark = @"~ "; break;
        case RowKind::Note: mark = @"! "; break;
        case RowKind::Context: break;
        case RowKind::Conflict:
            switch (p.rows[r].part) {
            case ChangeReview::ConflictPart::BaseMarker:
            case ChangeReview::ConflictPart::Base: mark = @"| "; break;
            case ChangeReview::ConflictPart::Separator: mark = @"= "; break;
            case ChangeReview::ConflictPart::Incoming:
            case ChangeReview::ConflictPart::IncomingMarker: mark = @"> "; break;
            default: mark = @"< "; break;
            }
            break;
        }
        [out addObject:[mark stringByAppendingString:t]];
    }
    return out;
}

// ------------------------------------------------------------- moving
- (void)stepBlock:(BOOL)forward {
    if (!_data || _data->page.blockRows.empty()) {
        NSBeep();
        return;
    }
    const long top = self.topRow;
    const NSRect visible = _scroll.contentView.bounds;
    long from;
    if (_currentBlockRow >= 0 && NSIntersectsRect([_text rectOfRow:(size_t)_currentBlockRow], visible))
        from = _currentBlockRow;
    else
        from = forward ? top - 1 : top;
    const long next = ChangeReview::stepBlock(_data->page, from, forward);
    if (next < 0) return;
    _currentBlockRow = next;
    // A quarter of the way down, as the compare view puts a change.
    [self scrollRowToTop:(size_t)next offset:-floor(NSHeight(visible) / 4)];
    if (next < (long)_data->page.starts.size())
        [_text setSelectedRange:NSMakeRange(_data->page.starts[(size_t)next], 0)];
}

- (void)openGapAtRow:(size_t)row {
    if (!_data || row >= _data->page.rows.size()) return;
    const PageRow &pr = _data->page.rows[row];
    if (pr.kind != RowKind::Gap || pr.file >= _data->diffs.size()) return;
    _opened[_data->diffs[pr.file].file.key()].insert(pr.gap);
    // Laid out again from the same diffs: nothing to read.
    auto d = std::make_shared<ReviewData>(*_data);
    d->page = ChangeReview::layout(d->diffs, _opened);
    [self show:d];
}

- (void)openLineAtRow:(size_t)row {
    if (!_data || row >= _data->page.rows.size() || !self.onOpenFile) return;
    const PageRow &pr = _data->page.rows[row];
    const ChangeReview::File &f = _data->diffs[pr.file].file;
    int line = pr.newLine;
    // A removed line: the line after it in the new text.
    for (size_t r = row; line < 0 && r < _data->page.rows.size() && _data->page.rows[r].file == pr.file; r++)
        line = _data->page.rows[r].newLine;
    self.onOpenFile([NS8(_data->top) stringByAppendingPathComponent:NS8(f.path)], line < 0 ? 0 : line + 1);
}

// ------------------------------------------------------------- acting
- (NSString *)absolutePath:(const ChangeReview::File &)f {
    return [NS8(_data->top) stringByAppendingPathComponent:NS8(f.path)];
}

// An action's work on the page's queue, HEAD read there first: a note about
// the action speaks of changes on that commit, whatever the page last read.
- (void)act:(dispatch_block_t)work {
    NSString *top = [self readKnowsHead:_data] ? NS8(_data->top) : nil;
    __weak MCChangeReviewPage *weakSelf = self;
    dispatch_async(_queue, ^{
        if (top) {
            MCGitResult *r = MCGitRunSync(top, @[@"rev-parse", @"-q", @"--verify", @"HEAD"]);
            // 1 with nothing written: no commit yet, which status reads as "".
            const BOOL known = r.status == 0 || (r.status == 1 && r.output.length == 0);
            NSString *out = [[NSString alloc] initWithData:r.output ?: [NSData data] encoding:NSUTF8StringEncoding];
            const std::string head = r.status == 0 ? Std([out ?: @"" stringByTrimmingCharactersInSet:
                                                                         NSCharacterSet.whitespaceAndNewlineCharacterSet])
                                                   : std::string();
            dispatch_async(dispatch_get_main_queue(), ^{
                MCChangeReviewPage *s = weakSelf;
                if (!s) return;
                s->_actionHead = head;
                s->_actionHeadKnown = known;
            });
        }
        work();
    });
}

// After an action that ran on the queue (`ran`), or one refused at once.
- (void)finish:(NSString *)text ok:(BOOL)ok about:(const std::vector<std::string> &)paths ran:(BOOL)ran {
    [self say:text ok:ok about:paths ran:ran];
    if (!ok) NSBeep();
    [_panel refresh];
    [self reload];
    if (self.onActionDone) self.onActionDone();
}

- (void)reviewView:(MCReviewTextView *)view pressedButton:(Act)act row:(size_t)row {
    (void)view;
    if (!_data || row >= _data->page.rows.size()) return;
    const PageRow &pr = _data->page.rows[row];
    if (pr.file >= _data->diffs.size()) return;
    const size_t fi = pr.file;
    switch (act) {
    case ActStage: [self runOp:Op::Stage file:fi block:pr.block oldLines:std::vector<int>() newLines:std::vector<int>()]; break;
    case ActUnstage: [self runOp:Op::Unstage file:fi block:pr.block oldLines:std::vector<int>() newLines:std::vector<int>()]; break;
    case ActDiscard: [self runOp:Op::Discard file:fi block:pr.block oldLines:std::vector<int>() newLines:std::vector<int>()]; break;
    case ActSide: [self sideBySide:fi]; break;
    case ActOpen: [self openFile:fi]; break;
    case ActDiscardFile: [self discardFile:fi]; break;
    case ActStageFile:
    case ActUnstageFile: [self stageFile:fi]; break;
    case ActKeepCurrent:
    case ActKeepIncoming:
    case ActKeepBoth: [self keep:KeepOf(act) file:fi block:pr.block data:_data]; break;
    case ActMarkResolved: [self markResolved:fi]; break;
    }
}

- (void)sideBySide:(size_t)fi {
    [self sideBySide:fi data:_data];
}
- (void)sideBySide:(size_t)fi data:(std::shared_ptr<ReviewData>)data {
    if (!data || fi >= data->diffs.size()) return;
    const ChangeReview::File &f = data->diffs[fi].file;
    MCComparison *c = [MCComparison new];
    NSString *path = NS8(f.path);
    const BOOL staged = f.section == Section::Staged;
    c.title = [path.lastPathComponent stringByAppendingString:@" (side by side)"];
    NSString *oldPath = NS8(f.origPath.empty() ? f.path : f.origPath);
    if (f.section == Section::Plain && _worktree) {
        c.leftLabel = [NSString stringWithFormat:@"%@: %@", NS8(data->baseLabel), oldPath];
        c.rightLabel = [NSString stringWithFormat:@"%@: %@", _worktreeBranch ?: @"worktree", path];
    } else if (f.section == Section::Plain) {
        const Git::CommitDetail &cd = data->commit;
        c.leftLabel = [NSString stringWithFormat:@"%@: %@",
                       cd.parents.empty() ? @"(nothing before)" : NS8(Git::shortHash(cd.parents[0])), oldPath];
        c.rightLabel = [NSString stringWithFormat:@"%@: %@", NS8(Git::shortHash(cd.hash)), path];
    } else {
        c.leftLabel = [(staged ? @"HEAD: " : @"Staged or HEAD: ") stringByAppendingString:oldPath];
        c.rightLabel = [(staged ? @"Staged: " : @"") stringByAppendingString:path];
    }
    const FileDiff &fd = data->diffs[fi];
    if (f.conflict && !fd.conflicts.empty()) {
        // A conflict's two sides: every block as the current side keeps it
        // against every block as the incoming side does.
        const MergeConflicts::Block &b0 = fd.conflicts[0];
        c.title = [path.lastPathComponent stringByAppendingString:@" (current and incoming)"];
        c.leftLabel = [NSString stringWithFormat:@"Current%@: %@",
                       b0.currentLabel.empty() ? @"" : [NSString stringWithFormat:@" (%@)", NS8(b0.currentLabel)], path];
        c.rightLabel = [NSString stringWithFormat:@"Incoming%@: %@",
                        b0.incomingLabel.empty() ? @"" : [NSString stringWithFormat:@" (%@)", NS8(b0.incomingLabel)], path];
        c.leftText = NS16(MergeConflicts::resolvedAll(f.newText, fd.conflicts, MergeConflicts::Keep::Current));
        c.rightText = NS16(MergeConflicts::resolvedAll(f.newText, fd.conflicts, MergeConflicts::Keep::Incoming));
    } else {
        c.leftText = NS16(f.oldText);
        c.rightText = NS16(f.newText);
    }
    c.fileName = path;
    // How it is made again with its tab (a conflict's sides and a
    // worktree's are not): the index, HEAD, the commit and its parent, or
    // the file on disk, as the page read them.
    NSString *top = NS8(data->top);
    if (top.length && !(f.conflict && !fd.conflicts.empty()) && !(f.section == Section::Plain && _worktree)) {
        c.leftPath = [top stringByAppendingPathComponent:oldPath];
        c.rightPath = [top stringByAppendingPathComponent:path];
        if (f.section == Section::Plain) {
            const Git::CommitDetail &cd = data->commit;
            c.leftRev = cd.parents.empty() ? [NS8(cd.hash) stringByAppendingString:@"^"] : NS8(cd.parents[0]);
            c.rightRev = NS8(cd.hash);
        } else if (staged) {
            c.leftRev = @"HEAD";
            c.rightRev = @":";
        } else {
            c.leftRev = @":";
            c.rightRev = @"";
        }
    }
    // (Neither side of a conflict's comparison is the file on disk.)
    if ((f.section == Section::Changes && !(f.conflict && !fd.conflicts.empty())) ||
        (f.section == Section::Plain && _worktree && f.newExists))
        c.path = [NS8(data->top) stringByAppendingPathComponent:path];
    if (self.onShowComparison) self.onShowComparison(c);
}

- (void)openFile:(size_t)fi {
    const FileDiff &d = _data->diffs[fi];
    int line = -1;
    for (const SideBySide::Row &r : d.rows)
        if (r.kind != SideBySide::Kind::Same && r.right >= 0) { line = r.right; break; }
    if (self.onOpenFile) self.onOpenFile([self absolutePath:d.file], line < 0 ? 0 : line + 1);
}

- (void)discardFile:(size_t)fi {
    // The panel's Discard Changes: it asks, trashes and restores, and its
    // refresh brings the page up to date.
    [_panel discardPath:[self absolutePath:_data->diffs[fi].file]];
}

- (void)stageFile:(size_t)fi {
    [self stageFile:fi done:nil];
}

// Mark Resolved: a conflicted file with no blocks left staged as it is,
// which is what tells git its conflict is resolved (the panel's question
// about conflict marks is not needed: none are left).
- (void)markResolved:(size_t)fi {
    if (!_data || fi >= _data->diffs.size() || !ChangeReview::canMarkResolved(_data->diffs[fi])) {
        NSBeep();
        return;
    }
    NSString *name = NS8(_data->diffs[fi].file.path).lastPathComponent;
    [self stageFile:fi done:[NSString stringWithFormat:@"Marked %@ resolved.", name]];
}

// Keep Current, Keep Incoming or Keep Both on a conflict: the work tree's
// file with that block replaced, written through the atomic writer once it
// is checked to be as read (a tab open on it follows the disk as one undo
// step). A tab with unsaved text for it is left alone.
- (void)keep:(MergeConflicts::Keep)keep file:(size_t)fi block:(long)block
        data:(std::shared_ptr<ReviewData>)data {
    if (!data || fi >= data->diffs.size()) return;
    const FileDiff &d = data->diffs[fi];
    if (block < 0 || (size_t)block >= d.conflicts.size()) {
        NSBeep();
        return;
    }
    const Extra x = data->extras[fi];
    const std::string rel = x.row.path;
    NSString *top = NS8(data->top);
    NSString *path = [top stringByAppendingPathComponent:NS8(rel)];
    if (self.hasUnsavedText && self.hasUnsavedText(path)) {
        [self finish:[NSString stringWithFormat:@"%@ has unsaved changes in a tab: resolve the "
                                                @"conflict there, or save the tab first.",
                                                path.lastPathComponent]
                  ok:NO
               about:{rel}
                 ran:NO];
        return;
    }
    const std::u16string text = ChangeReview::conflictResult(d, (size_t)block, keep);
    const std::string bytes =
        FolderSearch::encodeFileText(QuickDiff::toUtf8(text), x.workEnc, x.workBom);
    const size_t left = d.conflicts.size() - 1;
    NSString *done = NS8(MergeConflicts::resolvedNote(Std(path.lastPathComponent), left));
    if (left == 0)
        done = [NSString stringWithFormat:@"No conflicts left in %@. Mark Resolved stages it.",
                                          path.lastPathComponent];
    __weak MCChangeReviewPage *weakSelf = self;
    [self act:^{
        NSString *failure = nil;
        std::string now;
        FolderSearch::FileStamp stamp;
        if (FolderSearch::readFileStable(Std(path), kMaxSideBytes, &now, &stamp) !=
                FolderSearch::ReadStatus::Ok ||
            now != x.workBytes) {
            failure = NS8(ChangeReview::staleText(rel, false));
        } else {
            std::string error;
            if (!FolderSearch::writeFileAtomically(Std(path), bytes, &error))
                failure = [NSString stringWithFormat:@"%@ could not be written (%@).",
                                                     path.lastPathComponent, NS8(error)];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf finish:failure ?: done ok:failure == nil about:{rel} ran:YES];
        });
    }];
}

- (void)stageFile:(size_t)fi done:(NSString *)doneText {
    auto data = _data;
    const Extra x = data->extras[fi];
    const std::vector<std::string> args = GitUi::stageArgs(x.row, data->initial);
    NSString *name = NS8(x.row.path).lastPathComponent;
    NSString *top = NS8(data->top);
    __weak MCChangeReviewPage *weakSelf = self;
    [self act:^{
        MCGitResult *r = MCGitRunSync(top, Args(args));
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString *done = doneText ?: [NSString stringWithFormat:x.row.staged ? @"Unstaged %@." : @"Staged %@.", name];
            [weakSelf finish:r.ok ? done : (r.errorText.length ? r.errorText : @"git failed.")
                          ok:r.ok
                       about:{x.row.path, x.row.origPath}
                         ran:YES];
        });
    }];
}

// Stage, Unstage or Discard a block (`block` >= 0) or the changed lines
// given.
- (void)runOp:(Op)op file:(size_t)fi block:(long)block oldLines:(std::vector<int>)oldLines
     newLines:(std::vector<int>)newLines {
    [self runOp:op file:fi block:block oldLines:oldLines newLines:newLines data:_data];
}

// The same on the read `data` (a menu made before the page was read again
// acts on what it showed; the checks at writing catch what moved since).
- (void)runOp:(Op)op file:(size_t)fi block:(long)block oldLines:(std::vector<int>)oldLines
     newLines:(std::vector<int>)newLines data:(std::shared_ptr<ReviewData>)data {
    if (!data || fi >= data->diffs.size()) return;
    const FileDiff &d = data->diffs[fi];
    const bool lines = block < 0;
    const GitUi::ActionPlan plan = ChangeReview::plan(d, op, lines);
    if (!plan.refusal.empty()) {
        [self finish:NS8(plan.refusal) ok:NO about:{d.file.path, d.file.origPath} ran:NO];
        return;
    }
    if (lines && !ChangeReview::anyChanged(d, oldLines, newLines)) {
        [self finish:@"No changed line is selected." ok:NO about:{d.file.path, d.file.origPath} ran:NO];
        return;
    }
    const std::u16string result = lines ? ChangeReview::linesResult(d, oldLines, newLines, op)
                                        : ChangeReview::blockResult(d, (size_t)block, op);
    void (^go)(void) = ^{
        if (op == Op::Discard) [self discardText:result file:fi data:data plan:plan];
        else [self writeIndex:result file:fi data:data plan:plan];
    };
    if (plan.question.empty()) {
        go();
        return;
    }
    // A tab holding unsaved text for the file: its buffer is not what the
    // view shows, so nothing is written over it.
    NSString *path = [NS8(data->top) stringByAppendingPathComponent:NS8(d.file.path)];
    if (self.hasUnsavedText && self.hasUnsavedText(path)) {
        [self finish:[NSString stringWithFormat:@"%@ has unsaved changes in a tab: save or revert "
                                                @"them first, so the discard does not cross them.",
                                                path.lastPathComponent]
                  ok:NO
               about:{d.file.path, d.file.origPath}
                 ran:NO];
        return;
    }
    [self ask:NS8(plan.question) detail:NS8(plan.detail) button:NS8(plan.button) then:go otherwise:nil];
}

// The index's new text, written as the change bars' Stage Change writes
// it, once the entry is checked to be the one the view was drawn from.
- (void)writeIndex:(std::u16string)text file:(size_t)fi data:(std::shared_ptr<ReviewData>)data
              plan:(GitUi::ActionPlan)plan {
    const Extra x = data->extras[fi];
    const std::string rel = x.row.path;
    const std::string blob = QuickDiff::stagedBlob(text, x.indexHow);
    NSString *top = NS8(data->top);
    __weak MCChangeReviewPage *weakSelf = self;
    [self act:^{
        NSString *failure = nil;
        MCGitResult *ls = MCGitRunSync(top, Args(GitUi::indexEntryArgs(rel)));
        const GitUi::IndexEntry now = GitUi::parseIndexEntry(Bytes(ls.output));
        if (!ls.ok || !now.ok || now.object != x.indexObject) {
            failure = NS8(ChangeReview::staleText(rel, true));
        } else {
            NSData *bytes = [NSData dataWithBytes:blob.data() length:blob.size()];
            MCGitResult *h = MCGitRunSyncWithInput(top, Args(GitUi::hashObjectArgs(rel)), bytes);
            if (!h.ok) {
                failure = h.errorText.length ? h.errorText : @"git hash-object failed.";
            } else {
                MCGitResult *u = MCGitRunSync(top, Args(GitUi::updateIndexArgs(now, Bytes(h.output), rel)));
                if (!u.ok) failure = u.errorText.length ? u.errorText : @"git update-index failed.";
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf finish:failure ?: NS8(plan.doneText) ok:failure == nil about:{rel, x.row.origPath} ran:YES];
        });
    }];
}

// The work tree's new text: the file as it is goes to the Trash first, then
// the new text is written over it, once the file and the index are checked
// to be what the view was drawn from.
- (void)discardText:(std::u16string)text file:(size_t)fi data:(std::shared_ptr<ReviewData>)data
               plan:(GitUi::ActionPlan)plan {
    const Extra x = data->extras[fi];
    const std::string rel = x.row.path;
    NSString *top = NS8(data->top);
    NSString *path = [top stringByAppendingPathComponent:NS8(rel)];
    const std::string bytes =
        FolderSearch::encodeFileText(QuickDiff::toUtf8(text), x.workEnc, x.workBom);
    __weak MCChangeReviewPage *weakSelf = self;
    MCGitPanel *panel = _panel;
    [self act:^{
        NSString *failure = nil;
        NSURL *trashed = nil;
        std::string now;
        FolderSearch::FileStamp stamp;
        MCGitResult *ls = MCGitRunSync(top, Args(GitUi::indexEntryArgs(rel)));
        const GitUi::IndexEntry entry = GitUi::parseIndexEntry(Bytes(ls.output));
        if (FolderSearch::readFileStable(Std(path), kMaxSideBytes, &now, &stamp) !=
                FolderSearch::ReadStatus::Ok ||
            now != x.workBytes) {
            failure = NS8(ChangeReview::staleText(rel, false));
        } else if (!ls.ok || !entry.ok || entry.object != x.indexObject) {
            failure = NS8(ChangeReview::staleText(rel, true));
        } else {
            NSString *why = nil;
            trashed = [MCGitPanel trashCopyOf:path failure:&why];
            if (!trashed) {
                failure = why;
            } else {
                std::string error;
                if (!FolderSearch::writeFileAtomically(Std(path), bytes, &error)) {
                    failure = [NSString stringWithFormat:@"%@ could not be written (%@), so nothing "
                                                         @"was discarded.",
                                                         path.lastPathComponent, NS8(error)];
                    [[NSFileManager defaultManager] removeItemAtURL:trashed error:nil];
                    trashed = nil;
                }
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (trashed) [panel rememberDiscardAt:path rel:NS8(rel) trashed:trashed];
            [weakSelf finish:failure ?: NS8(plan.doneText) ok:failure == nil about:{rel, x.row.origPath} ran:YES];
        });
    }];
}

// A question as the panel asks one, over this page's window when there is
// no panel (a worktree's page belongs to the Agents side).
- (void)ask:(NSString *)question detail:(NSString *)detail button:(NSString *)button
       then:(void (^)(void))go otherwise:(void (^)(void))no {
    MCGitPanel *panel = _panel;
    if (panel && !no) {
        [panel askQuestion:question detail:detail button:button then:go];
        return;
    }
    NSAlert *a = [NSAlert new];
    a.alertStyle = NSAlertStyleWarning;
    a.messageText = question ?: @"";
    a.informativeText = detail ?: @"";
    [a addButtonWithTitle:button ?: @"OK"];
    [a addButtonWithTitle:@"Cancel"];
    void (^answer)(NSModalResponse) = ^(NSModalResponse r) {
        if (r == NSAlertFirstButtonReturn) go();
        else if (no) no();
    };
    NSWindow *w = _view.window;
    if (w) [a beginSheetModalForWindow:w completionHandler:answer];
    else answer([a runModal]);
}

// --------------------------------------------------------- Apply to Main
- (void)applyFromButton:(id)sender {
    (void)sender;
    [self applyToMain];
}

- (void)applyToMain {
    auto data = _data;
    if (!_worktree || !data || data->mainPath.empty() || data->diffs.empty() || _applying) {
        NSBeep();
        return;
    }
    std::vector<std::string> paths, untracked;
    for (const FileDiff &d : data->diffs) {
        paths.push_back(d.file.path);
        if (!d.file.origPath.empty()) paths.push_back(d.file.origPath);
        if (d.file.untracked) untracked.push_back(d.file.path);
    }
    NSString *mainPath = NS8(data->mainPath);
    const std::string mainName = Std(_mainRef.length ? _mainRef : @"main");
    const std::string wtName = Std(_worktree.lastPathComponent);
    _applying = YES;
    _applyButton.enabled = NO;
    __weak MCChangeReviewPage *weakSelf = self;
    [self act:^{
        MCGitResult *st = MCGitRunSync(mainPath, Args(GitUi::statusArgs()));
        GitUi::ApplyPlan plan;
        if (st.ok) plan = GitUi::applyToMainPlan(paths, Git::parseStatus(Bytes(st.output)), mainName, wtName);
        else plan.refusal = GitUi::applyFailureText(st.status, Bytes(st.output), Std(st.errorText));
        dispatch_async(dispatch_get_main_queue(), ^{
            MCChangeReviewPage *s = weakSelf;
            if (!s) return;
            if (!plan.refusal.empty()) {
                s->_applying = NO;
                [s finish:NS8(plan.refusal) ok:NO about:{} ran:YES];
                return;
            }
            [s ask:NS8(plan.question) detail:NS8(plan.detail) button:NS8(plan.button)
                then:^{ [weakSelf runApply:data paths:paths untracked:untracked main:mainName]; }
           otherwise:^{
               MCChangeReviewPage *me = weakSelf;
               if (!me) return;
               me->_applying = NO;
               [me updateSummary];
           }];
        });
    }];
}

// The worktree's patch against the merge base, and one for each untracked
// file, applied to main: plainly first (all or nothing), else three-way,
// whose cleanly applied files are then unstaged so everything arrives
// unstaged; never a commit.
- (void)runApply:(std::shared_ptr<ReviewData>)data paths:(std::vector<std::string>)paths
       untracked:(std::vector<std::string>)untracked main:(std::string)mainName {
    NSString *top = NS8(data->top), *mainPath = NS8(data->mainPath);
    const std::string base = data->baseRef;
    const size_t files = data->diffs.size();
    __weak MCChangeReviewPage *weakSelf = self;
    [self act:^{
        NSString *failure = nil;
        std::vector<std::string> conflicts;
        std::string patch;
        MCGitResult *d = MCGitRunSync(top, Args(GitUi::worktreeDiffArgs(base)));
        if (!d.ok) failure = NS8(GitUi::applyFailureText(d.status, Bytes(d.output), Std(d.errorText)));
        else patch = Bytes(d.output);
        for (size_t i = 0; !failure && i < untracked.size(); i++) {
            MCGitResult *n = MCGitRunSync(top, Args(GitUi::newFilePatchArgs(untracked[i])));
            if (n.status == 0 || n.status == 1) patch += Bytes(n.output);
            else failure = NS8(GitUi::applyFailureText(n.status, Bytes(n.output), Std(n.errorText)));
        }
        if (!failure && patch.empty()) failure = @"There was nothing to bring into the main checkout.";
        if (!failure) {
            NSData *input = [NSData dataWithBytes:patch.data() length:patch.size()];
            MCGitResult *plain = MCGitRunSyncWithInput(mainPath, Args(GitUi::applyToMainArgs(false)), input);
            if (!plain.ok) {
                MCGitResult *three = MCGitRunSyncWithInput(mainPath, Args(GitUi::applyToMainArgs(true)), input);
                MCGitResult *st = MCGitRunSync(mainPath, Args(GitUi::statusArgs()));
                std::vector<std::string> staged;
                if (st.ok)
                    for (const Git::Entry &e : Git::parseStatus(Bytes(st.output)).entries) {
                        if (std::find(paths.begin(), paths.end(), e.path) == paths.end()) continue;
                        if (e.unmerged) conflicts.push_back(e.path);
                        else if (e.staged != '.') staged.push_back(e.path);
                    }
                if (!three.ok && conflicts.empty()) {
                    failure = NS8(GitUi::applyFailureText(three.status, Bytes(three.output),
                                                          Std(three.errorText)));
                } else if (!staged.empty()) {
                    MCGitRunSync(mainPath, Args(GitUi::unstageArgs(staged)));
                }
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            MCChangeReviewPage *s = weakSelf;
            if (!s) return;
            s->_applying = NO;
            s->_lastConflicts = conflicts.size();
            if (!failure) {
                s->_applied = YES;
                s->_appliedPrint = data->fingerprint;
                s->_appliedMain = mainPath;
            }
            [s finish:failure ?: NS8(GitUi::applyDoneText(files, conflicts, mainName)) ok:failure == nil about:{} ran:YES];
        });
    }];
}

// The main checkout's changes, which Apply to Main just brought in, in
// the window's Source Control and its Uncommitted Changes page.
- (void)showSourceControl:(id)sender {
    (void)sender;
    if (!self.onShowSourceControl || !_appliedMain.length) {
        NSBeep();
        return;
    }
    self.onShowSourceControl(_appliedMain);
}

- (BOOL)applied { return _applied; }

- (void)undoDiscard:(id)sender {
    (void)sender;
    [_panel undoDiscard];
    [self performSelector:@selector(reload) withObject:nil afterDelay:0.3];
}

// The right click: the changed lines the selection covers (in the file of
// the first of them), else the block under the pointer.
- (NSMenu *)menuForRow:(long)row {
    if (!_data || _data->page.rows.empty()) return nil;
    const Page &p = _data->page;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    const NSRange sel = _text.selectedRange;
    long fileOfLines = -1;
    std::vector<int> oldLines, newLines;
    if (sel.length > 0) {
        for (size_t r = 0; r < p.rows.size(); r++) {
            const size_t from = p.starts[r];
            const size_t to = r + 1 < p.starts.size() ? p.starts[r + 1] : p.text.size() + 1;
            if (to <= sel.location || from >= NSMaxRange(sel)) continue;
            const PageRow &pr = p.rows[r];
            if (pr.kind != RowKind::Removed && pr.kind != RowKind::Added) continue;
            if (fileOfLines < 0) fileOfLines = (long)pr.file;
            if ((long)pr.file != fileOfLines) continue;
            if (pr.kind == RowKind::Removed) oldLines.push_back(pr.oldLine);
            else newLines.push_back(pr.newLine);
        }
    }
    __weak MCChangeReviewPage *weakSelf = self;
    auto add = [&](NSString *title, void (^run)(void)) {
        NSMenuItem *it = [menu addItemWithTitle:title action:@selector(runMenuItem:) keyEquivalent:@""];
        it.target = self;
        it.representedObject = [run copy];
    };
    auto data = _data;
    if (fileOfLines >= 0) {
        const size_t fi = (size_t)fileOfLines;
        const Section s = data->diffs[fi].file.section;
        const std::vector<int> o = oldLines, n = newLines;
        if (s == Section::Changes) {
            add(@"Stage Selected Lines", ^{
                [weakSelf runOp:Op::Stage file:fi block:-1 oldLines:o newLines:n data:data];
            });
            add(@"Discard Selected Lines…", ^{
                [weakSelf runOp:Op::Discard file:fi block:-1 oldLines:o newLines:n data:data];
            });
        } else if (s == Section::Staged) {
            add(@"Unstage Selected Lines", ^{
                [weakSelf runOp:Op::Unstage file:fi block:-1 oldLines:o newLines:n data:data];
            });
        }
    }
    const std::vector<int> none;
    if (row >= 0 && (size_t)row < p.rows.size() && p.rows[(size_t)row].block >= 0 &&
        ChangeReview::wholeOnlyReason(data->diffs[p.rows[(size_t)row].file].file).empty()) {
        const PageRow &pr = p.rows[(size_t)row];
        const size_t fi = pr.file;
        const long block = pr.block;
        const Section s = data->diffs[fi].file.section;
        if (menu.numberOfItems) [menu addItem:[NSMenuItem separatorItem]];
        if (s == Section::Changes) {
            add(@"Stage Change", ^{
                [weakSelf runOp:Op::Stage file:fi block:block oldLines:none newLines:none data:data];
            });
            add(@"Discard Change…", ^{
                [weakSelf runOp:Op::Discard file:fi block:block oldLines:none newLines:none data:data];
            });
        } else if (s == Section::Staged) {
            add(@"Unstage Change", ^{
                [weakSelf runOp:Op::Unstage file:fi block:block oldLines:none newLines:none data:data];
            });
        }
    }
    if (row >= 0 && (size_t)row < p.rows.size() && p.rows[(size_t)row].kind == RowKind::Conflict &&
        data->diffs[p.rows[(size_t)row].file].file.section == Section::Changes) {
        const PageRow &pr = p.rows[(size_t)row];
        const size_t fi = pr.file;
        const long block = pr.block;
        if (menu.numberOfItems) [menu addItem:[NSMenuItem separatorItem]];
        for (MergeConflicts::Keep k : {MergeConflicts::Keep::Current, MergeConflicts::Keep::Incoming,
                                       MergeConflicts::Keep::Both})
            add(NS8(MergeConflicts::actionName(k)), ^{
                [weakSelf keep:k file:fi block:block data:data];
            });
    }
    if (row >= 0 && (size_t)row < p.rows.size() && p.rows[(size_t)row].kind != RowKind::Section) {
        const PageRow &pr = p.rows[(size_t)row];
        const size_t fi = pr.file;
        const ChangeReview::File &f = data->diffs[fi].file;
        // Where Open File goes, worked out now: the row's line, or the line
        // after a removed one, or the file's first change.
        int line = -1;
        if (pr.kind == RowKind::File) {
            for (const SideBySide::Row &r : data->diffs[fi].rows)
                if (r.kind != SideBySide::Kind::Same && r.right >= 0) { line = r.right; break; }
        } else {
            for (size_t r = (size_t)row; line < 0 && r < p.rows.size() && p.rows[r].file == fi; r++)
                line = p.rows[r].newLine;
        }
        NSString *path = [NS8(data->top) stringByAppendingPathComponent:NS8(f.path)];
        if (menu.numberOfItems) [menu addItem:[NSMenuItem separatorItem]];
        add(@"Open File", ^{
            MCChangeReviewPage *me = weakSelf;
            if (me.onOpenFile) me.onOpenFile(path, line < 0 ? 0 : line + 1);
        });
        if (!f.binary && !f.submodule && (!f.conflict || !data->diffs[fi].conflicts.empty()))
            add(@"Show Side by Side", ^{ [weakSelf sideBySide:fi data:data]; });
    }
    return menu;
}

- (void)runMenuItem:(NSMenuItem *)item {
    void (^run)(void) = item.representedObject;
    if (run) run();
}

@end
