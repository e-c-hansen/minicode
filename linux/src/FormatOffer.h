// FormatOffer.h — the bar offering format on save, the Mac's MCFormatOnSaveOffer
// (FormatOnSave.mm). After Ctrl+S saves a file whose project sets up its
// formatter (a ruff section in pyproject.toml, a .clang-format, a
// .prettierrc; Formatter::offersOnSave) while format.on-save is off and that
// formatter is installed, a bar under the tab strip asks "This project
// formats Python files with ruff, set up in pyproject.toml. Format them each
// time you save?" with Turn On (writes format.on-save = project), Don't Ask
// Again (kept with the install bar's, in install-hints.json, under the key
// "format-on-save"), and × (until mutible starts again). Offered once per
// launch, in the window where the save was. The save itself never waits for
// it, and nothing is formatted until the next Ctrl+S.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

class FormatOffer {
public:
    struct Host {
        std::function<std::string()> root;   // the window's folder
        // format.<lang> in the settings, "" when unset.
        std::function<std::string(const std::string& lang)> setting;
        std::function<bool()> formatOnSave;   // format.on-save is on (true or project)
        // Writes format.on-save = project; false (with why) when it could not.
        std::function<bool(std::string* why)> turnOn;
        std::function<void(const std::string& message)> note;   // the status bar
    };

    explicit FormatOffer(Host host);
    ~FormatOffer();
    FormatOffer(const FormatOffer&) = delete;
    FormatOffer& operator=(const FormatOffer&) = delete;

    // The bar, for main.cpp to put under the tab strip (Editor::addBar).
    GtkWidget* widget() const { return box_; }
    // Ctrl+S saved `path`: offer format on save if it is due (format.on-save
    // off, not muted, not offered yet this launch, the project sets up an
    // installed formatter). The look runs off the main thread; the bar shows
    // when it answers.
    void offerAfterSaving(const std::string& path);
    // The settings changed: the bar goes when format on save was turned on
    // some other way.
    void settingsChanged();

    // For checks: whether the bar shows, its words, its buttons, and
    // whether the look for an offer is still running.
    bool shown() const;
    std::string text() const;
    GtkWidget* turnOnButton() const { return turnOn_; }
    GtkWidget* muteButton() const { return mute_; }
    GtkWidget* closeButton() const { return close_; }
    bool checking() const { return checking_; }
    // The offer's key in install-hints.json.
    static const char* key() { return "format-on-save"; }
    // For checks: the offer may be made again this launch.
    static void forgetOffered();

private:
    bool due() const;
    void show(const std::string& question);
    void hide();
    void doTurnOn();

    Host host_;
    GtkWidget* box_ = nullptr;
    GtkWidget* label_ = nullptr;
    GtkWidget* turnOn_ = nullptr;
    GtkWidget* mute_ = nullptr;
    GtkWidget* close_ = nullptr;
    bool checking_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};
