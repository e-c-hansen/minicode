// Ot.cpp — see Ot.h.
#include "Ot.h"

#include <algorithm>

namespace Ot {

std::size_t lengthOf(const std::u16string& s, Unit u) {
    return u == Unit::Utf16 ? s.size() : Model::countCodePoints(s);
}

Ops primitives(const Ops& ops) {
    Ops out;
    out.reserve(ops.size() * 2);
    for (const Op& o : ops) {
        if (o.del > 0) out.push_back(Op{o.pos, o.del, std::u16string()});
        if (!o.ins.empty()) out.push_back(Op{o.pos, 0, o.ins});
    }
    return out;
}

bool valid(const Ops& ops, std::size_t length, Unit u, std::size_t* after) {
    for (const Op& o : ops) {
        if (o.pos > length || o.del > length - o.pos) return false;
        length = length - o.del + lengthOf(o.ins, u);
    }
    if (after) *after = length;
    return true;
}

void apply(std::u16string& text, const Ops& ops, Unit u) {
    for (const Op& o : ops) {
        std::size_t a = o.pos, b = o.pos + o.del;
        if (u == Unit::CodePoint) {
            a = EditSpans::unitsAt(text, o.pos);
            b = EditSpans::unitsAt(text, o.pos + o.del);
        }
        a = std::min(a, text.size());
        b = std::min(std::max(a, b), text.size());
        text.replace(a, b - a, o.ins);
    }
}

bool apply(Model::Text& text, const Ops& ops, Unit u) {
    for (const Op& o : ops) {
        const std::size_t length = text.length(u);
        if (o.pos > length || o.del > length - o.pos) return false;
        const std::size_t a = text.toUnits(o.pos, u), b = text.toUnits(o.pos + o.del, u);
        text.replace(a, b, o.ins);
    }
    return true;
}

namespace {

// Where `at` goes when [q, q + n) is deleted.
std::size_t pastDelete(std::size_t at, std::size_t q, std::size_t n) {
    if (at <= q) return at;
    if (at >= q + n) return at - n;
    return q;
}

// One primitive against one: x and y on the same text, y winning ties.
// Outputs: x past y (0 to 2 primitives), y past x (0 to 2).
void prim(const Op& x, const Op& y, Unit u, Ops& xo, Ops& yo) {
    const bool xIns = x.del == 0, yIns = y.del == 0;
    if (xIns && yIns) {
        const std::size_t xl = lengthOf(x.ins, u), yl = lengthOf(y.ins, u);
        xo.push_back(Op{x.pos < y.pos ? x.pos : x.pos + yl, 0, x.ins});
        yo.push_back(Op{y.pos <= x.pos ? y.pos : y.pos + xl, 0, y.ins});
        return;
    }
    if (xIns || yIns) {
        // An insertion i against a deletion d, the outputs given back in
        // the caller's order.
        const Op& i = xIns ? x : y;
        const Op& d = xIns ? y : x;
        Ops& io = xIns ? xo : yo;
        Ops& dout = xIns ? yo : xo;
        const std::size_t il = lengthOf(i.ins, u);
        const std::size_t p = i.pos, q = d.pos, n = d.del;
        if (p <= q) {
            io.push_back(i);
            dout.push_back(Op{q + il, n, std::u16string()});
        } else if (p >= q + n) {
            io.push_back(Op{p - n, 0, i.ins});
            dout.push_back(d);
        } else {
            // Inside: the insertion survives at the deletion's start, and
            // the deletion goes either side of it.
            io.push_back(Op{q, 0, i.ins});
            dout.push_back(Op{q, p - q, std::u16string()});
            dout.push_back(Op{q + il, q + n - p, std::u16string()});
        }
        return;
    }
    // Two deletions: each loses what the other already deleted.
    const std::size_t xa = pastDelete(x.pos, y.pos, y.del), xb = pastDelete(x.pos + x.del, y.pos, y.del);
    const std::size_t ya = pastDelete(y.pos, x.pos, x.del), yb = pastDelete(y.pos + y.del, x.pos, x.del);
    if (xb > xa) xo.push_back(Op{xa, xb - xa, std::u16string()});
    if (yb > ya) yo.push_back(Op{ya, yb - ya, std::u16string()});
}

void append(Ops& to, Ops&& from) {
    if (to.empty()) {
        to = std::move(from);
        return;
    }
    to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
}

// Both lists of primitives. Halves the longer one, so the recursion is as
// deep as the lists' lengths' logarithms (a 10,000-caret edit is 20 deep):
//   x1·x2 past y:  x1 past y (y1 is y past x1), then x2 past y1;
//   x past y1·y2:  x past y1 (xa), then xa past y2.
void xf(const Op* x, std::size_t nx, const Op* y, std::size_t ny, Unit u, Ops& xo, Ops& yo) {
    if (nx == 0) {
        yo.assign(y, y + ny);
        return;
    }
    if (ny == 0) {
        xo.assign(x, x + nx);
        return;
    }
    if (nx == 1 && ny == 1) {
        prim(*x, *y, u, xo, yo);
        return;
    }
    if (nx >= ny) {
        const std::size_t h = nx / 2;
        Ops x1, y1, x2;
        xf(x, h, y, ny, u, x1, y1);
        xf(x + h, nx - h, y1.data(), y1.size(), u, x2, yo);
        append(x1, std::move(x2));
        xo = std::move(x1);
    } else {
        const std::size_t h = ny / 2;
        Ops xa, y1, y2;
        xf(x, nx, y, h, u, xa, y1);
        xf(xa.data(), xa.size(), y + h, ny - h, u, xo, y2);
        append(y1, std::move(y2));
        yo = std::move(y1);
    }
}

}  // namespace

void transform(const Ops& a, const Ops& b, Unit u, Ops* aAfterB, Ops* bAfterA) {
    const Ops pa = primitives(a), pb = primitives(b);
    Ops xo, yo;
    xf(pa.data(), pa.size(), pb.data(), pb.size(), u, xo, yo);
    if (aAfterB) *aAfterB = std::move(xo);
    if (bAfterA) *bAfterA = std::move(yo);
}

std::size_t mapOffset(std::size_t at, const Ops& ops, Unit u, bool afterInsert) {
    for (const Op& o : ops) {
        if (o.del > 0) at = pastDelete(at, o.pos, o.del);
        if (!o.ins.empty() && (at > o.pos || (at == o.pos && afterInsert))) at += lengthOf(o.ins, u);
    }
    return at;
}

Ops fromSpans(const std::vector<EditSpans::Span>& spans, const std::vector<std::u16string>& texts) {
    Ops out;
    out.reserve(spans.size());
    for (std::size_t i = spans.size(); i-- > 0;)
        out.push_back(Op{spans[i].start, spans[i].length, i < texts.size() ? texts[i] : std::u16string()});
    return out;
}

bool asReplacements(const Ops& ops, Unit u, std::vector<Replacement>* out) {
    (void)u;
    std::vector<Replacement> r;
    std::size_t floor = (std::size_t)-1;   // nothing may reach past this
    for (std::size_t i = 0; i < ops.size(); i++) {
        Op o = ops[i];
        // A deletion and the insertion right after it at its place: one.
        if (o.ins.empty() && o.del > 0 && i + 1 < ops.size() && ops[i + 1].del == 0 &&
            ops[i + 1].pos == o.pos) {
            o.ins = ops[i + 1].ins;
            i++;
        }
        if (o.del == 0 && o.ins.empty()) continue;
        if (o.pos > floor || o.del > floor - o.pos) return false;
        // An insertion may share a place with the one after it in the list
        // (before it in the text); a deletion's range must end by then.
        if (o.del > 0 && o.pos + o.del > floor) return false;
        r.push_back(Replacement{o.pos, o.del, std::move(o.ins)});
        floor = o.pos;
    }
    std::reverse(r.begin(), r.end());
    if (out) *out = std::move(r);
    return true;
}

}  // namespace Ot
