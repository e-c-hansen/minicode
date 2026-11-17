// SavePlan.cpp — see SavePlan.h.
#include "SavePlan.h"

#include <algorithm>

namespace SavePlan {

bool needsBackup(const Tab &t) {
    return t.dirty && t.keepsText &&
           (!t.backupWritten || t.backupGeneration != t.generation || t.backupPath != t.path);
}

bool autoSaves(const Tab &t) { return t.dirty && t.keepsText && t.mayAutoSave; }

void Planner::setAutoSave(Settings::AutoSave mode, int delayMs) {
    mode_ = mode;
    delay_ = std::max(0, delayMs) / 1000.0;
    if (mode_ != Settings::AutoSave::AfterDelay) autoSaveDue_ = kNever;
    if (mode_ != Settings::AutoSave::FocusChange) focusDue_ = kNever;
}

Plan Planner::changed(const std::vector<Tab> &tabs, double now) {
    Plan p = backupsChanged(tabs, now);
    autoSaveChanged(tabs, now);
    return p;
}

Plan Planner::backupsChanged(const std::vector<Tab> &tabs, double now) {
    Plan p;
    bool pending = false;
    for (const Tab &t : tabs) {
        if (t.dirty && t.keepsText) pending = pending || needsBackup(t);
        else if (t.backupWritten) p.discardBackups.push_back(t.id);
    }
    if (!pending) {
        firstPending_ = kNever;
        backupDue_ = kNever;
        return p;
    }
    if (firstPending_ == kNever) firstPending_ = now;
    const double waited = now - firstPending_;
    backupDue_ = now + std::max(0.0, std::min(kBackupDelay, kBackupMaxWait - waited));
    return p;
}

void Planner::autoSaveChanged(const std::vector<Tab> &tabs, double now) {
    autoSaveDue_ = kNever;
    if (mode_ != Settings::AutoSave::AfterDelay) return;
    if (std::any_of(tabs.begin(), tabs.end(), autoSaves)) autoSaveDue_ = now + delay_;
}

void Planner::focusChanged(double now) {
    if (mode_ == Settings::AutoSave::FocusChange) focusDue_ = std::min(focusDue_, now);
}

Plan Planner::tick(const std::vector<Tab> &tabs, double now) {
    Plan p;
    if (backupDue_ <= now) {
        backupDue_ = kNever;
        firstPending_ = kNever;
        for (const Tab &t : tabs)
            if (needsBackup(t)) p.writeBackups.push_back(t.id);
    }
    if (autoSaveDue_ <= now || focusDue_ <= now) {
        autoSaveDue_ = kNever;
        focusDue_ = kNever;
        p.autoSave = mode_ != Settings::AutoSave::Off;
    }
    return p;
}

double Planner::nextDue() const { return std::min({backupDue_, autoSaveDue_, focusDue_}); }

void Planner::cancel() {
    firstPending_ = backupDue_ = autoSaveDue_ = focusDue_ = kNever;
}

}  // namespace SavePlan
