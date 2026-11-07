# Homebrew Cask for MiniCode — install with:
#
#   brew install --cask e-c-hansen/tap/minicode
#
# This file is the cask. scripts/release.sh stamps the version and checksum into
# it and copies it into the homebrew-tap repo, which holds nothing else, so edit
# it here and never there. To cut a new version: scripts/release.sh 1.2.1
cask "minicode" do
  version "1.5.11"
  sha256 "2f62ae15a6091bffb35fa37ff4b2b72173b9999dceba1c5d30e57ac11992eab9"

  url "https://github.com/e-c-hansen/minicode/releases/download/v#{version}/MiniCode.zip"
  name "MiniCode"
  desc "Native code editor with a terminal, browser, and Markdown and LaTeX previews"
  homepage "https://github.com/e-c-hansen/minicode"

  # Built for Apple silicon only (the Makefile builds the host's architecture).
  depends_on arch: :arm64
  # The app is built for macOS 11 (Info.plist's LSMinimumSystemVersion), which
  # is also the oldest macOS Homebrew knows, so brew style wants no version
  # here. A higher minimum is written `depends_on macos: :ventura` (13 and up);
  # scripts/check-minos.sh checks this line against Info.plist.
  depends_on :macos

  app "MiniCode.app"
  binary "#{appdir}/MiniCode.app/Contents/Resources/minicode" # `minicode` on PATH

  # Signed with a self-signed certificate (ad hoc where none is set up) but not
  # notarized, which needs a paid Apple Developer account. Homebrew quarantines
  # downloads by default, so clear the flag after install to avoid a Gatekeeper
  # warning on first launch.
  postflight_steps do
    run "/usr/bin/xattr", args: ["-dr", "com.apple.quarantine", "{{appdir}}/MiniCode.app"]
  end

  zap trash: "~/Library/Preferences/com.minicode.editor.plist"
end
