# Homebrew Cask for MiniCode — install with:
#
#   brew install --cask e-c-hansen/tap/minicode
#
# This file is the cask. scripts/release.sh stamps the version and checksum into
# it and copies it into the homebrew-tap repo, which holds nothing else, so edit
# it here and never there. To cut a new version: scripts/release.sh 1.2.1
cask "minicode" do
  version "1.2.1"
  sha256 "2c9dd2383e96890b71d8484c22586c919922984c0836ff074c5523c2dbaa39c9"

  url "https://github.com/e-c-hansen/minicode/releases/download/v#{version}/MiniCode.zip"
  name "MiniCode"
  desc "Tiny native code editor, no Electron, no dependencies"
  homepage "https://github.com/e-c-hansen/minicode"

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
