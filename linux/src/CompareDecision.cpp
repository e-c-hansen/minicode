// CompareDecision.cpp — see CompareDecision.h.
#include "CompareDecision.h"

#include <glib.h>

#include "LspClient.h"

CompareDecision::CompareDecision(std::shared_ptr<const CompareData> data, std::string prompt,
                                 Callback done, double grace)
    : data_(std::move(data)), prompt_(std::move(prompt)), done_(std::move(done)), grace_(grace) {
    if (data_) choices_.assign(data_->hunks.size(), 0);
}

CompareDecision::~CompareDecision() { cancelSending(); }

bool CompareDecision::hunksDecided() const {
    for (signed char c : choices_)
        if (c) return true;
    return false;
}

void CompareDecision::cancelSending() {
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
}

void CompareDecision::decideHunk(size_t hunk, int choice) {
    if (prompt_.empty() || decision_ != 0 || hunk >= choices_.size()) return;
    choices_[hunk] = (signed char)(choice > 0 ? 1 : choice < 0 ? -1 : 0);
    cancelSending();
    for (signed char c : choices_)
        if (!c) {
            changed();
            return;
        }
    // The last hunk's choice: the answer goes after a moment, so a press on
    // the wrong button can still be taken back.
    if (grace_ <= 0) {
        finish();
        return;
    }
    timer_ = g_timeout_add(
        (guint)(grace_ * 1000),
        [](gpointer p) -> gboolean {
            CompareDecision* self = static_cast<CompareDecision*>(p);
            self->timer_ = 0;
            if (self->decision_ == 0) self->finish();
            return G_SOURCE_REMOVE;
        },
        this);
    changed();
}

void CompareDecision::sendNow() {
    if (!timer_ || decision_ != 0) return;
    cancelSending();
    finish();
}

void CompareDecision::decideRest(bool accepted) {
    if (prompt_.empty() || decision_ != 0) return;
    cancelSending();
    for (signed char& c : choices_)
        if (!c) c = accepted ? 1 : -1;
    // With no hunks (the texts differ only in the last line's "\n") the
    // button says it all.
    if (choices_.empty()) decision_ = accepted ? 1 : -1;
    finish();
}

void CompareDecision::decide(bool accepted) {
    if (prompt_.empty() || decision_ != 0) return;
    if (!accepted) choices_.assign(choices_.size(), -1);
    decideRest(accepted);
}

// Every hunk has its choice: the answer is the accepted ones, or a rejection
// when there are none.
void CompareDecision::finish() {
    size_t taken = 0;
    for (signed char c : choices_) taken += c > 0;
    if (!choices_.empty()) decision_ = taken ? 1 : -1;
    if (decision_ == 1) {
        if (taken == choices_.size()) {
            accepted_ = data_->spec.rightText;
        } else {
            std::vector<bool> pick(choices_.size());
            for (size_t i = 0; i < pick.size(); i++) pick[i] = choices_[i] > 0;
            accepted_ = Lsp::toUtf8(SideBySide::assemble(Lsp::toUtf16(data_->spec.leftText),
                                                         Lsp::toUtf16(data_->spec.rightText),
                                                         data_->rows, pick));
        }
    }
    Callback done = std::move(done_);
    done_ = nullptr;
    changed();
    if (done) done(decision_ == 1, accepted_);
}

void CompareDecision::endDecision(const std::string& why) {
    if (prompt_.empty() || decision_ != 0) return;
    cancelSending();
    decision_ = 2;
    note_ = why;
    done_ = nullptr;
    changed();
}

void CompareDecision::tabClosed() {
    if (decision_ != 0) return;
    if (timer_) sendNow();
    else decide(false);
}
