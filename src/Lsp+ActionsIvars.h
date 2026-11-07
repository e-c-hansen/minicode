// Lsp+ActionsIvars.h — the instance variables only Lsp+Actions.mm uses,
// which imports it, as does Lsp.mm, which lays them out. See the rule at
// the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@interface LspSession () {
    // Rename, references and quick fixes.
    MCPickerPopup *_picker;
    NSString *_pickerKind;                    // "references", "fixes", or nil
    std::vector<Lsp::Location> _refRows;      // per row; a heading's has no uri
    std::vector<Lsp::CodeAction> _actions;    // per row of the quick fix list
    // The servers the actions came from, by CodeAction::origin.
    NSArray<MCLspServer *> *_actionServers;
    NSRange _askedSelection;                  // the selection when one was asked for
    int _referencesGen, _renameGen, _actionGen;
    MCRenameBox *_rename;
    MCLspServer *_renameServer;               // the server rename was asked of
    NSRange _renameRange;                     // the name being renamed
    NSString *_renameText;                    // the text when the field opened
    NSString *_commandTitle;                  // the quick fix whose command is running
}
@end
