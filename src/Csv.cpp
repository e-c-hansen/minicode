// Csv.cpp — see Csv.h.
#include "Csv.h"

#include <algorithm>

namespace Csv {

namespace {

bool needsQuotes(const std::string &v, char delim) {
    for (char ch : v)
        if (ch == delim || ch == '"' || ch == '\n' || ch == '\r') return true;
    return false;
}

}  // namespace

std::string Document::encode(const std::string &value, char delimiter, bool quote) {
    if (!quote && !needsQuotes(value, delimiter)) return value;
    std::string out;
    out.reserve(value.size() + 2);
    out += '"';
    for (char ch : value) {
        if (ch == '"') out += '"';
        out += ch;
    }
    out += '"';
    return out;
}

Document Document::parse(const std::string &input, char delim, size_t maxRows, bool complete) {
    Document d;
    d.delim_ = delim;
    // Offsets are 32-bit; anything past 4 GB is left out (and said to be).
    if (input.size() > UINT32_MAX - 1) {
        d.text_ = input.substr(0, UINT32_MAX - 1);
        complete = false;
    } else {
        d.text_ = input;
    }
    const std::string &text = d.text_;
    const size_t n = text.size();
    size_t pos = 0;
    if (n >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        d.prefix_ = text.substr(0, 3);
        pos = 3;
    }
    size_t lf = 0, crlf = 0, cr = 0, fields = 0, quotedFields = 0;
    while (pos < n) {
        if (d.rows_.size() >= maxRows) { d.truncated_ = true; break; }
        Row row;
        row.start = (uint32_t)pos;
        for (;;) {
            Field f;
            f.start = (uint32_t)pos;
            if (pos < n && text[pos] == '"') {
                f.quoted = true;
                size_t i = pos + 1;
                while (i < n) {
                    if (text[i] == '"') {
                        if (i + 1 < n && text[i + 1] == '"') { i += 2; continue; }
                        i++;
                        break;
                    }
                    i++;
                }
                pos = i;
            }
            // Unquoted, or what follows a closing quote, up to the delimiter.
            while (pos < n && text[pos] != delim && text[pos] != '\n' && text[pos] != '\r') pos++;
            f.len = (uint32_t)(pos - f.start);
            row.fields.push_back(f);
            if (pos < n && text[pos] == delim) { pos++; continue; }
            break;
        }
        row.len = (uint32_t)(pos - row.start);
        if (pos < n) {
            if (text[pos] == '\r' && pos + 1 < n && text[pos + 1] == '\n') {
                row.ending = "\r\n";
                pos += 2;
                crlf++;
            } else if (text[pos] == '\r') {
                row.ending = "\r";
                pos++;
                cr++;
            } else {
                row.ending = "\n";
                pos++;
                lf++;
            }
        } else if (!complete) {
            d.truncated_ = true;   // the file goes on past what was read
            break;
        }
        if (row.len > 0) {
            fields += row.fields.size();
            for (const Field &f : row.fields) quotedFields += f.quoted;
        }
        d.rows_.push_back(std::move(row));
    }
    if (crlf > lf && crlf >= cr) d.ending_ = "\r\n";
    else if (cr > lf && cr > crlf) d.ending_ = "\r";
    d.allQuoted_ = fields > 0 && quotedFields == fields;
    return d;
}

char detectDelimiter(const std::string &text) {
    const std::string head = text.substr(0, 64 * 1024);
    const char candidates[] = {',', ';', '\t', '|'};
    char best = ',';
    long bestScore = 0;
    for (char c : candidates) {
        Document d = Document::parse(head, c, 20, head.size() == text.size());
        if (d.rows() == 0) continue;
        const size_t first = d.fieldsInRow(0);
        if (first < 2) continue;
        long agree = 0;
        for (size_t r = 0; r < d.rows(); r++)
            if (d.fieldsInRow(r) == first) agree++;
        const long score = agree * 1000 + (long)first;
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best;
}

size_t Document::columns() const {
    if (columns_ == SIZE_MAX) {
        size_t most = 0;
        for (const Row &r : rows_) most = std::max(most, r.fields.size());
        columns_ = most;
    }
    return columns_;
}

std::string Document::cell(size_t r, size_t c) const {
    if (r >= rows_.size() || c >= rows_[r].fields.size()) return "";
    const Field &f = rows_[r].fields[c];
    if (f.value >= 0) return values_[(size_t)f.value];
    const char *raw = text_.data() + f.start;
    const size_t len = f.len;
    if (!f.quoted) return std::string(raw, len);
    std::string out;
    out.reserve(len);
    size_t i = 1;
    while (i < len) {
        if (raw[i] == '"') {
            if (i + 1 < len && raw[i + 1] == '"') {
                out += '"';
                i += 2;
                continue;
            }
            i++;
            break;
        }
        out += raw[i++];
    }
    out.append(raw + i, len - i);   // anything after the closing quote, as is
    return out;
}

bool Document::quoted(size_t r, size_t c) const {
    if (r >= rows_.size() || c >= rows_[r].fields.size()) return false;
    return rows_[r].fields[c].quoted;
}

Document::Field Document::newField(const std::string &value, bool quote) {
    Field f;
    f.value = (int32_t)values_.size();
    values_.push_back(value);
    f.quoted = quote || needsQuotes(value, delim_);
    return f;
}

bool Document::blank(const Row &row) { return !row.dirty && row.len == 0; }

bool Document::setCell(size_t r, size_t c, const std::string &value) {
    if (r >= rows_.size()) return false;
    Row &row = rows_[r];
    if (c < row.fields.size() && cell(r, c) == value) return false;
    if (c >= row.fields.size() && value.empty()) return false;
    while (row.fields.size() <= c) row.fields.push_back(newField("", allQuoted_));
    Field &f = row.fields[c];
    const bool quote = f.quoted || needsQuotes(value, delim_);
    if (f.value >= 0) {
        values_[(size_t)f.value] = value;
    } else {
        f.value = (int32_t)values_.size();
        values_.push_back(value);
    }
    f.quoted = quote;
    row.dirty = true;
    edited_ = true;
    columns_ = SIZE_MAX;
    return true;
}

bool Document::insertRow(size_t at) {
    if (at > rows_.size()) return false;
    Row row;
    row.dirty = true;
    const size_t width = std::max<size_t>(columns(), 1);
    for (size_t i = 0; i < width; i++) row.fields.push_back(newField("", allQuoted_));
    row.ending = ending_;
    if (at == rows_.size() && !rows_.empty() && rows_.back().ending.empty()) {
        // The file ended without a line break: it still does, after the new row.
        rows_.back().ending = ending_;
        row.ending.clear();
    }
    rows_.insert(rows_.begin() + (long)at, std::move(row));
    edited_ = true;
    columns_ = SIZE_MAX;
    return true;
}

bool Document::deleteRow(size_t at) {
    if (at >= rows_.size()) return false;
    if (at + 1 == rows_.size() && at > 0 && rows_[at].ending.empty())
        rows_[at - 1].ending.clear();
    rows_.erase(rows_.begin() + (long)at);
    edited_ = true;
    columns_ = SIZE_MAX;
    return true;
}

bool Document::insertColumn(size_t at) {
    if (at > columns()) return false;
    bool changed = false;
    for (Row &row : rows_) {
        if (blank(row) || at > row.fields.size()) continue;
        row.fields.insert(row.fields.begin() + (long)at, newField("", allQuoted_));
        row.dirty = true;
        changed = true;
    }
    if (changed) {
        edited_ = true;
        columns_ = SIZE_MAX;
    }
    return changed;
}

bool Document::deleteColumn(size_t at) {
    if (at >= columns()) return false;
    bool changed = false;
    for (Row &row : rows_) {
        if (blank(row) || at >= row.fields.size()) continue;
        row.fields.erase(row.fields.begin() + (long)at);
        row.dirty = true;
        changed = true;
    }
    if (changed) {
        edited_ = true;
        columns_ = SIZE_MAX;
    }
    return changed;
}

std::string Document::serialize() const {
    std::string out;
    out.reserve(text_.size() + 64);
    out += prefix_;
    for (const Row &row : rows_) {
        if (!row.dirty) {
            out.append(text_, row.start, row.len);
        } else {
            for (size_t i = 0; i < row.fields.size(); i++) {
                if (i) out += delim_;
                const Field &f = row.fields[i];
                if (f.value >= 0) out += encode(values_[(size_t)f.value], delim_, f.quoted);
                else out.append(text_, f.start, f.len);
            }
        }
        out += row.ending;
    }
    return out;
}

}  // namespace Csv
