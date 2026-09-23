// lsp_jni.cpp — the shared LSP client (../../../src/LspClient.cpp) for the
// Android editor.
//
// The client does no I/O, and neither does this file: Kotlin owns the socket
// to the server running in Termux, hands every byte it reads to receive(),
// and writes whatever takeOutgoing() returns. What comes back the other way
// (diagnostics, a completion list, hover text, a definition) is queued here as
// a small JSON event and collected with nextEvent(), so no Java callback is
// ever made from native code and the reader thread and the UI thread only
// meet at one lock.
//
// Positions are LSP's: zero-based lines and UTF-16 units, which is how a Java
// string counts too, so offsets map across without conversion of units.
#include <jni.h>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "LspClient.h"
#include "jni_strings.h"

namespace {

struct Server {
    std::mutex lock;
    std::string outgoing;               // framed bytes for the server
    std::deque<std::string> events;     // JSON, oldest first
    std::unique_ptr<Lsp::Client> client;
    // The last completion list, which filter() narrows as the user types.
    int completionId = 0;
    std::vector<Lsp::CompletionItem> items;
};

Server *Get(jlong h) { return reinterpret_cast<Server *>(h); }

std::u16string Units(JNIEnv *env, jstring s) {
    if (!s) return {};
    const jsize n = env->GetStringLength(s);
    const jchar *u = env->GetStringChars(s, nullptr);
    std::u16string out(reinterpret_cast<const char16_t *>(u), static_cast<size_t>(n));
    env->ReleaseStringChars(s, u);
    return out;
}

jobjectArray Strings(JNIEnv *env, const std::vector<std::string> &v) {
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(v.size()),
                                           env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < v.size(); i++) {
        jstring s = jnistr::ToJava(env, v[i]);
        env->SetObjectArrayElement(out, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    return out;
}

Json RangeJson(const Lsp::Range &r) {
    Json a = Json::array();
    a.push(r.start.line).push(r.start.character);
    a.push(r.end.line).push(r.end.character);
    return a;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_org_minicode_editor_LspNative_create(JNIEnv *, jclass) {
    auto *s = new Server;
    // Every callback below runs inside a call that already holds s->lock.
    s->client = std::make_unique<Lsp::Client>(
        [s](const std::string &framed) { s->outgoing += framed; });
    Lsp::Client &c = *s->client;
    c.onReady = [s] {
        Json triggers = Json::array();
        for (const std::string &t : s->client->completionTriggers()) triggers.push(t);
        s->events.push_back(Json::object({{"type", "ready"},
                                          {"name", s->client->serverName()},
                                          {"triggers", triggers}}).dump());
    };
    c.onDiagnostics = [s](const std::string &uri, const std::vector<Lsp::Diagnostic> &list) {
        Json items = Json::array();
        for (const Lsp::Diagnostic &d : list)
            items.push(Json::object({{"range", RangeJson(d.range)},
                                     {"severity", static_cast<int>(d.severity)},
                                     {"message", d.message}}));
        s->events.push_back(Json::object({{"type", "diagnostics"}, {"uri", uri},
                                          {"items", items}}).dump());
    };
    c.onShowMessage = [s](int type, const std::string &message) {
        s->events.push_back(Json::object({{"type", "message"}, {"level", type},
                                          {"text", message}}).dump());
    };
    c.onProtocolError = [s](const std::string &problem) {
        s->events.push_back(Json::object({{"type", "error"}, {"text", problem}}).dump());
    };
    return reinterpret_cast<jlong>(s);
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_destroy(JNIEnv *, jclass, jlong h) {
    delete Get(h);
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_initialize(JNIEnv *env, jclass, jlong h,
                                              jstring root, jint pid) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    s->client->initialize(jnistr::FromJava(env, root), pid, "MiniCode");
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_didOpen(JNIEnv *env, jclass, jlong h, jstring uri,
                                           jstring languageId, jstring text) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    s->client->didOpen(jnistr::FromJava(env, uri), jnistr::FromJava(env, languageId),
                       jnistr::FromJava(env, text));
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_didChange(JNIEnv *env, jclass, jlong h, jstring uri,
                                             jstring text) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    s->client->didChange(jnistr::FromJava(env, uri), jnistr::FromJava(env, text));
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_didSave(JNIEnv *env, jclass, jlong h, jstring uri,
                                           jstring text) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    s->client->didSave(jnistr::FromJava(env, uri), jnistr::FromJava(env, text));
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_didClose(JNIEnv *env, jclass, jlong h, jstring uri) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    s->client->didClose(jnistr::FromJava(env, uri));
}

/** Asks for completion; the answer arrives as a "completion" event. */
JNIEXPORT jint JNICALL
Java_org_minicode_editor_LspNative_completion(JNIEnv *env, jclass, jlong h, jstring uri,
                                              jint line, jint character, jstring trigger) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    auto id = std::make_shared<int>(0);
    *id = s->client->completion(
        jnistr::FromJava(env, uri), Lsp::Position{line, character},
        [s, id](const Json &result, const Json &) {
            // Runs later, from receive(), with the lock held there. An
            // answer to an older request is dropped.
            if (*id != s->completionId) return;
            s->items = Lsp::parseCompletion(result);
            s->events.push_back(Json::object({{"type", "completion"}, {"id", *id},
                                              {"count", s->items.size()}}).dump());
        },
        jnistr::FromJava(env, trigger));
    s->completionId = *id;
    s->items.clear();
    return *id;
}

/**
 * The last completion list narrowed to `prefix` by the core's matcher, as a
 * JSON array of {label, detail, insert, edit}: `edit` is the start of the
 * server's replace range ([line, character]) or null.
 */
JNIEXPORT jstring JNICALL
Java_org_minicode_editor_LspNative_filter(JNIEnv *env, jclass, jlong h, jstring prefix) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    const std::vector<Lsp::CompletionItem> shown =
        Lsp::filterCompletions(s->items, jnistr::FromJava(env, prefix));
    Json out = Json::array();
    const size_t limit = shown.size() < 200 ? shown.size() : 200;
    for (size_t i = 0; i < limit; i++) {
        const Lsp::CompletionItem &it = shown[i];
        Json edit;
        if (it.hasEdit) {
            edit = Json::array();
            edit.push(it.editRange.start.line).push(it.editRange.start.character);
        }
        out.push(Json::object({{"label", it.label}, {"detail", it.detail},
                               {"insert", it.textToInsert()}, {"edit", edit}}));
    }
    return jnistr::ToJava(env, out.dump());
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_LspNative_hover(JNIEnv *env, jclass, jlong h, jstring uri,
                                         jint line, jint character) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    auto id = std::make_shared<int>(0);
    *id = s->client->hover(jnistr::FromJava(env, uri), Lsp::Position{line, character},
        [s, id](const Json &result, const Json &) {
            s->events.push_back(Json::object({{"type", "hover"}, {"id", *id},
                                              {"text", Lsp::parseHover(result)}}).dump());
        });
    return *id;
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_LspNative_definition(JNIEnv *env, jclass, jlong h, jstring uri,
                                              jint line, jint character) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    auto id = std::make_shared<int>(0);
    *id = s->client->definition(jnistr::FromJava(env, uri), Lsp::Position{line, character},
        [s, id](const Json &result, const Json &) {
            Json locations = Json::array();
            for (const Lsp::Location &l : Lsp::parseLocations(result))
                locations.push(Json::object({{"path", Lsp::pathFromUri(l.uri)},
                                             {"range", RangeJson(l.range)}}));
            s->events.push_back(Json::object({{"type", "definition"}, {"id", *id},
                                              {"locations", locations}}).dump());
        });
    return *id;
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_cancel(JNIEnv *, jclass, jlong h, jint id) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    s->client->cancel(id);
}

/** shutdown, then exit once the server agrees; "exited" is queued then. */
JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_shutdown(JNIEnv *, jclass, jlong h) {
    Server *s = Get(h);
    std::lock_guard<std::mutex> g(s->lock);
    s->client->shutdown([s] {
        s->events.push_back(Json::object({{"type", "exited"}}).dump());
    });
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LspNative_receive(JNIEnv *env, jclass, jlong h,
                                           jbyteArray data, jint length) {
    Server *s = Get(h);
    std::vector<jbyte> bytes(static_cast<size_t>(length));
    env->GetByteArrayRegion(data, 0, length, bytes.data());
    std::lock_guard<std::mutex> g(s->lock);
    s->client->receive(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<size_t>(length));
}

/** Bytes waiting to go to the server, or null when there are none. */
JNIEXPORT jbyteArray JNICALL
Java_org_minicode_editor_LspNative_takeOutgoing(JNIEnv *env, jclass, jlong h) {
    Server *s = Get(h);
    std::string out;
    {
        std::lock_guard<std::mutex> g(s->lock);
        out.swap(s->outgoing);
    }
    if (out.empty()) return nullptr;
    jbyteArray a = env->NewByteArray(static_cast<jsize>(out.size()));
    env->SetByteArrayRegion(a, 0, static_cast<jsize>(out.size()),
                            reinterpret_cast<const jbyte *>(out.data()));
    return a;
}

JNIEXPORT jstring JNICALL
Java_org_minicode_editor_LspNative_nextEvent(JNIEnv *env, jclass, jlong h) {
    Server *s = Get(h);
    std::string e;
    {
        std::lock_guard<std::mutex> g(s->lock);
        if (s->events.empty()) return nullptr;
        e = std::move(s->events.front());
        s->events.pop_front();
    }
    return jnistr::ToJava(env, e);
}

// ------------------------------------------------ helpers with no server

/** {server key, languageId} for an extension, or null. */
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_LspNative_language(JNIEnv *env, jclass, jstring ext) {
    Lsp::Language lang;
    if (!Lsp::languageForExtension(jnistr::FromJava(env, ext), lang)) return nullptr;
    return Strings(env, {lang.server, lang.languageId});
}

JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_LspNative_commands(JNIEnv *env, jclass, jstring server) {
    return Strings(env, Lsp::defaultCommands(jnistr::FromJava(env, server)));
}

JNIEXPORT jstring JNICALL
Java_org_minicode_editor_LspNative_displayName(JNIEnv *env, jclass, jstring server) {
    return jnistr::ToJava(env, Lsp::serverDisplayName(jnistr::FromJava(env, server)));
}

JNIEXPORT jstring JNICALL
Java_org_minicode_editor_LspNative_uriFromPath(JNIEnv *env, jclass, jstring path) {
    return jnistr::ToJava(env, Lsp::uriFromPath(jnistr::FromJava(env, path)));
}

/**
 * Positions to offsets, many at once: `positions` is (line, character)
 * pairs, and the result has one UTF-16 offset per pair.
 */
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_LspNative_offsets(JNIEnv *env, jclass, jstring text,
                                           jintArray positions) {
    const std::u16string t = Units(env, text);
    const jsize n = env->GetArrayLength(positions);
    std::vector<jint> in(static_cast<size_t>(n));
    env->GetIntArrayRegion(positions, 0, n, in.data());
    std::vector<jint> out(static_cast<size_t>(n / 2));
    for (size_t i = 0; i < out.size(); i++)
        out[i] = static_cast<jint>(
            Lsp::offsetForPosition(t, Lsp::Position{in[2 * i], in[2 * i + 1]}));
    jintArray a = env->NewIntArray(static_cast<jsize>(out.size()));
    env->SetIntArrayRegion(a, 0, static_cast<jsize>(out.size()), out.data());
    return a;
}

/** An offset as {line, character}. */
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_LspNative_position(JNIEnv *env, jclass, jstring text, jint offset) {
    const Lsp::Position p =
        Lsp::positionForOffset(Units(env, text), static_cast<size_t>(offset));
    jint v[2] = {p.line, p.character};
    jintArray a = env->NewIntArray(2);
    env->SetIntArrayRegion(a, 0, 2, v);
    return a;
}

}  // extern "C"
