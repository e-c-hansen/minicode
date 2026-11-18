# Releases, the PPA and distribution

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Releases for Linux and the PPA

(Moved from CLAUDE.md's "Build, test, run, release".)

- `scripts/release.sh --linux 1.2.1`: a Linux-only release, which runs on the
  ThinkPad too: tests, stamps `Info.plist` (the Linux version comes from it),
  pushes `main` with the tag, uploads to the PPA. No GitHub Release, zip, APK
  or cask, so Homebrew and Obtainium stay on the last full release. Ubuntu
  users get Linux changes only through a release of one kind or the other.
- `scripts/ppa.sh VERSION [N]` builds the Ubuntu source package (`debian/`,
  versions like `1.6.0-1~ppa1~resolute1`) in an ubuntu:26.04 container, signs
  it on the Mac with the gpg key named in `~/.config/mutible/ppa-key-id` (the
  old `~/.config/minicode/ppa-key-id` is read while that is missing), and
  uploads it to `ppa:echansen/minicode` (`--dry-run` and `--simulate` send
  nothing). From 1.6.0 the source package is `mutible`, building `mutible`
  and `minicode`, a transitional package that moves PPA users over (the PPA <!-- rename-mutible: keep -->
  keeps its name; `packaging/PPA.md`, "What the packages hold"). Setup is in `packaging/PPA.md`. Account `echansen`, key
  340F7652...1F40 in the Mac's gpg with its passphrase in the Keychain;
  Launchpad builds amd64 about 35 minutes after an upload, and ppa.sh (so
  release.sh too) waits for it (`scripts/ppa-wait.sh VERSION [N]`, the PPA's
  public getBuildRecords) and fails unless it is "Successfully built" (exit 3
  failed: upload N+1; 4 not seen to end: run ppa-wait.sh again; `--no-wait`
  or `MINICODE_PPA_NO_WAIT=1` skips it; `ppa:wait` tests it on canned
  answers). 1.5.13's build failed unseen when the scripts stopped at dput.
  Every Launchpad API query sends `Cache-Control: no-cache`: on 2026-11-18
  Launchpad's squid cache kept getBuildRecords' answer for one URL eight
  hours old, and the wait for 1.6.1 gave up while the build was uploading. The ThinkPad has its
  own key (AEE321CE...DB7F9335) and no Docker, so there ppa.sh runs the
  packaging tools directly (`packaging/PPA.md`, step 9). On the Mac it needs
  Docker, which must not be started unattended (see "Verification").

## Distribution

- `ible-ai/mutible` (public); each `vX.Y.Z` GitHub Release carries the zip
  and the signed APK.
- `ible-ai/homebrew-tap` holds only `Casks/mutible.rb`, written by
  `release.sh` from `packaging/mutible.rb` (the only copy anyone edits). The
  cask clears the quarantine flag in `postflight_steps` (Homebrew 7 deprecated
  `postflight`; `{{appdir}}` is the token); check edits with `brew style` in
  `/opt/homebrew/Library/Taps/ible-ai/homebrew-tap`. Homebrew is the only
  updater, so the cask must not declare `auto_updates` (a plain `brew upgrade`
  would skip it). Homebrew runs `lsregister` only when it reopens apps it quit;
  macOS registers a new bundle in /Applications itself.
- The rename (1.6.0): the app was minicode, in `e-c-hansen/minicode` with <!-- rename-mutible: keep -->
  the cask `e-c-hansen/tap/minicode`. That cask stays in the old tap as <!-- rename-mutible: keep -->
  `packaging/minicode.rb`, deprecated (`deprecate!` with <!-- rename-mutible: keep -->
  `replacement_cask: "ible-ai/tap/mutible"`, and caveats saying how to
  move), never stamped again; `release.sh` writes it into the old tap when
  it differs. The new cask links `mutible`, `mbl` and `minicode`, so it <!-- rename-mutible: keep -->
  `conflicts_with` the old one, which links `minicode` too. The cask's <!-- rename-mutible: keep -->
  `desc` is "Your machine, in plain sight" (brew style refuses a full stop).
  The Android APK keeps its name and key, and the PPA its name, through
  the rename. Both casks load with
  `brew ruby -e 'Cask::CaskLoader::FromPathLoader.new(Pathname(ARGV[0])).load(config: nil)' <file>`
  (`brew info` refuses a path).
- The homepage, https://mut.ible.ai (2026-11-18), is the README:
  `.github/workflows/pages.yml` renders it with GitHub's Markdown API
  (`gh api markdown -F text=@README.md`, which keeps the page as
  github.com shows it) and `site/build.py` puts it in `site/template.html`
  with `site/site.css`, copies `docs/demos` and `docs/brand` beside it (so
  the README's relative pictures work as they are), sends every other
  relative link to its page on github.com, gives the headings GitHub's ids
  (the API's output has none, so `#get-started` would go nowhere), and
  writes `CNAME`. It deploys on a push to main that touches the README,
  the demos, the brand, `site/` or the workflow; a pull request touching
  them only builds. Not Jekyll over the repository: that would publish
  every Markdown file, and some hold Liquid-like `{{`. To see a change
  without a push, run the workflow's two commands into a scratch folder.
  The domain and HTTPS are set in the repository's Pages settings, not
  here; the DNS is ible.ai's. Until the domain is set the site is at
  https://ible.ai/mutible/ (the org's own site holds ible.ai, so
  ible-ai.github.io/mutible/ redirects there); every path in the template
  is relative so both work. A later page (the browser version's landing
  page) is a template of its own beside this one.
- Not notarized (that needs the paid Apple Developer Program);
  `scripts/sign-and-notarize.sh` is ready.
- The APK is signed with `~/.config/minicode/release.keystore` (password in the
  Keychain, `minicode-android-keystore`); every update must carry that
  signature, so never regenerate it (the user keeps a backup). Installed by
  hand or through Obtainium.
- Ubuntu PPA `ppa:echansen/minicode` (a lost key can be replaced on Launchpad).
  Traps: macOS `tar` adds `._*` files `dpkg-source` rejects (`git archive`);
  `tar` fails with ENOSYS under Docker Desktop's amd64 emulation; gpg-agent
  cannot be forwarded into Docker Desktop, so signing is on the host.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- PPA: Every Launchpad API query sends `Cache-Control: no-cache`.
- Distribution: Traps: macOS `tar` adds `._*` files `dpkg-source` rejects (`git archive`);
