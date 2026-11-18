// UsageView.h — the Mac's UsageView.{h,mm} (mac/src). The plan usage
// forecast (A13) in the Agents panel, under its heading: the 5-hour and
// weekly windows' usage now, when each reaches 100% at the current rate
// (with a range), the burn rate at API prices, and the usage this
// computer's transcripts do not explain (the fit's line is the text's
// tooltip); then the action that turns the status line log on or off
// (StatusLineOffer.h).
//
// UsageModel is one per app (the plan is the user's, not a folder's): on a
// worker thread of its own (a GThreadPool of one) it reads the transcripts
// incrementally (ClaudeUsage::Scanner over AgentModel's projects folder)
// and the status line log (StatusLineOffer::usageDir), works out the
// forecast and its lines (ClaudeUsage::describe), and hands them to the
// main loop with g_idle_add. The Agents panel asks it to refresh when it
// rebuilds while shown (its model's changes and its 30 s timer); a look
// within 5 s of the last is skipped unless forced.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class UsageModel {
public:
    static UsageModel& shared();
    // The forecast as lines of prose, the fit's line last when there is one.
    const std::vector<std::string>& lines() const { return lines_; }
    bool loaded() const { return loaded_; }
    // Reads what was added since the last look; at most one look at a time,
    // and none within 5 s of the last unless forced.
    void refresh();
    // Forced; `done` on the main loop once the lines are new (for the checks).
    void refreshThen(std::function<void()> done);
    // Called on the main loop after every look. The id is for removeListener.
    int addListener(std::function<void()> fn);
    void removeListener(int id);

    struct Impl;

private:
    UsageModel();
    void look(std::function<void()> done);
    void finished(std::vector<std::string> lines);

    Impl* impl_;
    std::vector<std::string> lines_;
    bool loaded_ = false, busy_ = false, again_ = false;
    gint64 last_ = 0;   // monotonic microseconds of the last look's end, 0 for none
    std::vector<std::function<void()>> waiting_;
    std::map<int, std::function<void()>> listeners_;
    int nextListener_ = 1;
};

class UsageView {
public:
    UsageView();
    ~UsageView();

    GtkWidget* widget() const { return root_; }
    GtkWidget* actionButton() const { return action_; }
    std::string actionTitle() const;
    std::string forecastText() const;
    // The fit's line, shown as the text's tooltip ("" before there is one).
    std::string fitText() const { return fit_; }
    // The model refreshed, and the action's title for the settings as they are.
    void refresh();

private:
    GtkWidget* root_ = nullptr;
    GtkWidget* text_ = nullptr;
    GtkWidget* action_ = nullptr;
    GtkWidget* actionLabel_ = nullptr;
    std::string fit_;
    bool installed_ = false;
    int listener_ = 0;
    // Shared with a question still up when the panel goes, so its answer
    // reaches no freed view (null once the view is gone).
    std::shared_ptr<UsageView*> self_;

    void setActionTitle();
    void showLines();
    void act();
};
