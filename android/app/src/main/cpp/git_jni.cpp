// git_jni.cpp — the Source Control panel's model for Android.
//
// Everything that can be decided without a GUI is already written, and
// tested, for the other ports: the core's GitStatus and GitGraph read what
// git prints, and the GTK port's GitModel (../../../../../linux/src/GitModel,
// plain C++ with no GTK in it) turns that into rows, argument vectors,
// summary and error text, and styled diff text. This file puts those behind
// JNI; GitPanel.kt runs git (through Termux, see GitRunner.kt) and draws.
//
// Two handles cross: a Snapshot (one status: branch, rows) and a Graph (one
// load of the commit graph). Kotlin frees each one it is given.
//
// Argument vectors come back as byte arrays, not strings, so a path git
// printed goes back to git byte for byte whatever its encoding.
#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "../../../../../linux/src/GitModel.h"
#include "jni_strings.h"

namespace {

using GitUi::Graph;
using GitUi::Row;
using GitUi::Snapshot;

Snapshot *Snap(jlong h) { return reinterpret_cast<Snapshot *>(h); }
Graph *GraphOf(jlong h) { return reinterpret_cast<Graph *>(h); }

std::string Bytes(JNIEnv *env, jbyteArray a) {
    if (!a) return {};
    const jsize n = env->GetArrayLength(a);
    std::string out(static_cast<size_t>(n), '\0');
    if (n) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte *>(&out[0]));
    return out;
}

jbyteArray ToBytes(JNIEnv *env, const std::string &s) {
    jbyteArray a = env->NewByteArray(static_cast<jsize>(s.size()));
    if (!s.empty())
        env->SetByteArrayRegion(a, 0, static_cast<jsize>(s.size()),
                                reinterpret_cast<const jbyte *>(s.data()));
    return a;
}

jobjectArray Args(JNIEnv *env, const std::vector<std::string> &v) {
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(v.size()),
                                           env->FindClass("[B"), nullptr);
    for (size_t i = 0; i < v.size(); i++) {
        jbyteArray b = ToBytes(env, v[i]);
        env->SetObjectArrayElement(out, static_cast<jsize>(i), b);
        env->DeleteLocalRef(b);
    }
    return out;
}

jobjectArray Strings(JNIEnv *env, const std::vector<std::string> &v) {
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(v.size()),
                                           env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < v.size(); i++) {
        jstring s = jnistr::ToJava(env, GitUi::validUtf8(v[i]));
        env->SetObjectArrayElement(out, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    return out;
}

jintArray Ints(JNIEnv *env, const std::vector<jint> &v) {
    jintArray a = env->NewIntArray(static_cast<jsize>(v.size()));
    if (!v.empty()) env->SetIntArrayRegion(a, 0, static_cast<jsize>(v.size()), v.data());
    return a;
}

jobjectArray Pair(JNIEnv *env, jobject a, jobject b) {
    jobjectArray out = env->NewObjectArray(2, env->FindClass("java/lang/Object"), nullptr);
    env->SetObjectArrayElement(out, 0, a);
    env->SetObjectArrayElement(out, 1, b);
    return out;
}

std::string BaseName(const std::string &p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string DirName(const std::string &p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

const Row *RowAt(jlong h, jint i) {
    const Snapshot *s = Snap(h);
    if (!s || i < 0 || i >= static_cast<jint>(s->rows.size())) return nullptr;
    return &s->rows[static_cast<size_t>(i)];
}

}  // namespace

extern "C" {

// ------------------------------------------------------------- arguments
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_environment(JNIEnv *env, jclass) {
    std::vector<std::string> v;
    for (const GitUi::EnvVar &e : GitUi::gitEnvironment())
        v.push_back(std::string(e.name) + "=" + e.value);
    return Strings(env, v);
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_topLevelArgs(JNIEnv *env, jclass) {
    return Args(env, GitUi::topLevelArgs());
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_statusArgs(JNIEnv *env, jclass) {
    return Args(env, GitUi::statusArgs());
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_refArgs(JNIEnv *env, jclass) {
    return Args(env, GitUi::refArgs());
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_logArgs(JNIEnv *env, jclass, jint limit, jboolean all,
                                           jboolean compare) {
    return Args(env, GitUi::logArgs(limit, all, compare));
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_leftRightArgs(JNIEnv *env, jclass) {
    return Args(env, GitUi::leftRightArgs());
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_commitArgs(JNIEnv *env, jclass, jstring message) {
    return Args(env, GitUi::commitArgs(jnistr::FromJava(env, message)));
}

JNIEXPORT jstring JNICALL
Java_org_minicode_editor_GitNative_failureText(JNIEnv *env, jclass, jint status,
                                               jbyteArray out, jbyteArray err) {
    return jnistr::ToJava(env, GitUi::failureText(status, Bytes(env, out), Bytes(env, err)));
}

JNIEXPORT jstring JNICALL
Java_org_minicode_editor_GitNative_firstLine(JNIEnv *env, jclass, jbyteArray out) {
    return jnistr::ToJava(env, GitUi::firstLine(Bytes(env, out)));
}

// `rev-parse --show-toplevel --absolute-git-dir` output: {top, gitDir}, or
// null when it does not say.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_parseTopLevel(JNIEnv *env, jclass, jbyteArray out) {
    std::string top, gitDir;
    if (!GitUi::parseTopLevel(Bytes(env, out), top, gitDir)) return nullptr;
    return Strings(env, {top, gitDir});
}

// -------------------------------------------------------------- snapshot
// A snapshot outside a repository, or one whose status failed: a notice (one
// plain line where the list would be) or an error.
JNIEXPORT jlong JNICALL
Java_org_minicode_editor_GitNative_snapshotEmpty(JNIEnv *env, jclass, jstring notice,
                                                 jstring error) {
    auto *s = new Snapshot;
    s->notice = jnistr::FromJava(env, notice);
    s->errorText = jnistr::FromJava(env, error);
    return reinterpret_cast<jlong>(s);
}

// A repository at `top`: the status output parsed, or `error` when the
// status failed.
JNIEXPORT jlong JNICALL
Java_org_minicode_editor_GitNative_snapshotOf(JNIEnv *env, jclass, jstring top, jstring gitDir,
                                              jbyteArray status, jstring error) {
    auto *s = new Snapshot;
    s->inRepository = true;
    s->topLevel = jnistr::FromJava(env, top);
    s->gitDir = jnistr::FromJava(env, gitDir);
    s->errorText = jnistr::FromJava(env, error);
    if (s->errorText.empty()) GitUi::applyStatus(*s, Git::parseStatus(Bytes(env, status)));
    return reinterpret_cast<jlong>(s);
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_GitNative_snapshotFree(JNIEnv *, jclass, jlong h) {
    delete Snap(h);
}

// {branch line, notice, error, summary line, top level}.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_snapshotText(JNIEnv *env, jclass, jlong h) {
    const Snapshot &s = *Snap(h);
    const bool graph = GitUi::graphWanted(s);
    return Strings(env, {s.inRepository ? s.branchText : std::string("Source Control"),
                         s.notice, s.errorText, graph ? GitUi::summaryText(s) : std::string(),
                         s.topLevel});
}

// {in repository, no commit yet, graph wanted, compare with upstream,
//  ahead, behind}.
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_GitNative_snapshotFlags(JNIEnv *env, jclass, jlong h) {
    const Snapshot &s = *Snap(h);
    return Ints(env, {s.inRepository, s.initial, GitUi::graphWanted(s),
                      GitUi::compareWithUpstream(s), s.ahead, s.behind});
}

// The rows, for drawing: Object[2] of an int[] with (header, staged,
// untracked, unmerged, letter, letter color) per row, and a String[] with
// (title or name, folder, tooltip) per row.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_snapshotRows(JNIEnv *env, jclass, jlong h) {
    const Snapshot &s = *Snap(h);
    std::vector<jint> flags;
    std::vector<std::string> text;
    for (const Row &r : s.rows) {
        flags.insert(flags.end(), {r.header, r.staged, r.untracked, r.unmerged,
                                   static_cast<jint>(static_cast<unsigned char>(r.letter)),
                                   static_cast<jint>(GitUi::letterColor(r.letter, r.unmerged))});
        if (r.header) {
            text.insert(text.end(), {r.title, "", ""});
            continue;
        }
        std::string dir = DirName(r.path);
        if (!r.origPath.empty()) dir += (dir.empty() ? "" : "  ") + std::string("from ") + r.origPath;
        text.insert(text.end(), {BaseName(r.path), dir, GitUi::rowToolTip(r)});
    }
    return Pair(env, Ints(env, flags), Strings(env, text));
}

// For the debug log: "S M path <- old" or "# Heading" per row.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_rowDescriptions(JNIEnv *env, jclass, jlong h) {
    std::vector<std::string> out;
    for (const Row &r : Snap(h)->rows) {
        if (r.header) out.push_back("# " + r.title);
        else out.push_back(std::string(r.staged ? "S " : "W ") + r.letter + " " + r.path +
                           (r.origPath.empty() ? "" : " <- " + r.origPath));
    }
    return Strings(env, out);
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_GitNative_nextFileRow(JNIEnv *, jclass, jlong h, jint from, jint step) {
    return GitUi::nextFileRow(Snap(h)->rows, from, step);
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_GitNative_lastFileRow(JNIEnv *, jclass, jlong h) {
    return GitUi::lastFileRow(Snap(h)->rows);
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_GitNative_pickAfterRefresh(JNIEnv *, jclass, jlong old, jint sel,
                                                    jlong now) {
    if (!old) return -1;
    return GitUi::pickAfterRefresh(Snap(old)->rows, sel, Snap(now)->rows);
}

// Space on a row: stage or unstage it. Null for a heading.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_stageArgs(JNIEnv *env, jclass, jlong h, jint row) {
    const Row *r = RowAt(h, row);
    if (!r || r->header) return nullptr;
    return Args(env, GitUi::stageArgs(*r, Snap(h)->initial));
}

// Return on a row: its diff. Null for a heading.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_diffArgs(JNIEnv *env, jclass, jlong h, jint row) {
    const Row *r = RowAt(h, row);
    if (!r || r->header) return nullptr;
    return Args(env, GitUi::diffArgs(*r));
}

// ----------------------------------------------------------------- graph
// Whether `previous` was built from the same HEAD, branch, upstream, limit,
// switch and refs, so the log need not run again.
JNIEXPORT jboolean JNICALL
Java_org_minicode_editor_GitNative_graphSameKey(JNIEnv *env, jclass, jlong previous, jlong snap,
                                                jint limit, jboolean all, jbyteArray refs) {
    if (!previous) return JNI_FALSE;
    const Snapshot &s = *Snap(snap);
    const std::string key = GitUi::graphKey(s, limit, all, GitUi::compareWithUpstream(s),
                                            Bytes(env, refs));
    return GraphOf(previous)->key == key ? JNI_TRUE : JNI_FALSE;
}

// A graph from log and for-each-ref output, or holding `error` when the log
// failed.
JNIEXPORT jlong JNICALL
Java_org_minicode_editor_GitNative_graphBuild(JNIEnv *env, jclass, jlong snap, jint limit,
                                              jboolean all, jbyteArray refs, jbyteArray log,
                                              jstring error) {
    const Snapshot &s = *Snap(snap);
    const std::string refsOut = Bytes(env, refs);
    auto *g = new Graph;
    g->key = GitUi::graphKey(s, limit, all, GitUi::compareWithUpstream(s), refsOut);
    g->limit = limit;
    g->errorText = jnistr::FromJava(env, error);
    if (g->errorText.empty()) GitUi::buildGraph(*g, s, Bytes(env, log), refsOut, limit);
    return reinterpret_cast<jlong>(g);
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_GitNative_graphSetLeftRight(JNIEnv *env, jclass, jlong h,
                                                     jbyteArray out) {
    GraphOf(h)->divergence = Git::parseLeftRight(Bytes(env, out));
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_GitNative_graphFree(JNIEnv *, jclass, jlong h) {
    delete GraphOf(h);
}

JNIEXPORT jstring JNICALL
Java_org_minicode_editor_GitNative_graphError(JNIEnv *env, jclass, jlong h) {
    return jnistr::ToJava(env, GitUi::validUtf8(GraphOf(h)->errorText));
}

// The whole graph for drawing, as Object[2]. The int[] starts with
// (commits, has more, widest row), then per commit: lane, color, width,
// flags (1 outgoing, 2 incoming, 4 HEAD, 8 merge), edge count, label
// count, then (kind, from, to, color) per edge and (kind, current) per
// label; RefKind is 0 HEAD, 1 branch, 2 remote, 3 tag. The String[] holds,
// per commit, the subject and then each label's name.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_graphRows(JNIEnv *env, jclass, jlong h) {
    const Graph &g = *GraphOf(h);
    std::vector<jint> v = {static_cast<jint>(g.commits.size()), g.hasMore, g.maxWidth};
    std::vector<std::string> text;
    for (size_t i = 0; i < g.commits.size(); i++) {
        const Git::Commit &c = g.commits[i];
        const Git::GraphRow &row = g.rows[i];
        jint flags = 0;
        if (g.divergence.outgoing.count(c.hash)) flags |= 1;
        if (g.divergence.incoming.count(c.hash)) flags |= 2;
        if (c.hash == g.head) flags |= 4;
        if (c.parents.size() > 1) flags |= 8;
        auto it = g.labels.find(c.hash);
        const std::vector<Git::Ref> none;
        const std::vector<Git::Ref> &labels = it == g.labels.end() ? none : it->second;
        v.insert(v.end(), {row.lane, row.color, row.width, flags,
                           static_cast<jint>(row.edges.size()),
                           static_cast<jint>(labels.size())});
        for (const Git::GraphEdge &e : row.edges)
            v.insert(v.end(), {static_cast<jint>(e.kind), e.from, e.to, e.color});
        text.push_back(c.subject);
        for (const Git::Ref &r : labels) {
            v.insert(v.end(), {static_cast<jint>(r.kind), r.current});
            text.push_back(r.name);
        }
    }
    return Pair(env, Ints(env, v), Strings(env, text));
}

JNIEXPORT jstring JNICALL
Java_org_minicode_editor_GitNative_graphToolTip(JNIEnv *env, jclass, jlong h, jint row,
                                                jlong now) {
    if (row < 0) return jnistr::ToJava(env, "");
    return jnistr::ToJava(env, GitUi::validUtf8(GitUi::graphToolTip(*GraphOf(h),
                                                                    static_cast<size_t>(row), now)));
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_graphDescriptions(JNIEnv *env, jclass, jlong h) {
    return Strings(env, GitUi::graphDescriptions(*GraphOf(h)));
}

// {hash, title} of a commit, for showing it; null past the end.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_graphCommit(JNIEnv *env, jclass, jlong h, jint row) {
    const Graph &g = *GraphOf(h);
    if (row < 0 || row >= static_cast<jint>(g.commits.size())) return nullptr;
    const Git::Commit &c = g.commits[static_cast<size_t>(row)];
    return Strings(env, {c.hash, GitUi::commitTitle(c)});
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_showArgs(JNIEnv *env, jclass, jstring hash) {
    return Args(env, GitUi::showArgs(jnistr::FromJava(env, hash)));
}

// ------------------------------------------------------------ styled text
// A diff (or with `commit`, `git show` output) as Object[2]: the text, and
// an int[] of (start, length, style) runs in UTF-16 units, which is what a
// Java string counts. Styles are GitUi::Style's order: 0 plain, 1 added,
// 2 removed, 3 muted, 4 file header, 5 hash, 6 bold.
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_GitNative_styledText(JNIEnv *env, jclass, jbyteArray bytes,
                                              jboolean commit, jlong now) {
    const std::string in = Bytes(env, bytes);
    const GitUi::StyledText st = commit ? GitUi::commitText(in, now) : GitUi::diffText(in);

    // The runs count characters; a Java string counts UTF-16 units, and a
    // character past U+FFFF is two. Map one to the other while converting.
    const std::u16string u = jnistr::ToUtf16(st.text);
    std::vector<jint> unitAt;   // per character, its first UTF-16 unit
    unitAt.reserve(st.chars + 1);
    for (size_t i = 0; i < u.size(); i++) {
        const char16_t c = u[i];
        if (c >= 0xDC00 && c <= 0xDFFF && i > 0 && u[i - 1] >= 0xD800 && u[i - 1] <= 0xDBFF)
            continue;   // the second half of a pair
        unitAt.push_back(static_cast<jint>(i));
    }
    unitAt.push_back(static_cast<jint>(u.size()));
    auto unit = [&](size_t ch) {
        return ch < unitAt.size() ? unitAt[ch] : static_cast<jint>(u.size());
    };
    std::vector<jint> runs;
    for (const GitUi::StyledText::Run &r : st.runs) {
        const jint a = unit(r.start), b = unit(r.start + r.length);
        runs.insert(runs.end(), {a, b - a, static_cast<jint>(r.style)});
    }
    jstring text = env->NewString(reinterpret_cast<const jchar *>(u.data()),
                                  static_cast<jsize>(u.size()));
    return Pair(env, text, Ints(env, runs));
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_GitNative_laneColor(JNIEnv *, jclass, jint i) {
    return static_cast<jint>(GitUi::laneColor(i));
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_GitNative_pillColor(JNIEnv *, jclass, jint kind) {
    return static_cast<jint>(GitUi::pillColor(static_cast<Git::RefKind>(kind)));
}

JNIEXPORT jdouble JNICALL
Java_org_minicode_editor_GitNative_laneWidth(JNIEnv *, jclass, jdouble rowWidth, jint lanes) {
    return GitUi::laneWidth(rowWidth, lanes);
}

}  // extern "C"
