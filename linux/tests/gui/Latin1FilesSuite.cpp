// Latin1FilesSuite.cpp — text that is not UTF-8 opens as text, read as
// Windows-1252 by the core's rule (FolderSearch::readsAsWindows1252, the
// rule Find in Folder searches by), and is saved back in its encoding
// (`./build/mutible-guitest latin1_files`), the counterpart of the Mac's
// C3b checks in mac/tests/gui/SearchSuite.mm. A Windows-1252 file with é
// and €, and a Latin-1 one holding one of the five bytes Windows-1252
// leaves undefined, open with the right characters and the status bar's
// "Windows-1252"; a save with nothing changed writes the same bytes; an
// edit is saved in Windows-1252; "→", which Windows-1252 cannot store, has
// the file saved as UTF-8 with a note saying so, and the label goes. UTF-16
// with its byte order mark opens and round-trips the same way. A backup
// records the encoding, and a second app restoring a buffer whose file has
// gone saves it back in Windows-1252.
#include "Harness.h"

#include "Backups.h"
#include "DocumentView.h"
#include "Editor.h"
#include "FileTree.h"
#include "Persistence.h"
#include "Search.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }

std::string dir() { return MCGuiProjectFile("latin1"); }
std::string cp1252() { return dir() + "/cp1252.txt"; }
std::string latin1() { return dir() + "/latin1.txt"; }
std::string utf16() { return dir() + "/utf16.txt"; }
std::string hit() { return dir() + "/hit.txt"; }

// "café costs 5 €" and a second line, in Windows-1252.
const std::string kCp1252 = "caf\xE9 costs 5 \x80\nline two\n";
// "Grüße aus Köln ©" in Latin-1, then 0x81, one of the five bytes
// Windows-1252 leaves undefined, which reads as Latin-1's C1 control.
const std::string kLatin1 = std::string("Gr\xFC\xDF" "e aus K\xF6ln \xA9\n") + "x\x81y\n";
// "naïve" in UTF-16 LE with its byte order mark.
const std::string kUtf16 = std::string("\xFF\xFEn\0a\0\xEF\0v\0" "e\0\n\0", 14);

// The restore: a folder of its own, whose one buffer's file is gone.
std::string restoreDir() { return MCGuiScratch() + "/latin1-restore"; }
std::string gone() { return restoreDir() + "/gone.txt"; }

uint64_t inodeOf(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_ino : 0;
}

// The status bar's encoding label, shown, with this text.
bool encodingShown(const std::string& label) {
    GtkWidget* l = MCGuiFindLabel(MCGuiApp().statusBar, label);
    return l && gtk_widget_get_visible(l);
}

std::string status() {
    GtkWidget* l = MCGuiApp().statusLabel;
    return l ? gtk_label_get_text(GTK_LABEL(l)) : "";
}

void open(const std::string& path) { MCGuiOpenFileKept(path); }

bool showing(const std::string& path, const std::string& text) {
    return ed()->currentPath() == path && MCGuiEditorText() == text;
}

// The backup in the store naming `path`, or "".
std::string backupOf(const std::string& path) {
    const char* store = g_getenv("MUTIBLE_BACKUP_DIR");
    GDir* d = store ? g_dir_open(store, 0, nullptr) : nullptr;
    if (!d) return "";
    std::string found;
    while (const char* name = g_dir_read_name(d)) {
        const std::string f = name;
        if (f.size() < 9 || f.compare(f.size() - 9, 9, ".mcbackup") != 0) continue;
        const std::string body = MCGuiReadFile(std::string(store) + "/" + f);
        if (body.find("path: " + path + "\n") != std::string::npos) found = body;
    }
    g_dir_close(d);
    return found;
}

int caretLine() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    return gtk_text_iter_get_line(&it);
}

GtkWidget* labelHolding(GtkWidget* root, const std::string& part) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_LABEL(root)) {
        const char* text = gtk_label_get_text(GTK_LABEL(root));
        if (text && std::string(text).find(part) != std::string::npos)
            return MCGuiOnScreen(root) ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* found = labelHolding(c, part)) return found;
    return nullptr;
}

uint64_t gInode = 0;
GtkWindow* gSearch = nullptr;

}  // namespace

MC_GUI_SUITE(latin1_files) {
    MCGuiStep("a Windows-1252 file, a Latin-1 one and a UTF-16 one", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiWriteFile(cp1252(), kCp1252);
        MCGuiWriteFile(latin1(), kLatin1);
        MCGuiWriteFile(utf16(), kUtf16);
        open(cp1252());
    });
    MCGuiWait("the Windows-1252 file opens as text, é and € as they are", 10, [] {
        return showing(cp1252(), "café costs 5 €\nline two\n");
    });
    MCGuiWait("the status bar names its encoding", 5, [] { return encodingShown("Windows-1252"); });
    MCGuiStep("it is not unsaved; Ctrl+S", [] {
        GCHECK(!ed()->dirty());
        gInode = inodeOf(cp1252());
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("a save with nothing changed writes the file again", 10, [] {
        return inodeOf(cp1252()) != gInode;
    });
    MCGuiStep("byte for byte as it was", [] {
        GCHECK(MCGuiReadFile(cp1252()) == kCp1252);
        GCHECK(encodingShown("Windows-1252"));
    });

    MCGuiStep("the Latin-1 file", [] { open(latin1()); });
    MCGuiWait("opens as text, its undefined byte as U+0081", 10, [] {
        return showing(latin1(), "Grüße aus Köln ©\nx\xC2\x81y\n");
    });
    MCGuiWait("named Windows-1252 too", 5, [] { return encodingShown("Windows-1252"); });
    MCGuiStep("saved unchanged", [] {
        gInode = inodeOf(latin1());
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("written again", 10, [] { return inodeOf(latin1()) != gInode; });
    MCGuiStep("every byte back", [] { GCHECK(MCGuiReadFile(latin1()) == kLatin1); });

    MCGuiStep("the UTF-16 file", [] { open(utf16()); });
    MCGuiWait("opens as text without its mark", 10, [] { return showing(utf16(), "naïve\n"); });
    MCGuiWait("named UTF-16 LE", 5, [] { return encodingShown("UTF-16 LE"); });
    MCGuiStep("saved unchanged", [] {
        GCHECK(!encodingShown("Windows-1252"));
        gInode = inodeOf(utf16());
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("written again", 10, [] { return inodeOf(utf16()) != gInode; });
    MCGuiStep("byte for byte, its mark first", [] { GCHECK(MCGuiReadFile(utf16()) == kUtf16); });

    // ---- a Find in Folder hit in such a file opens it as text at its line
    MCGuiStep("a Windows-1252 file with a hit on line 3; its folder selected", [] {
        MCGuiWriteFile(hit(), "one\ntwo\nthe caf\xE9 l1needle\n");
        MCGuiApp().tree->revealPath(dir());
    });
    MCGuiWait("selected", 10, [] { return MCGuiApp().tree->selectedDir() == dir(); });
    MCGuiStep("Find in Folder", [] { MCGuiKey("ctrl+shift+f"); });
    MCGuiWait("its window", 10, [] { return MCGuiTopWindow() != MCGuiApp().window; });
    MCGuiStep("the word typed over whatever was there", [] {
        gSearch = MCGuiTopWindow();
        MCGuiSetKeyWindow(gSearch);
        if (SearchPanel* s = MCGuiApp().search) {
            gtk_editable_set_text(GTK_EDITABLE(s->includeEntry()), "");
            gtk_editable_set_text(GTK_EDITABLE(s->excludeEntry()), "");
        }
        MCGuiKey("ctrl+a");
        MCGuiType("l1needle");
    });
    MCGuiWait("the hit, read as Windows-1252", 10, [] {
        return labelHolding(GTK_WIDGET(gSearch), "the café l1needle") != nullptr;
    });
    MCGuiStep("a click on it", [] {
        MCGuiClick(labelHolding(GTK_WIDGET(gSearch), "the café l1needle"));
    });
    MCGuiWait("the file open as text at line 3", 10, [] {
        return showing(hit(), "one\ntwo\nthe café l1needle\n") && caretLine() == 2;
    });
    MCGuiWait("named Windows-1252", 5, [] { return encodingShown("Windows-1252"); });
    MCGuiStep("the search window closed", [] {
        GCHECK(!ed()->dirty());
        MCGuiSetKeyWindow(gSearch);
        MCGuiKey("escape");
    });
    MCGuiWait("gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("the editor's window", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        MCGuiResetFolderSearch();   // its scope is not the next suite's
    });

    // ---- an edit, in the encoding; then a character it cannot store
    MCGuiStep("back to the Windows-1252 file, ü typed at its end", [] { open(cp1252()); });
    MCGuiWait("on show", 10, [] { return ed()->currentPath() == cp1252(); });
    MCGuiStep("type ü", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+End");
        MCGuiType("ü");
    });
    MCGuiWait("in the buffer, unsaved", 10, [] {
        return MCGuiEditorText() == "café costs 5 €\nline two\nü" && ed()->dirty();
    });
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("saved", 10, [] { return !ed()->dirty(); });
    MCGuiStep("written in Windows-1252, the label kept", [] {
        GCHECK(MCGuiReadFile(cp1252()) == kCp1252 + "\xFC");
        GCHECK(encodingShown("Windows-1252"));
    });
    MCGuiStep("type →, which Windows-1252 cannot store", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+End");
        MCGuiType("→");
    });
    MCGuiWait("in the buffer", 10, [] {
        return MCGuiEditorText() == "café costs 5 €\nline two\nü→" && ed()->dirty();
    });
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("saved", 10, [] { return !ed()->dirty(); });
    MCGuiStep("written as UTF-8, said so, and the label gone", [] {
        GCHECK_EQ_STR(MCGuiReadFile(cp1252()), "café costs 5 €\nline two\nü→");
        GCHECK_EQ_STR(status(), "Saved cp1252.txt as UTF-8: Windows-1252 cannot store “→”");
        GCHECK(!encodingShown("Windows-1252"));
    });

    // ---- the backup records the encoding
    MCGuiStep("the Latin-1 file, typed into", [] { open(latin1()); });
    MCGuiWait("on show", 10, [] { return ed()->currentPath() == latin1(); });
    MCGuiStep("type x", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+End");
        MCGuiType("x");
    });
    MCGuiWait("its backup, saying how the file is written", 15, [] {
        return backupOf(latin1()).find("\nfile-encoding: windows1252\n") != std::string::npos;
    });
    MCGuiStep("the x taken back", [] { MCGuiKey("BackSpace"); });
    MCGuiWait("clean again, its backup gone", 15, [] {
        return !ed()->dirty() && backupOf(latin1()).empty();
    });

    // A backup no running process holds, of a file gone since, recorded as
    // Windows-1252: a second app opening the folder restores it, and its
    // save writes Windows-1252.
    MCGuiStep("a backup left by a process that went, its file gone", [] {
        g_mkdir_with_parents(restoreDir().c_str(), 0755);
        g_remove(gone().c_str());
        Backups::Entry e;
        e.id = "latin1-restore-0001";
        e.path = gone();
        e.root = Persistence::keyFor(restoreDir());
        e.diskExisted = true;
        e.diskMtime = 1700000000;
        e.diskInode = 1;
        e.fileEncoding = "windows1252";
        e.saved = (double)g_get_real_time() / 1e6;
        e.ownerPid = 0;   // nobody's
        MCGuiWriteFile(std::string(g_getenv("MUTIBLE_BACKUP_DIR")) + "/" + Backups::fileName(e.id),
                       Backups::encode(e, "naïve café\n"));
    });
    MCGuiAsync("a second app on the folder restores it and saves it", 90, [](MCGuiDone done) {
        MCGuiRunChild("_latin1_restore", restoreDir(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("restore (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiStep("written back in Windows-1252", [] {
        GCHECK(MCGuiReadFile(gone()) == "na\xEFve caf\xE9\n");
        GCHECK(backupOf(gone()).empty());
    });

    MCGuiStep("the tabs closed", [] {
        for (const std::string& p : {cp1252(), latin1(), utf16(), hit()}) {
            if (Document* d = ed()->documentFor(p)) {
                GCHECK(!d->dirty());
                ed()->closeTab(d);
            }
        }
    });
}

MC_GUI_SUITE(_latin1_restore) {
    MCGuiWait("the window on the folder, its buffer back", 20, [] {
        return MCGuiApp().editor && MCGuiApp().editor->documentFor(gone()) &&
               gtk_widget_get_mapped(GTK_WIDGET(MCGuiApp().window));
    });
    MCGuiStep("shown", [] {
        MCGuiSetKeyWindow(MCGuiApp().window);
        open(gone());
    });
    MCGuiWait("on show, unsaved", 10, [] {
        return showing(gone(), "naïve café\n") && ed()->dirty();
    });
    MCGuiWait("the backup's encoding, in the status bar", 5, [] {
        return encodingShown("Windows-1252");
    });
    MCGuiStep("Ctrl+S", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("saved", 10, [] { return !ed()->dirty() && MCGuiFileExists(gone()); });
    MCGuiStep("in Windows-1252; quit", [] {
        GCHECK(MCGuiReadFile(gone()) == "na\xEFve caf\xE9\n");
        MCGuiKey("ctrl+q");
    });
    MCGuiWait("every window closes", 15, [] { return MCGuiAppWindows().empty(); });
}
