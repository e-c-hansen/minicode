#!/bin/sh
":" //; d=$(dirname "$0"); for c in "$d/../MacOS/minicode-cli" "$d/minicode-cli"; do if [ -x "$c" ]; then exec "$c" open-url -- "$@"; fi; done; for u in "$@"; do :; done; exec /usr/bin/open "$u"

// minicode-browser.js: what BROWSER names in minicode's terminals
// (Terminal.mm), so that a program opening a web page opens it in minicode.
//
// Most programs run $BROWSER with the address as its argument: Claude Code
// when a link is clicked, and Python's webbrowser. They run this file as a
// shell script, and the line above hands the address to `minicode open-url`
// (the client beside the app in the bundle, or beside this file in build/).
// Vite (--open) and Create React App instead run a BROWSER whose name ends
// in .js with node, passing their own arguments first and the address last,
// so the file is JavaScript too: to node the line above is a string and a
// comment. `minicode open-url` loads a local page in the browser pane of
// the terminal's window and sends any other address, or any address when
// minicode does not answer, to the default browser.
const load = (name) =>
    typeof require === "function" ? require(name) : process.getBuiltinModule(name);
const { spawnSync } = load("child_process");
const fs = load("fs");
const path = load("path");

if (process.argv.length < 3) process.exit(2);
const here = path.dirname(fs.realpathSync(process.argv[1]));
const url = process.argv[process.argv.length - 1];
const cli = [path.join(here, "..", "MacOS", "minicode-cli"), path.join(here, "minicode-cli")].find(
    (p) => {
        try {
            fs.accessSync(p, fs.constants.X_OK);
            return true;
        } catch (e) {
            return false;
        }
    });
const r = cli ? spawnSync(cli, ["open-url", "--", url], { stdio: "inherit" })
              : spawnSync("/usr/bin/open", [url], { stdio: "inherit" });
process.exit(r.status === null ? 1 : r.status);
