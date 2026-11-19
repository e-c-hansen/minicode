// ListKeyboard.h — a GtkListBox's rows made again while one has the keyboard.
//
// GTK moves the keyboard off a removed widget only at the window's next frame.
// Until then the window's focus is the removed row, which has no parent, so a
// key pressed then goes to that row alone, past every controller and
// accelerator of the window. A panel that remakes its rows on a timer or a
// file's change lost a key pressed in the frame after: Ctrl+Shift+A, pressed
// just after the Agents panel's last session went, did nothing (the agents
// suite, about half its runs alone, November 2026; HANDOFF.md item 110). So
// while the rows are remade the panel itself holds the keyboard, and a new row
// takes it at once: the window's focus is never a removed widget.
//
//     const bool had = ListKeyboard::hold(list_, root_);
//     ... the old rows removed, the new ones added ...
//     ListKeyboard::give(list_, root_, had, selectedRow);
#pragma once

#include <gtk/gtk.h>

namespace ListKeyboard {

// Before the old rows go (and before anything around them is hidden):
// whether the keyboard is in `list`, or on `holder` (kept there when the last
// remake left no rows); `holder`, an ancestor that stays, then has it.
inline bool hold(GtkWidget* list, GtkWidget* holder) {
    GtkRoot* root = gtk_widget_get_root(list);
    GtkWidget* focus = root ? gtk_root_get_focus(root) : nullptr;
    if (!focus) return false;
    if (focus == holder) return true;
    if (focus != list && !gtk_widget_is_ancestor(focus, list)) return false;
    gtk_widget_set_focusable(holder, TRUE);
    gtk_widget_grab_focus(holder);
    return true;
}

// After the new rows are in: if `had`, the keyboard to `row`, else to the
// first row; with no rows `holder` keeps it, on screen (a hidden list cannot
// have it). `holder` stays focusable only while it has the keyboard.
inline void give(GtkWidget* list, GtkWidget* holder, bool had, GtkListBoxRow* row) {
    if (!had) return;
    if (!row) row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);
    if (row && gtk_widget_grab_focus(GTK_WIDGET(row))) gtk_widget_set_focusable(holder, FALSE);
}

}  // namespace ListKeyboard
