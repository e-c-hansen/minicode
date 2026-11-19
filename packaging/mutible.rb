# Homebrew Cask for mutible — install with:
#
#   brew install --cask ible-ai/tap/mutible
#
# This file is the cask. scripts/release.sh stamps the version and checksum into
# it and copies it into the homebrew-tap repo, which holds nothing else, so edit
# it here and never there. To cut a new version: scripts/release.sh 1.6.1
# mutible was called mutible until 1.6.0: packaging/mutible.rb is the old
# cask, deprecated, in the old tap.
cask "mutible" do
  version "1.6.3"
  sha256 "96757955765cc162b6c58610f24354c824f7da6528319ceacdac4821de05fb0f"

  url "https://github.com/ible-ai/mutible/releases/download/v#{version}/Mutible.zip"
  name "Mutible"
  # "Your machine, in plain sight." without its full stop, which brew style refuses.
  desc "Your machine, in plain sight"
  homepage "https://mut.ible.ai"

  # Built for Apple silicon only (the Makefile builds the host's architecture).
  depends_on arch: :arm64
  # The app is built for macOS 11 (Info.plist's LSMinimumSystemVersion), which
  # is also the oldest macOS Homebrew knows, so brew style wants no version
  # here. A higher minimum is written `depends_on macos: :ventura` (13 and up);
  # scripts/check-minos.sh checks this line against Info.plist.
  depends_on :macos
  # The old cask links the same `mutible` command; uninstall it first.
  conflicts_with cask: "e-c-hansen/tap/minicode" # rename-mutible: keep

  app "Mutible.app"
  binary "#{appdir}/Mutible.app/Contents/Resources/mutible" # `mutible` on PATH
  binary "#{appdir}/Mutible.app/Contents/Resources/mutible", target: "mbl"
  # The command's name before 1.6.0, kept for a while; run on a terminal, it
  # says it is now mutible.
  binary "#{appdir}/Mutible.app/Contents/Resources/mutible", target: "minicode" # rename-mutible: keep

  # Signed with a self-signed certificate (ad hoc where none is set up) but not
  # notarized, which needs a paid Apple Developer account. Homebrew quarantines
  # downloads by default, so clear the flag after install to avoid a Gatekeeper
  # warning on first launch.
  postflight_steps do
    run "/usr/bin/xattr", args: ["-dr", "com.apple.quarantine", "{{appdir}}/Mutible.app"]
  end

  zap trash: "~/Library/Preferences/ai.ible.mutible.plist"
end
