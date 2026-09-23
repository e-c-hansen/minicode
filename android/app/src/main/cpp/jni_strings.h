// jni_strings.h — moving text between Java strings and the core's UTF-8.
//
// JNI's own UTF-8 calls (GetStringUTFChars, NewStringUTF) speak *modified*
// UTF-8: a character outside the Basic Multilingual Plane, such as an emoji,
// is two 3-byte surrogates rather than one 4-byte sequence, and NUL is two
// bytes. The core expects real UTF-8, and CheckJNI (on in every debuggable
// build) aborts the app when NewStringUTF is handed a 4-byte sequence. So all
// user text crosses through UTF-16 here instead.
#pragma once
#include <jni.h>

#include <string>

namespace jnistr {

inline void AppendUtf8(std::string &out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// UTF-16 in, UTF-8 out. A lone surrogate becomes U+FFFD.
inline std::string ToUtf8(const jchar *units, size_t count) {
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; i++) {
        char32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < count &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            i++;   // a surrogate pair is two UTF-16 units, one character
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

// UTF-8 in, UTF-16 out. Malformed bytes become U+FFFD, one per byte, so
// nothing the core hands back can make a string the VM refuses.
constexpr char16_t kBad = 0xFFFD;   // U+FFFD, the replacement character

inline std::u16string ToUtf16(const std::string &s) {
    std::u16string out;
    out.reserve(s.size());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        char32_t cp;
        size_t len;
        if (b < 0x80) { cp = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
        else { out += kBad; i++; continue; }
        bool ok = i + len <= n;
        for (size_t k = 1; ok && k < len; k++) {
            const unsigned char cb = static_cast<unsigned char>(s[i + k]);
            if ((cb & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (cb & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF are malformed.
        static const char32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (ok && (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)))
            ok = false;
        if (!ok) { out += kBad; i++; continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
        i += len;
    }
    return out;
}

// A Java string as real UTF-8.
inline std::string FromJava(JNIEnv *env, jstring s) {
    if (!s) return {};
    const jsize count = env->GetStringLength(s);
    const jchar *units = env->GetStringChars(s, nullptr);
    if (!units) return {};
    std::string out = ToUtf8(units, static_cast<size_t>(count));
    env->ReleaseStringChars(s, units);
    return out;
}

// Real UTF-8 as a Java string.
inline jstring ToJava(JNIEnv *env, const std::string &s) {
    const std::u16string u = ToUtf16(s);
    return env->NewString(reinterpret_cast<const jchar *>(u.data()),
                          static_cast<jsize>(u.size()));
}

}  // namespace jnistr
