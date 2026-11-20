// SessionWebTypeset.cpp — the LaTeX preview's typeset in the session's web
// page (R3's third slice, SessionWeb.h): which files are LaTeX documents,
// the address their PDF is served at, the app's typesets kept by the
// document's real path, and what the page is told of one. Nothing here
// runs tectonic: the app's preview publishes what it typeset.
#include "SessionWeb.h"

#include "SqliteRead.h"

#include <cctype>
#include <climits>
#include <cstdlib>

namespace Shared {
namespace Web {

namespace {

std::string lowerExtension(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string realOf(const std::string& path) {
    char buf[PATH_MAX];
    if (!path.empty() && ::realpath(path.c_str(), buf)) return buf;
    return "";
}

// `path` under `dir` (both spelled the same way): the rest, or false.
bool under(const std::string& dir, const std::string& path, std::string* rest) {
    if (dir.empty()) return false;
    const std::string d = dir.back() == '/' ? dir : dir + "/";
    if (path.compare(0, d.size(), d) != 0) return false;
    *rest = path.substr(d.size());
    return true;
}

}  // namespace

bool typesetSource(const std::string& path) {
    const std::string e = lowerExtension(path);
    return e == "tex" || e == "ltx" || e == "latex";
}

std::string typesetTarget(const std::string& base, const std::string& rel) {
    // fileTarget's encoding, under typeset/ instead of file/.
    const std::string file = fileTarget(base, rel);
    return base + "typeset/" + file.substr(base.size() + 5);
}

bool relativeInFolder(const std::string& folder, const std::string& path, std::string* rel) {
    if (path.empty() || path[0] != '/') {
        *rel = path;
        return true;
    }
    if (under(folder, path, rel)) return true;
    const std::string realFolder = realOf(folder), realPath = realOf(path);
    return !realFolder.empty() && !realPath.empty() && under(realFolder, realPath, rel);
}

std::string Typesets::key(const std::string& path) {
    const std::string r = realOf(path);
    return r.empty() ? path : r;
}

Typesets::Kept& Typesets::at(const std::string& key) {
    auto it = kept_.find(key);
    if (it != kept_.end()) return it->second;
    if (kept_.size() >= kMaxEntries) {
        auto oldest = kept_.begin();
        for (auto o = kept_.begin(); o != kept_.end(); ++o)
            if (o->second.changed < oldest->second.changed) oldest = o;
        kept_.erase(oldest);
    }
    return kept_[key];
}

bool Typesets::working(const std::string& path) {
    const std::string k = key(path);
    std::lock_guard<std::mutex> lock(mutex_);
    Kept& e = at(k);
    if (e.entry.state == State::Working) return false;
    e.entry.state = State::Working;
    e.entry.version++;
    e.changed = ++changes_;
    return true;
}

bool Typesets::ready(const std::string& path, std::shared_ptr<const std::string> pdf) {
    if (!pdf || pdf->size() > kMaxPdfBytes)
        return failed(path, pdf ? "The typeset PDF is over " + std::to_string(kMaxPdfBytes >> 20) +
                                      " MB, too large to show on this page."
                                : "The typeset made no PDF.");
    const std::string k = key(path);
    std::lock_guard<std::mutex> lock(mutex_);
    Kept& e = at(k);
    const bool same = e.entry.pdf && *e.entry.pdf == *pdf;
    if (same && e.entry.state == State::Ready) return false;
    if (!same) {
        e.entry.pdf = std::move(pdf);
        e.entry.pdfVersion = ++pdfVersions_;
    }
    e.entry.state = State::Ready;
    e.entry.error.clear();
    e.entry.version++;
    e.changed = ++changes_;
    return true;
}

bool Typesets::failed(const std::string& path, const std::string& error) {
    std::string text = SqliteRead::validUtf8(SqliteRead::cutUtf8(error, kMaxErrorBytes));
    if (text.empty()) text = "The document did not typeset.";
    const std::string k = key(path);
    std::lock_guard<std::mutex> lock(mutex_);
    Kept& e = at(k);
    if (e.entry.state == State::Failed && e.entry.error == text) return false;
    e.entry.state = State::Failed;
    e.entry.error = std::move(text);
    e.entry.version++;
    e.changed = ++changes_;
    return true;
}

Typesets::Entry Typesets::get(const std::string& path) const {
    const std::string k = key(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = kept_.find(k);
    return it == kept_.end() ? Entry() : it->second.entry;
}

std::uint64_t Typesets::changes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return changes_;
}

std::size_t Typesets::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kept_.size();
}

Json typesetJson(const std::string& asked, const std::string& rel, const Typesets::Entry& e,
                 const std::string& base, bool canTypeset) {
    Json j = Json::object();
    j.set("t", Json("latex"));
    j.set("path", Json(asked));
    j.set("rel", Json(rel));
    j.set("version", Json((int64_t)e.version));
    const char* state = "none";
    switch (e.state) {
    case Typesets::State::Ready: state = "ready"; break;
    case Typesets::State::Working: state = "working"; break;
    case Typesets::State::Failed: state = "failed"; break;
    case Typesets::State::None: state = canTypeset ? "working" : "none"; break;
    }
    j.set("state", Json(state));
    if (e.pdf && !rel.empty())
        j.set("url", Json(typesetTarget(base, rel) + "?v=" + std::to_string(e.pdfVersion)));
    if (e.state == Typesets::State::Failed) {
        j.set("text", Json(e.error));
    } else if (e.state == Typesets::State::None && !canTypeset) {
        j.set("text", Json("This host does not typeset LaTeX. The mutible window's own page "
                           "(View > Show Session in Browser…) shows the pages its LaTeX preview "
                           "typesets; Show Source shows the text."));
    }
    return j;
}

}  // namespace Web
}  // namespace Shared
