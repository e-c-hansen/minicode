// AllocCount.h — the test binary's own operator new and delete, which count
// the bytes the program holds (asked for, not what the allocator rounds them
// up to) and the most it held since allocMarkPeak(), so a check can bound
// what a structure keeps (host:memory) without the allocator's own
// statistics. Each block carries its size in a 16-byte header in front of
// it. The aligned forms (std::align_val_t) are the library's own, which
// pair with each other and are not counted. Included once, by
// run_tests.cpp, at file scope.
//
// Not in a sanitizer's build (kAllocCounted false, host:memory skipped):
// their runtimes define operator new and delete themselves, which Linux's
// static runtime then defines twice (CI's TSan job failed to link,
// 2026-11-21; the Mac's dynamic one let ours win).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MC_ALLOC_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(hwaddress_sanitizer)
#define MC_ALLOC_SANITIZED 1
#endif
#endif

#ifdef MC_ALLOC_SANITIZED
constexpr bool kAllocCounted = false;
static long long allocLive() { return 0; }
static long long allocPeak() { return 0; }
static void allocMarkPeak() {}
#else
constexpr bool kAllocCounted = true;

namespace AllocCount {
inline std::atomic<long long> live{0}, peak{0};
constexpr std::size_t kHeader = 16;   // keeps max_align_t's alignment

inline void* take(std::size_t n) noexcept {
    void* raw = std::malloc(n + kHeader);
    if (!raw) return nullptr;
    *static_cast<std::size_t*>(raw) = n;
    const long long now = live.fetch_add((long long)n, std::memory_order_relaxed) + (long long)n;
    long long high = peak.load(std::memory_order_relaxed);
    while (now > high && !peak.compare_exchange_weak(high, now, std::memory_order_relaxed)) {}
    return static_cast<char*>(raw) + kHeader;
}
inline void give(void* p) noexcept {
    if (!p) return;
    void* raw = static_cast<char*>(p) - kHeader;
    live.fetch_sub((long long)*static_cast<std::size_t*>(raw), std::memory_order_relaxed);
    std::free(raw);
}
inline void* takeOrThrow(std::size_t n) {
    for (;;) {
        if (void* p = take(n ? n : 1)) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}
}  // namespace AllocCount

// Bytes held now, and the most held since the last allocMarkPeak().
static long long allocLive() { return AllocCount::live.load(); }
static long long allocPeak() { return AllocCount::peak.load(); }
static void allocMarkPeak() { AllocCount::peak.store(AllocCount::live.load()); }

void* operator new(std::size_t n) { return AllocCount::takeOrThrow(n); }
void* operator new[](std::size_t n) { return AllocCount::takeOrThrow(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return AllocCount::takeOrThrow(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return AllocCount::takeOrThrow(n);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { AllocCount::give(p); }
void operator delete[](void* p) noexcept { AllocCount::give(p); }
void operator delete(void* p, std::size_t) noexcept { AllocCount::give(p); }
void operator delete[](void* p, std::size_t) noexcept { AllocCount::give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { AllocCount::give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { AllocCount::give(p); }
#endif  // MC_ALLOC_SANITIZED
