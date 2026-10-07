// MarkdownTasks.h — pure C++. Task lists ("- [ ] buy milk", "- [x] done"),
// the TODO format GitHub, Obsidian and VS Code read, and the edits an editor
// makes to them: a click on a box in the preview, a key that toggles the
// lines the caret or selection touches, and Return continuing a list.
//
// Every edit replaces one range, so the rest of the file is never touched.
// The preview's edits work in UTF-8 bytes, like MarkdownEdit; the editor's
// in UTF-16 code units, like LineComments, so results map straight onto
// NSRange and Java strings (the GTK port converts). What counts as a task is
// decided by MarkdownParser::lines, so a "- [ ]" inside a code block is
// never one.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace MarkdownTasks {

// A task's box on one line of the source.
struct Box {
    bool found = false;
    bool checked = false;
    size_t mark = 0;   // byte offset of the character between the brackets
};

// The box on 0-based source `line`, if the parser reads that line as a task
// list item (at any depth, in a block quote too).
Box boxOnLine(const std::string& source, int line);

// The source with the box on `line` ticked or cleared: the one character
// between its brackets becomes 'x' or ' '. Unchanged when the line holds no
// task. `mark`, if given, gets the byte offset of the changed character.
std::string toggleBox(const std::string& source, int line, size_t* mark = nullptr);

struct Counts {
    int done = 0, total = 0;
};
Counts count(const std::string& source);

// An open task on a line, as the TODO list across a project shows it.
struct OpenTask {
    int line = 0;          // 0-based
    size_t column = 0;     // byte offset of the box's '[' in the line
};
std::vector<OpenTask> openTasks(const std::string& source);

// One replacement in UTF-16 text: [replaceStart, replaceStart +
// replaceLength) becomes `replacement`, and the selection moves to
// [selStart, selEnd).
struct Edit {
    bool changed = false;
    size_t replaceStart = 0, replaceLength = 0;
    std::u16string replacement;
    size_t selStart = 0, selEnd = 0;
};

// The task key, on the lines [selStart, selEnd) touches (a selection ending
// at the very start of a line does not include it). A line that is not a
// task becomes one, unchecked: "text" -> "- [ ] text", "- text" ->
// "- [ ] text", "1. text" -> "1. [ ] text", "> text" -> "> - [ ] text".
// When every touched line is already a task, they are all checked, or, if
// they all were, all cleared. A wrapped item's continuation line acts on its
// item. Headings, code, tables and rules are left alone, and blank lines
// too, unless only one blank line is touched, which becomes "- [ ] ".
Edit toggle(const std::u16string& text, size_t selStart, size_t selEnd);

// Return in a list item, with the caret after its marker (and box): a new
// item starts on the next line with the same indentation, quote markers and
// marker ("- [ ] " after a task, whether or not it was checked; "4. " after
// "3. "), taking the text after the caret with it. Return on an item with
// nothing in it ends the list: the marker is removed and the line left
// empty. Anything else (not a list, caret in the marker, a code block) gives
// changed == false, and the editor does its usual Return.
Edit newline(const std::u16string& text, size_t selStart, size_t selEnd);

}  // namespace MarkdownTasks
