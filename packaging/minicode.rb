# Homebrew Cask for MiniCode — install with:
#
#   brew install --cask e-c-hansen/tap/minicode
#
# To publish this: create a repo named "homebrew-tap" under your GitHub account,
# put this file at Casks/minicode.rb, cut a GitHub Release that includes the
# MiniCode.zip from `make dist-zip`, and fill in the real version and sha256
# (get it with: shasum -a 256 MiniCode.zip).
cask "minicode" do
  version "1.0.0"
  sha256 :no_check   # replace with the real zip sha256 for a published release

  url "https://github.com/e-c-hansen/minicode/releases/download/v#{version}/MiniCode.zip"
  name "MiniCode"
  desc "Tiny native macOS code editor, no Electron, no dependencies"
  homepage "https://github.com/e-c-hansen/minicode"

  app "MiniCode.app"

  # MiniCode is ad-hoc signed but not notarized (no paid Apple account), so macOS
  # quarantines the download and would show a Gatekeeper warning on first launch.
  # Clear the quarantine flag after install so `brew install` opens cleanly.
  postflight do
    system_command "/usr/bin/xattr",
                   args: ["-dr", "com.apple.quarantine", "#{appdir}/MiniCode.app"]
  end

  zap trash: "~/Library/Preferences/com.minicode.editor.plist"
end
