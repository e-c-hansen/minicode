// LatexScratch.h — pure C++. The files the LaTeX preview leaves while it
// typesets: each document's own output folder, and the record of the hidden
// sibling (".<name>.minicode.tex") written next to the user's file, so that
// a sibling a crash left behind can be found and removed at the next launch
// without ever removing a file of the user's. Latex.mm does the I/O and the
// hashing; nothing here touches the disk.
#pragma once
#include <cstdint>
#include <string>

namespace LatexScratch {

// The name of a document's output folder under MiniCode-latex: the first 16
// hex digits of the SHA-256 of its resolved path (`pathSha256Hex`), so two
// paper.tex files in different folders never share a PDF or SyncTeX file.
// "untitled" when there is no hash to go by.
std::string folderName(const std::string& pathSha256Hex);

// What was written where, and by whom, just before tectonic ran.
struct Record {
    std::string sibling;        // the hidden copy's full path
    long long pid = 0;          // the process that wrote it
    double start = 0;           // and when that process started, against pid reuse
    unsigned long long size = 0;
    std::string sha256;         // of the bytes written, lowercase hex
};

// The record as a file's text, and back. parse is false for anything that
// is not a whole record of this format.
std::string format(const Record& r);
bool parse(const std::string& text, Record* out);

// What to do with a record found on disk.
enum class Action {
    Keep,             // its writer is still running: leave both alone
    RemoveSibling,    // its writer is gone and the sibling is our copy
    ForgetRecord,     // its writer is gone and the sibling is gone or not ours
};
// `writerAlive` is whether record.pid is running and started at
// record.start; the sibling's size and hash are what is on disk now
// (`siblingExists` false when there is nothing there). A file is ours only
// when it has the preview copy's name and exactly the bytes recorded, so a
// file the user made or changed since is never removed.
Action decide(const Record& r, bool writerAlive, bool siblingExists,
              unsigned long long size, const std::string& sha256);

// Whether `path` is named as the preview's copies are (".x.minicode.tex",
// or "untitled.minicode.tex" in the output folder).
bool isPreviewCopyName(const std::string& path);

}  // namespace LatexScratch
