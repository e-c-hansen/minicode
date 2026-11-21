// RemoteWorkspace.mm — see RemoteWorkspace.h.
#import "RemoteWorkspace.h"
#import "SessionLink.h"

#include <algorithm>
#include <string>

@implementation MCRemoteWorkspace {
    MCSessionLinkRef *_link;
    int _diskToken;
    std::string _watchSent;            // the last watch sent, as its paths and rule
    std::vector<std::string> _watchPaths;
    BOOL _watchHidden, _watchAsked;
    NSMutableSet<NSString *> *_new;
}

- (instancetype)initWithFolder:(NSString *)folder {
    if ((self = [super init])) {
        _folder = [folder copy];
        _new = [NSMutableSet set];
    }
    return self;
}

- (void)dealloc {
    [self detach];
}

- (MCSession::Link *)live {
    MCSession::Link *l = _link.link.get();
    return l && !l->lost() ? l : nullptr;
}

// '/'-separated from the folder, "" for the folder itself; NO for a path
// outside it.
- (BOOL)relative:(NSString *)path out:(std::string *)rel {
    if ([path isEqualToString:_folder]) {
        rel->clear();
        return YES;
    }
    NSString *prefix = [_folder hasSuffix:@"/"] ? _folder : [_folder stringByAppendingString:@"/"];
    if (![path hasPrefix:prefix] || path.length == prefix.length) return NO;
    *rel = [path substringFromIndex:prefix.length].UTF8String ?: "";
    return !rel->empty();
}

- (NSString *)absolute:(const std::string &)rel {
    if (rel.empty()) return _folder;
    NSString *r = [NSString stringWithUTF8String:rel.c_str()];
    return r ? [_folder stringByAppendingPathComponent:r] : nil;
}

- (void)attachLink:(MCSessionLinkRef *)link {
    if (_link.link && _diskToken) _link.link->removeDiskListener(_diskToken);
    _diskToken = 0;
    _link = link;
    _watchSent.clear();   // a new host knows nothing of the old one's watch
    if (!_link.link) return;
    __weak MCRemoteWorkspace *weakSelf = self;
    _diskToken = _link.link->addDiskListener([weakSelf](const Json &changed) {
        // Told inside the link's dispatch, which may be inside a wait for a
        // read: the tree takes it on the next turn.
        Json copy = changed;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf tookNotice:copy]; });
    });
    if (_watchAsked) [self sendWatch];
}

- (void)detach {
    if (_link.link && _diskToken) _link.link->removeDiskListener(_diskToken);
    _diskToken = 0;
    _link = nil;
}

// ------------------------------------------------------------ the tree

- (NSArray<MCTreeEntry *> *)entriesOf:(const Json &)answer {
    const std::string rel = answer["path"].asString();
    NSString *dir = [self absolute:rel];
    if (!dir) return nil;
    if (!answer["error"].isNull()) return @[];   // gone, or not a folder: nothing in it
    NSMutableArray<MCTreeEntry *> *out = [NSMutableArray array];
    for (const Json &e : answer["entries"].elements()) {
        NSString *name = [NSString stringWithUTF8String:e["name"].asString().c_str()];
        if (!name.length) continue;
        MCTreeEntry *t = [MCTreeEntry new];
        t.path = [dir stringByAppendingPathComponent:name];
        t.isDir = e["dir"].asBool();
        t.isLink = e["link"].asBool();
        t.ignored = e["ignored"].asBool();
        t.outside = e["outside"].asBool();
        t.size = -1;   // the host's listing names no sizes or dates
        [out addObject:t];
    }
    return out;
}

- (NSArray<MCTreeEntry *> *)treeListFolder:(NSString *)path hidden:(BOOL)hidden {
    std::string rel;
    MCSession::Link *l = [self live];
    if (!l || ![self relative:path out:&rel]) return nil;
    Json ask = Json::object();
    ask.set("op", Json("list"));
    ask.set("path", Json(rel));
    ask.set("hidden", Json((bool)hidden));
    const Json answer = l->client().read(0, std::move(ask), 20000);
    if (answer.isNull()) return nil;   // no answer: what the tree has stays
    _listings++;
    return [self entriesOf:answer];
}

- (void)tookNotice:(const Json &)changed {
    if (!_link) return;
    _notices++;
    NSMutableDictionary<NSString *, NSArray<MCTreeEntry *> *> *listings = [NSMutableDictionary dictionary];
    for (const Json &answer : changed["listings"].elements()) {
        NSString *dir = [self absolute:answer["path"].asString()];
        NSArray<MCTreeEntry *> *entries = [self entriesOf:answer];
        // A folder gone comes as its error: its parent's listing drops it.
        if (dir && entries && answer["error"].isNull()) listings[dir] = entries;
    }
    if (listings.count && self.onListings) self.onListings(listings);
}

- (void)watchFolders:(NSArray<NSString *> *)paths hidden:(BOOL)hidden {
    std::vector<std::string> rels;
    for (NSString *p in paths) {
        std::string rel;
        if ([self relative:p out:&rel]) rels.push_back(rel);
    }
    std::sort(rels.begin(), rels.end());
    rels.erase(std::unique(rels.begin(), rels.end()), rels.end());
    _watchPaths = rels;
    _watchHidden = hidden;
    _watchAsked = YES;
    [self sendWatch];
}

- (void)sendWatch {
    MCSession::Link *l = [self live];
    if (!l || !l->client().hostWatches()) return;
    std::string key = _watchHidden ? "h" : "-";
    for (const std::string &r : _watchPaths) key += "\n" + r;
    if (key == _watchSent) return;
    const std::uint64_t req = l->client().watchFolders(0, _watchPaths, _watchHidden);
    if (!req) return;
    l->client().forgetRead(req);   // its answer says nothing the listings will not
    _watchSent = key;
}

// ------------------------------------------------- Quick Open, Find in Folder

- (void)listFiles:(void (^)(NSArray<NSString *> *, BOOL))done {
    MCSession::Link *l = [self live];
    Json ask = Json::object();
    ask.set("op", Json("files"));
    ask.set("hidden", Json(true));   // as the Mac's own Quick Open lists them
    void (^finish)(NSArray<NSString *> *, BOOL) = [done copy];
    const std::uint64_t req = l ? l->stream(std::move(ask), [finish, self](std::vector<Json> &&answers, bool last) {
        if (!last) return;
        NSMutableArray<NSString *> *files = [NSMutableArray array];
        BOOL truncated = NO;
        for (const Json &a : answers) {
            for (const Json &f : a["files"].elements()) {
                NSString *s = [NSString stringWithUTF8String:f.asString().c_str()];
                if (s.length) [files addObject:s];
            }
            truncated = truncated || a["truncated"].asBool();
        }
        self->_fileLists++;
        finish(files, truncated);
    }) : 0;
    if (!req) finish(@[], NO);
}

- (uint64_t)search:(Json)request answers:(void (^)(const std::vector<Json> &, BOOL))answers {
    MCSession::Link *l = [self live];
    if (!l) return 0;
    request.set("op", Json("search"));
    void (^take)(const std::vector<Json> &, BOOL) = [answers copy];
    const std::uint64_t req =
        l->stream(std::move(request), [take](std::vector<Json> &&got, bool last) { take(got, last); });
    if (req) _searches++;
    return req;
}

- (void)cancelSearch:(uint64_t)req {
    if (req && _link.link) _link.link->cancelStream(req);
}

// ------------------------------------------------------------ new files

- (void)markNew:(NSString *)path {
    if (path) [_new addObject:path];
}
- (BOOL)isNew:(NSString *)path { return path && [_new containsObject:path]; }

@end
