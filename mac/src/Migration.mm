// Migration.mm — see Migration.h.
#import "Migration.h"
#import "Environment.h"
#include "Rename.h"
#include <string>
#include <vector>

void MCMigrateFromOldName(void) {
    NSString *newId = @(Rename::kNewBundleId);
    if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:newId]) return;
    NSDictionary *env = MCEnvironment();
    for (NSString *name in @[@"MUTIBLE_DEFAULTS_SUITE", @"MUTIBLE_DEMO", @"MUTIBLE_BENCH"])
        if ([env[name] length]) return;
    const std::string home = NSHomeDirectory().fileSystemRepresentation;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *marker = @(Rename::macMarker(home).c_str());
    if ([fm fileExistsAtPath:marker]) return;

    // The defaults: window frames, recent folders, zoom, the tree's columns.
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSString *oldId = @(Rename::kOldBundleId);
    NSDictionary *oldDomain = [defaults persistentDomainForName:oldId];
    NSDictionary *newDomain = [defaults persistentDomainForName:newId];
    const bool copyDefaults = Rename::shouldCopyDefaults(newDomain.count, oldDomain.count);
    if (copyDefaults) [defaults setPersistentDomain:oldDomain forName:newId];

    // The folders and files, each only when the new one is missing. On APFS
    // a copy is a clone, so even the Backups and tectonic copy at once.
    std::vector<Rename::Copy> done, failed;
    for (const Rename::Copy &c : Rename::pending(Rename::macItems(home), [&](const std::string &p) {
             return [fm fileExistsAtPath:@(p.c_str())];
         })) {
        NSString *to = @(c.to.c_str());
        NSError *err = nil;
        [fm createDirectoryAtPath:to.stringByDeletingLastPathComponent
            withIntermediateDirectories:YES attributes:nil error:nil];
        if ([fm copyItemAtPath:@(c.from.c_str()) toPath:to error:&err]) {
            done.push_back(c);
        } else {
            failed.push_back(c);
            NSLog(@"mutible: could not copy %s to %s: %@", c.from.c_str(), c.to.c_str(),
                  err.localizedDescription);
        }
    }

    NSISO8601DateFormatter *iso = [[NSISO8601DateFormatter alloc] init];
    const std::string when = [iso stringFromDate:NSDate.date].UTF8String;
    NSString *text = @(Rename::markerText(when, done, failed, copyDefaults).c_str());
    [fm createDirectoryAtPath:marker.stringByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:nil];
    [text writeToFile:marker atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
