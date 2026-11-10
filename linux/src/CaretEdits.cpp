// CaretEdits.cpp — what Carets.h does to the text: the edits at every caret,
// the selection commands (Ctrl+D, Ctrl+Shift+L, Add Cursor), the line
// commands, and Copy, Cut and Paste. The rules are the core's MultiCaret.h;
// every edit goes through MultiEdit::apply (D5) as one undo step, and the
// carets come from the core afterwards. The Mac's MultiCaretView.mm.
#include "Carets.h"

#include "AutoCloseInput.h"
#include "AutoIndent.h"
#include "CaretText.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "MultiEdit.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>

using MultiCaret::Caret;
using MultiCaret::Move;

// ---------------------------------------------------------------- CaretText

namespace {

constexpr char16_t kSlots = 0xE000;          // the Private Use Area's first unit
constexpr size_t kSlotCount = 0xF900 - 0xE000;

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Units the core made of ASCII and the buffer's own BMP characters (the
// auto-closing rules' closers), as UTF-8.
std::string plainUtf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) appendUtf8(out, c);
    return out;
}

double threadCpuMs() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

}  // namespace

char16_t CaretText::unitOf(uint32_t cp) {
    if (cp < 0xE000 || (cp >= 0xF900 && cp <= 0xFFFF)) return (char16_t)cp;
    auto it = slot_.find(cp);
    if (it != slot_.end()) return it->second;
    if (back_.size() >= kSlotCount) {
        full_ = true;
        return 0xFFFD;
    }
    const char16_t u = (char16_t)(kSlots + back_.size());
    back_.push_back(cp);
    slot_.emplace(cp, u);
    return u;
}

std::u16string CaretText::units(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    const char* p = s.c_str();
    const char* end = p + s.size();
    while (p < end) {
        const char* next = g_utf8_find_next_char(p, end);
        if (!next) next = end;
        const gunichar c = g_utf8_get_char_validated(p, next - p);
        out += c == (gunichar)-1 || c == (gunichar)-2 ? char16_t(0xFFFD) : unitOf(c);
        p = next;
    }
    return out;
}

std::string CaretText::utf8(const std::u16string& s) const {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) {
        const size_t k = (size_t)(c - kSlots);
        if (c >= kSlots && k < back_.size()) appendUtf8(out, back_[k]);
        else appendUtf8(out, c);
    }
    return out;
}

std::u16string CaretText::slice(size_t from, size_t to) {
    if (to <= from) return std::u16string();
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)from);
    gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)to);
    // A slice, as BufferText reads it: an embedded object is a character.
    char* raw = gtk_text_buffer_get_slice(buffer_, &a, &b, TRUE);
    std::u16string out = units(raw ? raw : "");
    g_free(raw);
    out.resize(to - from, u' ');
    return out;
}

MultiCaret::Text CaretText::text() {
    const size_t n = (size_t)gtk_text_buffer_get_char_count(buffer_);
    return MultiCaret::Text(n, [this](size_t start, size_t length, char16_t* out) {
        const std::u16string s = slice(start, start + length);
        std::copy(s.begin(), s.end(), out);
    });
}

// ---------------------------------------------------------------- edits

// One MultiEdit, one undo step, then the carets the core says. The buffer's
// selection is changed with the primary clipboard taken off the buffer:
// MultiEdit empties the selection while it edits and sets it again after,
// and emptying and setting it in one key press gave up the primary
// selection, X's late word of which then took the new selection away, now
// and then (HANDOFF.md, items 20 and 21).
bool Carets::applyEdit(const MultiCaret::Edit& e, const CaretText* text) {
    if (text && text->full()) return false;
    if (e.changes.empty()) {
        set(e.carets);
        return true;
    }
    std::vector<MultiEdit::Change> changes;
    changes.reserve(e.changes.size());
    for (const MultiCaret::Change& c : e.changes)
        changes.push_back({c.start, c.length, text ? text->utf8(c.text) : plainUtf8(c.text)});
    goalX_.clear();
    const double t0 = threadCpuMs();
    applying_++;
    GdkClipboard* primary = takePrimary();
    const bool ok = MultiEdit::apply(buffer_, changes, false);
    if (ok) set(e.carets);
    givePrimary(primary);
    applying_--;
    lastEditMs_ = threadCpuMs() - t0;
    return ok;
}

// Each caret's selection replaced by text: one for all, or one per caret.
void Carets::insertStrings(const std::vector<std::string>& texts) {
    CaretText text(buffer_);
    std::vector<std::u16string> units;
    units.reserve(texts.size());
    for (const std::string& t : texts) units.push_back(text.units(t));
    applyEdit(MultiCaret::insert(current(), units), &text);
}

void Carets::typeChar(gunichar c) {
    const std::vector<Caret> carets = current();
    AutoCloseInput* ac = editor_->autoClose();
    // A bracket or quote closing, typed over, or wrapping every selection,
    // at each caret (AutoCloseInput's rules, which take carets).
    if (ac && c >= 0x20 && c < 0x7F) {
        const AutoClose::Typed t = ac->typed((char16_t)c, carets);
        if (t.kind != AutoClose::Typed::Kind::None) {
            if (applyEdit(t.edit, nullptr) && t.kind == AutoClose::Typed::Kind::Close)
                ac->remember(t.closed);
            return;
        }
    }
    std::string s;
    appendUtf8(s, c);
    insertStrings({s});
}

void Carets::backspace() {
    const std::vector<Caret> carets = current();
    if (AutoCloseInput* ac = editor_->autoClose()) {
        const MultiCaret::Edit e = ac->deletePair(carets);
        if (!e.changes.empty()) {
            applyEdit(e, nullptr);
            return;
        }
    }
    CaretText text(buffer_);
    applyEdit(MultiCaret::backspace(text.text(), carets), &text);
}

void Carets::deleteForward() {
    CaretText text(buffer_);
    applyEdit(MultiCaret::deleteForward(text.text(), current()), &text);
}

// Empty carets stretched by a move, never across a line break, so a column
// stays a column; then everything selected goes.
void Carets::deleteBy(Move m) {
    CaretText reader(buffer_);
    const MultiCaret::Text t = reader.text();
    std::vector<Caret> stretched;
    for (const Caret& c : current()) {
        if (!c.empty()) {
            stretched.push_back(c);
            continue;
        }
        const Caret moved = MultiCaret::move(t, {c}, m, true)[0];
        bool crosses = false;
        for (size_t k = moved.lo(); k < moved.hi() && !crosses; k++)
            crosses = t[k] == u'\n' || t[k] == u'\r';
        stretched.push_back(crosses ? c : moved);
    }
    stretched = MultiCaret::normalize(stretched, t.size());
    applyEdit(MultiCaret::insert(stretched, {u""}), &reader);
    if (!active()) revealCaret();
}

// Return: a line break and each caret's line's leading blanks.
void Carets::newline() {
    CaretText reader(buffer_);
    const MultiCaret::Text t = reader.text();
    std::vector<std::string> texts;
    for (const Caret& c : current()) {
        size_t s = c.lo();
        while (s > 0 && t[s - 1] != u'\n' && t[s - 1] != u'\r') s--;
        size_t e = s;
        while (e < c.lo() && (t[e] == u' ' || t[e] == u'\t')) e++;
        texts.push_back("\n" + reader.utf8(t.substr(s, e - s)));
    }
    insertStrings(texts);
}

// Tab: the file's indent unit at every caret. The unit is worked out from
// the first 200,000 characters at most (a 25 MB file was copied for it on
// the Mac, as Return's AutoIndent copied it here before item 1.12).
void Carets::tab() {
    const size_t n = std::min<size_t>(length(), 200000);
    CaretText reader(buffer_);
    const AutoIndent::Unit unit =
        AutoIndent::detect(reader.slice(0, n), editor_->active() ? editor_->active()->path() : "");
    insertStrings({reader.utf8(unit.text())});
}

void Carets::moveAll(Move m, bool extend) {
    CaretText reader(buffer_);
    goalX_.clear();
    set(MultiCaret::move(reader.text(), current(), m, extend));
    if (m == Move::DocumentStart || m == Move::DocumentEnd || !active()) revealCaret();
}

// Up and Down: each caret a display line up or down at the x it had when
// the run of them began, through GTK's own layout of the lines near it.
void Carets::moveVertically(int dir, bool extend) {
    const std::vector<Caret> carets = current();
    const bool keep = goalX_.size() == carets.size();
    std::vector<Caret> out;
    std::vector<int> goals;
    out.reserve(carets.size());
    goals.reserve(carets.size());
    const size_t n = length();
    for (size_t i = 0; i < carets.size(); i++) {
        const Caret& c = carets[i];
        const int x = keep ? goalX_[i] : xOf(c.head);
        long p = acrossLine(c.head, dir, x);
        if (p < 0) p = dir > 0 ? (long)n : 0;
        out.push_back(extend ? Caret{c.anchor, (size_t)p} : Caret{(size_t)p, (size_t)p});
        goals.push_back(x);
    }
    const size_t merged = MultiCaret::normalize(out, n).size();
    set(out);
    // Carets that merged lose their goals; kept only when none did.
    goalX_ = merged == goals.size() && active() ? goals : std::vector<int>();
}

// Escape: back to one caret, where the first was.
void Carets::collapse() {
    if (carets_.empty()) return;
    const size_t head = carets_[0].head;
    set({{head, head}});
}

// ---------------------------------------------------------------- commands

namespace {
std::string limitNote() {
    return "The number of cursors has been limited to " + std::to_string(Carets::kMaxCarets);
}
}  // namespace

bool Carets::findStateHolds() const {
    return hasFind_ && findLength_ == length() && current() == findCarets_;
}

void Carets::rememberFind(const MultiCaret::Found& f) {
    hasFind_ = true;
    findLast_ = f.added;
    findWholeWord_ = f.wholeWord;
    findCarets_ = current();
    findLength_ = length();
}

bool Carets::addNextOccurrence() {
    if (!applies()) return false;
    const std::vector<Caret> carets = current();
    if (carets.size() >= kMaxCarets) {
        if (onNote) onNote(limitNote());
        return false;
    }
    // A selection made by hand starts a plain search; one that Ctrl+D began
    // from an empty caret goes on matching whole words (VS Code's rule).
    const bool holds = findStateHolds();
    CaretText reader(buffer_);
    const MultiCaret::Found f = MultiCaret::addNextOccurrence(
        reader.slice(0, length()), carets, holds ? findLast_ : carets.back(), holds && findWholeWord_);
    if (!f.changed) return false;
    goalX_.clear();
    set(f.carets);
    reveal(f.added.head);
    rememberFind(f);
    return true;
}

bool Carets::selectAllOccurrences() {
    if (!applies()) return false;
    const std::vector<Caret> carets = current();
    const bool holds = findStateHolds();
    CaretText reader(buffer_);
    MultiCaret::Found f = MultiCaret::selectAllOccurrences(
        reader.slice(0, length()), holds ? findLast_ : carets.back(), holds && findWholeWord_);
    if (!f.changed) return false;
    if (f.carets.size() > kMaxCarets) {
        // The ones around the selection it started from.
        const auto at = std::lower_bound(f.carets.begin(), f.carets.end(), f.added,
                                         [](const Caret& a, const Caret& b) { return a.lo() < b.lo(); });
        const size_t i = (size_t)(at - f.carets.begin());
        const size_t from =
            std::min(i > kMaxCarets / 2 ? i - kMaxCarets / 2 : 0, f.carets.size() - kMaxCarets);
        f.carets = std::vector<Caret>(f.carets.begin() + (long)from,
                                      f.carets.begin() + (long)(from + kMaxCarets));
        if (onNote) onNote(limitNote());
    }
    goalX_.clear();
    set(f.carets);
    rememberFind(f);
    return true;
}

bool Carets::selectRanges(const std::vector<std::pair<size_t, size_t>>& ranges, size_t current) {
    if (!applies() || ranges.empty()) return false;
    size_t from = 0, count = ranges.size();
    if (count > kMaxCarets) {
        from = std::min(current > kMaxCarets / 2 ? current - kMaxCarets / 2 : 0, count - kMaxCarets);
        count = kMaxCarets;
        if (onNote) onNote(limitNote());
    }
    std::vector<Caret> carets;
    carets.reserve(count);
    for (size_t i = from; i < from + count; i++)
        carets.push_back({ranges[i].first, ranges[i].first + ranges[i].second});
    goalX_.clear();
    set(carets);
    return true;
}

bool Carets::addCursorVertically(int dir) {
    if (!applies()) return false;
    const std::vector<Caret> carets = current();
    if (carets.size() * 2 > kMaxCarets) {
        if (onNote) onNote(limitNote());
        return false;
    }
    // Each caret keeps the x it had when the run began, so a column grown
    // past a short line comes back to its place below it.
    const bool keep = active() && goalX_.size() == carets.size();
    std::vector<Caret> all = carets;
    std::vector<std::pair<size_t, int>> goalAt;   // head, goal
    long reveal = -1;
    for (size_t i = 0; i < carets.size(); i++) {
        const Caret& c = carets[i];
        const int goal = keep ? goalX_[i] : xOf(c.head);
        goalAt.push_back({c.head, goal});
        const long head = acrossLine(c.head, dir, goal);
        if (head < 0) continue;
        long anchor = head;
        if (!c.empty()) {
            anchor = acrossLine(c.anchor, dir, xOf(c.anchor));
            if (anchor < 0) anchor = head;
        }
        all.push_back({(size_t)anchor, (size_t)head});
        goalAt.push_back({(size_t)head, goal});
        if (reveal < 0 || (dir > 0 ? head > reveal : head < reveal)) reveal = head;
    }
    if (reveal < 0) return false;
    set(all);
    // The goals follow their carets into normalize's order.
    std::stable_sort(goalAt.begin(), goalAt.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<int> kept;
    for (const Caret& c : current()) {
        auto it = std::lower_bound(goalAt.begin(), goalAt.end(), c.head,
                                   [](const auto& a, size_t h) { return a.first < h; });
        kept.push_back(it != goalAt.end() && it->first == c.head ? it->second : xOf(c.head));
    }
    goalX_ = active() ? kept : std::vector<int>();
    this->reveal((size_t)reveal);
    return true;
}

bool Carets::expandLineSelection() {
    if (!applies()) return false;
    CaretText reader(buffer_);
    goalX_.clear();
    set(MultiCaret::expandLineSelection(reader.text(), current()));
    if (!active()) revealCaret();
    return true;
}

bool Carets::deleteLines() {
    if (!applies()) return false;
    CaretText reader(buffer_);
    const bool ok = applyEdit(MultiCaret::deleteLines(reader.text(), current()), &reader);
    if (!active()) revealCaret();
    return ok;
}

bool Carets::moveLines(bool down) {
    if (!applies()) return false;
    CaretText reader(buffer_);
    const MultiCaret::Edit e = MultiCaret::moveLines(reader.text(), current(), down);
    // At the edge nothing moves, and the key does nothing (no bell: VS Code
    // and the Mac are silent there).
    const bool ok = applyEdit(e, &reader);
    if (!active()) revealCaret();
    return ok;
}

bool Carets::duplicateLines(bool down) {
    if (!applies()) return false;
    CaretText reader(buffer_);
    const bool ok = applyEdit(MultiCaret::duplicateLines(reader.text(), current(), down), &reader);
    if (!active()) revealCaret();
    return ok;
}

// ---------------------------------------------------------------- the clipboard

// Copy and Cut with nothing selected take whole lines (VS Code's
// editor.emptySelectionClipboard), and such a copy, still what the
// clipboard holds, pastes above the caret's line. It is told from any other
// copy by the clipboard's content being the one put there (the Mac marks
// the pasteboard with a type of its own; a copy from anywhere else replaces
// the content either way). One for the whole process, so a line copied in
// one window pastes as lines in another.
namespace {
GdkContentProvider* gLineCopy = nullptr;
std::string gLineCopyText;
}  // namespace

bool Carets::nothingSelected() const {
    for (const Caret& c : current())
        if (!c.empty()) return false;
    return true;
}

bool Carets::copy(bool cut) {
    if (!inSource()) return false;
    GdkClipboard* clipboard = gtk_widget_get_clipboard(view_);
    const bool editable = gtk_text_view_get_editable(view());
    if (editable && nothingSelected()) {
        CaretText reader(buffer_);
        const std::string s = reader.utf8(MultiCaret::lineCopy(reader.text(), current()));
        if (reader.full()) return false;
        GValue v = G_VALUE_INIT;
        g_value_init(&v, G_TYPE_STRING);
        g_value_set_string(&v, s.c_str());
        GdkContentProvider* p = gdk_content_provider_new_for_value(&v);
        g_value_unset(&v);
        gdk_clipboard_set_content(clipboard, p);
        if (gLineCopy) g_object_remove_weak_pointer(G_OBJECT(gLineCopy), (gpointer*)&gLineCopy);
        gLineCopy = p;
        g_object_add_weak_pointer(G_OBJECT(p), (gpointer*)&gLineCopy);
        g_object_unref(p);   // the clipboard holds it
        gLineCopyText = s;
        if (cut) {
            CaretText again(buffer_);
            applyEdit(MultiCaret::deleteLines(again.text(), current()), &again);
            if (!active()) revealCaret();
        }
        return true;
    }
    if (!active()) return false;
    CaretText reader(buffer_);
    const std::string s = reader.utf8(MultiCaret::copy(reader.text(), carets_));
    if (!s.empty()) gdk_clipboard_set_text(clipboard, s.c_str());
    if (cut && editable && !s.empty()) applyEdit(MultiCaret::insert(carets_, {u""}), &reader);
    return true;
}

bool Carets::paste() {
    if (!applies()) return false;
    GdkClipboard* clipboard = gtk_widget_get_clipboard(view_);
    if (gLineCopy && gdk_clipboard_get_content(clipboard) == gLineCopy && nothingSelected()) {
        pasteText(gLineCopyText, true);
        return true;
    }
    if (!active()) return false;
    // Another app's text comes later; it goes in only if nothing moved the
    // carets meanwhile.
    struct Job {
        std::weak_ptr<int> alive;
        Carets* self;
        std::vector<Caret> carets;
        GtkTextBuffer* buffer;
    };
    auto* job = new Job{alive_, this, carets_, buffer_};
    gdk_clipboard_read_text_async(
        clipboard, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer data) {
            std::unique_ptr<Job> j(static_cast<Job*>(data));
            char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), res, nullptr);
            if (text && !j->alive.expired() && j->self->buffer_ == j->buffer &&
                j->self->carets_ == j->carets)
                j->self->pasteText(text, false);
            g_free(text);
        },
        job);
    return true;
}

void Carets::pasteText(const std::string& utf8, bool lines) {
    if (utf8.empty()) return;
    CaretText reader(buffer_);
    const std::u16string clip = reader.units(utf8);
    if (lines) {
        applyEdit(MultiCaret::pasteLinesAbove(reader.text(), current(), clip), &reader);
        if (!active()) revealCaret();
        return;
    }
    std::vector<std::u16string> pieces = MultiCaret::pieces(clip, current().size());
    applyEdit(MultiCaret::insert(current(), pieces), &reader);
}

// ---------------------------------------------------------------- actions

namespace {
struct ActionCtx {
    Editor* editor;
    bool (*run)(Carets*);
};
}  // namespace

void Carets::addActions(GActionMap* map, Editor* editor) {
    static const struct {
        const char* name;
        bool (*run)(Carets*);
    } table[] = {
        {"addnextoccurrence", [](Carets* c) { return c->addNextOccurrence(); }},
        {"selectalloccurrences", [](Carets* c) { return c->selectAllOccurrences(); }},
        {"addcursorabove", [](Carets* c) { return c->addCursorVertically(-1); }},
        {"addcursorbelow", [](Carets* c) { return c->addCursorVertically(1); }},
        {"selectline", [](Carets* c) { return c->expandLineSelection(); }},
        {"deleteline", [](Carets* c) { return c->deleteLines(); }},
        {"movelinesup", [](Carets* c) { c->moveLines(false); return true; }},
        {"movelinesdown", [](Carets* c) { c->moveLines(true); return true; }},
        {"copylinesup", [](Carets* c) { return c->duplicateLines(false); }},
        {"copylinesdown", [](Carets* c) { return c->duplicateLines(true); }},
    };
    for (const auto& entry : table) {
        GSimpleAction* a = g_simple_action_new(entry.name, nullptr);
        g_signal_connect_data(
            a, "activate",
            G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer p) {
                auto* ctx = static_cast<ActionCtx*>(p);
                GtkWidget* view = ctx->editor->textView();
                Carets* c = ctx->editor->carets();
                if (!c || !gtk_widget_has_focus(view) || !ctx->run(c)) gtk_widget_error_bell(view);
            }),
            new ActionCtx{editor, entry.run},
            [](gpointer p, GClosure*) { delete static_cast<ActionCtx*>(p); }, (GConnectFlags)0);
        g_action_map_add_action(map, G_ACTION(a));
        g_object_unref(a);
    }
}
