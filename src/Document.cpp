// Document.cpp — see Document.h. One open file's text with the rules for
// when it is dirty, when its file changed, and how it is read and written.
#include "Document.h"

#include "LargeFile.h"

namespace Model {

bool stampChanged(const FolderSearch::FileStamp& known, const FolderSearch::FileStamp& now) {
    if (!known.exists || !now.exists) return known.exists != now.exists;
    return known.mtimeSec != now.mtimeSec || known.mtimeNsec != now.mtimeNsec ||
           known.inode != now.inode || known.size != now.size;
}

bool decodeFileText(std::string bytes, std::u16string* text, FolderSearch::TextEncoding* encoding,
                    bool* bom) {
    FolderSearch::TextEncoding e = FolderSearch::TextEncoding::Utf8;
    bool b = false;
    if (!FolderSearch::decodeFileText(&bytes, &e, &b)) return false;
    if (e == FolderSearch::TextEncoding::Utf8 && !FolderSearch::isValidUtf8(bytes)) return false;
    *text = utf16FromUtf8(bytes);
    *encoding = e;
    *bom = b;
    return true;
}

Document::Document(const std::u16string& text) : text_(text), saved_(text) {
    comparedAt_ = text_.version();
    large_ = LargeFile::isLargeText(text.data(), text.size());
}

void Document::take(std::u16string text, FolderSearch::TextEncoding encoding, bool bom,
                    const FolderSearch::FileStamp& stamp) {
    encoding_ = encoding;
    bom_ = bom;
    stamp_ = stamp;
    gone_ = false;
    large_ = LargeFile::isLargeText(text.data(), text.size());
    saved_ = std::move(text);
    differs_ = false;
}

bool Document::load(std::string bytes, const FolderSearch::FileStamp& stamp) {
    std::u16string text;
    FolderSearch::TextEncoding e;
    bool b = false;
    if (!decodeFileText(std::move(bytes), &text, &e, &b)) return false;
    // One change of the text, so its version moves on (never back).
    if (!(text_ == text)) text_.replace(0, text_.size(), text);
    take(std::move(text), e, b, stamp);
    comparedAt_ = text_.version();
    return true;
}

bool Document::reload(std::string bytes, const FolderSearch::FileStamp& stamp) {
    return load(std::move(bytes), stamp);
}

bool Document::apply(const std::vector<Text::Edit>& edits, Unit u) { return text_.apply(edits, u); }

bool Document::apply(const std::vector<EditSpans::Span>& spans,
                     const std::vector<std::u16string>& texts, Unit u) {
    return text_.apply(spans, texts, u);
}

void Document::replace(std::size_t a, std::size_t b, const std::u16string& with) {
    text_.replace(a, b, with);
}

bool Document::dirty() const {
    if (gone_) return true;
    if (comparedAt_ != text_.version()) {
        comparedAt_ = text_.version();
        differs_ = text_.size() != saved_.size() || !(text_ == saved_);
    }
    return differs_;
}

void Document::markSaved(const FolderSearch::FileStamp& written) {
    saved_ = text_.str();
    stamp_ = written;
    gone_ = false;
    differs_ = false;
    comparedAt_ = text_.version();
}

void Document::markSavedAs(std::u16string saved) {
    saved_ = std::move(saved);
    stamp_ = FolderSearch::FileStamp();
    gone_ = false;
    differs_ = text_.size() != saved_.size() || !(text_ == saved_);
    comparedAt_ = text_.version();
}

bool Document::bytesToWrite(std::string* bytes, std::string* why) const {
    const std::u16string text = text_.str();
    if (hasLoneSurrogate(text)) {
        if (why) *why = "the text holds a character UTF-8 cannot store";
        return false;
    }
    const std::string utf8 = utf8FromUtf16(text);
    std::string unheld;
    if (!FolderSearch::canEncode(utf8, encoding_, &unheld)) {
        if (why)
            *why = std::string("the text holds \u201C") + unheld + "\u201D, which " +
                   FolderSearch::encodingLabel(encoding_, bom_) + " cannot store";
        return false;
    }
    *bytes = FolderSearch::encodeFileText(utf8, encoding_, bom_);
    return true;
}

}  // namespace Model
