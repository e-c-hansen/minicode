// EditorController+LayoutIvars.h — the instance variables only
// EditorController+Layout.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    MCSourceStorage *_sourceStorage;       // the editor's text storage (the view does not own it)
    CGFloat _sidebarShownWidth;            // the sidebar's width when Cmd+B last hid it
    MCPane _maximizedPane;                 // Maximize Pane's, or MCPaneNone
}
@end
