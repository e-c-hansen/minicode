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
#include "GitStatus.h"
#include "QuickDiff.h"
#include "SyntaxHighlighter.h"
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
    std::vector<FileDiff> diffs;
    std::vector<Extra> extras;
    Page page;
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
// mark, UTF-8 with or without one.
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
           ActUnstageFile };

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
    }
    return @"";
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
    if (pr.kind == RowKind::File) {
        const bool shows = !f.binary && !f.submodule && !f.conflict;
        if (shows) acts.push_back(ActSide);
        if (f.section != Section::Staged || f.newExists) acts.push_back(ActOpen);
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
        if (pr.kind == RowKind::Context || pr.kind == RowKind::Removed ||
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
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) [self.owner reload];
}
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
    size_t _columns;          // the longest row, a tab as 8
}

- (instancetype)initWithRoot:(NSString *)root panel:(MCGitPanel *)panel {
    if ((self = [super init])) {
        _root = [root copy];
        _panel = panel;
        _queue = dispatch_queue_create("minicode.review", DISPATCH_QUEUE_SERIAL);
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
    for (NSView *v in @[_titleLabel, _summaryLabel, _messageLabel, _undoButton]) [_view addSubview:v];

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
    if (!_undoButton.hidden) {
        const NSSize sz = _undoButton.frame.size;
        right -= sz.width;
        _undoButton.frame = NSMakeRect(right, floor((kTopBarHeight - sz.height) / 2), sz.width, sz.height);
        right -= 8;
    }
    const CGFloat titleW = ceil(_titleLabel.fittingSize.width);
    _titleLabel.frame = NSMakeRect(12, 9, titleW, 17);
    const CGFloat sumX = 12 + titleW + 10;
    const CGFloat sumW = MIN(ceil(_summaryLabel.fittingSize.width), MAX(0, right - sumX));
    _summaryLabel.frame = NSMakeRect(sumX, 10, sumW, 16);
    const CGFloat msgX = sumX + sumW + 12;
    _messageLabel.frame = NSMakeRect(msgX, 10, MAX(0, right - msgX), 16);
    _scroll.frame = NSMakeRect(0, kTopBarHeight, W, MAX(0, NSHeight(b) - kTopBarHeight));
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
- (NSString *)pageTitle { return @"Uncommitted Changes"; }
- (NSString *)pageKind { return @"review"; }
- (NSString *)pageKey { return [@"review:" stringByAppendingString:_root ?: @""]; }
- (NSView *)pageView { return _view; }
- (NSView *)pageFocusView { return _text; }
- (NSTextView *)textView { return _text; }
- (NSScrollView *)scrollView { return _scroll; }

- (void)pageApplySettings {
    AppSettings *cfg = [AppSettings shared];
    NSColor *bg = [cfg background:Surface::Editor];
    NSColor *text = [cfg text:Surface::Editor];
    _view.layer.backgroundColor = bg.CGColor;
    _titleLabel.textColor = text;
    _summaryLabel.textColor = [text colorWithAlphaComponent:0.6];
    _text.numberColor = [text colorWithAlphaComponent:0.4];
    _text.textColor = text;
    _text.insertionPointColor = text;
    if (_data) [self fillText];
    _text.needsDisplay = YES;
}

- (void)pageClosed {
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

// ------------------------------------------------------------ reading
- (void)panelRefreshed:(NSNotification *)note {
    (void)note;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(reload) object:nil];
    [self performSelector:@selector(reload) withObject:nil afterDelay:0.05];
}

- (void)reload {
    if (_loading) {
        _again = YES;
        return;
    }
    _loading = YES;
    NSString *root = _root;
    const ChangeReview::Opened opened = _opened;
    __weak MCChangeReviewPage *weakSelf = self;
    dispatch_async(_queue, ^{
        std::shared_ptr<ReviewData> d = ReadReview(root, opened);
        dispatch_async(dispatch_get_main_queue(), ^{
            MCChangeReviewPage *s = weakSelf;
            if (!s) return;
            s->_loading = NO;
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
    want.origin.y = NSMinY([_text rectOfRow:row]) - 6 + offset;
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
    const double c0 = ThreadCpuMs();
    ChangeReview::Anchor anchor;
    CGFloat offset = 0;
    if (_data && !_data->page.rows.empty()) {
        const NSInteger top = self.topRow;
        anchor = ChangeReview::anchorAt(_data->page, _data->diffs, (size_t)top);
        offset = NSMinY(_scroll.contentView.bounds) - NSMinY([_text rectOfRow:(size_t)top]) + 6;
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
        NSString *why = !_data->error.empty() ? NS8(_data->error) : @"No uncommitted changes.";
        s = [[NSMutableAttributedString alloc] initWithString:why attributes:base];
        [s addAttribute:NSForegroundColorAttributeName value:muted range:NSMakeRange(0, s.length)];
        [s addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:12.5]
                  range:NSMakeRange(0, s.length)];
        [_text.textStorage setAttributedString:s];
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
    [_text.textStorage setAttributedString:s];
    [self fitText];
}

- (void)updateSummary {
    size_t files = _data ? _data->diffs.size() : 0, added = 0, removed = 0;
    if (_data)
        for (const FileDiff &d : _data->diffs) {
            added += d.added;
            removed += d.removed;
        }
    _summaryLabel.stringValue =
        files == 0 ? @""
                   : [NSString stringWithFormat:@"%zu %@, +%zu -%zu", files,
                                                files == 1 ? @"file" : @"files", added, removed];
    MCGitPanel *panel = _panel;
    NSString *undo = panel.undoDiscardTitle;
    _undoButton.hidden = undo == nil;
    _undoButton.toolTip = undo;
    [self layoutView];
}

- (void)say:(NSString *)text ok:(BOOL)ok {
    _message = [text copy];
    _messageLabel.stringValue = text ?: @"";
    _messageLabel.toolTip = text;
    AppSettings *cfg = [AppSettings shared];
    _messageLabel.textColor = ok ? [[cfg text:Surface::Editor] colorWithAlphaComponent:0.7]
                                 : RHex(0xF14C4C, 1);
    [_panel showActionMessage:text ok:ok];
    [self updateSummary];
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

- (void)finish:(NSString *)text ok:(BOOL)ok {
    [self say:text ok:ok];
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
    c.leftLabel = [(staged ? @"HEAD: " : @"Staged or HEAD: ") stringByAppendingString:NS8(f.origPath.empty() ? f.path : f.origPath)];
    c.rightLabel = [(staged ? @"Staged: " : @"") stringByAppendingString:path];
    c.leftText = NS16(f.oldText);
    c.rightText = NS16(f.newText);
    c.fileName = path;
    if (!staged) c.path = [NS8(data->top) stringByAppendingPathComponent:path];
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
    auto data = _data;
    const Extra x = data->extras[fi];
    const std::vector<std::string> args = GitUi::stageArgs(x.row, data->initial);
    NSString *name = NS8(x.row.path).lastPathComponent;
    NSString *top = NS8(data->top);
    __weak MCChangeReviewPage *weakSelf = self;
    dispatch_async(_queue, ^{
        MCGitResult *r = MCGitRunSync(top, Args(args));
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString *done = [NSString stringWithFormat:x.row.staged ? @"Unstaged %@." : @"Staged %@.", name];
            [weakSelf finish:r.ok ? done : (r.errorText.length ? r.errorText : @"git failed.") ok:r.ok];
        });
    });
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
        [self finish:NS8(plan.refusal) ok:NO];
        return;
    }
    if (lines && !ChangeReview::anyChanged(d, oldLines, newLines)) {
        [self finish:@"No changed line is selected." ok:NO];
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
                  ok:NO];
        return;
    }
    MCGitPanel *panel = _panel;
    if (!panel) { go(); return; }
    [panel askQuestion:NS8(plan.question) detail:NS8(plan.detail) button:NS8(plan.button) then:go];
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
    dispatch_async(_queue, ^{
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
            [weakSelf finish:failure ?: NS8(plan.doneText) ok:failure == nil];
        });
    });
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
    dispatch_async(_queue, ^{
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
            [weakSelf finish:failure ?: NS8(plan.doneText) ok:failure == nil];
        });
    });
}

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
        if (!f.binary && !f.submodule && !f.conflict)
            add(@"Show Side by Side", ^{ [weakSelf sideBySide:fi data:data]; });
    }
    return menu;
}

- (void)runMenuItem:(NSMenuItem *)item {
    void (^run)(void) = item.representedObject;
    if (run) run();
}

@end
