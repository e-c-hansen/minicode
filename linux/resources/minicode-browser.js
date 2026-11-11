#!/bin/sh
":" //; if [ -n "$MINICODE_EXE" ] && [ -x "$MINICODE_EXE" ]; then exec "$MINICODE_EXE" open-url -- "$@"; fi; for u in "$@"; do :; done; exec xdg-open "$u"

// minicode-browser.js: what BROWSER names in minicode's terminals (Terminal.cpp),
// so that a program opening a web page opens it in minicode (the Mac's
// resources/minicode-browser.js; PARITY.md 3.7).
//
// Most programs run $BROWSER with the address as its argument: Claude Code
// when a link is clicked, and Python's webbrowser. They run this file as a
// shell script, and the line above hands the address to `minicode open-url`
// (MINICODE_EXE, the app's own binary). Vite (--open) and Create React App
// instead run a BROWSER whose name ends in .js with node, passing their own
// arguments first and the address last, so the file is JavaScript too: to
// node the line above is a string and a comment. `minicode open-url` loads a
// local page in the browser pane of the terminal's window and sends any other
// address, or any address when minicode does not answer, to xdg-open.
const load = (name) =>
    typeof require === "function" ? require(name) : process.getBuiltinModule(name);
const { spawnSync } = load("child_process");

if (process.argv.length < 3) process.exit(2);
const url = process.argv[process.argv.length - 1];
const exe = process.env.MINICODE_EXE;
const r = exe ? spawnSync(exe, ["open-url", "--", url], { stdio: "inherit" })
              : spawnSync("xdg-open", [url], { stdio: "inherit" });
process.exit(r.status === null ? 1 : r.status);
