# Homebrew Cask for MiniCode — install with:
#
#   brew install --cask e-c-hansen/tap/minicode
#
# This is a reference copy; the live cask lives in the homebrew-tap repo and is
# updated by scripts/release.sh. To cut a new version: scripts/release.sh 1.2.0
cask "minicode" do
  version "1.2.0"
  sha256 "3e59bcc2289af5142b31c75a06669c6214f9081807de14561071b81cd185b870"

  url "https://github.com/e-c-hansen/homebrew-tap/releases/download/v#{version}/MiniCode.zip"
  name "MiniCode"
  desc "Tiny native macOS code editor, no Electron, no dependencies"
  homepage "https://github.com/e-c-hansen/homebrew-tap"

  app "MiniCode.app"
  binary "#{appdir}/MiniCode.app/Contents/Resources/minicode"   # `minicode` on PATH

  # MiniCode is ad-hoc signed but not notarized (no paid Apple account), so macOS
  # quarantines the download and would show a Gatekeeper warning on first launch.
  # Clear the quarantine flag after install so `brew install` opens cleanly.
  postflight do
    system_command "/usr/bin/xattr",
                   args: ["-dr", "com.apple.quarantine", "#{appdir}/MiniCode.app"]
  end

  zap trash: "~/Library/Preferences/com.minicode.editor.plist"
end
