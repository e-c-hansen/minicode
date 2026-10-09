# MiniCode on Ubuntu through a Launchpad PPA

The GTK 4 port is packaged for Ubuntu in `debian/` at the top of the
repository, and published as source packages to the PPA
`ppa:echansen/minicode`, where Launchpad builds the binaries. People on
Ubuntu then install and update MiniCode with apt.

## Installing it (for users)

    sudo add-apt-repository ppa:echansen/minicode
    sudo apt install minicode

That gives `minicode` on the PATH, MiniCode in the app grid, and its manual
page. Updates arrive with everything else: Software Updater, or
`sudo apt update && sudo apt upgrade`.

apt also installs the recommended packages by default: `gstreamer1.0-libav`
and the base and good GStreamer plugins (so H.264 video and AAC audio play),
`git` for the Source Control panel, and `curl` for downloading tectonic, the
LaTeX preview's typesetter, which is not in Ubuntu's archive and is fetched
into `~/.local/share/minicode/bin` the first time a `.tex` file is opened
and you ask for it. Language servers are suggestions: `sudo apt install
clangd` and the like, when you want them.

If you built MiniCode yourself earlier and installed it into your home
folder, remove that copy first, or it shadows the packaged one: `~/.local/bin`
comes before `/usr/bin` on the PATH, and GNOME prefers a launcher in
`~/.local/share/applications` over the system one of the same name.

    rm -f ~/.local/bin/minicode \
          ~/.local/share/applications/org.minicode.Editor.desktop \
          ~/.local/share/icons/hicolor/*/apps/org.minicode.Editor.png
    update-desktop-database ~/.local/share/applications
    gtk4-update-icon-cache -f -t ~/.local/share/icons/hicolor

Rebuild the icon cache, not just the launcher's. The old cache still lists
the deleted icons, and since it sits ahead of the system one, the dock looks
there, finds no file, and shows a black square instead of the icon.

To remove it again: `sudo apt remove minicode`, and
`sudo add-apt-repository --remove ppa:echansen/minicode` to drop the PPA.

The PPA builds for Ubuntu 26.04 LTS (resolute). Other releases are not
built; see "Other Ubuntu releases" below.

## One-time setup (for the maintainer)

Nothing is uploaded until this is done, and `scripts/release.sh` skips the
PPA step with a message until then. The steps, in order:

### 1. gpg on the Mac

Launchpad accepts only uploads signed by a key registered to the account
that owns the PPA. `scripts/ppa.sh` signs on the Mac with gpg; the key never
goes into the build container.

    brew install gnupg pinentry-mac
    mkdir -p ~/.gnupg && chmod 700 ~/.gnupg
    echo "pinentry-program $(brew --prefix)/bin/pinentry-mac" >> ~/.gnupg/gpg-agent.conf
    gpgconf --kill gpg-agent

### 2. A key (or an existing one)

To make one:

    gpg --full-generate-key

and answer: `(1) RSA and RSA`, `4096` bits, an expiry you are happy to renew
(for example `5y`), your real name, and the address you will use on
Launchpad. Give it a passphrase; macOS's pinentry offers to keep it in the
Keychain. RSA with a separate encryption subkey is what Launchpad is known to
take, and the encryption subkey matters: Launchpad confirms the key by
sending you an encrypted mail.

Then note the fingerprint (the 40 hex digits):

    gpg --list-secret-keys --keyid-format long

and send the public key to Ubuntu's keyserver, which is where Launchpad
fetches it from:

    gpg --keyserver hkps://keyserver.ubuntu.com --send-keys FINGERPRINT

It can take a few minutes before the keyserver serves it.

Back the secret key up somewhere safe (a password manager, say):

    gpg --export-secret-keys --armor FINGERPRINT > minicode-ppa-secret-key.asc

Losing it is not the disaster losing the Android release key would be: a new
key added to the Launchpad account works for the same PPA, and apt users
notice nothing, because what they trust is Launchpad's signing key for the
PPA, not yours.

### 3. A Launchpad account (username echansen)

Sign up at <https://login.launchpad.net/> (an Ubuntu One account), then log
in at <https://launchpad.net/> once so the Launchpad profile is created. The
PPA's address uses the Launchpad *username*, which is `echansen` (created
2026-10-07). Under another account, set `MINICODE_PPA=ppa:<name>/minicode`
when running the scripts and change the address in this file and in
BUILD-LINUX.md.

### 4. The key on the account

At <https://launchpad.net/~echansen/+editpgpkeys>, paste the fingerprint
and choose "Import Key". Launchpad mails an encrypted message to the
address on the key. Save the part from `-----BEGIN PGP MESSAGE-----` to
`-----END PGP MESSAGE-----` to a file, then

    gpg --decrypt message.txt

and open the link it contains. The key is then listed on your profile.

### 5. The Ubuntu Code of Conduct, if Launchpad asks

Launchpad has at times required signing the Ubuntu Code of Conduct before
it activates a PPA. If the next step asks for it, go to
<https://launchpad.net/codeofconduct>, download the current version, sign it
with `gpg --clearsign UbuntuCodeofConduct-2.0.txt`, and paste the contents of
the `.asc` file into the form. If it does not ask, skip this.

### 6. The PPA

At <https://launchpad.net/~echansen/+activate-ppa> create a PPA with URL
`minicode`, display name `MiniCode`, and a line of description (for example
"MiniCode, a small native code editor, for Ubuntu"). Then under the PPA's
"Change details":

- Processors: amd64 is on by default. Tick arm64 too if you want builds for
  ARM machines.
- "Publish debug symbols" makes the `minicode-dbgsym` package available, for
  readable crash traces. Optional.

### 7. Tell ppa.sh which key to use

    mkdir -p ~/.config/minicode
    echo FINGERPRINT > ~/.config/minicode/ppa-key-id

(or export `MINICODE_PPA_KEY=FINGERPRINT` instead). Only the id goes in that
file, never key material, and nothing under `~/.config/minicode` is in the
repository.

### 8. The first upload

Docker Desktop must be running (`open -a Docker`). Check everything first,
which signs the package (gpg asks for the passphrase) and runs dput's
checks but sends nothing:

    scripts/ppa.sh --simulate 1.4.12

then upload the current release for real:

    scripts/ppa.sh 1.4.12

Launchpad mails the address on the key when the upload is accepted (or
rejected, with the reason), then builds it; the build log and status are at
<https://launchpad.net/~echansen/+archive/ubuntu/minicode/+packages>. A
build takes a few minutes, and publishing another few after that, before
`apt install minicode` finds it.

### 9. A second machine (the ThinkPad)

Any Ubuntu machine can upload too, with a key of its own: Launchpad takes
several keys per account, and each machine keeps its secret key to itself.
The ThinkPad's is `AEE321CE250697CFEB921ED8EEF9635EDB7F9335`, made on
2026-10-09; the Mac's is `340F7652C7B877AB7F1CB3E3C45D7F425A211F40`. To set
up another:

1. Make the key with the same shape as the Mac's (RSA 4096, an encryption
   subkey, a comment saying which machine), with GNOME's pinentry asking
   for the passphrase:

       gpg --batch --generate-key <<'EOF'
       Key-Type: RSA
       Key-Length: 4096
       Key-Usage: sign
       Subkey-Type: RSA
       Subkey-Length: 4096
       Subkey-Usage: encrypt
       Name-Real: Eric Hansen
       Name-Comment: MiniCode releases, ThinkPad
       Name-Email: eric.calvin.hansen@gmail.com
       Expire-Date: 5y
       %commit
       EOF

2. Send it to the keyserver and add it on Launchpad, as in steps 2 and 4
   (the confirmation mail is decrypted on the new machine, with the new key).
3. Put its fingerprint in `~/.config/minicode/ppa-key-id`, as in step 7.
4. Install the tools the container has, since there is no Docker there:

       sudo apt install --no-install-recommends debhelper devscripts \
           distro-info dput libdistro-info-perl lintian

5. Check with `scripts/ppa.sh --simulate` on the latest release.

ppa.sh builds in the container whenever Docker is running, and otherwise
runs the same steps directly in a scratch folder, which needs those tools.
The changelog's signer is the key's name without its comment, so uploads
from either machine read the same.

## How a release flows

`scripts/release.sh 1.4.13` does what it always did (tests, stamping, the
zip, the APK, the GitHub release, the cask) and then, as its last step, runs
`scripts/ppa.sh 1.4.13` when a key is configured. With no key it prints that
it skipped the PPA and why. With a key it checks at the start that gpg has
the secret key and that Docker is running, before anything is pushed, and
`MINICODE_NO_PPA=1` releases without the PPA. If the upload itself fails,
the rest of the release is already out, and `scripts/ppa.sh 1.4.13` retries
it alone.

`scripts/release.sh --linux 1.4.13` is the release for a change only Ubuntu
users would notice, and it runs on the ThinkPad as well as the Mac: it runs
the tests, stamps the version into `Info.plist` (the Linux build reads its
version from there), commits, pushes `main` with the tag `v1.4.13`, and
uploads to the PPA. There is no GitHub Release, zip, APK or cask, so
Homebrew users and Obtainium stay on the last full release; Obtainium in
particular would find no APK in a release without one. The next full
release takes a higher version and brings the change to the Mac and Android.
Either kind of release refuses a version whose tag already exists.

`scripts/ppa.sh VERSION` does this:

1. Makes `minicode_VERSION.orig.tar.gz` with `git archive` of the tag
   `vVERSION`, leaving out `debian/` (the tag is fetched from GitHub first if
   it is not in the local repository, since `gh release create` makes it
   there), and compresses it with zlib at level 9 and a header with no name
   or time, which is what the Mac's `gzip -n -9` did. That gives the same
   bytes every time and on either machine, which matters: Launchpad refuses
   a second tarball of the same name with different contents. Ubuntu's GNU
   `gzip` deflates with code of its own, and its tarball for 1.4.12 did not
   match the one the PPA holds.
2. Takes `debian/` from HEAD as committed (uncommitted changes there are left
   out, with a warning).
3. In an `ubuntu:26.04` container (`packaging/ppa/Dockerfile`, built on first
   use, about 780 MB) that sees only those two files, read-only, writes the
   changelog entry with `dch` from the version on the command line, builds
   the source package with `dpkg-buildpackage -S`, and runs lintian on it,
   stopping on any error or warning. Without Docker, the same steps run
   directly in a scratch folder (step 9 above).
4. Copies the `.dsc` out, signs it on the host with gpg, copies it back,
   regenerates the `.changes` so it carries the signed `.dsc`'s checksums,
   and signs that the same way.
5. Uploads with `dput ppa:echansen/minicode` from where it built.

The committed `debian/changelog` is only a placeholder, numbered `0~local-0`
so that apt replaces anything built from it; each upload's changelog is the
single entry ppa.sh writes. The release history lives in git and on the PPA.

### Version numbers

An upload of release 1.4.13 is numbered `1.4.13-1~ppa1~resolute1`:

- `1.4.13` is the release, from the command line.
- `-1` is the Debian revision, always 1 here.
- `~ppa1` counts uploads of this release. Launchpad never accepts the same
  version twice, even after a deletion, so a packaging fix for a release
  already uploaded goes out as `scripts/ppa.sh 1.4.13 2`, which makes
  `~ppa2`; ppa.sh refuses a number the PPA already has. The tilde sorts
  before anything, so `1.4.13-1~ppa2` is newer than `~ppa1` but older than a
  plain `1.4.13-1`, should Ubuntu itself ever package MiniCode.
- `~resolute1` names the Ubuntu series, so the same release can go to
  several series with distinct versions, and an older series' build
  (`~noble1`) sorts below a newer one's, which keeps upgrades between Ubuntu
  releases moving forward.

Every later release sorts above every upload of an earlier one, whatever
its `~ppaN`, so apt always upgrades to the newest release.

### Other Ubuntu releases

`MINICODE_PPA_SERIES=noble scripts/ppa.sh 1.4.13` would build for 24.04, but
that has not been tried: 24.04 has GTK 4.14, VTE 0.76 and WebKitGTK 2.44,
older than the 26.04 versions the port was developed and checked against,
and Launchpad's build log is where you would find out whether it compiles.

## Checking the packaging without uploading

    scripts/ppa.sh --dry-run 1.4.12

builds the unsigned source package into `build/ppa/` and lints it, with no
key and no upload. To build the binary package from it the way Launchpad
does (the build dependencies installed without their recommends, the build
itself with no network and as an ordinary user):

    docker run --name mc-deps -v "$PWD/build/ppa:/src:ro" ubuntu:26.04 bash -c '
      apt-get update && apt-get install -y --no-install-recommends build-essential
      cp /src/* /tmp && apt-get build-dep -y --no-install-recommends /tmp/minicode_*.dsc
      useradd -m builder'
    docker commit mc-deps minicode-builddeps && docker rm mc-deps
    docker run --rm --network none -v "$PWD/build/ppa:/src:ro" minicode-builddeps bash -c '
      mkdir /b && chown builder /b && su builder -c "cd /b && cp /src/* . &&
        dpkg-source -x minicode_*.dsc m && cd m && dpkg-buildpackage -b -us -uc"'

The image with the build dependencies is about 1.9 GB; remove it afterwards
with `docker rmi minicode-builddeps`.

## What the package holds

- `/usr/bin/minicode`, built with every panel forced on (terminal, browser,
  PDF and LaTeX preview, web pictures in Markdown), so a missing build
  dependency fails the build rather than quietly dropping a panel.
- `/usr/share/applications/org.minicode.Editor.desktop`, with
  `Exec=/usr/bin/minicode %f`, and the icon at five sizes under
  `/usr/share/icons/hicolor/*/apps/org.minicode.Editor.png`. The launcher,
  the icons and the GTK application id share the name `org.minicode.Editor`,
  which is how GNOME matches the window to its dock icon (see
  BUILD-LINUX.md).
- The manual page, `minicode(1)`.
- `minicode-dbgsym`, the debugging symbols, built automatically.

The build runs the port's tests (`meson test`) and the core suite (`make
test` at the top level); neither needs a display or the network, and
Launchpad's builders have no network. `DEB_BUILD_OPTIONS=nocheck` skips them.

## Troubleshooting

- **dput cannot connect.** It uploads over FTP to `ppa.launchpad.net`,
  which some networks block. dput can also upload over SFTP
  (`ssh-ppa:echansen/minicode`, with an SSH key on the Launchpad profile),
  but ppa.sh does not set that up, since the container has no SSH key; try
  another network first.
- **Rejected: "Signer has no upload rights" or "not a valid key".** The key
  that signed is not on the Launchpad account that owns the PPA (step 4), or
  `ppa-key-id` names a different key.
- **Rejected: "File minicode_X.orig.tar.gz already exists ... different
  contents".** The tarball for that release was made differently before (a
  different git version can change `git archive`'s output). Upload the
  revision with `-sd`, which leaves the tarball out: ppa.sh does that for
  any `~ppa2` and later.
- **Rejected: the version is already there.** Give a higher revision:
  `scripts/ppa.sh 1.4.13 2`.
