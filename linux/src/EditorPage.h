// EditorPage.h — a read-only page in an editor tab (the Mac's EditorPage in
// src/EditorPage.h): a widget of its own in the editor's slot, found again by
// its key so a second open shows the tab that has it. The Evidence view is
// one (PARITY.md item 3.5). A page is a diff-kind tab with no file, so
// sessions, backups, saving and the language servers leave it alone.
#pragma once
#include <gtk/gtk.h>
#include <string>

class EditorPage {
public:
    virtual ~EditorPage() = default;
    virtual GtkWidget* widget() = 0;
    virtual GtkWidget* focusWidget() { return widget(); }
    virtual std::string key() const = 0;
    virtual std::string title() const = 0;
};
