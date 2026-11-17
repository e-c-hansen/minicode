// save_plan_tests.inc — the `saveplan:*` group of run_tests.cpp
// (SavePlan.h), kept in its own file and included there. Each case is one of
// the Mac's rules from docs/notes/sessions.md's "Backups, hot exit and auto-save" (its
// scheduleBackups, writeBackups, scheduleAutoSave and autoSaveOnFocusChange
// in EditorController+Persistence.mm), so the Mac can move onto the planner
// (stage 3c) with its behaviour pinned.

static void testSavePlan() {
    using namespace SavePlan;
    using Mode = Settings::AutoSave;

    auto dirtyTab = [](uint64_t id, uint64_t gen) {
        Tab t;
        t.id = id;
        t.dirty = true;
        t.keepsText = true;
        t.generation = gen;
        t.path = "/p/" + std::to_string(id) + ".txt";
        return t;
    };
    // The port's part of a write: the tab records what its backup holds.
    auto wrote = [](std::vector<Tab> &tabs, const Plan &p) {
        for (uint64_t id : p.writeBackups)
            for (Tab &t : tabs)
                if (t.id == id) {
                    t.backupWritten = true;
                    t.backupGeneration = t.generation;
                    t.backupPath = t.path;
                }
    };
    auto has = [](const std::vector<uint64_t> &v, uint64_t id) {
        return std::find(v.begin(), v.end(), id) != v.end();
    };

    GROUP("saveplan:needs");
    {
        Tab t = dirtyTab(1, 3);
        CHECK(needsBackup(t));
        t.backupWritten = true;
        t.backupGeneration = 3;
        t.backupPath = t.path;
        CHECK(!needsBackup(t));          // the backup holds this text
        t.generation = 4;
        CHECK(needsBackup(t));           // typed since
        t.backupGeneration = 4;
        t.path = "/p/renamed.txt";
        CHECK(needsBackup(t));           // renamed since: written under the new path
        t.backupPath = t.path;
        CHECK(!needsBackup(t));
        t.dirty = false;
        CHECK(!needsBackup(t));          // clean: nothing to keep
        Tab picture = dirtyTab(2, 1);
        picture.keepsText = false;       // a picture, a diff, a message, no file
        CHECK(!needsBackup(picture) && !autoSaves(picture));
        Tab waits = dirtyTab(3, 1);
        waits.mayAutoSave = false;       // its Ctrl+S waits for the formatter
        CHECK(needsBackup(waits) && !autoSaves(waits));
        CHECK(autoSaves(dirtyTab(4, 1)));
    }

    GROUP("saveplan:backup-after-typing-stops");
    {
        // One second after the last key.
        Planner p;
        std::vector<Tab> tabs = {dirtyTab(1, 1)};
        CHECK(p.changed(tabs, 10.0).empty());
        CHECK(p.nextDue() == 11.0);
        CHECK(p.tick(tabs, 10.99).empty());   // not yet
        tabs[0].generation = 2;
        p.changed(tabs, 10.5);                // another key restarts the second
        CHECK(p.nextDue() == 11.5);
        CHECK(p.tick(tabs, 11.2).empty());
        const Plan due = p.tick(tabs, 11.5);
        CHECK(due.writeBackups.size() == 1 && due.writeBackups[0] == 1 && !due.autoSave);
        wrote(tabs, due);
        CHECK(p.nextDue() == kNever);         // nothing left
        CHECK(p.tick(tabs, 30).empty());      // and a late tick writes it once only
        // Clean again: no more backups planned, and the backup goes at once.
        tabs[0].dirty = false;
        const Plan clean = p.changed(tabs, 31);
        CHECK(clean.discardBackups.size() == 1 && clean.discardBackups[0] == 1);
        CHECK(p.nextDue() == kNever);
    }

    GROUP("saveplan:backup-at-most-five-seconds");
    {
        // Typing that never pauses is written 5 s after its first change,
        // then the count starts over.
        Planner p;
        std::vector<Tab> tabs = {dirtyTab(1, 1)};
        uint64_t gen = 1;
        double writes = 0, firstWrite = -1;
        for (double t = 0; t <= 12.0; t += 0.25) {
            if (p.nextDue() <= t) {
                const Plan d = p.tick(tabs, t);
                if (!d.writeBackups.empty()) {
                    writes++;
                    if (firstWrite < 0) firstWrite = t;
                    wrote(tabs, d);
                }
            }
            tabs[0].generation = ++gen;   // a key every quarter second
            p.changed(tabs, t);
            CHECK(p.nextDue() <= t + kBackupDelay);
        }
        CHECK(firstWrite == 5.0);
        CHECK(writes == 2);   // at 5 and at 10.25, the first change after 5 starting the count
        // The wait never goes below now.
        Planner q;
        q.changed(tabs, 0);
        tabs[0].generation = ++gen;
        q.changed(tabs, 7);   // 7 s since the first unwritten change
        CHECK(q.nextDue() == 7.0);
    }

    GROUP("saveplan:backup-every-tab");
    {
        // One write covers every tab that needs one; clean tabs with backups
        // lose them; a tab with nothing to keep is ignored.
        Planner p;
        std::vector<Tab> tabs = {dirtyTab(1, 1), dirtyTab(2, 5), dirtyTab(3, 1), dirtyTab(4, 1)};
        tabs[2].dirty = false;
        tabs[2].backupWritten = true;    // clean since its backup
        tabs[3].keepsText = false;
        tabs[3].backupWritten = true;    // became a picture: nothing to keep
        const Plan now = p.changed(tabs, 0);
        CHECK(now.discardBackups.size() == 2 && has(now.discardBackups, 3) &&
              has(now.discardBackups, 4));
        CHECK(now.writeBackups.empty());
        tabs[2].backupWritten = tabs[3].backupWritten = false;   // the port removed them
        const Plan d = p.tick(tabs, 1.0);
        CHECK(d.writeBackups.size() == 2 && has(d.writeBackups, 1) && has(d.writeBackups, 2));
        // A tab already backed up is not planned at all.
        wrote(tabs, d);
        CHECK(p.changed(tabs, 2).empty());
        CHECK(p.nextDue() == kNever);
        // A restored backup (written under another process's name, the
        // port's record says nothing of it yet) is written again soon.
        Tab restored = dirtyTab(9, 1);
        restored.backupWritten = true;
        p.changed({restored}, 3);
        CHECK(p.nextDue() == 4.0);
    }

    GROUP("saveplan:auto-save-after-delay");
    {
        Planner p;
        std::vector<Tab> tabs = {dirtyTab(1, 1)};
        p.changed(tabs, 0);
        CHECK(p.autoSaveDue() == kNever);    // off by default
        p.setAutoSave(Mode::AfterDelay, 1000);
        p.autoSaveChanged(tabs, 0);
        CHECK(p.autoSaveDue() == 1.0);
        // Every edit restarts the delay, with no longest wait.
        for (double t = 0.5; t < 20; t += 0.5) p.changed(tabs, t);
        CHECK(p.autoSaveDue() == 20.5);
        Plan d = p.tick(tabs, 20.5);
        CHECK(d.autoSave);
        CHECK(p.autoSaveDue() == kNever);
        // Its own delay.
        p.setAutoSave(Mode::AfterDelay, 250);
        p.changed(tabs, 30);
        CHECK(p.autoSaveDue() == 30.25);
        // No tab takes it: nothing planned.
        tabs[0].mayAutoSave = false;
        p.changed(tabs, 31);
        CHECK(p.autoSaveDue() == kNever);
        tabs[0].mayAutoSave = true;
        tabs[0].dirty = false;
        p.changed(tabs, 32);
        CHECK(p.autoSaveDue() == kNever);
        // Turned off with one planned: the tick writes nothing for it.
        tabs[0].dirty = true;
        p.changed(tabs, 33);
        p.setAutoSave(Mode::Off, 1000);
        CHECK(!p.tick(tabs, 40).autoSave);
        // Focus changes do nothing in after-delay mode.
        p.setAutoSave(Mode::AfterDelay, 1000);
        p.focusChanged(50);
        CHECK(p.focusSaveDue() == kNever);
    }

    GROUP("saveplan:auto-save-focus-change");
    {
        Planner p;
        std::vector<Tab> tabs = {dirtyTab(1, 1)};
        p.setAutoSave(Mode::FocusChange, 1000);
        p.changed(tabs, 0);
        CHECK(p.autoSaveDue() == kNever);    // edits plan nothing
        p.focusChanged(2);
        CHECK(p.focusSaveDue() == 2 && p.nextDue() <= 2);
        // Nothing an edit does cancels it (the Mac's trap: the debounce once
        // cancelled the focus-change save after a tab switch).
        tabs[0].generation = 2;
        p.changed(tabs, 2.01);
        p.autoSaveChanged(tabs, 2.02);
        CHECK(p.focusSaveDue() == 2);
        Plan d = p.tick(tabs, 2.03);
        CHECK(d.autoSave);
        CHECK(p.focusSaveDue() == kNever);
        // A second change before the tick: one save.
        p.focusChanged(3);
        p.focusChanged(3.1);
        CHECK(p.tick(tabs, 3.2).autoSave);
        CHECK(!p.tick(tabs, 3.3).autoSave);
        // Off: a focus change plans nothing.
        p.setAutoSave(Mode::Off, 1000);
        p.focusChanged(4);
        CHECK(p.nextDue() == kNever || !p.tick(tabs, 4).autoSave);
    }

    GROUP("saveplan:cancel");
    {
        Planner p;
        std::vector<Tab> tabs = {dirtyTab(1, 1)};
        p.setAutoSave(Mode::AfterDelay, 1000);
        p.changed(tabs, 0);
        CHECK(p.nextDue() == 1.0);
        p.cancel();
        CHECK(p.nextDue() == kNever);
        CHECK(p.tick(tabs, 100).empty());
    }

    GROUP("saveplan:session");
    {
        // The tabs change: the session is written a second after the first
        // change, however many follow.
        Planner p;
        const std::vector<Tab> none;
        CHECK(!p.takeSession());           // nothing planned
        p.sessionChanged(10);
        CHECK(p.sessionDue() == 11.0 && p.nextDue() == 11.0);
        p.sessionChanged(10.6);            // not put off
        CHECK(p.sessionDue() == 11.0);
        CHECK(!p.tick(none, 10.9).saveSession);
        Plan due = p.tick(none, 11.0);
        CHECK(due.saveSession && !due.empty());
        CHECK(p.nextDue() == kNever && !p.tick(none, 20).saveSession);
    }

    GROUP("saveplan:session-before-another-window-reads");
    {
        // GTK's split_editor flake (#81): a window changes its tabs, and a
        // second window opens on the same folder within the second. The
        // first window's pending save is taken (and written by the port)
        // before the second reads the folder's session, and is not written
        // a second time when its tick comes.
        Planner first;
        const std::vector<Tab> none;
        first.sessionChanged(5);
        CHECK(first.takeSession());        // the new window asks: written now
        CHECK(!first.takeSession());       // once
        CHECK(first.nextDue() == kNever);
        CHECK(!first.tick(none, 6).saveSession);
        // A change after it is planned again as before.
        first.sessionChanged(7);
        CHECK(first.sessionDue() == 8.0);
        // Cancel (the window closing) drops it with the rest.
        first.cancel();
        CHECK(!first.takeSession());
    }

    GROUP("saveplan:walk");
    {
        // A random walk of edits, saves, focus changes and ticks: a dirty
        // tab's text is never more than 5 s (and a tick) from its backup,
        // a backup is never written for a clean tab, and nextDue never lies
        // in the past after a tick.
        uint64_t seed = 0x5a7e;
        auto rnd = [&seed](int n) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            return (int)((seed >> 33) % (uint64_t)n);
        };
        Planner p;
        p.setAutoSave(Mode::AfterDelay, 700);
        std::vector<Tab> tabs = {dirtyTab(1, 0), dirtyTab(2, 0), dirtyTab(3, 0)};
        for (Tab &t : tabs) t.dirty = false;
        std::vector<double> unwrittenSince(3, -1);
        double now = 0;
        bool ok = true;
        for (int step = 0; step < 20000; step++) {
            // The port's timer: every tick due before the next event, at the
            // time it was due.
            const double next = now + rnd(400) / 1000.0;
            while (p.nextDue() <= next) {
                const double at = std::max(now, p.nextDue());
                const Plan d = p.tick(tabs, at);
                for (uint64_t id : d.writeBackups) {
                    Tab &t = tabs[(size_t)id - 1];
                    ok = ok && t.dirty && needsBackup(t);
                    ok = ok && at - unwrittenSince[(size_t)id - 1] <= kBackupMaxWait + 1e-9;
                    unwrittenSince[(size_t)id - 1] = -1;
                }
                wrote(tabs, d);
                ok = ok && p.nextDue() > at;
                now = at;
            }
            now = next;
            const int k = rnd(3);
            switch (rnd(6)) {
            case 0: case 1: case 2:   // a key
                tabs[(size_t)k].generation++;
                tabs[(size_t)k].dirty = true;
                if (unwrittenSince[(size_t)k] < 0) unwrittenSince[(size_t)k] = now;
                break;
            case 3:                   // saved
                tabs[(size_t)k].dirty = false;
                unwrittenSince[(size_t)k] = -1;
                break;
            case 4:
                p.focusChanged(now);
                break;
            default:
                break;
            }
            const Plan c = p.changed(tabs, now);
            for (uint64_t id : c.discardBackups) {
                Tab &t = tabs[(size_t)id - 1];
                ok = ok && !t.dirty;
                t.backupWritten = false;
            }
        }
        CHECK(ok);
    }
}
