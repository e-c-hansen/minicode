// Environment.mm — see Environment.h.
#import "Environment.h"
#include "ProcessEnv.h"
#include <os/lock.h>

static os_unfair_lock gDictLock = OS_UNFAIR_LOCK_INIT;
static NSDictionary<NSString *, NSString *> *gDict;
static unsigned long gDictGeneration;

NSDictionary<NSString *, NSString *> *MCEnvironment(void) {
    std::shared_ptr<const ProcessEnv::Snapshot> snap = ProcessEnv::current();
    os_unfair_lock_lock(&gDictLock);
    NSDictionary *cached = gDict && gDictGeneration == snap->generation ? gDict : nil;
    os_unfair_lock_unlock(&gDictLock);
    if (cached) return cached;
    NSMutableDictionary<NSString *, NSString *> *d =
        [NSMutableDictionary dictionaryWithCapacity:snap->entries.size()];
    for (const auto &kv : snap->pairs()) {
        NSString *k = [[NSString alloc] initWithBytes:kv.first.data() length:kv.first.size()
                                             encoding:NSUTF8StringEncoding];
        NSString *v = [[NSString alloc] initWithBytes:kv.second.data() length:kv.second.size()
                                             encoding:NSUTF8StringEncoding];
        if (k && v) d[k] = v;
    }
    NSDictionary *made = [d copy];
    os_unfair_lock_lock(&gDictLock);
    if (!gDict || gDictGeneration < snap->generation) {
        gDict = made;
        gDictGeneration = snap->generation;
    }
    os_unfair_lock_unlock(&gDictLock);
    return made;
}

NSString *MCEnv(NSString *name) {
    std::optional<std::string> v = ProcessEnv::current()->get(name.UTF8String ?: "");
    if (!v) return nil;
    return [[NSString alloc] initWithBytes:v->data() length:v->size()
                                  encoding:NSUTF8StringEncoding];
}

void MCSetEnv(const char *name, const char *value) {
    NSCAssert(NSThread.isMainThread, @"MCSetEnv off the main thread");
    ProcessEnv::set(name, value);
}
