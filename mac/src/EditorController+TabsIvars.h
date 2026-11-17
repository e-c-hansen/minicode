// EditorController+TabsIvars.h — the instance variables only
// EditorController+Tabs.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#import "LayoutCatchUp.h"

@interface EditorController () {
    Model::Session::WindowId _winId;       // _win's id in the app's Model::Session
    NSUndoManager *_noTabUndo;             // the text view's while no tab is open
    // A tab shown again far down its text: the lines above its place laid
    // out in the background (restoreTextPlaceOf:).
    MCLayoutCatchUp *_layoutCatchUp;
}
@end
