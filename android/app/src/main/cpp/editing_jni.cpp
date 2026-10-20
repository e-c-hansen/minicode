// editing_jni.cpp — the indentation keys (src/AutoIndent.h) and line sorting
// (src/KeepSorted.h) for Editing.kt. Text and offsets are UTF-16, as the
// Editable holds them, so nothing is converted on the way.
#include <jni.h>

#include <algorithm>
#include <string>

#include "AutoIndent.h"
#include "KeepSorted.h"
#include "jni_strings.h"

namespace {

std::u16string Utf16(JNIEnv *env, jstring s) {
    if (!s) return {};
    const jsize count = env->GetStringLength(s);
    std::u16string out(static_cast<size_t>(count), u'\0');
    if (count > 0) env->GetStringRegion(s, 0, count, reinterpret_cast<jchar *>(&out[0]));
    return out;
}

jstring Java16(JNIEnv *env, const std::u16string &s) {
    return env->NewString(reinterpret_cast<const jchar *>(s.data()), static_cast<jsize>(s.size()));
}

// {int[]{replaceStart, replaceLength, selStart, selEnd}, the replacement},
// the shape Core.TextEdit reads, or null when nothing changes.
jobjectArray EditArray(JNIEnv *env, bool changed, size_t start, size_t length,
                       const std::u16string &replacement, size_t selStart, size_t selEnd) {
    if (!changed) return nullptr;
    jobjectArray out = env->NewObjectArray(2, env->FindClass("java/lang/Object"), nullptr);
    const jint n[4] = {static_cast<jint>(start), static_cast<jint>(length),
                       static_cast<jint>(selStart), static_cast<jint>(selEnd)};
    jintArray ints = env->NewIntArray(4);
    env->SetIntArrayRegion(ints, 0, 4, n);
    env->SetObjectArrayElement(out, 0, ints);
    env->DeleteLocalRef(ints);
    jstring rep = Java16(env, replacement);
    env->SetObjectArrayElement(out, 1, rep);
    env->DeleteLocalRef(rep);
    return out;
}

jobjectArray IndentEdit(JNIEnv *env, const AutoIndent::Edit &e) {
    return EditArray(env, e.changed, e.replaceStart, e.replaceLength, e.replacement, e.selStart,
                     e.selEnd);
}

// The selection as an ordered pair inside the text, or false when it is not.
bool Range(const std::u16string &t, jint selStart, jint selEnd, size_t &a, size_t &b) {
    a = static_cast<size_t>(std::max(0, std::min(selStart, selEnd)));
    b = static_cast<size_t>(std::max(0, std::max(selStart, selEnd)));
    return b <= t.size();
}

}  // namespace

extern "C" {

/** Return over [selStart, selEnd): the edit, or null for a plain newline. */
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_Editing_nativeNewline(JNIEnv *env, jclass, jstring text, jint selStart,
                                               jint selEnd, jstring filename) {
    const std::u16string t = Utf16(env, text);
    size_t a, b;
    if (!Range(t, selStart, selEnd, a, b)) return nullptr;
    return IndentEdit(env, AutoIndent::newline(t, a, b, jnistr::FromJava(env, filename)));
}

/** After a character was typed, the caret just past it: a line moved back, or null. */
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_Editing_nativeTyped(JNIEnv *env, jclass, jstring text, jint caret,
                                             jstring filename) {
    const std::u16string t = Utf16(env, text);
    if (caret < 0 || static_cast<size_t>(caret) > t.size()) return nullptr;
    return IndentEdit(env, AutoIndent::typed(t, static_cast<size_t>(caret),
                                             jnistr::FromJava(env, filename)));
}

/** 0 Tab, 1 Shift+Tab, 2 Backspace: the edit, or null for the key's usual work. */
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_Editing_nativeKey(JNIEnv *env, jclass, jint which, jstring text,
                                           jint selStart, jint selEnd, jstring filename) {
    const std::u16string t = Utf16(env, text);
    size_t a, b;
    if (!Range(t, selStart, selEnd, a, b)) return nullptr;
    const std::string name = jnistr::FromJava(env, filename);
    switch (which) {
        case 0: return IndentEdit(env, AutoIndent::tab(t, a, b, name));
        case 1: return IndentEdit(env, AutoIndent::backtab(t, a, b, name));
        case 2: return IndentEdit(env, AutoIndent::backspace(t, a, b, name));
        default: return nullptr;
    }
}

/**
 * Sort Lines (all false) or every keep-sorted block (all true):
 *   [0] the edit, as nativeNewline gives it, or null
 *   [1] String  what to tell the user, or ""
 *   [2] int[]   {1 when something was left alone, else 0}
 */
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_Editing_nativeSort(JNIEnv *env, jclass, jstring text, jint selStart,
                                            jint selEnd, jboolean all) {
    const std::u16string t = Utf16(env, text);
    size_t a, b;
    if (!Range(t, selStart, selEnd, a, b)) return nullptr;
    const KeepSorted::Edit e = all ? KeepSorted::fixCommand(t, a, b)
                                   : KeepSorted::sortCommand(t, a, b);
    jobjectArray out = env->NewObjectArray(3, env->FindClass("java/lang/Object"), nullptr);
    jobjectArray edit = EditArray(env, e.changed, e.replaceStart, e.replaceLength, e.replacement,
                                  e.selStart, e.selEnd);
    env->SetObjectArrayElement(out, 0, edit);
    if (edit) env->DeleteLocalRef(edit);
    const std::string message = !e.message.empty() ? e.message
                                : e.changed        ? ""
                                                   : "Already sorted";
    jstring m = jnistr::ToJava(env, message);
    env->SetObjectArrayElement(out, 1, m);
    env->DeleteLocalRef(m);
    jintArray flags = env->NewIntArray(1);
    const jint error = e.error ? 1 : 0;
    env->SetIntArrayRegion(flags, 0, 1, &error);
    env->SetObjectArrayElement(out, 2, flags);
    env->DeleteLocalRef(flags);
    return out;
}

}  // extern "C"
