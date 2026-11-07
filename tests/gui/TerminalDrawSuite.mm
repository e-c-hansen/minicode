// TerminalDrawSuite.mm — `make guitest SUITES=terminal_draw`: the grid
// draws in proportion to what changed, and draws it right.
//   - tests/fixtures/terminal/agent-session.bin.gz (tools/gen-term-fixture.py:
//     Claude Code's redraw pattern under synchronized output, then a build's
//     scrolling output) is replayed into a 120 by 40 grid, a frame at a time
//     and then 16 KB at a time. Each frame draws what the view invalidated
//     into a bitmap at the window's scale, as Core Animation would (the
//     rects cleared, then drawn), and its time is taken. Median and 95th
//     percentile are printed and bounded; an agent frame that changes only
//     the spinner redraws a row or two, not the screen.
//   - Every so often the picture built up this way is compared with the
//     whole view drawn at once: equal, byte for byte.
//   - A character echoed at the prompt invalidates its row and no other.
//   - What W1 and W6 draw stays right through partial redraws: the red bar
//     of a failed command, the outline of the command moved to, emoji
//     sequences (a family, a flag) in two cells and in color, and the
//     secure input lock in the corner over row 0.
#import "Harness.h"
#import "TerminalGridView.h"
#include <algorithm>
#include <climits>
#include <memory>
#include <string>
#include <vector>
#include <zlib.h>

namespace {

std::string ReadFixture() {
    NSString *path = [MCGuiBuildDir().stringByDeletingLastPathComponent
        stringByAppendingPathComponent:@"tests/fixtures/terminal/agent-session.bin.gz"];
    std::string out;
    gzFile f = gzopen(path.fileSystemRepresentation, "rb");
    if (!f) return out;
    char buf[65536];
    int n;
    while ((n = gzread(f, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    gzclose(f);
    return out;
}

// The fixture in the pieces Terminal.mm would hand the screen: the agent's
// frames one by one (each ends its synchronized update), then the build's
// output 16 KB at a time.
std::vector<std::string> Pieces(const std::string &all, size_t *agentPieces) {
    std::vector<std::string> out;
    const std::string marker = "\x1b]1337;minicode-fixture-build\x07";
    const std::string end = "\x1b[?2026l";
    size_t split = all.find(marker);
    std::string agent = all.substr(0, split);
    std::string build = split == std::string::npos ? "" : all.substr(split + marker.size());
    size_t p = 0;
    while (p < agent.size()) {
        size_t e = agent.find(end, p);
        e = e == std::string::npos ? agent.size() : e + end.size();
        out.push_back(agent.substr(p, e - p));
        p = e;
    }
    *agentPieces = out.size();
    for (size_t q = 0; q < build.size(); q += 16384) out.push_back(build.substr(q, 16384));
    return out;
}

struct Stats { double median = 0, p95 = 0, total = 0; size_t n = 0; };
Stats Summarize(std::vector<double> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.n = v.size();
    s.median = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, (size_t)(v.size() * 0.95))];
    for (double x : v) s.total += x;
    return s;
}

// A grid on a screen of its own in a window that is never shown, and the
// picture it has drawn so far.
struct Rig {
    std::unique_ptr<TerminalScreen> screen;
    NSWindow *window = nil;
    TerminalGridView *grid = nil;
    NSBitmapImageRep *picture = nil;
    CGFloat scale = 2;

    Rig(int cols, int rows) {
        screen = std::make_unique<TerminalScreen>(cols, rows);
        screen->setScrollbackLimit(10000);
        NSSize cs = [TerminalGridView cellSize];
        CGFloat pad = [TerminalGridView padding];
        NSRect frame = NSMakeRect(0, 0, cols * cs.width + 2 * pad, rows * cs.height + 2 * pad);
        window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered defer:YES];
        window.releasedWhenClosed = NO;
        grid = [[TerminalGridView alloc] initWithFrame:frame];
        grid.screen = screen.get();
        grid.textInput = YES;
        [window.contentView addSubview:grid];
        scale = window.backingScaleFactor > 0 ? window.backingScaleFactor : 2;
        picture = NewPicture();
        // The first drawing in a window builds its layers, which invalidates
        // the whole view once; that is done before anything is counted.
        [grid cacheDisplayInRect:grid.bounds toBitmapImageRep:NewPicture()];
        grid.recordsInvalidations = YES;
        Feed("");   // the first look at the screen draws all of it
    }
    ~Rig() {
        grid.recordsInvalidations = NO;
        [grid removeFromSuperview];
        grid.screen = nullptr;
        [window close];
    }

    NSBitmapImageRep *NewPicture() const {
        return [grid bitmapImageRepForCachingDisplayInRect:grid.bounds];
    }

    // The rects asked for since the last frame, overlapping ones merged
    // (as a region would hold them), in the view's coordinates.
    std::vector<NSRect> Invalid() {
        std::vector<NSRect> rs;
        for (NSValue *v : [grid takeRecordedInvalidations]) {
            NSRect r = NSIntersectionRect(v.rectValue, grid.bounds);
            if (!NSIsEmptyRect(r)) rs.push_back(r);
        }
        std::sort(rs.begin(), rs.end(), [](NSRect a, NSRect b) { return NSMinY(a) < NSMinY(b); });
        std::vector<NSRect> merged;
        for (NSRect r : rs) {
            if (!merged.empty() && NSMinY(r) <= NSMaxY(merged.back()))
                merged.back() = NSUnionRect(merged.back(), r);
            else
                merged.push_back(r);
        }
        return merged;
    }

    // Output: what Terminal.mm does with it, then the frame Core Animation
    // would draw. Returns the time spent drawing (ms), and the rects.
    double Feed(const std::string &bytes, std::vector<NSRect> *rects = nullptr,
                double *cpuMs = nullptr) {
        screen->feed(bytes);
        [grid screenChanged];
        [grid flushScreenChanges];
        return Draw(rects, cpuMs);
    }

    // Each invalid rect drawn on its own (into a clear bitmap of its size,
    // as Core Animation clears what it redraws), then put in its place in
    // the picture. Wall time, and the main thread's CPU time, which a busy
    // machine disturbs less.
    double Draw(std::vector<NSRect> *rects = nullptr, double *cpuMs = nullptr) {
        std::vector<NSRect> rs = Invalid();
        if (rects) *rects = rs;
        double ms = 0, cpu = 0;
        for (NSRect r : rs) {
            r = NSIntersectionRect(NSIntegralRect(r), grid.bounds);
            NSBitmapImageRep *piece = [grid bitmapImageRepForCachingDisplayInRect:r];
            CFTimeInterval t = CACurrentMediaTime();
            uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
            [grid cacheDisplayInRect:r toBitmapImageRep:piece];
            cpu += (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c) / 1e6;
            ms += (CACurrentMediaTime() - t) * 1000;
            NSInteger y0 = lround(NSMinY(r) * scale), x0 = lround(NSMinX(r) * scale);
            NSInteger bytes = MIN(piece.pixelsWide, picture.pixelsWide - x0) * 4;
            for (NSInteger y = 0; y < piece.pixelsHigh && y0 + y < picture.pixelsHigh; y++)
                memcpy(picture.bitmapData + (y0 + y) * picture.bytesPerRow + x0 * 4,
                       piece.bitmapData + y * piece.bytesPerRow, (size_t)bytes);
        }
        if (cpuMs) *cpuMs = cpu;
        return ms;
    }

    // The picture built up so far is what drawing the whole view gives.
    bool MatchesFullDraw() {
        NSBitmapImageRep *full = NewPicture();
        [grid cacheDisplayInRect:grid.bounds toBitmapImageRep:full];
        [grid takeRecordedInvalidations];
        size_t bytes = (size_t)picture.bytesPerRow * (size_t)picture.pixelsHigh;
        bool same = full.bytesPerRow == picture.bytesPerRow &&
                    memcmp(full.bitmapData, picture.bitmapData, bytes) == 0;
        if (!same) {
            // Where it differs (MC_GUI_SNAPSHOTS keeps the grid's picture).
            NSInteger n = 0, y0 = INT_MAX, y1 = -1;
            for (NSInteger y = 0; y < picture.pixelsHigh; y++)
                for (NSInteger x = 0; x < picture.pixelsWide; x++)
                    if (memcmp(full.bitmapData + y * full.bytesPerRow + x * 4,
                               picture.bitmapData + y * picture.bytesPerRow + x * 4, 4) != 0) {
                        n++;
                        y0 = MIN(y0, y);
                        y1 = MAX(y1, y);
                    }
            MCGuiNote(@"%ld pixels differ, from %.0f to %.0f points down", (long)n, y0 / scale,
                      y1 / scale);
        }
        return same;
    }

    // A pixel of the picture, at a point from the top left.
    NSColor *Pixel(CGFloat x, CGFloat y) const {
        return [picture colorAtX:(NSInteger)(x * scale) y:(NSInteger)(y * scale)];
    }

    // Pixels in a rect (points) that are colored, not gray: an emoji.
    int ColorfulPixels(NSRect r) const {
        int n = 0;
        for (NSInteger y = (NSInteger)(NSMinY(r) * scale); y < (NSInteger)(NSMaxY(r) * scale); y++)
            for (NSInteger x = (NSInteger)(NSMinX(r) * scale); x < (NSInteger)(NSMaxX(r) * scale);
                 x++) {
                NSColor *c = [picture colorAtX:x y:y];
                if (c.alphaComponent < 0.5) continue;
                CGFloat hi = MAX(c.redComponent, MAX(c.greenComponent, c.blueComponent));
                CGFloat lo = MIN(c.redComponent, MIN(c.greenComponent, c.blueComponent));
                if (hi - lo > 0.3) n++;
            }
        return n;
    }

    int InkedPixels(NSRect r) const {
        int n = 0;
        for (NSInteger y = (NSInteger)(NSMinY(r) * scale); y < (NSInteger)(NSMaxY(r) * scale); y++)
            for (NSInteger x = (NSInteger)(NSMinX(r) * scale); x < (NSInteger)(NSMaxX(r) * scale);
                 x++)
                if ([picture colorAtX:x y:y].alphaComponent > 0.3) n++;
        return n;
    }

    NSRect CellRect(int row, int col, int width = 1) const {
        NSSize cs = [TerminalGridView cellSize];
        CGFloat pad = [TerminalGridView padding];
        return NSMakeRect(pad + col * cs.width, pad + row * cs.height, width * cs.width, cs.height);
    }
};

double RowsIn(const std::vector<NSRect> &rects) {
    double h = 0;
    for (NSRect r : rects) h += NSHeight(r);
    return h / [TerminalGridView cellSize].height;
}

}  // namespace

MC_GUI_SUITE(terminal_draw) {
    MCGuiStep(@"the replay: a frame at a time, drawn as invalidated", ^{
        std::string all = ReadFixture();
        GCHECK(all.size() > 1000000);
        size_t agentN = 0;
        std::vector<std::string> pieces = Pieces(all, &agentN);
        GCHECK(agentN > 400 && pieces.size() > agentN + 20);
        Rig rig(120, 40);
        std::vector<double> agentMs, buildMs, agentCpu, buildCpu, agentRows;
        int compared = 0, differed = 0;
        CFTimeInterval t0 = CACurrentMediaTime();
        for (size_t i = 0; i < pieces.size(); i++) {
            std::vector<NSRect> rects;
            double cpu = 0;
            double ms = rig.Feed(pieces[i], &rects, &cpu);
            if (i < agentN) {
                agentMs.push_back(ms);
                agentCpu.push_back(cpu);
                agentRows.push_back(RowsIn(rects));
            } else {
                buildMs.push_back(ms);
                buildCpu.push_back(cpu);
            }
            if (i % 25 == 0 || i + 1 == agentN || i + 1 == pieces.size()) {
                compared++;
                if (!rig.MatchesFullDraw()) {
                    if (differed++ == 0) {
                        MCGuiNote(@"piece %zu: the picture differs from a full draw", i);
                        MCGuiSnapshot(rig.grid, @"terminal-draw-differs");
                    }
                }
            }
        }
        double wall = (CACurrentMediaTime() - t0) * 1000;
        Stats sa = Summarize(agentMs), sb = Summarize(buildMs), rows = Summarize(agentRows);
        Stats ca = Summarize(agentCpu), cb = Summarize(buildCpu);
        MCGuiNote(@"agent frames: %zu, draw median %.2f ms, p95 %.2f ms (CPU %.2f, %.2f), "
                  @"total %.0f ms; rows redrawn median %.1f, p95 %.1f",
                  sa.n, sa.median, sa.p95, ca.median, ca.p95, sa.total, rows.median, rows.p95);
        MCGuiNote(@"build pieces (16 KB, scrolling): %zu, draw median %.2f ms, p95 %.2f ms "
                  @"(CPU %.2f, %.2f)", sb.n, sb.median, sb.p95, cb.median, cb.p95);
        MCGuiNote(@"whole replay %.0f ms at scale %.0f; %d pictures compared with a full draw",
                  wall, rig.scale, compared);
        MCGuiSnapshot(rig.grid, @"terminal-draw-replay");
        GCHECK(differed == 0);
        // Most agent frames change the spinner line and the newest words:
        // two rows and their margins, never the screen.
        GCHECK(rows.median < 5);
        // CPU time, which a busy machine disturbs less than the clock, with
        // room for a slower one (CI, at 1x). Before (2026-11-06, this Mac, at
        // 2x) every frame drew everything: agent frames median 32 ms, p95 45
        // ms; build pieces median 15 ms, p95 18 ms. After (2026-11-07): about 0.2
        // and 1.0 ms, and 1.3 and 1.6 ms.
        GCHECK(ca.p95 < 8);
        GCHECK(cb.p95 < 15);
    });

    MCGuiStep(@"an echoed character invalidates its row only", ^{
        Rig rig(80, 24);
        rig.Feed("one\r\ntwo\r\nthree\r\n% ");
        std::vector<NSRect> rects;
        rig.Feed("x", &rects);
        NSSize cs = [TerminalGridView cellSize];
        NSRect row3 = rig.CellRect(3, 0, 80);
        GCHECK(!rects.empty());
        for (NSRect r : rects)
            GCHECK(NSMinY(r) >= NSMinY(row3) - 4 && NSMaxY(r) <= NSMaxY(row3) + 4 &&
                   NSHeight(r) < 2 * cs.height);
        GCHECK(rig.MatchesFullDraw());
        // Nothing changed: nothing drawn.
        rig.Feed("", &rects);
        GCHECK(rects.empty());
        // The same text written again over itself (Ink's way): nothing.
        rig.Feed("\r\x1b[2K% x", &rects);
        GCHECK(rects.empty());
        GCHECK(rig.MatchesFullDraw());
    });

    MCGuiStep(@"a failed command's bar and the outline survive partial redraws", ^{
        Rig rig(60, 12);
        rig.Feed("\x1b]133;A\x07% false\r\n\x1b]133;C\x07\x1b]133;D;1\x07"
                 "\x1b]133;A\x07% ");
        NSRect bar = rig.CellRect(0, 0);
        CGFloat midY = NSMidY(bar);
        NSColor *c = [rig.Pixel(2, midY) colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        GCHECK(c.redComponent > 0.75 && c.greenComponent < 0.5);
        // Typing at the next prompt redraws its row; the bar two rows up stays.
        std::vector<NSRect> rects;
        rig.Feed("ls", &rects);
        GCHECK(RowsIn(rects) < 2);
        c = [rig.Pixel(2, midY) colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        GCHECK(c.redComponent > 0.75 && c.greenComponent < 0.5);
        GCHECK(rig.MatchesFullDraw());
        // Outlined (Cmd+Up): output while it shows draws it all again.
        [rig.grid moveToCommand:-1 select:NO];
        rig.Draw();
        GCHECK(rig.grid.currentCommandLine == 0);
        rig.Feed("\r\nout\r\n");
        GCHECK(rig.MatchesFullDraw());
    });

    MCGuiStep(@"emoji sequences: two cells, in color, through partial redraws", ^{
        Rig rig(40, 6);
        // A family (ZWJ), a flag, a heart with VS16 and a rainbow flag (VS16
        // and ZWJ), each followed by a letter.
        const std::string family =
            "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
        const std::string rainbow = "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88";
        rig.Feed("a" + family + "b\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5" "c\xE2\x9D\xA4\xEF\xB8\x8F" "d" +
                 rainbow + "e\r\n% ");
        const TerminalScreen &s = *rig.screen;
        GCHECK(s.cell(0, 1).width == 2 && s.cell(0, 3).ch == "b");
        GCHECK(s.cell(0, 4).width == 2 && s.cell(0, 6).ch == "c");
        GCHECK(s.cell(0, 7).width == 2 && s.cell(0, 9).ch == "d");
        GCHECK(s.cell(0, 10).width == 2 && s.cell(0, 12).ch == "e");
        // Drawn as emoji: the family (Apple's is grayish blue) inked, the
        // flags and the heart in color.
        GCHECK(rig.InkedPixels(rig.CellRect(0, 1, 2)) > 100);
        GCHECK(rig.ColorfulPixels(rig.CellRect(0, 4, 2)) > 10);
        GCHECK(rig.ColorfulPixels(rig.CellRect(0, 7, 2)) > 10);
        GCHECK(rig.ColorfulPixels(rig.CellRect(0, 10, 2)) > 10);
        // The letter after each sits in its own column, not pushed along (an
        // emoji is a little wider than two cells at the text's size, as in
        // the old drawing, so the cell's middle is looked at).
        for (int col : {3, 6, 9, 12}) {
            NSRect mid = NSInsetRect(rig.CellRect(0, col), NSWidth(rig.CellRect(0, col)) / 4, 0);
            GCHECK(rig.ColorfulPixels(mid) < 4 && rig.InkedPixels(mid) > 4);
        }
        MCGuiSnapshot(rig.grid, @"terminal-draw-emoji");
        // A change on the row below redraws it without spoiling the emoji.
        int heart = rig.ColorfulPixels(rig.CellRect(0, 7, 2));
        rig.Feed("typing");
        GCHECK(rig.ColorfulPixels(rig.CellRect(0, 7, 2)) == heart);
        GCHECK(rig.MatchesFullDraw());
        // Rewritten on their own row, they come out the same.
        std::vector<NSRect> rects;
        rig.Feed("\x1b[1;2H" + family, &rects);
        GCHECK(rig.MatchesFullDraw());
    });

    MCGuiStep(@"the secure input lock over row 0 survives partial redraws", ^{
        Rig rig(60, 8);
        MCGuiSetKeyWindow(rig.window);
        [rig.window makeFirstResponder:rig.grid];
        rig.grid.hiddenInput = YES;
        GCHECK(rig.grid.secureInputOn);
        rig.Draw();
        NSRect lock = NSMakeRect(NSWidth(rig.grid.bounds) - 30, 4, 22, 16);
        int inked = rig.InkedPixels(lock);
        GCHECK(inked > 20);
        // Output on row 0, under the lock.
        std::vector<NSRect> rects;
        rig.Feed("Password: ", &rects);
        GCHECK(!rects.empty() && RowsIn(rects) < 2);
        GCHECK(rig.InkedPixels(lock) == inked);
        GCHECK(rig.MatchesFullDraw());
        rig.grid.hiddenInput = NO;
        GCHECK(!rig.grid.secureInputOn);
        [rig.window makeFirstResponder:nil];
        MCGuiSetKeyWindow(nil);
    });
}
