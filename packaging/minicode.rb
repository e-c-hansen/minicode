# The Homebrew Cask from before the rename: minicode is called mutible since
# 1.6.0, and its cask is packaging/mutible.rb, in the ible-ai tap:
#
#   brew install --cask ible-ai/tap/mutible
#
# This one stays in the old tap (e-c-hansen/homebrew-tap), deprecated and
# pointing to the new one, for people who have minicode installed:
# scripts/release.sh writes it there when it differs, and never stamps it
# again, so it stays on the last minicode release.
cask "minicode" do
  version "1.5.38"
  sha256 "5cdbb02a44247414afa5875406ed484232280128046b8a7563f4d4561106429a"

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

  deprecate! date: "2026-11-18", because: "is now called mutible", replacement_cask: "ible-ai/tap/mutible"

  caveats <<~EOS
    minicode is now called mutible, and its cask is ible-ai/tap/mutible:
      brew uninstall --cask minicode
      brew install --cask ible-ai/tap/mutible
    mutible copies your settings, sessions, and backups on its first launch.
  EOS

  zap trash: "~/Library/Preferences/com.minicode.editor.plist"
end
