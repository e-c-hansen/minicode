// LspSessionPrivate.h — what the language server session's parts share
// (Lsp.h lists them), and each part's own state. Only Lsp.cpp and the
// LspSession*.cpp files include it, as only the Mac's Lsp files import
// Lsp+Private.h.
//
// The rule, as the Mac's for its ivars: state one part uses is in that
// part's struct below, which no other part touches; a part reaches another's
// through a method declared in Lsp.h (detachDocument asks each part to
// forget what it holds for the file leaving the view). State two or more
// parts use is in the class itself (Lsp.h, "shared by the parts").
#pragma once

#include "Lsp.h"
#include "LspPicker.h"
#include "LspProcess.h"
#include "Utf8Offsets.h"

#include <climits>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lspui {

inline std::string realPath(const std::string& p) {
    if (p.empty()) return p;
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

inline bool isIdent(gunichar c) {
    return c == '_' || c == '$' || g_unichar_isalnum(c);
}

inline std::string lowerExt(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)g_ascii_tolower(c);
    return e;
}

// The URI a file is open under: its real path's, so a symlink and its
// target, and /tmp and /private/tmp spellings, are one document.
inline std::string uriOf(const std::string& path) { return Lsp::uriFromPath(realPath(path)); }

// A URI as that one spelling (Lsp::CanonicalUri, for parseWorkspaceEdit and
// groupReferences).
inline std::string canonicalUri(const std::string& uri) {
    const std::string path = Lsp::pathFromUri(uri);
    return path.empty() ? uri : uriOf(path);
}

// The UTF-8 text of line `line` of `text`, lines ending at \n, \r\n or \r
// (both LSP's rule and GtkTextBuffer's). False past the last line.
inline bool lineOf(const std::string& text, int line, std::string& out) {
    size_t pos = 0;
    for (int l = 0; l < line; ++l) {
        size_t nl = text.find_first_of("\r\n", pos);
        if (nl == std::string::npos) return false;
        pos = nl + ((text[nl] == '\r' && nl + 1 < text.size() && text[nl + 1] == '\n') ? 2 : 1);
    }
    size_t end = text.find_first_of("\r\n", pos);
    out = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    return true;
}

// A UTF-16 column in a line -> its byte offset in the line's UTF-8.
inline size_t byteColumn(const std::string& lineText, int utf16Col) {
    return utf16::byteOffsetOfUtf16(lineText, (size_t)std::max(0, utf16Col));
}

// A buffer's whole text as UTF-8, hidden text included.
inline std::string bufferText(GtkTextBuffer* b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_slice(b, &s, &e, TRUE);
    std::string out = raw ? raw : "";
    g_free(raw);
    return out;
}

// Whether UTF-8 text holds a character outside the Basic Multilingual Plane,
// which takes two UTF-16 units and one GtkTextIter character.
inline bool hasAstral(const char* text, size_t len) {
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)text[i] >= 0xF0) return true;
    return false;
}

}  // namespace lspui

// ---- documents (LspSessionSync.cpp)
struct LspSession::SyncState {
    // A tab's file open on one server. Keyed by the document's id and the
    // server's settings key, as the Mac keys its documents by URI and server.
    struct Entry {
        uint64_t docId = 0;
        std::string path, uri;
        std::shared_ptr<lspgtk::Server> server;
        GtkTextBuffer* buffer = nullptr;   // the document's own, held
        // Its edits since the text was last sent, in UTF-16 units, as the
        // Mac's ChangeTracker follows its text storage. The buffer counts
        // characters, which are UTF-16 units until a character outside the
        // Basic Multilingual Plane is in it; then (`astral`) the next send is
        // the whole text, and the flag is looked at again from that text.
        Lsp::ChangeTracker changes;
        bool astral = false;
        bool pending = false;              // edited since the last flush
    };
    std::map<std::pair<uint64_t, std::string>, Entry> entries;
    // Each buffer a server has is watched once (insert-text and delete-range,
    // before the buffer changes), and held while it is: a tab's document may
    // go before the sync that closes it.
    struct Watch {
        LspSession* self;
        uint64_t docId;
    };
    std::map<GtkTextBuffer*, std::unique_ptr<Watch>> watched;
    guint changeTimer = 0;   // didChange, 0.3 s after typing pauses
    guint syncIdle = 0;      // syncOpenDocuments after the tabs changed
    bool syncing = false;
    size_t opened = 0;       // didOpen notifications sent, for the checks
};

// ---- diagnostics (LspSessionDiagnostics.cpp)
struct LspSession::DiagState {
    // What the servers published, by document URI: the list, and the text
    // the server had been sent when it computed them (Client::textFor), so
    // they can be placed on the text as it is when the tab comes on show.
    struct Published {
        std::vector<Lsp::Diagnostic> list;
        std::shared_ptr<const std::u16string> base;
    };
    std::map<std::string, Published> byUri;
    // The squiggles in the buffer on show, each with marks for the tooltip.
    struct Mark {
        GtkTextMark* start = nullptr;
        GtkTextMark* end = nullptr;
        int severity = 1;
        std::string message;
    };
    std::vector<Mark> marks;
    GtkTextTag* tags[4] = {};   // error, warning, information, hint
};

// ---- completion (LspSessionCompletion.cpp)
struct LspSession::CompletionState {
    GtkWidget* popover = nullptr;
    GtkWidget* list = nullptr;
    GtkWidget* listScroll = nullptr;
    GtkTextMark* anchor = nullptr;   // where the word being completed starts
    std::vector<Lsp::CompletionItem> items, shown;
    bool completing = false;
    bool gotItems = false;
    bool incomplete = false;         // the server will list more as the word grows
    bool navigated = false;          // the arrows chose the highlighted item
    std::string filteredFor;         // the word the list was last filtered for
    int gen = 0;
    guint refilterId = 0;
    guint triggerId = 0;             // a request after a typed character, next idle
    std::string pendingTrigger;
    // A choice stop's options (SnippetSession.h) in the list: taking one
    // replaces the stop's text at every caret. The stop, in characters.
    bool choiceMode = false;
    size_t choiceStart = 0, choiceEnd = 0;
    guint choiceCheckId = 0;
    // An accept being taken back and put in again with its late import:
    // that undo is not the user's.
    bool replaying = false;
    // A completion accepted before its server's completionItem/resolve
    // answered, waiting (a second at most) for the additional edits that
    // answer may bring, as the Mac's MCPendingAccept.
    struct Pending {
        LspSession* self = nullptr;
        int key = 0;
        int requestId = 0;
        std::weak_ptr<lspgtk::Server> server;
        std::string uri, label;
        GtkTextBuffer* buffer = nullptr;   // held
        int docGen = 0;                    // docGen_ once the word was in
        // Everything before it (characters) is as it was when the item was
        // accepted: the word's start, or the start of an edit made since.
        size_t limit = 0;
        // The accept itself, to put it in again with the late edits as one
        // undo step: `removed` characters at `start` (reading
        // `removedText`) became `insert`, in a text `before` long.
        size_t start = 0, removed = 0, before = 0;
        std::string removedText, insert;
        // Where the inserted text is, and whether an undo came since: both
        // say whether the accept was taken back before the answer.
        GtkTextMark* wordStart = nullptr;
        GtkTextMark* wordEnd = nullptr;
        bool sawUndo = false;
        guint timer = 0;
    };
    std::vector<std::unique_ptr<Pending>> pending;
    int nextPendingKey = 0;
};

// ---- the signature hint (LspSessionSignature.cpp)
struct LspSession::SignatureState {
    GtkWidget* popover = nullptr;
    GtkWidget* label = nullptr;
    std::string text;      // what it shows, as plain text, for the checks
    int gen = 0;
    guint updateTimer = 0;   // brought up to date 0.15 s after an edit or a caret move
};

// Every window's session, oldest first (Lsp.cpp): the applier asks the
// others about unsaved changes to a file it edits.
std::vector<LspSession*>& lspAllSessions();

// ---- rename, references, quick fixes (LspSessionActions.cpp)
struct LspSession::ActionsState {
    std::unique_ptr<lspgtk::Picker> picker;
    std::unique_ptr<lspgtk::RenameBox> rename;
    std::string pickerKind;                  // "references", "fixes", "" for none
    std::vector<Lsp::Location> refRows;      // a references row's place; a heading's empty
    std::vector<Lsp::CodeAction> actions;    // the fixes list's
    std::weak_ptr<lspgtk::Server> actionServer;
    int renameGen = 0, referencesGen = 0, actionGen = 0;
    // The caret, the selection and the text (docGen_) when a request went
    // out: an answer is for that, or dropped.
    int askedInsert = -1, askedBound = -1, askedDocGen = -1;
    std::weak_ptr<lspgtk::Server> renameServer;
    int renameStart = 0, renameEnd = 0;      // the name, in characters
    std::string renameOld;
};

// ---- hover and definition (LspSessionNavigation.cpp)
struct LspSession::NavState {
    GtkWidget* hoverPopover = nullptr;
    GtkWidget* hoverLabel = nullptr;
    long hoverKey = -1;     // word start (characters) the hover text is for
    long hoverAsked = -1;   // word start a request is out for
    std::string hoverText;
    std::string lastHover;
    int hoverGen = 0, definitionGen = 0;
};

// ---- the folder's own tools (LspSessionFolderTools.cpp)
struct LspSession::FolderToolsState {
    std::string consentUsed;   // the folder's answer the servers were started with
    int listener = 0;          // FolderConsent::listen's
    // Lsp::checkProjectCode's verdict per server key, as last started.
    std::map<std::string, Lsp::ProjectCodeCheck> projectCode;
};
