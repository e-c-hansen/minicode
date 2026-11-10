// FormatRun.cpp — see FormatRun.h. The decisions follow the Mac's
// MCFormatText (FormatRunner.mm) step for step, with its words.
#include "FormatRun.h"

#include "EditSpans.h"
#include "LspClient.h"   // Lsp::toUtf16, Lsp::toUtf8

#include <algorithm>
#include <cstdio>

namespace FormatRun {

namespace {

std::string parentOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string lastComponent(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string extensionOf(const std::string& path) {
    const std::string name = lastComponent(path);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return "";
    return name.substr(dot + 1);
}

std::string resolved(const Env& env, const std::string& p) {
    if (p.empty() || !env.realPath) return p;
    const std::string r = env.realPath(p);
    return r.empty() ? p : r;
}

// Where a plan's program may be found. `local`: the project's own copy, in
// its virtualenv for a Python tool or its node_modules/.bin for Prettier and
// Biome, "" for none; `outside`: one the user installed, from $VIRTUAL_ENV
// (activated where minicode started, so trusted as the servers trust it) or
// the usual search. A path as given (from the settings) is only ever
// `outside`. A project copy outside the window's folder is never used: no
// folder's answer covers it.
struct Lookup {
    std::string local, outside;
};

Lookup findFormatter(const std::string& program, const std::string& lang, const std::string& path,
                     const std::string& root, const Env& env) {
    Lookup found;
    if (program.find('/') != std::string::npos) {
        if (env.find) found.outside = env.find(program);
        return found;
    }
    const std::string activated =
        env.virtualEnv.empty() ? "" : join(resolved(env, env.virtualEnv), "bin");
    std::vector<std::string> dirs;
    if (lang == "python") dirs = Formatter::venvBinDirs(path, root, env.virtualEnv);
    if (Formatter::fromNodeModules(program))
        for (const std::string& d : Formatter::nodeBinDirs(path, root)) dirs.push_back(d);
    for (const std::string& dir : dirs) {
        const std::string p = join(dir, program);
        if (!env.executable || !env.executable(p)) continue;
        const bool trusted =
            !activated.empty() && (dir == activated || resolved(env, dir) == activated);
        if (trusted) {
            if (found.outside.empty()) found.outside = p;
        } else if (found.local.empty() && Formatter::insideFolder(dir, root)) {
            found.local = p;
        }
    }
    if (found.outside.empty() && env.find) found.outside = env.find(program);
    return found;
}

// The project folder a local tool comes from, as the window's folder names
// it: ".venv" for .venv/bin/black, "web/node_modules" for
// web/node_modules/.bin/prettier.
std::string toolFolderName(const std::string& program, const std::string& root) {
    std::string dir = parentOf(parentOf(program));
    const std::string prefix = root + "/";
    if (!root.empty() && dir.compare(0, prefix.size(), prefix) == 0) dir = dir.substr(prefix.size());
    return dir;
}

// A file as the window's folder names it: "web/prettier.config.js"; one
// above the folder by its whole path from "~":
// "~/Code/repo/prettier.config.js".
std::string folderRelativeName(const std::string& path, const std::string& root,
                               const std::string& home) {
    const std::string prefix = root + "/";
    if (!root.empty() && path.compare(0, prefix.size(), prefix) == 0)
        return path.substr(prefix.size());
    if (!home.empty() && home != "/") {
        if (path == home) return "~";
        const std::string h = home + "/";
        if (path.compare(0, h.size(), h) == 0) return "~/" + path.substr(h.size());
    }
    return path;
}

// The folder's own copy of a program a package runner launches by name
// (npx prettier): node_modules/.bin in the file's folder and above it, up
// to the window's folder, as the runners look; "" for none.
std::string folderNodeBin(const std::string& tool, const std::string& path,
                          const std::string& root, const Env& env) {
    if (root.empty() || tool.empty() || tool.find('/') != std::string::npos) return "";
    for (const std::string& dir : Formatter::nodeBinDirs(path, root)) {
        const std::string p = join(dir, tool);
        if (env.executable && env.executable(p) && Formatter::insideFolder(p, root)) return p;
    }
    return "";
}

// Whether `s` is UTF-8 throughout.
bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        size_t n;
        unsigned cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; k++) {
            const unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates and past U+10FFFF are not UTF-8.
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            return false;
        i += n;
    }
    return true;
}

}  // namespace

Decision decide(const std::string& path, const std::string& root, const std::string& setting,
                const std::string& consent, int firstLine, int lastLine, const Env& env) {
    Decision result;
    const std::string lang = Formatter::languageFor(path);
    const std::string ext = extensionOf(path).empty() ? lastComponent(path) : extensionOf(path);
    if (lang.empty()) {
        result.kind = Kind::Unavailable;
        result.serverMayFormat = true;
        result.message = Formatter::unplannedMessage("", ext);
        return result;
    }
    const std::string display = Formatter::displayName(lang);

    // Planned on one spelling of both (the tree may name a folder through
    // a link the file's path does not), so "inside the open folder" holds.
    const std::string realPath = resolved(env, path);
    const std::string realRoot = root.empty() ? "" : resolved(env, root);
    const Formatter::Plan plan = Formatter::plan(lang, setting, realPath, realRoot, env.fs);
    if (plan.off) {
        result.kind = Kind::Unavailable;
        result.message = "Formatting is off for " + display + " files (format." + lang +
                         " = off in the settings).";
        return result;
    }
    if (Formatter::serverMayFormat(lang, plan)) {
        // Markdown, JSON, YAML or CSS in a project not set up for a
        // formatter: the file's language server may format it.
        result.kind = Kind::Unavailable;
        result.serverMayFormat = true;
        result.message = Formatter::unplannedMessage(lang, ext);
        return result;
    }

    const Formatter::Candidate* chosen = nullptr;
    std::string program;      // the copy that runs
    std::string kept;         // a folder copy not allowed
    std::string keptRunner;   // the runner that would have run it
    for (const Formatter::Candidate& c : plan.candidates) {
        Lookup found = findFormatter(c.program, lang, realPath, realRoot, env);
        // A package runner (npx prettier, pnpm exec, bunx, yarn) is the
        // program it launches: the folder's own copy when the folder has
        // one, and that program's configuration counts.
        const Formatter::Launch launch = Formatter::launched(c.command);
        const std::string tool = launch.tool.empty() ? c.program : launch.tool;
        std::string launchedLocal;   // the folder's copy a runner would run
        if (!launch.tool.empty()) {
            launchedLocal = folderNodeBin(launch.tool, realPath, realRoot, env);
            if (launchedLocal.empty() && launch.folderCode && !realRoot.empty())
                launchedLocal = join(realRoot, "package.json");
        }
        // A configuration that runs code when the tool reads it
        // (prettier.config.js, a plugin), in the folder or above it, is the
        // folder's own program, whichever copy of the tool reads it.
        const std::string codeConfig =
            Formatter::codeConfigFile(tool, realPath, realRoot, plan.configFile, env.fs);
        const Formatter::Source source =
            launch.tool.empty()
                ? Formatter::chooseProgram(plan.fromSettings, found.local, found.outside, consent,
                                           !codeConfig.empty())
                : Formatter::chooseLaunched(plan.fromSettings, !launchedLocal.empty(),
                                            found.outside, consent, !codeConfig.empty());
        if (!launchedLocal.empty()) {
            // The runner stands in for the folder's copy.
            found.local = found.outside;
            found.outside.clear();
        }
        if (source == Formatter::Source::Ask) {
            // Nothing runs until the user answers for the folder; the
            // editor asks and formats again.
            result.kind = Kind::NeedsConsent;
            result.tool = tool;
            if (!launchedLocal.empty())
                result.consentFolder = lastComponent(launchedLocal) == "package.json"
                                           ? "package.json"
                                           : toolFolderName(launchedLocal, realRoot);
            else if (!found.local.empty())
                result.consentFolder = toolFolderName(found.local, realRoot);
            if (!codeConfig.empty())
                result.consentConfig = folderRelativeName(codeConfig, realRoot, env.home);
            return result;
        }
        if (source == Formatter::Source::Refused) {
            result.kind = Kind::Failed;
            result.tool = tool;
            result.message = folderRelativeName(codeConfig, realRoot, env.home) +
                             " can run code when " + tool + " reads it, and this folder's own "
                             "programs are not allowed to run, so " + tool +
                             " did not run. Ask Again About This Folder's Tools (" +
                             env.askAgainMenu + " menu) asks again.";
            return result;
        }
        if (source == Formatter::Source::Local) program = found.local;
        if (source == Formatter::Source::Outside) program = found.outside;
        if (!program.empty()) {
            chosen = &c;
            break;
        }
        if (!found.local.empty() && kept.empty()) {
            kept = launchedLocal.empty() ? found.local : launchedLocal;
            if (!launchedLocal.empty()) keptRunner = Formatter::programName(c.program);
        }
    }
    const std::string configName = lastComponent(plan.configFile);
    if (!chosen && !kept.empty() && !keptRunner.empty()) {
        // A runner that would run the folder's own copy (or script).
        result.kind = Kind::Failed;
        result.message = "format." + lang + " runs " + keptRunner +
                         ", which would run this folder's own " +
                         (lastComponent(kept) == "package.json" ? std::string("package.json scripts")
                                                                : toolFolderName(kept, realRoot)) +
                         ", and this folder's own programs are not allowed to run.";
        return result;
    }
    if (!chosen && !kept.empty()) {
        // The folder has the tool, and the user said no to the folder's
        // own programs.
        const std::string tool = lastComponent(kept);
        result.kind = Kind::Failed;
        if (!plan.installHint.empty() || !plan.fromSettings) result.installPrograms = {tool};
        result.message = tool + " is installed only in this folder's " +
                         toolFolderName(kept, realRoot) +
                         ", and this folder's own programs are not allowed to run. Install " +
                         tool + " yourself to use it here.";
        return result;
    }
    if (!chosen) {
        result.kind = Kind::Failed;
        std::vector<std::string> programs;
        for (const Formatter::Candidate& c : plan.candidates)
            programs.push_back(Formatter::programName(c.program));
        if (!plan.installHint.empty()) result.installPrograms = programs;
        if (plan.fromSettings) {
            result.message = "format." + lang + " in the settings runs " +
                             plan.candidates.front().program + ", which was not found.";
        } else if (!plan.configFile.empty()) {
            result.message = "This project configures " + plan.candidates.front().program +
                             " (in " + configName + "), which is not installed.";
        } else {
            std::string names;
            for (const Formatter::Candidate& c : plan.candidates)
                names += (names.empty() ? "" : " and ") + c.program;
            result.message =
                "No formatter for " + display + " files was found (looked for " + names + ").";
        }
        return result;
    }

    result.kind = Kind::Run;
    result.tool = Formatter::programName(chosen->program);
    result.style = chosen->projectStyle ? "the project's " + configName
                   : plan.fromSettings  ? "format." + lang + " in the settings"
                                        : "Google style";
    bool ranged = false;
    const std::vector<std::string> words = Formatter::arguments(
        chosen->command, path, firstLine, lastLine, &ranged, plan.configFile);
    result.ranged = ranged;
    result.argv.push_back(program);
    for (size_t i = 1; i < words.size(); i++) result.argv.push_back(words[i]);
    result.cwd = parentOf(path);
    return result;
}

Question consentQuestion(const Decision& d) {
    Question q;
    const std::string keep = "The answer is kept for this folder, and also covers its Python "
                             "environment for the language servers.";
    // A configuration above the folder (the repository's, with only its
    // docs folder open) comes by its whole path.
    const bool above = !d.consentConfig.empty() &&
                       (d.consentConfig[0] == '/' || d.consentConfig[0] == '~');
    if (above)
        q.message = "Let " + d.tool + " run the code in " + d.consentConfig + ", above this folder?";
    else if (!d.consentConfig.empty() && !d.consentFolder.empty())
        q.message = "Run " + d.tool + " from this folder's " + d.consentFolder + ", with its " +
                    d.consentConfig + "?";
    else if (!d.consentConfig.empty())
        q.message = "Let " + d.tool + " run the code in this folder's " + d.consentConfig + "?";
    else
        q.message = "Run " + d.tool + " from this folder's " + d.consentFolder + "?";
    q.detail = !d.consentConfig.empty()
                   ? d.consentConfig + " can run code when " + d.tool + " reads it, with your "
                     "rights, so minicode asks first. With Don't Allow, " + d.tool +
                     " does not run for this folder. " + keep
                   : "Programs in the folder run with your rights, so minicode asks before "
                     "running one. " + keep;
    return q;
}

Outcome outcome(const std::string& tool, const std::string& path, bool timedOut,
                double timeoutSeconds, const std::string& launchError, int status,
                const std::string& out, const std::string& err, std::size_t inputBytes) {
    Outcome o;
    if (timedOut) {
        char secs[32];
        std::snprintf(secs, sizeof secs, "%.0f", timeoutSeconds);
        o.message = tool + " took longer than " + secs +
                    " seconds and was stopped. The file was not changed.";
    } else if (!launchError.empty()) {
        o.message = tool + " " + launchError + ".";
    } else if (status != 0) {
        const std::string summary = Formatter::errorSummary(err, out, path);
        o.message = !summary.empty()
                        ? tool + " could not format " + lastComponent(path) + ":\n" + summary
                        : tool + " exited with status " + std::to_string(status) + ".";
    } else if (!validUtf8(out)) {
        o.message = tool + " wrote something that is not UTF-8 text. The file was not changed.";
    } else if (out.empty() && inputBytes > 0) {
        o.message = tool + " returned no text. The file was not changed.";
    } else {
        o.ok = true;
        o.text = out;
    }
    return o;
}

std::string alreadyFormatted(const std::string& tool, const std::string& style) {
    return "Already formatted (" + tool + ", " + style + ").";
}

Replacement::Replacement(const std::string& before, const std::string& after)
    : from_(Lsp::toUtf16(before)), to_(Lsp::toUtf16(after)) {
    hunks_ = LineDiff::diff(from_, to_);
    if (hunks_.empty()) return;
    // Every hunk's two ends in the old text's units, as code points in one
    // pass (EditSpans::codePoints wants them ascending).
    std::vector<std::size_t> units;
    units.reserve(hunks_.size() * 2);
    for (const LineDiff::Hunk& h : hunks_) {
        units.push_back(h.oldStart);
        units.push_back(h.oldStart + h.oldLength);
    }
    const std::vector<std::size_t> points = EditSpans::codePoints(from_, units);
    changes_.reserve(hunks_.size());
    for (size_t i = 0; i < hunks_.size(); i++) {
        const LineDiff::Hunk& h = hunks_[i];
        Change c;
        c.start = points[2 * i];
        c.length = points[2 * i + 1] - points[2 * i];
        c.text = Lsp::toUtf8(to_.substr(h.newStart, h.newLength));
        changes_.push_back(std::move(c));
    }
}

std::size_t Replacement::map(std::size_t point) const {
    const std::size_t unit = EditSpans::unitsAt(from_, point);
    const std::size_t moved = LineDiff::mapOffset(from_, to_, hunks_, unit);
    return EditSpans::codePoints(to_, {moved})[0];
}

void selectedLines(const std::string& text, std::size_t start, std::size_t end, int* firstLine,
                   int* lastLine) {
    *firstLine = 0;
    *lastLine = 0;
    if (end <= start) return;
    // Code points to bytes: the character before `end`, and the bytes up to it.
    std::vector<size_t> byteOf;   // byte offset of each code point up to end
    byteOf.reserve(end + 1);
    size_t b = 0, cp = 0;
    while (cp <= end && b <= text.size()) {
        byteOf.push_back(b);
        if (b == text.size()) break;
        const unsigned char c = (unsigned char)text[b];
        b += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (b > text.size()) b = text.size();
        cp++;
    }
    if (end >= byteOf.size()) end = byteOf.size() - 1;
    if (start >= end) return;
    // A selection ending just after a line break leaves the next line out.
    size_t e = end;
    if (text[byteOf[e - 1]] == '\n' && e - 1 > start) e--;
    int line = 1;
    for (size_t i = 0; i < byteOf[e]; i++) {
        if (i == byteOf[start]) *firstLine = line;
        if (text[i] == '\n') line++;
    }
    if (*firstLine == 0) *firstLine = line;
    *lastLine = line;
}

}  // namespace FormatRun
