// Keymap.mm — see Keymap.h.
#import "Keymap.h"
#import "Lsp.h"                // CodeTextView
#import "Terminal.h"           // TerminalView
#import "TerminalGridView.h"
#import "PageZoom.h"
#include "Commands.h"
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

using Commands::Key;

NSNotificationName const MCKeymapDidChangeNotification = @"MCKeymapDidChangeNotification";

// ------------------------------------------------------------ keys <-> AppKit
namespace {

struct NamedCode { unsigned short code; const char *name; };
const NamedCode kCodes[] = {
    {122, "f1"}, {120, "f2"}, {99, "f3"}, {118, "f4"}, {96, "f5"}, {97, "f6"}, {98, "f7"},
    {100, "f8"}, {101, "f9"}, {109, "f10"}, {103, "f11"}, {111, "f12"}, {105, "f13"},
    {107, "f14"}, {113, "f15"}, {106, "f16"}, {64, "f17"}, {79, "f18"}, {80, "f19"},
    {90, "f20"}, {123, "left"}, {124, "right"}, {125, "down"}, {126, "up"}, {115, "home"},
    {119, "end"}, {116, "pageup"}, {121, "pagedown"}, {36, "enter"}, {76, "enter"},
    {48, "tab"}, {49, "space"}, {53, "escape"}, {51, "backspace"}, {117, "delete"},
    {0x6E, "contextmenu"}, {82, "numpad0"}, {83, "numpad1"}, {84, "numpad2"}, {85, "numpad3"},
    {86, "numpad4"}, {87, "numpad5"}, {88, "numpad6"}, {89, "numpad7"}, {91, "numpad8"},
    {92, "numpad9"},
};
// The punctuation keys of a US keyboard, by position, for when Shift has
// turned the character into another (Ctrl+Shift+` arrives as ~).
const NamedCode kPunctuationCodes[] = {
    {50, "`"}, {27, "-"}, {24, "="}, {33, "["}, {30, "]"}, {42, "\\"}, {41, ";"},
    {39, "'"}, {43, ","}, {47, "."}, {44, "/"},
};

unsigned ModsOf(NSEventModifierFlags f) {
    unsigned m = 0;
    if (f & NSEventModifierFlagControl) m |= Commands::Ctrl;
    if (f & NSEventModifierFlagOption) m |= Commands::Alt;
    if (f & NSEventModifierFlagShift) m |= Commands::Shift;
    if (f & NSEventModifierFlagCommand) m |= Commands::Cmd;
    return m;
}

NSEventModifierFlags FlagsOf(unsigned m) {
    NSEventModifierFlags f = 0;
    if (m & Commands::Ctrl) f |= NSEventModifierFlagControl;
    if (m & Commands::Alt) f |= NSEventModifierFlagOption;
    if (m & Commands::Shift) f |= NSEventModifierFlagShift;
    if (m & Commands::Cmd) f |= NSEventModifierFlagCommand;
    return f;
}

Key KeyForEvent(NSEvent *e) {
    Key k;
    k.mods = ModsOf(e.modifierFlags);
    for (const NamedCode &c : kCodes)
        if (e.keyCode == c.code) {
            k.name = c.name;
            return k;
        }
    // Without Shift the character ignoring modifiers is the key's own; with
    // it, ask the layout for the unshifted one (Shift+[ gives {).
    NSString *chars = e.charactersIgnoringModifiers;
    if (k.mods & Commands::Shift) {
        @try {
            NSString *plain = [e charactersByApplyingModifiers:0];
            if (plain.length == 1) chars = plain;
        } @catch (NSException *) {
        }
    }
    unichar ch = chars.length == 1 ? [chars characterAtIndex:0] : 0;
    if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
        (ch < 128 && ch && std::strchr("`-=[]\\;',./", (char)ch))) {
        k.name = std::string(1, (char)ch);
        return k;
    }
    for (const NamedCode &c : kPunctuationCodes)
        if (e.keyCode == c.code) {
            k.name = c.name;
            return k;
        }
    return k;   // a key minicode has no name for
}

// The item's keyEquivalent for a key; nil when a menu cannot show it.
NSString *EquivalentFor(const Key &k) {
    const std::string &n = k.name;
    if (n.size() == 1) return [NSString stringWithFormat:@"%c", n[0]];
    if (n[0] == 'f' && n.size() >= 2 && isdigit((unsigned char)n[1]))
        return [NSString stringWithFormat:@"%C",
                         (unichar)(NSF1FunctionKey + std::stoi(n.substr(1)) - 1)];
    static const struct { const char *name; unichar ch; } kChars[] = {
        {"up", NSUpArrowFunctionKey}, {"down", NSDownArrowFunctionKey},
        {"left", NSLeftArrowFunctionKey}, {"right", NSRightArrowFunctionKey},
        {"home", NSHomeFunctionKey}, {"end", NSEndFunctionKey},
        {"pageup", NSPageUpFunctionKey}, {"pagedown", NSPageDownFunctionKey},
        {"enter", '\r'}, {"tab", '\t'}, {"space", ' '}, {"escape", 0x1B},
        {"backspace", NSBackspaceCharacter}, {"delete", NSDeleteFunctionKey},
    };
    for (const auto &c : kChars)
        if (n == c.name) return [NSString stringWithFormat:@"%C", c.ch];
    return nil;   // numpad0..9, contextmenu
}

// The key a menu item holds (as built), or an empty key.
Key KeyForItem(NSMenuItem *item) {
    Key k;
    NSString *eq = item.keyEquivalent;
    if (!eq.length) return k;
    k.mods = ModsOf(item.keyEquivalentModifierMask);
    unichar ch = [eq characterAtIndex:0];
    if (ch >= 'A' && ch <= 'Z') {
        ch = ch - 'A' + 'a';
        k.mods |= Commands::Shift;
    }
    if (ch >= NSF1FunctionKey && ch <= NSF1FunctionKey + 19) {
        k.name = "f" + std::to_string(ch - NSF1FunctionKey + 1);
        return k;
    }
    static const struct { unichar ch; const char *name; } kNames[] = {
        {NSUpArrowFunctionKey, "up"}, {NSDownArrowFunctionKey, "down"},
        {NSLeftArrowFunctionKey, "left"}, {NSRightArrowFunctionKey, "right"},
        {NSHomeFunctionKey, "home"}, {NSEndFunctionKey, "end"},
        {NSPageUpFunctionKey, "pageup"}, {NSPageDownFunctionKey, "pagedown"},
        {'\r', "enter"}, {'\t', "tab"}, {' ', "space"}, {0x1B, "escape"},
        {NSBackspaceCharacter, "backspace"}, {NSDeleteFunctionKey, "delete"},
    };
    for (const auto &c : kNames)
        if (ch == c.ch) {
            k.name = c.name;
            return k;
        }
    if (ch < 128) k.name = std::string(1, (char)ch);
    return k;
}

NSString *NS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

// What has the keyboard, in the registry's terms.
Commands::When WhereIs(NSResponder *fr) {
    if ([fr isKindOfClass:[CodeTextView class]]) return Commands::When::Editor;
    NSView *v = nil;
    if ([fr isKindOfClass:[NSTextView class]] && ((NSTextView *)fr).isFieldEditor &&
        [((NSTextView *)fr).delegate isKindOfClass:[NSView class]])
        v = (NSView *)((NSTextView *)fr).delegate;
    else if ([fr isKindOfClass:[NSView class]])
        v = (NSView *)fr;
    Class outline = NSClassFromString(@"ClickOutline");
    for (; v; v = v.superview) {
        if ([v isKindOfClass:[TerminalGridView class]] || [v isKindOfClass:[TerminalView class]])
            return Commands::When::Terminal;
        if (outline && [v isKindOfClass:outline]) return Commands::When::Tree;
    }
    // A web page in the browser pane (or its toolbar), or a PDF.
    if (MCPageViewForResponder(fr)) return Commands::When::Page;
    return Commands::When::Any;
}

// Keys that run only where they apply, from the key monitor, and never sit
// on a menu item (which would run them from every pane): the file tree's
// and a page's.
bool KeysOffMenu(const Commands::Command *c) {
    return c && (c->when == Commands::When::Tree || c->when == Commands::When::Page);
}

// The language of the file the editor shows, for commands scoped to one
// (Commands::Command::language): the window's controller says.
std::string LanguageIn(NSWindow *w) {
    id d = w.delegate;
    if (![d respondsToSelector:@selector(keymapLanguage)]) return std::string();
    NSString *lang = [(id<MCKeymapLanguage>)d keymapLanguage];
    return lang.length ? std::string(lang.UTF8String) : std::string();
}

// Whether a menu item may hold this key of the command: a menu key works in
// every pane, so never a tree command's, and a language command's only when
// it is the key's one command and has Command or Control (Option+C would
// stop typing its character in every field).
bool MenuShows(const Commands::Command *c, const Key &k, const Commands::Keymap &map) {
    if (!EquivalentFor(k) || KeysOffMenu(c)) return false;
    if (c->language)
        return (k.mods & (Commands::Cmd | Commands::Ctrl)) && map.commandsFor(k).size() == 1;
    return true;
}

}  // namespace

// ------------------------------------------------------------ the keymap
@implementation MCKeymap {
    Commands::Keymap _map;
    NSString *_text;
    dispatch_source_t _watch;
    // command id -> the main menu's items for it, in menu order
    NSMutableDictionary<NSString *, NSMutableArray<NSMenuItem *> *> *_items;
    NSHashTable<NSMenuItem *> *_extras;   // hidden items made for second keys
    NSArray<NSString *> *_problems;
}

+ (instancetype)shared {
    static MCKeymap *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[MCKeymap alloc] init]; });
    return s;
}

- (instancetype)init {
    if ((self = [super init])) {
        NSString *env = NSProcessInfo.processInfo.environment[@"MINICODE_KEYS"];
        _path = env.length ? env.stringByExpandingTildeInPath
                           : [NSHomeDirectory() stringByAppendingPathComponent:
                                 @".config/minicode/keys.conf"];
        _errors = @[];
        _map = Commands::defaults();
        _extras = [NSHashTable weakObjectsHashTable];
        [self load];
        [self watch];
    }
    return self;
}

- (const Commands::Keymap &)map { return _map; }

- (BOOL)load {
    NSString *text = [NSString stringWithContentsOfFile:_path encoding:NSUTF8StringEncoding
                                                  error:nil] ?: @"";
    if (_text && [text isEqualToString:_text]) return NO;
    _text = text;
    std::vector<Commands::Error> errs;
    _map = Commands::resolve(text.UTF8String ?: "", &errs);
    NSMutableArray *list = [NSMutableArray array];
    for (const Commands::Error &e : errs)
        [list addObject:[NSString stringWithFormat:@"line %d: %@", e.line, NS(e.message)]];
    _errors = list;
    for (NSString *e in list) NSLog(@"minicode keys.conf %@", e);
    return YES;
}

- (void)reload {
    if (![self load]) return;
    [self applyToMenu:NSApp.mainMenu];
    [[NSNotificationCenter defaultCenter] postNotificationName:MCKeymapDidChangeNotification
                                                        object:self];
}

// As AppSettings watches settings.conf: the file itself, re-armed when an
// editor saves by renaming a new file over it, and polled while missing.
- (void)watch {
    if (_watch) {
        dispatch_source_cancel(_watch);
        _watch = nil;
    }
    int fd = open(_path.fileSystemRepresentation, O_EVTONLY);
    __weak MCKeymap *weakSelf = self;
    if (fd < 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            MCKeymap *s = weakSelf;
            if (s && !s->_watch) {
                [s reload];
                [s watch];
            }
        });
        return;
    }
    dispatch_source_t src = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_VNODE, fd,
        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND | DISPATCH_VNODE_DELETE |
            DISPATCH_VNODE_RENAME,
        dispatch_get_main_queue());
    dispatch_source_set_event_handler(src, ^{
        MCKeymap *s = weakSelf;
        if (!s) return;
        unsigned long flags = dispatch_source_get_data(src);
        if (flags & (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME)) {
            dispatch_source_cancel(src);
            if (s->_watch == src) s->_watch = nil;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 10),
                           dispatch_get_main_queue(), ^{
                [weakSelf reload];
                [weakSelf watch];
            });
        } else {
            [s reload];
        }
    });
    dispatch_source_set_cancel_handler(src, ^{ close(fd); });
    _watch = src;
    dispatch_resume(src);
}

- (BOOL)ensureFileExists {
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:_path]) return YES;
    [fm createDirectoryAtPath:_path.stringByDeletingLastPathComponent
  withIntermediateDirectories:YES attributes:nil error:nil];
    BOOL ok = [NS(Commands::defaultFileText()) writeToFile:_path atomically:YES
                                                  encoding:NSUTF8StringEncoding error:nil];
    if (ok) {
        [self reload];
        [self watch];
    }
    return ok;
}

// ---- the menus
static void MCCollect(NSMenu *menu, NSString *path,
                      NSMutableDictionary<NSString *, NSMutableArray<NSMenuItem *> *> *items,
                      NSMutableDictionary<NSValue *, NSString *> *paths) {
    for (NSMenuItem *it in menu.itemArray) {
        if (it.hasSubmenu) {
            NSString *name = !path ? (it.submenu.title.length ? it.submenu.title : @"minicode")
                                   : [NSString stringWithFormat:@"%@ > %@", path, it.title];
            MCCollect(it.submenu, name, items, paths);
            continue;
        }
        if (paths) paths[[NSValue valueWithNonretainedObject:it]] =
            [NSString stringWithFormat:@"%@ > %@", path ?: @"", it.title];
        if (!it.action) continue;
        const Commands::Command *c =
            Commands::findBySelector(NSStringFromSelector(it.action).UTF8String, (int)it.tag);
        if (!c) {
            if (it.keyEquivalent.length && paths) items[@""] = items[@""] ?: [NSMutableArray array];
            if (it.keyEquivalent.length && paths) [items[@""] addObject:it];
            continue;
        }
        NSString *cid = NS(c->id);
        if (!items[cid]) items[cid] = [NSMutableArray array];
        [items[cid] addObject:it];
    }
}

static void MCSetKey(NSMenuItem *it, const Key *k) {
    NSString *eq = k ? EquivalentFor(*k) : nil;
    if (!eq) {
        it.keyEquivalent = @"";
        it.keyEquivalentModifierMask = 0;
        return;
    }
    it.keyEquivalent = eq;
    it.keyEquivalentModifierMask = FlagsOf(k->mods);
}

- (void)applyToMenu:(NSMenu *)menu {
    if (!menu) return;
    NSMutableDictionary<NSString *, NSMutableArray<NSMenuItem *> *> *items =
        [NSMutableDictionary dictionary];
    const BOOL first = _problems == nil;
    NSMutableDictionary<NSValue *, NSString *> *paths = first ? [NSMutableDictionary dictionary] : nil;
    MCCollect(menu, nil, items, paths);
    if (first) {
        // What the menus were built with, against the registry's defaults.
        const Commands::Keymap defaults = Commands::defaults();
        NSMutableArray *problems = [NSMutableArray array];
        for (NSMenuItem *it in items[@""])
            [problems addObject:[NSString stringWithFormat:@"%@ (%@ %@) has a key but no command",
                                 paths[[NSValue valueWithNonretainedObject:it]],
                                 NSStringFromSelector(it.action), @(it.tag)]];
        for (NSString *cid in items) {
            if (!cid.length) continue;
            const Commands::Command *c = Commands::find(cid.UTF8String);
            std::vector<Key> defaultKeys = Commands::defaultKeys(*c);
            NSArray<NSMenuItem *> *group = items[cid];
            for (NSUInteger i = 0; i < group.count; i++) {
                Key built = KeyForItem(group[i]);
                // The default keys a menu can show, in order (none of a
                // tree command's, which run only from the tree).
                std::vector<Key> shown;
                for (const Key &k : defaultKeys)
                    if (MenuShows(c, k, defaults)) shown.push_back(k);
                Key want = i < shown.size() ? shown[i] : Key();
                if (built != want)
                    [problems addObject:[NSString stringWithFormat:
                        @"%@ was built with \"%@\" where %@'s default is \"%@\"",
                        paths[[NSValue valueWithNonretainedObject:group[i]]],
                        NS(Commands::format(built)), cid, NS(Commands::format(want))]];
            }
        }
        _problems = problems;
    }
    [items removeObjectForKey:@""];
    for (NSString *cid in items) {
        NSMutableArray<NSMenuItem *> *group = items[cid];
        std::vector<Key> keys;
        const Commands::Command *cmd = Commands::find(cid.UTF8String);
        // A menu key works in every pane, so a tree or page command's stays
        // off it (MenuShows).
        if (cmd)
            for (const Key &k : _map.keysOf(cid.UTF8String))
                if (MenuShows(cmd, k, _map)) keys.push_back(k);
        NSMenuItem *primary = group.firstObject;
        for (NSMenuItem *it in group)
            if (!it.isHidden) {
                primary = it;
                break;
            }
        NSMutableArray<NSMenuItem *> *others = [group mutableCopy];
        [others removeObject:primary];
        MCSetKey(primary, keys.empty() ? nullptr : &keys[0]);
        for (size_t i = 1; i < keys.size(); i++) {
            NSMenuItem *it = i - 1 < others.count ? others[i - 1] : nil;
            if (!it) {
                NSMenuItem *last = group.lastObject;
                it = [[NSMenuItem alloc] initWithTitle:primary.title action:primary.action
                                         keyEquivalent:@""];
                it.tag = primary.tag;
                it.target = primary.target;
                it.representedObject = primary.representedObject;
                it.hidden = YES;
                it.allowsKeyEquivalentWhenHidden = YES;
                [last.menu insertItem:it atIndex:[last.menu indexOfItem:last] + 1];
                [group addObject:it];
                [_extras addObject:it];
            }
            MCSetKey(it, &keys[i]);
        }
        for (NSUInteger i = keys.empty() ? 0 : keys.size() - 1; i < others.count; i++) {
            NSMenuItem *it = others[i];
            if ([_extras containsObject:it]) {
                [it.menu removeItem:it];
                [_extras removeObject:it];
            } else {
                MCSetKey(it, nullptr);
            }
        }
    }
    _items = items;
}

- (NSArray<NSMenuItem *> *)itemsFor:(NSString *)cid { return _items[cid]; }
- (NSArray<NSString *> *)problems { return _problems ?: @[]; }

@end

void MCKeymapInstall(void) {
    MCKeymap *k = [MCKeymap shared];
    [k applyToMenu:NSApp.mainMenu];
    MCPageWatchPointer();   // a scroll or click in a page makes Cmd+= its
}

// ------------------------------------------------------------ running
// Runs a command as its menu item would: the item's action through the
// responder chain, validated first. NO when nothing takes it or it is off.
static BOOL MCPerform(const Commands::Command *c) {
    if (!c || !c->selector) return NO;
    SEL sel = NSSelectorFromString(@(c->selector));
    NSMenuItem *item = [[MCKeymap shared] itemsFor:NS(c->id)].firstObject;
    if (!item) {
        item = [[NSMenuItem alloc] initWithTitle:NS(c->title) action:sel keyEquivalent:@""];
        item.tag = c->tag;
    }
    id target = [NSApp targetForAction:sel to:item.target from:item];
    if (!target) return NO;
    if ([target respondsToSelector:@selector(validateMenuItem:)]) {
        if (![target validateMenuItem:item]) return NO;
    } else if ([target respondsToSelector:@selector(validateUserInterfaceItem:)]) {
        if (![target validateUserInterfaceItem:item]) return NO;
    }
    return [NSApp sendAction:sel to:target from:item];
}

BOOL MCKeymapEventIsBound(NSEvent *e) {
    Key k = KeyForEvent(e);
    if (k.empty()) return NO;
    for (const Commands::Command *c : [[MCKeymap shared] map].commandsFor(k))
        if (!(c->flags & Commands::Fixed)) return YES;
    return NO;
}

// Typing: no Command, Control or Option, and not a function key or the
// Menu key. No command has such a key (keys.conf refuses them), so most
// key presses end here.
static BOOL MCJustTyping(NSEvent *e) {
    if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                           NSEventModifierFlagOption))
        return NO;
    const unsigned short code = e.keyCode;
    for (const NamedCode &c : kCodes)
        if (code == c.code) return !(c.name[0] == 'f' || code == 0x6E);
    return YES;
}

BOOL MCKeymapHandleKeyDown(NSEvent *e) {
    // A typing key is a command's only in one language's files
    // (Shift+Return in Python), and only in the editor there.
    const BOOL typing = MCJustTyping(e);
    if (typing && ![e.window.firstResponder isKindOfClass:[CodeTextView class]]) {
        MCPageForgetPointer();
        return NO;
    }
    Key k = KeyForEvent(e);
    if (k.empty()) return NO;
    const Commands::Keymap &map = [[MCKeymap shared] map];
    std::vector<const Commands::Command *> all;
    for (const Commands::Command *c : map.commandsFor(k))
        if (!(c->flags & Commands::Fixed)) all.push_back(c);
    if (all.empty()) {
        MCPageForgetPointer();
        return NO;
    }
    NSResponder *fr = e.window.firstResponder;
    const Commands::When where = WhereIs(fr);
    const std::string language =
        where == Commands::When::Editor ? LanguageIn(e.window) : std::string();
    const Commands::Command *c = map.commandFor(k, where, language);
    if (c && (c->flags & Commands::Fixed)) c = nullptr;
    // A page the pointer was last in (a scroll or a click, no key since)
    // takes its own keys, Cmd+= and the rest, from wherever the keyboard is.
    if (where != Commands::When::Page && MCPagePointedAt(e.window)) {
        const Commands::Command *pc = map.commandFor(k, Commands::When::Page);
        if (pc && pc->when == Commands::When::Page && !(pc->flags & Commands::Fixed)) c = pc;
    }
    if (!c || c->when != Commands::When::Page) MCPageForgetPointer();
    if (typing) {
        // An input method composing (marked text) gets the key, as typing.
        if (!c || !c->language || ((NSTextView *)fr).hasMarkedText) return NO;
        return MCPerform(c);
    }
    // A command for the file's language goes first where it applies.
    if (c && c->language) {
        if (MCPerform(c)) return YES;
        c = map.commandFor(k, where);   // off (a preview): what the key is elsewhere
        if (!c || (c->flags & Commands::Fixed)) return NO;
    }
    const BOOL cmd = (k.mods & Commands::Cmd) != 0, ctrl = (k.mods & Commands::Ctrl) != 0;
    if (c && (c->flags & Commands::Global)) return MCPerform(c);
    // The file tree's commands (F2, Cmd+Delete) and a page's (Cmd+= in the
    // browser) have no menu key, so they run from here while the tree or
    // the page has the keyboard, or not at all.
    if (KeysOffMenu(c)) return MCPerform(c);
    // A Control key whose command does not apply in the terminal is the
    // program's there (Ctrl+Shift+K, the editor's Delete Line, for one).
    if (!c && ctrl && !cmd && [fr isKindOfClass:[TerminalGridView class]]) {
        [fr keyDown:e];
        return YES;
    }
    if (all.size() > 1) return c ? MCPerform(c) : NO;
    if (!cmd && !ctrl) {
        if ([fr isKindOfClass:[CodeTextView class]]) return c ? MCPerform(c) : NO;
        if (!Commands::isFunctionKey(k) &&
            ([fr conformsToProtocol:@protocol(NSTextInputClient)] ||
             [fr isKindOfClass:[TerminalGridView class]])) {
            [fr keyDown:e];
            return YES;
        }
        return NO;
    }
    // A plain Control key is the program's on the terminal's grid.
    if (ctrl && !cmd && !(k.mods & (Commands::Alt | Commands::Shift)) &&
        [fr isKindOfClass:[TerminalGridView class]] && k.name.size() == 1 &&
        !isdigit((unsigned char)k.name[0])) {
        [fr keyDown:e];
        return YES;
    }
    // Keys a menu cannot show or would not match.
    const bool unmenued = !EquivalentFor(k) ||
        (ctrl && !cmd && (k.mods & Commands::Shift) && k.name.size() == 1 &&
         !isalnum((unsigned char)k.name[0]));
    if (unmenued) return c ? MCPerform(c) : NO;
    return NO;
}

// ------------------------------------------------------------ for the views
NSString *MCKeymapGlyphs(NSString *commandId) {
    const std::vector<Key> &keys = [[MCKeymap shared] map].keysOf(commandId.UTF8String ?: "");
    return keys.empty() ? @"" : NS(Commands::glyphs(keys[0]));
}

NSArray<NSString *> *MCKeymapAllGlyphs(NSString *commandId) {
    NSMutableArray *out = [NSMutableArray array];
    for (const Key &k : [[MCKeymap shared] map].keysOf(commandId.UTF8String ?: ""))
        [out addObject:NS(Commands::glyphs(k))];
    return out;
}

NSString *MCKeymapCommandForMenuItem(NSMenuItem *item) {
    if (!item.action) return nil;
    const Commands::Command *c =
        Commands::findBySelector(NSStringFromSelector(item.action).UTF8String, (int)item.tag);
    return c ? NS(c->id) : nil;
}

NSString *MCKeymapGlyphsForMenuItem(NSMenuItem *item) {
    NSString *cid = MCKeymapCommandForMenuItem(item);
    return cid ? MCKeymapGlyphs(cid) : nil;
}

NSArray<NSMenuItem *> *MCKeymapCommandsWithoutMenuItems(void) {
    MCKeymap *km = [MCKeymap shared];
    NSMutableArray *out = [NSMutableArray array];
    for (const Commands::Command &c : Commands::all()) {
        if (!c.selector || (c.flags & Commands::Fixed) || [km itemsFor:NS(c.id)].count) continue;
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:NS(c.title)
                                                    action:NSSelectorFromString(@(c.selector))
                                             keyEquivalent:@""];
        it.tag = c.tag;
        it.representedObject = NS(c.menu);
        [out addObject:it];
    }
    return out;
}

NSArray<NSString *> *MCKeymapMenuProblems(void) { return [[MCKeymap shared] problems]; }
