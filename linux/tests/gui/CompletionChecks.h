// CompletionChecks.h — what the completion suites share (PARITY.md item
// 2.5: completion_keys, snippets, completion_edits, signature_help): the
// text and caret of the editor on show, a file shown with the keyboard,
// and the scripted server's JSON (FakeLsp.h).
#pragma once

#include "Harness.h"

#include "Carets.h"
#include "Editor.h"
#include "FakeLspLog.h"
#include "Lsp.h"
#include "MultiEdit.h"
#include "SnippetSession.h"

#include <string>
#include <utility>
#include <vector>

namespace cmpcheck {

inline Editor* ed() { return MCGuiApp().editor; }
inline LspSession* lsp() { return MCGuiApp().lsp; }
inline SnippetSession* snippets() { return ed()->snippets(); }
inline GtkTextBuffer* buf() { return ed()->sourceBuffer(); }

inline std::string text() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

inline size_t caret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    return (size_t)gtk_text_iter_get_offset(&it);
}

inline std::string selected() {
    GtkTextIter a, b;
    if (!gtk_text_buffer_get_selection_bounds(buf(), &a, &b)) return "";
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

// The caret's line, without its line break.
inline std::string lineAtCaret() {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_mark(buf(), &a, gtk_text_buffer_get_insert(buf()));
    gtk_text_iter_set_line_offset(&a, 0);
    b = a;
    if (!gtk_text_iter_ends_line(&b)) gtk_text_iter_forward_to_line_end(&b);
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

// Characters (what GtkTextIter counts) before byte `bytes` of UTF-8 text.
inline size_t chars(const std::string& s, size_t bytes) {
    return (size_t)g_utf8_strlen(s.data(), (gssize)std::min(bytes, s.size()));
}

// The caret just after the first `what` in the text.
inline void caretAfter(const std::string& what) {
    const std::string t = text();
    const size_t at = t.find(what);
    MCGuiCheck(at != std::string::npos, "the text holds " + what, __FILE__, __LINE__);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)chars(t, at + what.size()));
    gtk_text_buffer_place_cursor(buf(), &it);
}

// The whole text replaced as one user action (undone like any other), the
// caret at character `caretAt` (-1: the end), the lists and the snippet
// gone.
inline void setText(const std::string& t, long caretAt = -1) {
    if (lsp()->completionVisible()) MCGuiKey("escape");
    snippets()->end();
    MultiEdit::apply(buf(), {{0, (size_t)gtk_text_buffer_get_char_count(buf()), t}});
    GtkTextIter it;
    if (caretAt < 0) gtk_text_buffer_get_end_iter(buf(), &it);
    else gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)caretAt);
    gtk_text_buffer_place_cursor(buf(), &it);
}

// The list shows with `label` highlighted.
inline bool shows(const std::string& label) {
    return lsp()->completionVisible() && lsp()->completionSelectedLabel() == label;
}

// A file in a tab of its own, on show, with the keyboard.
inline void showFile(const std::string& path) {
    MCGuiOpenFileKept(path);
    MCGuiFocusKeyWindow();
    ed()->focusActiveDocument();
}

inline bool showing(const std::string& path) {
    return ed()->currentPath() == path && ed()->isSource() && gtk_widget_has_focus(ed()->textView());
}

// ---- the script, in JSON
inline std::string str(const std::string& s) { return Json(s).dump(); }
inline std::string pos(int l, int c) {
    return "{\"line\": " + std::to_string(l) + ", \"character\": " + std::to_string(c) + "}";
}
inline std::string rng(int l0, int c0, int l1, int c1) {
    return "{\"start\": " + pos(l0, c0) + ", \"end\": " + pos(l1, c1) + "}";
}
inline std::string edit(int l0, int c0, int l1, int c1, const std::string& t) {
    return "{\"range\": " + rng(l0, c0, l1, c1) + ", \"newText\": " + str(t) + "}";
}
// An item: its label, kind, sortText, and anything more (",\"insertText\": ...").
inline std::string item(const std::string& label, int kind, const std::string& sort,
                        const std::string& more = "") {
    return "{\"label\": " + str(label) + ", \"kind\": " + std::to_string(kind) +
           ", \"sortText\": " + str(sort) + more + "}";
}
inline std::string joined(const std::vector<std::string>& parts) {
    std::string s;
    for (const std::string& p : parts) s += (s.empty() ? "" : ", ") + p;
    return s;
}

}  // namespace cmpcheck
