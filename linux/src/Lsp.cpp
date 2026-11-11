// Lsp.cpp — see Lsp.h. The session's life: made with its parts, the buffer
// on show it holds, positions in it, the view's key controller, and the
// popovers' teardown. The parts are LspSession*.cpp (Lsp.h lists them); the
// server processes are LspProcess.cpp. The macOS counterpart is src/Lsp.mm,
// and the behaviour follows it closely.
#include "LspSessionPrivate.h"

#include "Palette.h"
#include "SnippetSession.h"
#include "StartupLog.h"
#include "WhenDrawn.h"

#include <algorithm>

namespace {

void installCss() {
    static bool done = false;
    if (done || !gdk_display_get_default()) return;
    done = true;
    std::string css =
        "popover.minicode-completion > contents, popover.minicode-hover > contents {"
        "  background-color: " + std::string(pal::SidebarBg) + ";"
        "  color: " + pal::EditorText + ";"
        "  border: 1px solid #454545; border-radius: 4px; padding: 0; }"
        "popover.minicode-completion list { background: transparent; }"
        "popover.minicode-completion row { padding: 1px 8px; min-height: 0;"
        "  font-family: monospace; }"
        "popover.minicode-completion row:selected { background-color: #04395E; }"
        "popover.minicode-hover label { font-family: monospace; padding: 8px 10px; }"
        "popover.minicode-signature > contents {"
        "  background-color: " + std::string(pal::SidebarBg) + ";"
        "  color: " + pal::EditorText + ";"
        "  border: 1px solid #454545; border-radius: 4px; padding: 0; }"
        "popover.minicode-signature label { padding: 4px 8px; }";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

}  // namespace

std::vector<LspSession*>& lspAllSessions() {
    static std::vector<LspSession*>* all = new std::vector<LspSession*>();
    return *all;
}

LspSession::LspSession(Editor* editor, const std::string& root, const Settings& settings)
    : editor_(editor), view_(editor->textView()),
      root_(root), settings_(settings),
      also_(std::make_unique<AlsoState>()),
      sync_(std::make_unique<SyncState>()), diag_(std::make_unique<DiagState>()),
      probs_(std::make_unique<ProblemsState>()),
      cmp_(std::make_unique<CompletionState>()), sig_(std::make_unique<SignatureState>()),
      nav_(std::make_unique<NavState>()),
      act_(std::make_unique<ActionsState>()), tools_(std::make_unique<FolderToolsState>()),
      watch_(std::make_unique<WatchState>()), sem_(std::make_unique<SemanticState>()),
      sym_(std::make_unique<SymbolsState>()), fold_(std::make_unique<FoldingState>()) {
    lspAllSessions().push_back(this);
    signature_ = settingsSignature(settings);
    useBuffer(editor->sourceBuffer());
    installCss();
    // No server starts before the window is on screen (WhenDrawn.h): the
    // tab a session brings back gets its server from the idle after the
    // window's first frame.
    std::weak_ptr<int> alive = life_;
    whenDrawn(view_, [this, alive] {
        if (alive.expired()) return;
        drawn_ = true;
        StartupLog::markf("language servers: may start now%s", held_ ? ", the tab on show's" : "");
        if (!held_) return;
        held_ = false;
        const std::string path = path_;   // a copy: documentChanged clears path_ first
        documentChanged(path);
    });

    // The folder's answer about its own tools changing starts the servers
    // that depend on it again (LspSessionFolderTools.cpp).
    watchFolderTools();

    makeDiagnosticTags();

    // Keys for the completion list and the picker, ahead of the text view's own handling.
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKey), this);
    gtk_widget_add_controller(view_, keys);

    // Ctrl+click for the definition, and hover info in the tooltip.
    installNavigation();

    // A snippet's choice stop lists its options in the completion list.
    if (SnippetSession* sn = editor->snippets())
        sn->onChoices = [this](const std::vector<std::string>& choices, size_t a, size_t b) {
            showSnippetChoices(choices, a, b);
        };

    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(onFocusLeave), this);
    gtk_widget_add_controller(view_, focus);

    // The popovers are the text view's children only by gtk_widget_set_parent,
    // which GtkTextView's own dispose does not know about: it tries to remove
    // them as its own children, fails, and tries again forever (the window
    // never closed). So they go when the view is unrealized, which comes
    // before dispose, and are built again on next use.
    g_signal_connect(view_, "unrealize", G_CALLBACK(onUnrealize), this);
}

// The buffer the session's marks, squiggles and positions are in: the tab on
// show's source, held so that a tab closing cannot pull it away first.
void LspSession::useBuffer(GtkTextBuffer* b) {
    if (b == buffer_) return;
    if (buffer_) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
        forgetCompletionAnchor();   // a mark of this buffer's
        g_object_unref(buffer_);
    }
    buffer_ = b;
    if (!buffer_) return;
    g_object_ref(buffer_);
    g_signal_connect(buffer_, "mark-set", G_CALLBACK(onMarkSet), this);
}

LspSession::~LspSession() {
    auto& all = lspAllSessions();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
    unwatchFolderTools();
    if (SnippetSession* sn = editor_->snippets()) sn->onChoices = nullptr;
    dropPendingAccepts();
    if (sig_->updateTimer) g_source_remove(sig_->updateTimer);
    sig_->updateTimer = 0;
    shutdown();
    g_signal_handlers_disconnect_by_data(view_, this);
    useBuffer(nullptr);
    if (sync_->syncIdle) g_source_remove(sync_->syncIdle);
    if (sync_->changeTimer) g_source_remove(sync_->changeTimer);
    if (flashId_) g_source_remove(flashId_);
    if (probs_->reportTimer) g_source_remove(probs_->reportTimer);
    if (sem_->timer) g_source_remove(sem_->timer);
    sem_->timer = 0;
    forgetFoldingRanges();
    stopWatchingFiles();
    dropPopovers();
}

void LspSession::dropPopovers() {
    dropCompletionPopover();
    dropSignaturePopover();
    dropHoverPopover();
    dropActionPopovers();
}

void LspSession::later(std::function<void()> fn) {
    std::weak_ptr<int> alive = life_;
    auto* job = new std::function<void()>([alive, fn = std::move(fn)] {
        if (!alive.expired()) fn();
    });
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        (*static_cast<std::function<void()>*>(p))();
        return G_SOURCE_REMOVE;
    }, job, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
}

// The view lost the keyboard: the lists by the caret close (the rename box
// takes it, and closes them too).
void LspSession::onFocusLeave(GtkEventControllerFocus*, gpointer selfp) {
    LspSession* self = static_cast<LspSession*>(selfp);
    self->closeCompletion();
    self->closeSignature();
    if (!self->applyingEdit_) self->closePicker();
}

void LspSession::onMarkSet(GtkTextBuffer* buf, GtkTextIter*, GtkTextMark* mark, gpointer selfp) {
    LspSession* self = static_cast<LspSession*>(selfp);
    if (mark != gtk_text_buffer_get_insert(buf)) return;
    self->completionFollowsCaret();
    self->signatureFollowsCaret();
    self->actionsFollowCaret();
    self->hoverFollowsCaret();
}

void LspSession::onUnrealize(GtkWidget*, gpointer selfp) {
    static_cast<LspSession*>(selfp)->dropPopovers();
}

// The lists' keys, from this controller or from the editor's own, which
// asks first (EditorObserver::listKey): whichever runs first takes the key.
gboolean LspSession::onKey(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state,
                           gpointer selfp) {
    return static_cast<LspSession*>(selfp)->listKey(keyval, state) ? TRUE : FALSE;
}

bool LspSession::listKey(guint keyval, GdkModifierType mods) {
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return false;
    if (completionKey(keyval, mods) || pickerKey(keyval)) return true;
    // Escape closes the signature hint before anything else has it.
    if (keyval == GDK_KEY_Escape && !(mods & GDK_SHIFT_MASK) && signatureVisible()) {
        closeSignature();
        return true;
    }
    return false;
}

// ---- positions
// LSP counts UTF-16 units within a line, GTK counts characters. Lines are
// the same in both (\n, \r\n and \r end one), except for U+2029, which GTK
// also breaks a line at and LSP does not; source files do not use it.

Lsp::Position LspSession::positionOf(const GtkTextIter* it) const {
    GtkTextIter ls = *it;
    gtk_text_iter_set_line_offset(&ls, 0);
    char* s = gtk_text_buffer_get_slice(buffer_, &ls, it, TRUE);
    Lsp::Position p;
    p.line = gtk_text_iter_get_line(it);
    p.character = (int)utf16::fromUtf8(s ? s : "").size();
    g_free(s);
    return p;
}

void LspSession::iterAt(Lsp::Position p, GtkTextIter* it) const {
    if (p.line >= gtk_text_buffer_get_line_count(buffer_)) {
        gtk_text_buffer_get_end_iter(buffer_, it);
        return;
    }
    gtk_text_buffer_get_iter_at_line(buffer_, it, std::max(0, p.line));
    GtkTextIter le = *it;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* s = gtk_text_buffer_get_slice(buffer_, it, &le, TRUE);
    std::u16string u = utf16::fromUtf8(s ? s : "");
    g_free(s);
    gtk_text_iter_forward_chars(
        it, (int)utf16::toCharOffset(u, (size_t)std::max(0, p.character)));
}

void LspSession::cursorIter(GtkTextIter* it) const {
    gtk_text_buffer_get_iter_at_mark(buffer_, it, gtk_text_buffer_get_insert(buffer_));
}

void LspSession::caretRect(const GtkTextIter* it, GdkRectangle* r) const {
    GdkRectangle loc;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), it, &loc);
    int wx, wy;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET,
                                          loc.x, loc.y, &wx, &wy);
    r->x = wx;
    r->y = wy;
    r->width = std::max(loc.width, 1);
    r->height = loc.height;
}
