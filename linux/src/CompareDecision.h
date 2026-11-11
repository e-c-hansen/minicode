// CompareDecision.h — a comparison offered for the user's decision, the Mac's
// MCComparison decision (src/CompareView.mm), linux/PARITY.md item 3.2: the
// Claude Code IDE link's openDiff (item 3.9) shows a change and waits for
// Accept or Reject. The view puts a bar with `prompt` over the panes and a
// check and a cross for each hunk in a gutter between them; this class holds
// the choices and ends the decision once.
//
// `onDecision(accepted, text)` is called once: accepted false when nothing was
// accepted, else true with `text` the text made of the accepted hunks (the
// right text when all were, SideBySide::assemble otherwise). The last hunk's
// choice sends the answer only after `grace` seconds (3; 0 sends at once), so
// a press on the wrong button can be taken back; `sendNow`, the bar's own
// buttons and closing the tab (`tabClosed`) send it at once, closing before
// the last choice being a rejection.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ComparePanes.h"

class CompareDecision {
public:
    using Callback = std::function<void(bool accepted, const std::string& text)>;
    CompareDecision(std::shared_ptr<const CompareData> data, std::string prompt, Callback done,
                    double grace = 3);
    ~CompareDecision();
    CompareDecision(const CompareDecision&) = delete;
    CompareDecision& operator=(const CompareDecision&) = delete;

    const std::string& prompt() const { return prompt_; }
    // Pending (0), accepted (1, all or some hunks), rejected (-1), or no
    // longer wanted (2: the other side stopped waiting, endDecision).
    int decision() const { return decision_; }
    const std::string& acceptedText() const { return accepted_; }
    const std::string& note() const { return note_; }
    size_t hunkCount() const { return choices_.size(); }
    // A hunk's own choice: 1, -1, or 0 while undecided.
    int choiceOfHunk(size_t hunk) const { return hunk < choices_.size() ? choices_[hunk] : 0; }
    bool hunksDecided() const;
    bool sending() const { return timer_ != 0; }
    void setGrace(double seconds) { grace_ = seconds; }

    // The whole change: false rejects all of it, true accepts every hunk not
    // decided on its own. Either ends the decision.
    void decide(bool accepted);
    // One hunk: 1, -1, or 0 for undecided again.
    void decideHunk(size_t hunk, int choice);
    // Every hunk still undecided takes `accepted`, and the decision ends.
    void decideRest(bool accepted);
    // Ends the grace at once.
    void sendNow();
    // Closing the tab: an answer about to go goes now; else a rejection.
    void tabClosed();
    // The other side no longer waits: the bar says `why`, onDecision is not called.
    void endDecision(const std::string& why);

    // Told after every change (the view redraws).
    std::function<void()> onChanged;

private:
    void cancelSending();
    void finish();
    void changed() { if (onChanged) onChanged(); }

    std::shared_ptr<const CompareData> data_;
    std::string prompt_;
    Callback done_;
    double grace_;
    std::vector<signed char> choices_;
    int decision_ = 0;
    std::string accepted_, note_;
    unsigned timer_ = 0;
};
