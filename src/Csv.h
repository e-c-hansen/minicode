// Csv.h — pure C++. A CSV or TSV file as a table that can be edited and
// written back without disturbing what was not edited.
//
// The parser is RFC 4180's, leniently: a field that starts with a quote
// runs to the closing quote (two quotes inside are one), anything else runs
// to the delimiter or the line's end, and a row ends at LF, CRLF or a lone
// CR. Each row keeps its exact bytes and its own line ending, and each field
// its exact bytes and whether it was quoted. Writing the table back copies
// every row nobody touched byte for byte, and in a touched row every field
// nobody touched, so an unedited file comes back byte-identical and one
// cell edit changes one line. An edited field keeps its quoting (quoted
// stays quoted), and gains quotes only when its new text needs them; new
// fields follow the file (quoted when every field in it is). A UTF-8 byte
// order mark is kept and not shown. A touched row left with one empty
// field is written as "", since a blank line is a row most readers skip.
//
// Rows and columns count from 0. Rows may be ragged: cell() past a row's
// last field is "", and setting it adds the fields in between.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Csv {

// The delimiter a file most likely uses: of comma, semicolon, tab and
// pipe, the one that splits its first lines (outside quotes) into the most
// fields, the same number on each line counting most. Comma when none does.
char detectDelimiter(const std::string &text);

class Document {
public:
    Document() = default;
    // Reads `text` with `delimiter`. With `maxRows`, stops after that many
    // rows (truncated() says so). `complete` false means the text is only
    // the start of a file: a last row with no line ending may be cut short,
    // so it is dropped and truncated() is set.
    static Document parse(const std::string &text, char delimiter,
                          size_t maxRows = SIZE_MAX, bool complete = true);

    char delimiter() const { return delim_; }
    size_t rows() const { return rows_.size(); }
    size_t columns() const;                 // the most fields in any row
    size_t fieldsInRow(size_t r) const { return r < rows_.size() ? rows_[r].fields.size() : 0; }
    bool truncated() const { return truncated_; }
    // The line ending new rows get: the one most rows have ("\n" if none).
    const std::string &lineEnding() const { return ending_; }

    std::string cell(size_t r, size_t c) const;   // decoded: no quotes, "" for ""
    bool quoted(size_t r, size_t c) const;        // as it is (or will be) written

    // Edits. Each returns false when it changed nothing (a cell set to what
    // it holds, a row or column out of range).
    bool setCell(size_t r, size_t c, const std::string &value);
    // A new empty row before row `at` (rows() appends), as wide as the table.
    bool insertRow(size_t at);
    bool deleteRow(size_t at);
    // A new empty column before column `at` (columns() appends) in every
    // row that reaches it; blank lines stay blank.
    bool insertColumn(size_t at);
    bool deleteColumn(size_t at);

    bool edited() const { return edited_; }
    std::string serialize() const;

    // How a value is written in a field: quoted if `quote` or if it needs
    // to be (it holds the delimiter, a quote or a line break), quotes inside
    // doubled.
    static std::string encode(const std::string &value, char delimiter, bool quote);

private:
    struct Field {
        uint32_t start = 0, len = 0;   // its bytes in text_, quotes included
        int32_t value = -1;            // an edited field's text, in values_
        bool quoted = false;
    };
    struct Row {
        uint32_t start = 0, len = 0;   // its bytes in text_, without the ending
        std::vector<Field> fields;
        std::string ending;            // "\n", "\r\n", "\r", or "" at the end
        bool dirty = false;            // written again from its fields
    };
    std::string text_;
    std::string prefix_;               // a byte order mark, kept
    std::vector<Row> rows_;
    std::vector<std::string> values_;
    std::string ending_ = "\n";
    char delim_ = ',';
    bool truncated_ = false;
    bool allQuoted_ = false;           // every field of the file was quoted
    bool edited_ = false;
    mutable size_t columns_ = SIZE_MAX;   // cached

    Field newField(const std::string &value, bool quote);
    static bool blank(const Row &row);
    bool emptyField(const Field &f) const;
};

}  // namespace Csv
