// EditorController+Remote.mm — EditorController: a window over SSH's folder
// through its host (docs/notes/session-host.md, "The window's tree, Quick
// Open and Find in Folder over SSH"). The tree lists through the host
// (MCRemoteWorkspace as the root's MCTreeSource) and follows the host's
// notices in place of FSEvents; Quick Open ranks the host's list of files;
// Find in Folder searches on the host; New File opens a tab its first save
// makes there; what would change or read this Mac's disk at the folder's
// path (New Folder, Rename, Move to Trash, drags, Reveal in Finder, Quick
// Look, the TODO list, Replace in Folder) says it does not work over SSH
// yet. Every method does nothing (or NO) in a window on this Mac. The
// window's state is the MCRemoteWorkspace, an associated object, so the
// controller gains no instance variables.
#import "EditorController+Private.h"
#import "RemoteWorkspace.h"
#import "SessionLink.h"
#import <objc/runtime.h>

static char kRemoteWorkspaceKey;

@implementation EditorController (Remote)

- (MCRemoteWorkspace *)remoteWorkspace {
    return objc_getAssociatedObject(self, &kRemoteWorkspaceKey);
}

// From useRemoteSessionLink: (before showWindow): the root lists through
// the host, never this Mac's disk at that path.
- (void)startRemoteWorkspace {
    MCSessionLinkRef *link = [self sessionRemoteLink];
    if (!link.link || [self remoteWorkspace]) return;
    MCRemoteWorkspace *ws = [[MCRemoteWorkspace alloc] initWithFolder:_root.path];
    objc_setAssociatedObject(self, &kRemoteWorkspaceKey, ws, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [ws attachLink:link];
    // The address named a file not there yet: its tab opens empty.
    if (link.link->askedFileNew()) [ws markNew:@(link.link->askedFile().c_str())];
    __weak EditorController *weakSelf = self;
    ws.onListings = ^(NSDictionary<NSString *, NSArray<MCTreeEntry *> *> *listings) {
        // The host's notices, as FSEvents' listings are taken in: only the
        // folders the tree has listed, keeping what is open and selected.
        EditorController *s = weakSelf;
        if (s && !s->_closing) [s applyTreeListings:listings stats:@{} reshowRoot:NO];
    };
    _root.source = ws;
    _root.children = nil;
    _root.shown = nil;
    _root.loaded = NO;
    [_root loadChildren];
    if (self.outline) [self reloadTreeKeepingState:NO];
    [self remoteTreeFoldersChanged];
}

// Reconnect: the new host lists the tree again (what changed there while
// the connection was gone shows) and watches what it has open.
- (void)remoteWorkspaceMovedToLink {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    MCSessionLinkRef *link = [self sessionRemoteLink];
    if (!ws || !link.link) return;
    [ws attachLink:link];
    [self reloadTreeKeepingState:YES];
}

- (void)remoteTreeFoldersChanged {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || ws.watchQueued) return;
    ws.watchQueued = YES;
    __weak EditorController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        EditorController *s = weakSelf;
        MCRemoteWorkspace *w = [s remoteWorkspace];
        if (!w) return;
        w.watchQueued = NO;
        if (s->_closing) return;
        // Every folder the tree has listed, open or closed since: the host
        // sends each one's listing again when it changes there.
        [w watchFolders:[s loadedTreeFolders].allKeys hidden:gShowHidden];
    });
}

- (BOOL)refuseOverSsh:(NSString *)what {
    if (![self sessionIsRemote]) return NO;
    NSBeep();
    [self showStatusNote:[NSString stringWithFormat:@"%@ does not work in a window over SSH yet: the files are on %@. "
                                                    @"The terminal runs there.",
                                                    what, self.sessionRemoteLabel]];
    return YES;
}

// ------------------------------------------------- Quick Open, Find in Folder

- (void)addRemoteFilesToPaletteContext:(MCPaletteContext *)context {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !context) return;
    __weak MCRemoteWorkspace *weakWs = ws;
    context.listFiles = ^(void (^done)(NSArray<NSString *> *, BOOL)) {
        MCRemoteWorkspace *w = weakWs;
        if (w) [w listFiles:done];
        else done(@[], NO);
    };
}

- (void)searchOnHost:(SearchPanel *)panel {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !panel) return;
    __weak MCRemoteWorkspace *weakWs = ws;
    panel.hostSearch = ^uint64_t(Json request, void (^answers)(const std::vector<Json> &, BOOL)) {
        return [weakWs search:std::move(request) answers:answers];
    };
    panel.cancelHostSearch = ^(uint64_t req) { [weakWs cancelSearch:req]; };
}

// ------------------------------------------------------------ new files

- (BOOL)remotePathIsNew:(NSString *)path {
    return [[self remoteWorkspace] isNew:path];
}

- (BOOL)showRemoteFile:(NSString *)path isNew:(BOOL)isNew {
    if (isNew) [[self remoteWorkspace] markNew:path];
    return [self showFile:path];
}

// New File in the tree over SSH: a kept tab on the file, empty and
// untitled at its path, which its first save (Cmd+S) makes on the host.
// A name the folder has already is refused, as on this Mac.
- (void)openNewRemoteFile:(NSString *)path {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !path.length) return;
    NSString *dir = path.stringByDeletingLastPathComponent;
    for (MCTreeEntry *e in [ws treeListFolder:dir hidden:YES] ?: @[])
        if ([e.path isEqualToString:path]) {
            [self warn:@"A file with that name already exists."];
            return;
        }
    [ws markNew:path];
    [self openFileAtPath:path from:EditorTabs::OpenFrom::TreeReturn];
    [self focusEditor:nil];
    [self flashStatus:[NSString stringWithFormat:@"%@ is new: saving it makes it on %@",
                                                 path.lastPathComponent, self.sessionRemoteLabel]];
}

@end
