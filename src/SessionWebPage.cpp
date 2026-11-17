// SessionWebPage.cpp — the one page a browser's view of a session loads
// (SessionWeb.h): its CSS and script, no third-party code. The script draws
// only what the server describes (tabs, lines with the highlighter's spans,
// a Markdown page, terminal rows of styled runs) and sends only which
// document and terminal it shows. A Markdown page goes into a sandboxed
// frame with no scripts. "{{NONCE}}" is the response's script nonce.
#include "SessionWeb.h"

namespace Shared {
namespace Web {

const std::string& pageHtml() {
    static const std::string page = R"PAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<title>minicode</title>
<style>
:root { --bg: #1f1f1f; --fg: #cccccc; --tbg: #181818; --tfg: #cccccc; --muted: #9d9d9d;
        --line: #2b2b2b; --accent: #4ea1f7; --lh: 1.4em; }
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; }
body { background: var(--bg); color: var(--fg); display: flex; flex-direction: column;
       font: 13px -apple-system, BlinkMacSystemFont, "Segoe UI", Ubuntu, sans-serif; }
header { display: flex; gap: 10px; align-items: baseline; padding: 6px 10px;
         border-bottom: 1px solid var(--line); white-space: nowrap; overflow: hidden; }
#machine { font-weight: 600; }
#folder { color: var(--muted); overflow: hidden; text-overflow: ellipsis; direction: rtl;
          text-align: left; flex: 1; min-width: 0; }
#status { color: var(--muted); font-size: 12px; }
#status.live::before { content: "\25CF "; color: #3fb950; }
.strip { display: flex; overflow-x: auto; border-bottom: 1px solid var(--line);
         scrollbar-width: none; flex: none; }
.strip button { background: none; border: 0; border-right: 1px solid var(--line);
                color: var(--muted); font: inherit; padding: 6px 12px; white-space: nowrap;
                cursor: pointer; }
.strip button.shown { color: var(--fg); background: var(--tbg);
                      box-shadow: inset 0 1px 0 var(--accent); }
.strip button.onshow::after { content: " \2022"; color: var(--accent); }
.strip button.dirty::before { content: "\25CF "; font-size: 9px; vertical-align: middle; }
.strip button.busy::after { content: " \2026"; }
.strip .empty { color: var(--muted); padding: 6px 12px; }
main { flex: 1; display: flex; flex-direction: column; min-height: 0; }
#docsec { flex: 3; display: flex; flex-direction: column; min-height: 0; }
#termsec { flex: 2; display: flex; flex-direction: column; min-height: 0;
           border-top: 1px solid var(--line); background: var(--tbg); color: var(--tfg); }
#termsec.none { display: none; }
#docpath { color: var(--muted); font-size: 12px; padding: 3px 10px; white-space: nowrap;
           overflow: hidden; text-overflow: ellipsis; flex: none; }
#code, #term { flex: 1; overflow: auto; min-height: 0;
               font: 12px/var(--lh) ui-monospace, SFMono-Regular, Menlo, "DejaVu Sans Mono", monospace; }
#code { counter-reset: line; padding-bottom: 2em; }
#code .l { white-space: pre; min-height: var(--lh); padding-right: 1em; }
#code .l::before { counter-increment: line; content: counter(line); display: inline-block;
                   width: 4.5em; padding-right: 1.2em; text-align: right; color: var(--muted);
                   opacity: .6; user-select: none; }
#md { flex: 1; border: 0; width: 100%; background: #fff; min-height: 0; }
#note { padding: 2em 1em; color: var(--muted); }
.hidden { display: none !important; }
#term { padding: 4px 6px; }
#term .r { white-space: pre; min-height: var(--lh); }
#screen { position: relative; }
#cursor { position: absolute; width: 1ch; height: var(--lh); background: var(--tfg);
          opacity: .55; pointer-events: none; }
#banner { padding: 6px 10px; background: #5a1d1d; color: #fff; }
@media (max-width: 700px) {
  body { font-size: 14px; }
  #code, #term { font-size: 11px; }
  #code .l { white-space: pre-wrap; word-break: break-all; padding-left: 3.4em;
             text-indent: -3.4em; }
  #code .l::before { width: 3.4em; padding-right: .6em; text-indent: 0; }
  #docsec { flex: 1.3; }
  #termsec { flex: 1; }
}
</style>
</head>
<body>
<header><span id="machine">minicode</span><span id="folder"></span><span id="status">connecting</span></header>
<div id="banner" class="hidden"></div>
<nav id="tabs" class="strip"></nav>
<main>
  <section id="docsec">
    <div id="docpath"></div>
    <div id="code"></div>
    <iframe id="md" class="hidden" sandbox="" title="Markdown page"></iframe>
    <div id="note" class="hidden"></div>
  </section>
  <section id="termsec" class="none">
    <nav id="termtabs" class="strip"></nav>
    <div id="term"><div id="hist"></div><div id="screen"><div id="cursor" class="hidden"></div></div></div>
  </section>
</main>
<script nonce="{{NONCE}}">
"use strict";
(function () {
  const $ = (id) => document.getElementById(id);
  const token = new URLSearchParams(location.search).get("t") || "";
  let ws = null, palette = null, state = null, ended = false, folder = "";
  const want = { doc: 0, term: 0 };
  let followDoc = true, followTerm = true, docKind = "", termInfo = null;
  const kHist = 5000;

  function base(p) { const i = p.lastIndexOf("/"); return i < 0 ? p : p.slice(i + 1); }
  function inFolder(p) { return folder && p.startsWith(folder + "/") ? p.slice(folder.length + 1) : p; }
  function color(n) { return "#" + (n >>> 0).toString(16).padStart(6, "0"); }

  function applyPalette(p) {
    palette = p;
    const root = document.documentElement.style;
    root.setProperty("--bg", p.background); root.setProperty("--fg", p.text);
    root.setProperty("--tbg", p.terminalBackground); root.setProperty("--tfg", p.terminalText);
    let css = "";
    p.syntax.forEach((c, i) => { if (i) css += "#code .s" + i + "{color:" + c + "}"; });
    const style = document.createElement("style");
    style.textContent = css;
    document.head.appendChild(style);
  }

  // Says what this page shows, when that changed since it last said.
  let said = "";
  function send() {
    const text = JSON.stringify({ t: "show", doc: want.doc, term: want.term });
    if (!ws || ws.readyState !== 1 || text === said) return;
    said = text;
    ws.send(text);
  }

  // ---- tabs
  function sessionDoc() {
    if (!state) return 0;
    for (const w of state.windows) {
      const g = w.groups && w.groups[w.focused || 0];
      if (g && g.active) return g.active;
    }
    return state.docs.length ? state.docs[0].id : 0;
  }
  function tabButton(label, title, cls, onclick) {
    const b = document.createElement("button");
    b.textContent = label; b.title = title; b.className = cls; b.onclick = onclick;
    return b;
  }
  function drawTabs() {
    const strip = $("tabs");
    strip.textContent = "";
    const dirty = new Map(state.docs.map((d) => [d.id, d.dirty]));
    const seen = new Set();
    const onShow = sessionDoc();
    const add = (id, path) => {
      if (seen.has(id)) return;
      seen.add(id);
      let cls = id === want.doc ? "shown" : "";
      if (id === onShow) cls += " onshow";
      if (dirty.get(id)) cls += " dirty";
      strip.appendChild(tabButton(base(path) || "untitled", path, cls, () => {
        followDoc = id === sessionDoc();
        showDoc(id);
      }));
    };
    for (const w of state.windows)
      for (const g of w.groups || []) for (const t of g.tabs || []) add(t.doc, t.path);
    for (const d of state.docs) add(d.id, d.path);
    if (!seen.size) {
      const e = document.createElement("span");
      e.className = "empty"; e.textContent = "No documents are open in this session.";
      strip.appendChild(e);
    }
  }
  function drawTermTabs() {
    const strip = $("termtabs");
    strip.textContent = "";
    $("termsec").classList.toggle("none", !state.terms.length);
    state.terms.forEach((t, i) => {
      const label = t.title || (t.cwd ? base(t.cwd.replace(/\/+$/, "")) : "") || "Terminal " + (i + 1);
      let cls = t.id === want.term ? "shown" : "";
      if (t.busy) cls += " busy";
      strip.appendChild(tabButton(label, t.busy && t.command ? t.command : t.cwd || label, cls, () => {
        followTerm = i === 0;
        showTerm(t.id);
      }));
    });
  }
  function showDoc(id) { if (id !== want.doc) { want.doc = id; send(); } drawTabs(); }
  function showTerm(id) { if (id !== want.term) { want.term = id; send(); } drawTermTabs(); }

  function onState(m) {
    state = m;
    const docs = new Set(m.docs.map((d) => d.id));
    if (followDoc || !docs.has(want.doc)) { followDoc = true; want.doc = sessionDoc(); }
    const terms = m.terms.map((t) => t.id);
    if (followTerm && terms.length && !terms.includes(want.term)) want.term = terms[0];
    if (!terms.includes(want.term)) { followTerm = true; want.term = terms.length ? terms[0] : 0; }
    send();
    drawTabs(); drawTermTabs();
    if (!want.doc) showNote("No document is on show.");
  }

  // ---- documents
  function showNote(text) {
    $("code").classList.add("hidden"); $("md").classList.add("hidden");
    const n = $("note"); n.textContent = text; n.classList.remove("hidden");
    $("docpath").textContent = "";
    docKind = "";
  }
  function lineEl(l) {
    const d = document.createElement("div");
    d.className = "l";
    const text = l[0];
    let at = 0;
    for (const s of l[1]) {
      if (s[0] > at) d.appendChild(document.createTextNode(text.slice(at, s[0])));
      const e = document.createElement("span");
      e.className = "s" + s[2];
      e.textContent = text.slice(s[0], s[0] + s[1]);
      d.appendChild(e);
      at = s[0] + s[1];
    }
    if (at < text.length) d.appendChild(document.createTextNode(text.slice(at)));
    return d;
  }
  function onDoc(m) {
    if (m.id !== want.doc) return;
    docKind = m.kind;
    $("docpath").textContent = inFolder(m.path);
    $("note").classList.add("hidden");
    const code = $("code"), md = $("md");
    if (m.kind === "code") {
      md.classList.add("hidden"); md.removeAttribute("srcdoc");
      const top = code.scrollTop;
      const f = document.createDocumentFragment();
      for (const l of m.lines) f.appendChild(lineEl(l));
      code.textContent = "";
      code.appendChild(f);
      code.classList.remove("hidden");
      code.scrollTop = top;
      code.dataset.version = m.version;
    } else if (m.kind === "markdown") {
      code.classList.add("hidden"); code.textContent = "";
      md.srcdoc = m.html;
      md.classList.remove("hidden");
    } else {
      showNote(m.note);
      $("docpath").textContent = inFolder(m.path);
    }
  }
  function onLines(m) {
    if (m.id !== want.doc || docKind !== "code") return;
    const code = $("code");
    for (const e of m.edits) {
      const at = e[0], del = e[1];
      for (let i = 0; i < del && code.children[at]; i++) code.children[at].remove();
      const f = document.createDocumentFragment();
      for (const l of e[2]) f.appendChild(lineEl(l));
      code.insertBefore(f, code.children[at] || null);
    }
    code.dataset.version = m.version;
  }

  // ---- terminals
  function runEls(line) {
    const f = document.createDocumentFragment();
    for (const r of line) {
      if (r.length === 1) { f.appendChild(document.createTextNode(r[0])); continue; }
      const e = document.createElement("span");
      e.textContent = r[0];
      let fg = r[1] >= 0 ? color(r[1]) : "", bg = r[2] >= 0 ? color(r[2]) : "";
      const fl = r[3];
      if (fl & 16) { const t = fg || "var(--tfg)"; fg = bg || "var(--tbg)"; bg = t; }
      if (fg) e.style.color = fg;
      if (bg) e.style.background = bg;
      if (fl & 1) e.style.fontWeight = "bold";
      if (fl & 2) e.style.opacity = ".6";
      if (fl & 4) e.style.fontStyle = "italic";
      const deco = [];
      if (fl & 8) deco.push("underline");
      if (fl & 32) deco.push("line-through");
      if (deco.length) e.style.textDecoration = deco.join(" ");
      if (fl & 64) e.style.color = "transparent";
      f.appendChild(e);
    }
    return f;
  }
  function rowEl(line) { const d = document.createElement("div"); d.className = "r"; d.appendChild(runEls(line)); return d; }
  function atBottom() { const t = $("term"); return t.scrollHeight - t.scrollTop - t.clientHeight < 8; }
  function placeCursor(c) {
    const k = $("cursor");
    k.classList.toggle("hidden", !c[2]);
    k.style.top = "calc(" + c[0] + " * var(--lh))";
    k.style.left = c[1] + "ch";
  }
  function addHist(lines) {
    const h = $("hist");
    for (const l of lines) h.appendChild(rowEl(l));
    while (h.children.length > kHist) h.firstChild.remove();
  }
  function onTerm(m) {
    if (m.id !== want.term) return;
    const stick = atBottom();
    termInfo = m;
    const h = $("hist"), s = $("screen"), k = $("cursor");
    h.textContent = "";
    addHist(m.hist);
    s.textContent = "";
    for (const l of m.screen) s.appendChild(rowEl(l));
    s.appendChild(k);
    s.style.minWidth = m.cols + "ch";
    placeCursor(m.cursor);
    if (stick) $("term").scrollTop = $("term").scrollHeight;
  }
  function onTermDelta(m) {
    if (m.id !== want.term || !termInfo) return;
    const stick = atBottom();
    addHist(m.hist);
    const s = $("screen");
    for (const [row, line] of m.rows) {
      const old = s.children[row];
      if (old && old.id !== "cursor") s.replaceChild(rowEl(line), old);
    }
    placeCursor(m.cursor);
    if (stick) $("term").scrollTop = $("term").scrollHeight;
  }

  // ---- the socket
  function setStatus(text, live) { const s = $("status"); s.textContent = text; s.classList.toggle("live", !!live); }
  function banner(text) { const b = $("banner"); b.textContent = text; b.classList.toggle("hidden", !text); }
  function connect() {
    const scheme = location.protocol === "https:" ? "wss://" : "ws://";
    ws = new WebSocket(scheme + location.host + "/ws?t=" + encodeURIComponent(token));
    ws.onopen = () => { said = ""; setStatus("live, read-only", true); banner(""); };
    ws.onmessage = (ev) => {
      const m = JSON.parse(ev.data);
      switch (m.t) {
        case "hello":
          $("machine").textContent = m.machine || "minicode";
          folder = m.folder;
          // Marks keep the path's slashes where they belong in a field
          // that cuts from the left.
          $("folder").textContent = "\u200e" + m.folder + "\u200e";
          document.title = (m.machine ? m.machine + ": " : "") + base(m.folder) + " (minicode)";
          if (!palette) applyPalette(m.palette);
          break;
        case "state": onState(m); break;
        case "doc": onDoc(m); break;
        case "lines": onLines(m); break;
        case "gone": if (m.id === want.doc) { showNote("This document was closed."); } break;
        case "term": onTerm(m); break;
        case "termd": onTermDelta(m); break;
        case "ended": ended = true; banner(m.why); setStatus("ended"); break;
      }
    };
    ws.onclose = () => {
      ws = null;
      if (ended) return;
      setStatus("reconnecting");
      termInfo = null;
      setTimeout(connect, 2000);
    };
  }
  window.mcSessionView = { want: want, state: () => state };
  connect();
})();
</script>
</body>
</html>
)PAGE";
    return page;
}

}  // namespace Web
}  // namespace Shared
