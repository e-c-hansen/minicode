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
  desc "Tiny native code editor, no Electron, no dependencies"
  homepage "https://github.com/e-c-hansen/homebrew-tap"

  depends_on :macos

  app "MiniCode.app"
  binary "#{appdir}/MiniCode.app/Contents/Resources/minicode" # `minicode` on PATH

  # Ad-hoc signed but not notarized (free, no Apple Developer account). Homebrew
  # quarantines downloads by default, so clear the flag after install to avoid a
  # Gatekeeper warning on first launch.
  postflight_steps do
    run "/usr/bin/xattr", args: ["-dr", "com.apple.quarantine", "{{appdir}}/MiniCode.app"]
  end

  zap trash: "~/Library/Preferences/com.minicode.editor.plist"
end
