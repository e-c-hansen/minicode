// ChangeReviewRead.h — pure C++ over a git runner the caller gives, part of
// the portable core. What the review page of uncommitted changes shows and
// what its Stage, Unstage and Discard buttons do to the disk, the part the
// Mac's ChangeReviewPage.mm holds in its ReadReview, writeIndex and
// discardText, so the GTK port's page (linux/src/ChangeReviewPage.cpp) has
// the same rules without a second copy of them to keep level.
//
// `readUncommitted` is one status, one ls-files, one ls-tree and one
// cat-file --batch for every file, then the files from the work tree, each
// worked out as a ChangeReview::FileDiff with what the page needs to act on
// it later (Extra). `stageText` writes the index's new text as the change
// bars' Stage Change does, after checking the entry still names the object
// the view was drawn from; `discardText` checks the file and the entry are
// still as read, has the caller put a copy in the Trash, and writes the
// work tree's new text through the atomic writer. Each returns "" or why
// nothing was done. Called off the main thread; the runner blocks.
//
// Tested in tests/change_review_read_tests.inc (`review-read:*`) against a
// real git in a scratch repository.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ChangeReview.h"
#include "FolderSearch.h"
#include "GitModel.h"
#include "QuickDiff.h"

namespace ChangeReview {

// What a git command gave back.
struct GitOut {
    bool ok = false;
    std::string out, err;
};
// git `args` in `dir`, `input` on its stdin (empty for none).
using GitRunner = std::function<GitOut(const std::string& dir, const std::vector<std::string>& args,
                                       const std::string& input)>;

// Past this a side is not read (the header says so); past kMaxTotalBytes in
// all, the rest of the files are not read either.
constexpr size_t kMaxSideBytes = 4u << 20;
constexpr size_t kMaxTotalBytes = 64u << 20;

// What the page keeps beside each file to act on it: its row in the panel,
// the index entry the view was drawn from, how the index's text was read
// (so a staged text is written the same way), and the work tree's bytes
// and encoding (so a discard checks nothing changed and writes it back as
// it was written).
struct Extra {
    GitUi::Row row;
    std::string indexObject;       // empty when the index has none
    QuickDiff::Original indexHow;  // its encoding and mark
    FolderSearch::TextEncoding workEnc = FolderSearch::TextEncoding::Utf8;
    bool workBom = false;
    bool workRead = false;         // the work tree's file was read
    std::string workBytes;
};

struct Read {
    std::string top;               // the repository's top level
    bool initial = false;          // no commit yet
    std::string error;             // why there is nothing to show
    std::vector<FileDiff> diffs;
    std::vector<Extra> extras;     // one per diff
};

// The staged files, then the changed ones, of the repository at `root`.
Read readUncommitted(const std::string& root, const GitRunner& run);

// The syntax name for a path (its extension, or a Makefile's own name).
std::string extensionOf(const std::string& path);

// Stage, Unstage: `text` is the index's new text for file `fi`. "" when
// written, else why not (the entry moved: staleText).
std::string stageText(const Read& r, size_t fi, const std::u16string& text, const GitRunner& run);

// Discard: `text` is the work tree's new text for file `fi`. `trashCopy`
// puts the file as it is in the Trash and gives where it went; it runs
// only after the file and the index are checked, and the file is written
// only if it worked. On success `*trashed` says where the copy is.
std::string discardText(const Read& r, size_t fi, const std::u16string& text, const GitRunner& run,
                        const std::function<bool(std::string* where, std::string* why)>& trashCopy,
                        std::string* trashed);

}  // namespace ChangeReview
