// Commands.h — the command registry and the user's keymap file.
//
// Every command mutible has a key for (and most it has none for) is one row
// of a table here: a stable ID, in VS Code's spelling where VS Code has the
// command (workbench.action.showCommands, editor.action.formatDocument, ...)
// and mutible.* where it does not, its title and menu, its default keys in
// mutible's key notation, where it applies (the editor, the terminal, the
// file tree, or anywhere), and the selector its menu item sends.
//
// The keymap file, ~/.config/mutible/keys.conf (or $MUTIBLE_KEYS), adds to
// and takes from the defaults, one line at a time, as VS Code's
// keybindings.json does:
//
//   ctrl+alt+cmd+k = editor.action.deleteLines   another key for a command
//   ctrl+shift+k = -                              the key, from every command
//   cmd+k = -workbench.action.terminal.clear      the key, from that command
//
// A key given to a command is taken from any other command that had it by
// default where the two would both apply (a key of the terminal's is kept
// beside an editor-only command's). Two lines giving one key to two
// commands that both apply in one place are a conflict: the later line is
// refused, with its line number, as the settings file's errors are.
//
// Key notation: modifiers ctrl, alt (or opt, option), shift, cmd joined by
// "+" before the key, written in that order (Apple's ⌃⌥⇧⌘): "shift+cmd+p",
// "ctrl+`", "shift+f12", "alt+shift+f". Keys are letters, digits, the
// unshifted punctuation `-=[]\;',./, f1 to f20, up, down, left, right, home,
// end, pageup, pagedown, enter (Return), tab, space, escape, backspace,
// delete (forward), numpad0 to numpad9 and contextmenu (a PC keyboard's Menu
// key). Pure C++, no I/O; mac/src/Keymap.mm puts the result on the menus.
//
// The keys in each row are the Mac's. The GTK port's are a table of their
// own, src/CommandsLinux.cpp (linux/PARITY.md, D1), in the same notation and
// over the same commands; the functions that take a Platform read it, and
// those without one are the Mac's.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace Commands {

// Where a command applies, for conflicts and for a key two commands share.
// Page is a web page in the browser pane (or its toolbar) or a PDF (the
// LaTeX preview's too); NotPage is everywhere else, so Cmd+0 can be a
// page's Actual Size there and Focus File Tree anywhere else.
// NotPageOrTerminal is everywhere but a page and the terminal, so Cmd+= can
// zoom a page in a page, the terminals in a terminal, and the editor's text
// anywhere else. NotTree is everywhere but the file tree, so Shift+Cmd+N
// can be New Folder in the tree and New Window anywhere else.
enum class When { Any, Editor, Terminal, Tree, Page, NotPage, NotPageOrTerminal, NotTree };
// "anywhere", "the editor", "the terminal", "the file tree", "a web page or
// a PDF", "anywhere but a web page or a PDF", "anywhere but a web page, a
// PDF, or the terminal", "anywhere but the file tree"
const char* whenName(When w);
// Can one key press be meant for both? Any meets everything, NotPage
// everything but Page, NotPageOrTerminal everything but Page and Terminal,
// NotTree everything but Tree.
bool overlaps(When a, When b);
// Does a command for `w` apply where `where` has the keyboard? Any does
// everywhere, NotPage everywhere but in a page, NotPageOrTerminal
// everywhere but in a page or the terminal, NotTree everywhere but in the
// file tree.
bool appliesIn(When w, When where);

enum Mod : unsigned { Ctrl = 1, Alt = 2, Shift = 4, Cmd = 8 };

struct Key {
    unsigned mods = 0;
    std::string name;   // "p", "`", "f12", "enter", "numpad0", ...
    bool empty() const { return name.empty(); }
    bool operator==(const Key& o) const { return mods == o.mods && name == o.name; }
    bool operator!=(const Key& o) const { return !(*this == o); }
};

// "shift+cmd+p" -> Key. Case and spaces around "+" are ignored, and so are
// the modifiers' other names (command, control, option, opt) and keys'
// (return, esc, ...). On failure returns false and says why.
bool parseKey(const std::string& text, Key* out, std::string* error = nullptr);
// The notation, modifiers in order: "ctrl+alt+shift+cmd+k".
std::string format(const Key& k);
// As a Mac menu draws it: "⌃⌥⇧⌘K", "⇧F12", "⌃Space", "⌘⏎".
std::string glyphs(const Key& k);
// A function key (f1 to f20) or the Menu key: these type nothing.
bool isFunctionKey(const Key& k);
// Does pressing it type or move in a text field, so that it cannot be a
// command's key on its own? Keys without Command, Control or Option, other
// than function keys.
bool typesText(const Key& k);

// Command flags.
enum Flag : unsigned {
    // The key monitor runs it from anywhere in the window, before any view:
    // the terminal's grid, which claims most Control keys, included.
    Global = 1,
    // Its keys are a view's own (the editor's text view, the terminal):
    // listed for reference and protected from other commands, but keys.conf
    // cannot change them yet.
    Fixed = 2,
};

struct Command {
    const char* id;
    const char* title;     // the menu item's title, "Command Palette…"
    const char* menu;      // "View", "Edit > Find Options"
    const char* keys;      // default keys, space separated; "" for none
    When when;
    const char* selector;  // the menu item's action, "showCommandPalette:"; null for none
    int tag;               // the item's tag when the action is shared
    unsigned flags;
    const char* note;      // Fixed: who handles the keys; else why there is no menu item
    // A language scope: the command applies only in the editor (when must
    // be Editor) while it shows a file of this language ("markdown",
    // "python", as languageOfPath names them). Such a command's key may be
    // another command's elsewhere: it shadows the other there, and is no
    // conflict (Option+C toggles a task in Markdown and types ç anywhere
    // else). Its keys are put on its menu item only when no other command
    // has them and they hold Command or Control; the key monitor runs the
    // rest. A typing key (Shift+Return) is allowed for these alone.
    const char* language = nullptr;
};

// The language scope of a file, by its extension: "markdown" (.md,
// .markdown, .mdown, .mkd, .mkdn), "python" (.py, .pyi, .pyw), else "".
std::string languageOfPath(const std::string& path);
// "Markdown files", "Python files", for messages.
std::string languageName(const std::string& language);
// Where a command applies, as messages say it: "the editor, in Python
// files", "the terminal", "anywhere".
std::string placeName(const Command& c);

// Every command, in menu order.
const std::vector<Command>& all();
const Command* find(const std::string& id);
const Command* findBySelector(const std::string& selector, int tag);
std::vector<Key> defaultKeys(const Command& c);

struct Error {
    int line = 0;   // 1-based; 0 for the whole file
    std::string message;
};

// Whose keys (see "platforms" below).
enum class Platform { Mac, Linux };

// A key as the shortcut hints show it, in symbols where a key has one. The
// Mac's is glyphs(): "⇧⌘P", "⌥⌘↓", "⌘⏎". Linux's names its modifiers in
// words, in the order GTK and VS Code write them, joined by "+", and draws
// the keys that have a symbol as the Mac does: "Ctrl+Shift+P",
// "Ctrl+Shift+⏎", "Alt+↑", "Shift+⇥", "⎋", "Ctrl+⌫"; Home, End, Page Up,
// Page Down, Space, Menu, and the keypad keep their names, which read more
// plainly there than ↖ ↘ ⇞ ⇟. Menus draw their own keys; this is for the
// hints panels (HintsSearch.h finds keys in it by name on either platform).
std::string keyLabel(const Key& k, Platform p);

// The keys in effect: every command's, in order (the first is the one a
// menu shows; a user's keys come before the defaults).
class Keymap {
public:
    const std::vector<Key>& keysOf(const std::string& id) const;
    // The commands a key runs, in table order (several only where they
    // apply in different places).
    std::vector<const Command*> commandsFor(const Key& k) const;
    // Of commandsFor(k), the one for what has the keyboard: one for that
    // place and the language of the file on show (the editor only), else
    // one for that place, else one that applies there too (Any, or NotPage
    // outside a page); null for none.
    const Command* commandFor(const Key& k, When where,
                              const std::string& language = std::string()) const;
    // Pairs of commands sharing a key where both apply: "cmd+k: Clear
    // Terminal and Delete Line". The defaults have none (a test says so).
    std::vector<std::string> conflicts() const;

    std::map<std::string, std::vector<Key>> keys;   // by command id
private:
    friend Keymap defaults(Platform);
    friend Keymap resolve(const std::string&, std::vector<Error>*, Platform);
    void index();
    std::map<std::string, std::vector<const Command*>> byKey_;   // format(key) -> commands
};

Keymap defaults();
// The defaults with the keymap file's lines applied; problems in errors.
Keymap resolve(const std::string& fileText, std::vector<Error>* errors);

// What Keyboard Shortcuts writes when there is no file yet: how the file
// works, then every command and its default keys, commented out.
std::string defaultFileText();

// ------------------------------------------------------------ platforms
// Mac: the rows' own keys, every command. Linux: CommandsLinux.cpp's table,
// and only the commands it has a row for, the ones the GTK port has. A
// command without a row there has no keys on Linux, keys.conf cannot give it
// one, and the port shows it nowhere.

// One command on Linux (CommandsLinux.cpp). Titles and menus are the
// registry's unless Linux words them otherwise ("Open Containing Folder",
// Quit in the File menu).
struct LinuxRow {
    const char* id;
    const char* keys;     // default keys, space separated, never cmd; "" for none
    unsigned flags;       // Global: kept while the terminal has the keyboard
    const char* title;    // null: the registry's
    const char* menu;     // null: the registry's
};
// In the registry's order.
const std::vector<LinuxRow>& linuxRows();
const LinuxRow* linuxRow(const std::string& id);

bool available(const Command& c, Platform p);
std::vector<Key> defaultKeys(const Command& c, Platform p);
const char* titleOf(const Command& c, Platform p);
const char* menuOf(const Command& c, Platform p);
// The registry's flags, with Linux's Global added from its row.
unsigned flagsOf(const Command& c, Platform p);
Keymap defaults(Platform p);
// On Linux a line naming a command Linux lacks, or a key with cmd (a Mac's
// keys.conf), is refused; a file tree command may have a key that types
// elsewhere (Delete, as in VS Code's explorer), since the tree takes no text.
Keymap resolve(const std::string& fileText, std::vector<Error>* errors, Platform p);
// Linux's lists only the commands Linux has, with Linux's modifiers.
std::string defaultFileText(Platform p);

// ---- Linux keys and the terminal (PARITY.md D1)
// While the terminal has the keyboard, plain Ctrl keys, keys with no
// modifier (F9, F12) and Alt with a letter alone (Meta to the program) are
// the program's unless the command is Global, so a terminal command takes
// Ctrl+Shift. Other keys with Shift or Alt stay the app's, except VTE's own
// copy and paste (Ctrl+Shift+C and Ctrl+Shift+V) and the two a shell reads
// as their Ctrl key (Ctrl+Shift+G as Ctrl+G, and Ctrl+Shift+Space as
// Ctrl+Space). Does `k`, one of `c`'s keys, go to the
// terminal rather than run `c` there? Says nothing of whether `c` applies
// in the terminal at all (appliesIn). With `macStyle`
// (terminal.mac-style-keys, below) no key with Ctrl goes to the terminal
// but VTE's copy and paste.
bool linuxTerminalTakes(const Command& c, const Key& k, bool macStyle = false);
// VTE's copy and paste: no command has them while the terminal has the
// keyboard, whatever keys.conf says.
bool linuxTerminalOwnKey(const Key& k);

// ---- Mac-style keys in the terminal (terminal.mac-style-keys, Linux only)
// A Mac-style remapper (Toshy, Kinto) gives a PC keyboard the Mac's keys:
// the key beside the space bar, where the Mac has Cmd, arrives as Ctrl,
// and the physical Ctrl as Super. With the setting on, while the terminal
// has the keyboard, they do what they do in the Mac's Terminal: Ctrl keys
// are mutible's, as Cmd keys are there (linuxTerminalTakes with macStyle
// leaves every Ctrl key to its command), and Super keys are the program's
// Ctrl keys. What a key no command took does then:
enum class MacStyleAction {
    Pass,        // on to VTE, as with the setting off
    Drop,        // nothing: a Ctrl key no command has never reaches the program
    Copy,        // Ctrl+C: the terminal's selection to the clipboard
    Paste,       // Ctrl+V: the clipboard to the program, as VTE pastes
    SelectAll,   // Ctrl+A: the whole terminal selected
    Send,        // Super: `bytes` to the program, what Ctrl with the key sends
};
struct MacStyleRoute {
    MacStyleAction action = MacStyleAction::Pass;
    std::string bytes;   // Send's
};
// `mods`: Ctrl, Alt and Shift held (Mod; Cmd is not used); `super`: Super
// held; `ch`: the character the key types with them, Shift applied as
// GDK's keyval has it ('W' for Shift+W, '@' for Shift+2, '_' for Shift+-),
// 0 for a key that types none (arrows, F keys, Return, Tab, Escape,
// Backspace, Delete), which is left to VTE either way.
//   - Super (Shift and Alt or not, Ctrl not) with a character that has a
//     control byte sends that byte: Super+W ^W, Super+C ^C, Super+[ Esc,
//     Super+Space and Super+Shift+2 NUL, Super+Shift+- ^_. Shift changes
//     nothing a terminal sends for a letter, and Alt is for the keys
//     Toshy sends as Super+Alt so that its own keymap and GNOME leave them
//     alone (Super+Alt+K ^K, Super+Alt+L ^L). Super with any other key
//     passes.
//   - Ctrl (Super not) with a character: Ctrl+C copies, Ctrl+V pastes,
//     Ctrl+A selects all, VTE's own Ctrl+Shift+C and Ctrl+Shift+V pass, and
//     the rest is dropped (with Shift or Alt too).
//   - Anything else passes: Alt alone (Meta), plain keys, Ctrl with a key
//     that types nothing (Ctrl+Left, Ctrl+Backspace).
MacStyleRoute linuxMacStyleKey(unsigned mods, bool super, char32_t ch);
// The byte a terminal sends for Ctrl with the key that types `ch` (Shift
// applied), as VTE sends it: a letter's control code (^A 0x01 to ^Z 0x1A,
// either case), Space, @ and 2 NUL, [ { and 3 Esc, \ | and 4 0x1C, ] } and
// 5 0x1D, ^ ~ and 6 0x1E, _ - / and 7 0x1F, 8 and ? DEL; -1 for any other.
int terminalControlByte(char32_t ch);
// GTK's accelerator strings for a key (gtk_accelerator_parse): "<Ctrl>s",
// "<Ctrl><Shift>p". Shift with a digit or punctuation is the character it
// types on a US layout ("<Ctrl>greater" for ctrl+shift+.), since GTK matches
// the keyval a key produced and counts Shift as used up by it; the keypad's
// plus and minus are added beside "+" and "-". None for a key with cmd.
std::vector<std::string> linuxAccelerators(const Key& k);
// As the shortcut hints, the status bar and docs/REFERENCE.md's Linux column
// write it:
// "Ctrl Shift P", "Ctrl `", "Shift F3", "Page Up"; with "+" between,
// "Ctrl+Shift+H".
std::string linuxLabel(const Key& k, const char* between = " ");

}  // namespace Commands
