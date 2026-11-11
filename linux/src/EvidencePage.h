// EvidencePage.h — the Evidence view of one agent session (PARITY.md item
// 3.5, part two; the Mac's MCEvidencePage in src/AgentPages.mm), a page in an
// editor tab (EditorPage.h). The session's final message sits beside the last
// test command's result; below, every shell command the agent ran, in order,
// with what it said it was for, whether it failed, and the first and last 20
// lines of its output. The outputs are read lazily from the transcript (only
// its ends; "Show all" reads up to 1 MB) on a thread of its own, and the page
// follows the transcript as it grows. Open in Terminal types the command at
// the session's folder and never presses Return. Read-only toward Claude
// Code's data.
#pragma once
#include <gtk/gtk.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "AgentEvidence.h"
#include "EditorPage.h"

class EvidencePage : public EditorPage {
public:
    EvidencePage(const std::string& transcript, const std::string& name);
    ~EvidencePage() override;
    GtkWidget* widget() override { return root_; }
    GtkWidget* focusWidget() override { return commands_; }
    std::string key() const override { return "evidence:" + transcript_; }
    std::string title() const override { return "Evidence: " + name_; }

    // command, the folder it ran in.
    std::function<void(const std::string&, const std::string&)> onOpenInTerminal;

    // Reread what changed (coalesced).
    void update();
    // For the checks and the links.
    bool loaded() const { return loaded_; }
    bool outputsLoaded() const { return outputsLoaded_; }
    std::string finalMessageText() const;
    std::string lastTestText() const;
    std::string commandsText() const;
    size_t commandCount() const { return cmds_.size(); }
    void expand(size_t index);
    void collapse(size_t index);
    bool isExpanded(size_t index) const { return index < cmds_.size() && cmds_[index].haveFull; }
    void openInTerminal(size_t index);
    // The link under a point of the commands view (view coordinates), for clicks.
    bool linkAt(double x, double y, std::string* name) const;
    GtkWidget* commandsView() const { return commands_; }

private:
    struct Cmd {
        std::string desc, command, cwd, result, testName, summary, summaryLine;
        bool commandCut = false, answered = false, isError = false, isTest = false;
        bool summaryFailed = false, loaded = false, whole = false, haveFull = false, fullCut = false;
        std::vector<std::string> head, tail;
        long long omitted = 0;
        std::string full;
    };
    struct Shown {
        std::vector<Cmd> cmds;
        std::string final, cwd, info;
        int lastTest = -1;
        bool outputs = false;
    };
    struct Loaded {
        AgentTranscript::Output out;
        std::string full;
        bool haveFull = false, fullCut = false;
    };
    std::string transcript_, name_;
    GtkWidget *root_ = nullptr, *title_ = nullptr, *info_ = nullptr, *claim_ = nullptr,
              *test_ = nullptr, *commands_ = nullptr, *scroll_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    std::vector<Cmd> cmds_;
    std::string final_, cwd_, info2_;
    int lastTest_ = -1;
    bool loaded_ = false, outputsLoaded_ = false, closed_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    // The worker's side, under mutex_.
    std::mutex mutex_;
    std::unique_ptr<AgentEvidence::Reader> reader_;
    std::map<std::string, Loaded> outputs_;
    std::map<size_t, bool> wantFull_;
    bool updating_ = false, again_ = false, force_ = false, started_ = false;
    unsigned timer_ = 0;
    std::thread worker_;
    void work();
    void show(Shown s);
    void render();
    void renderLastTest();
    static int tick(void*);
    static void onPressed(GtkGestureClick*, int, double, double, gpointer);
};
