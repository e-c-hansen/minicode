// Document.h — one open file as the core keeps it (SESSION-DESIGN.md,
// stage 1): its text (Model::Text), the text as last read or written (the
// saved text), the file's stamp from then, how its bytes are written (the
// encoding and byte order mark), whether the file has gone, and whether it
// is large. The rules are the Mac's, as docs/notes/sessions.md's "Tabs, sessions and
// the disk" gives them, with the same tests:
//
//   - Dirty means "differs from the saved text", not "was edited": an edit
//     taken back leaves none. A file deleted outside is dirty, having
//     nothing on disk to match. Lengths are compared first.
//   - The stamp is taken before the bytes are read (FolderSearch's
//     readFileStable), so a file still being written never reads as clean,
//     and a file changed when its date, inode or size moved.
//   - UTF-16 with its byte order mark is text, decoded exactly (a lone
//     surrogate or an odd byte leaves the file to another viewer), and is
//     written back in its own encoding; UTF-8's mark is kept apart and
//     written back; a text holding a lone surrogate cannot be written.
//   - Large files are VS Code's (LargeFile.h).
//
// The ports keep their own documents until the session moves into the
// core (stages 3 and 4); the language server client's copy of each file is
// already a Model::Text. Pure C++17; no I/O (the caller reads and writes,
// with FolderSearch's readFileStable and writeFileAtomically).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "FolderSearch.h"
#include "Text.h"

namespace Model {

// Whether a file changed since `known` was taken (both by stampOf or
// readFileStable): something there now and not then, or the other way, or
// a different date, inode or size. Two missing files are the same.
bool stampChanged(const FolderSearch::FileStamp& known, const FolderSearch::FileStamp& now);

// A file's bytes as text: UTF-16 with its byte order mark (exactly), else
// UTF-8 with its mark taken off, which must then be valid. False when the
// bytes are not text that way.
bool decodeFileText(std::string bytes, std::u16string* text, FolderSearch::TextEncoding* encoding,
                    bool* bom);

class Document {
public:
    // A new document, with no file behind it yet: clean, written as UTF-8.
    Document() = default;
    explicit Document(const std::u16string& text);

    // Read from a file: its bytes and the stamp taken before reading them.
    // False, and nothing changed, when the bytes are not text.
    bool load(std::string bytes, const FolderSearch::FileStamp& stamp);
    // The text replaced from the disk (Reload, or a clean document
    // following the file): as load, as one change of the text, so its
    // version moves on.
    bool reload(std::string bytes, const FolderSearch::FileStamp& stamp);

    const Text& text() const { return text_; }
    // The text's version: every change moves it on.
    std::uint64_t version() const { return text_.version(); }
    // Edits, as Text::apply. False, and nothing changed, when they are not
    // ascending, apart and inside the text.
    bool apply(const std::vector<Text::Edit>& edits, Unit u = Unit::Utf16);
    bool apply(const std::vector<EditSpans::Span>& spans, const std::vector<std::u16string>& texts,
               Unit u = Unit::Utf16);
    void replace(std::size_t a, std::size_t b, const std::u16string& with);

    bool dirty() const;
    const std::u16string& savedText() const { return saved_; }
    // The text as it is now was written, and the file's stamp as written.
    void markSaved(const FolderSearch::FileStamp& written);

    // The bytes a save writes: the text in its encoding, with its mark.
    // False, with *why, when the text holds a lone surrogate, or a
    // character its encoding cannot store (Windows-1252's 256: canEncode).
    bool bytesToWrite(std::string* bytes, std::string* why) const;
    FolderSearch::TextEncoding encoding() const { return encoding_; }
    bool bom() const { return bom_; }
    // Save As keeps the encoding; a new document may be given one.
    void setEncoding(FolderSearch::TextEncoding encoding, bool bom) {
        encoding_ = encoding;
        bom_ = bom;
    }

    const FolderSearch::FileStamp& stamp() const { return stamp_; }
    bool changedOnDisk(const FolderSearch::FileStamp& now) const { return stampChanged(stamp_, now); }
    // Deleted outside ("(deleted)" on its tab), until the file returns.
    bool gone() const { return gone_; }
    void setGone(bool gone) { gone_ = gone; }
    // Over VS Code's thresholds when last read (LargeFile::isLarge).
    bool large() const { return large_; }

private:
    Text text_;
    std::u16string saved_;
    FolderSearch::FileStamp stamp_;
    FolderSearch::TextEncoding encoding_ = FolderSearch::TextEncoding::Utf8;
    bool bom_ = false, gone_ = false, large_ = false;
    // dirty() compares only when the text moved on since it last did.
    mutable std::uint64_t comparedAt_ = 0;
    mutable bool differs_ = false;
    void take(std::u16string text, FolderSearch::TextEncoding encoding, bool bom,
              const FolderSearch::FileStamp& stamp);
};

}  // namespace Model
