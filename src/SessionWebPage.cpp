// SessionWebPage.cpp — the one page a browser's view of a session loads
// (SessionWeb.h): its CSS and script, no third-party code. The script draws
// only what the server describes (tabs, lines with the highlighter's spans,
// a Markdown page, terminal rows of styled runs, the folder's tree in a
// sidebar, listed by the host one folder at a time). Read-only, it sends
// only which document and terminal it shows, which folders to list, and
// which file to open in the window. Editable, it is a small editor of
// its own: a caret and selection drawn over the lines, a hidden field at the
// caret for keys, input methods and the clipboard, edits made on its lines
// at once and sent with the version they were made on (moved past others'
// with the same rules as src/Ot.cpp), undo and save asked of the server,
// and keys for the terminal on show. A Markdown page goes into a sandboxed
// frame with no scripts. "{{NONCE}}" is the response's script nonce. Every
// color and the font sizes come from the host's settings (paletteJson), and a
// "palette" message recolors the page as it is.
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
<style id="theme"></style>
<style>
:root { --bg: #1f1f1f; --fg: #cccccc; --tbg: #181818; --tfg: #cccccc; --muted: #9d9d9d;
        --sbg: #181818; --sfg: #cccccc; --ign: #8c8c8c;
        --line: #2b2b2b; --accent: #4ea1f7; --lh: 1.4em; --hbg: #181818; --hfg: #cccccc;
        --gutter: #1f1f1f; --ln: #6e7681; --lnact: #cccccc; --efs: 13px; --tfs: 12px;
        --sfs: 13px; --selbg: rgba(38, 79, 120, .9); }
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; }
body { background: var(--bg); color: var(--fg); display: flex; flex-direction: column;
       font: var(--sfs) -apple-system, BlinkMacSystemFont, "Segoe UI", Ubuntu, sans-serif; }
header { display: flex; gap: 10px; align-items: baseline; padding: 6px 10px;
         background: var(--hbg); color: var(--hfg);
         border-bottom: 1px solid var(--line); white-space: nowrap; overflow: hidden; }
#machine { font-weight: 600; }
#folder { color: var(--muted); overflow: hidden; text-overflow: ellipsis; direction: rtl;
          text-align: left; flex: 1; min-width: 0; }
#status { color: var(--muted); font-size: 12px; }
#status.live::before { content: "\25CF "; color: #3fb950; }
.strip { display: flex; overflow-x: auto; border-bottom: 1px solid var(--line);
         scrollbar-width: none; flex: none; background: var(--hbg); }
.strip button { background: none; border: 0; border-right: 1px solid var(--line);
                color: var(--muted); font: inherit; padding: 6px 12px; white-space: nowrap;
                cursor: pointer; }
.strip button.shown { color: var(--fg); background: var(--bg);
                      box-shadow: inset 0 1px 0 var(--accent); }
.strip button.onshow::after { content: " \2022"; color: var(--accent); }
.strip button.dirty::before { content: "\25CF "; font-size: 9px; vertical-align: middle; }
.strip button.busy::after { content: " \2026"; }
.strip .empty { color: var(--muted); padding: 6px 12px; }
#wrap { flex: 1; display: flex; min-height: 0; position: relative; }
#side { flex: none; width: clamp(180px, 22vw, 320px); display: flex; flex-direction: column;
        background: var(--sbg); color: var(--sfg); border-right: 1px solid var(--line);
        min-height: 0; }
body.sideclosed #side { display: none; }
#sidebar { display: flex; align-items: center; gap: 6px; padding: 4px 6px 4px 10px; flex: none;
           font-size: 11px; text-transform: uppercase; letter-spacing: .04em; }
#sidename { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
            font-weight: 600; }
.iconbtn { background: none; border: 0; color: inherit; font: inherit; font-size: 14px;
           padding: 0 6px; cursor: pointer; opacity: .75; line-height: 1.4; }
.iconbtn:hover { opacity: 1; }
#sidebtn { color: var(--hfg); font-size: 15px; padding: 0 4px; align-self: center; }
#tree { flex: 1; overflow: auto; min-height: 0; padding-bottom: 1em; }
#tree .row { display: flex; align-items: center; white-space: nowrap; cursor: pointer;
             line-height: 22px; padding-right: 8px; user-select: none; }
#tree .row:hover { background: rgba(127, 127, 127, .12); }
#tree .row.current { background: rgba(127, 127, 127, .24); }
#tree .chev { flex: none; width: 16px; text-align: center; font-size: 11px; opacity: .75; }
#tree .name { overflow: hidden; text-overflow: ellipsis; }
#tree .row.ignored .name { color: var(--ign); }
#tree .row.outside { cursor: default; opacity: .55; }
#tree .row.link .name { font-style: italic; }
#tree .more, #tree .err { color: var(--muted); font-size: 12px; line-height: 20px;
                          white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#treenote { margin: 0; padding: 6px 10px; color: var(--muted); font-size: 12px; flex: none;
            border-top: 1px solid var(--line); }
#work { flex: 1; display: flex; flex-direction: column; min-width: 0; min-height: 0; }
main { flex: 1; display: flex; flex-direction: column; min-height: 0; }
#docsec { flex: 3; display: flex; flex-direction: column; min-height: 0; }
#termsec { flex: 2; display: flex; flex-direction: column; min-height: 0;
           border-top: 1px solid var(--line); background: var(--tbg); color: var(--tfg); }
#termsec.none { display: none; }
#docbar { display: flex; align-items: center; gap: 8px; flex: none; padding: 3px 10px; }
#docpath { color: var(--muted); font-size: 12px; white-space: nowrap; overflow: hidden;
           text-overflow: ellipsis; flex: 1; min-width: 0; }
#modebtn { background: none; border: 1px solid var(--line); color: var(--fg); font: inherit;
           font-size: 12px; padding: 1px 8px; border-radius: 3px; cursor: pointer; flex: none; }
#code, #term { flex: 1; overflow: auto; min-height: 0;
               font: var(--efs)/var(--lh) ui-monospace, SFMono-Regular, Menlo, "DejaVu Sans Mono", monospace; }
#term { font-size: var(--tfs); }
#termsec .strip { background: var(--tbg); }
#termsec .strip button.shown { background: var(--tbg); color: var(--tfg); }
#code { counter-reset: line; padding-bottom: 2em; position: relative; cursor: text; }
#lines { position: relative; z-index: 1; }
#selection { position: absolute; left: 0; top: 0; width: 0; height: 0; z-index: 0;
             pointer-events: none; }
#selection div { position: absolute; background: var(--selbg); }
#caret { position: absolute; width: 2px; background: var(--fg); z-index: 2; pointer-events: none;
         opacity: .45; animation: blink 1.06s steps(1) infinite; }
#code.focused #caret { opacity: 1; }
body:not(.editable) #caret { display: none; }
@keyframes blink { 50% { visibility: hidden; } }
#input { position: absolute; left: 0; top: 0; z-index: 3; width: 2px; height: var(--lh);
         padding: 0; margin: 0; border: 0; outline: none; resize: none; overflow: hidden;
         opacity: 0; white-space: pre; font: inherit; line-height: var(--lh);
         background: transparent; color: var(--fg); caret-color: transparent;
         pointer-events: none; }
#input.composing { opacity: 1; width: 24em; background: var(--bg); outline: 1px solid var(--accent); }
#toast { position: fixed; right: 12px; bottom: 12px; max-width: 60ch; padding: 8px 12px;
         background: var(--hbg); color: var(--hfg); border: 1px solid var(--line);
         border-radius: 4px; z-index: 10; }
#term:focus { outline: 1px solid var(--accent); outline-offset: -1px; }
#code .l { white-space: pre; min-height: var(--lh); padding-right: 1em; }
#code .l::before { counter-increment: line; content: counter(line); display: inline-block;
                   width: 4.5em; padding-right: 1.2em; text-align: right; color: var(--ln);
                   background: var(--gutter); user-select: none; }
#code.wrap .l { white-space: pre-wrap; overflow-wrap: anywhere; padding-left: 4.5em;
                text-indent: -4.5em; }
#code.wrap .l::before { text-indent: 0; }
#md { flex: 1; border: 0; width: 100%; background: var(--bg); min-height: 0; }
#note { padding: 2em 1em; color: var(--muted); }
#termnote { margin: 0; padding: 6px 10px; color: var(--muted); font-size: 12px;
            border-top: 1px solid #333333; }
.hidden { display: none !important; }
#term { padding: 4px 6px; }
#term .r { white-space: pre; min-height: var(--lh); }
#screen { position: relative; }
#cursor { position: absolute; width: 1ch; height: var(--lh); background: var(--tfg);
          opacity: .55; pointer-events: none; }
#banner { padding: 6px 10px; background: #5a1d1d; color: #fff; }
@media (max-width: 700px) {
  body { font-size: 14px; }
  #code, #term { font-size: calc(var(--efs) - 1px); }
  #code .l { white-space: pre-wrap; word-break: break-all; padding-left: 3.4em;
             text-indent: -3.4em; }
  #code .l::before { width: 3.4em; padding-right: .6em; text-indent: 0; }
  #docsec { flex: 1.3; }
  #termsec { flex: 1; }
  /* The tree is behind the header's button, over the page while open. */
  #side { display: none; position: absolute; left: 0; top: 0; bottom: 0; z-index: 20;
          width: min(85vw, 340px); box-shadow: 2px 0 12px rgba(0, 0, 0, .35); }
  body.sideopen #side { display: flex; }
  #tree .row { line-height: 30px; }
}
</style>
</head>
<body>
<header><button id="sidebtn" class="iconbtn" type="button" title="Files" aria-label="Files" aria-expanded="true">&#9776;</button><span id="machine">minicode</span><span id="folder"></span><span id="status">connecting</span></header>
<div id="banner" class="hidden"></div>
<div id="wrap">
<aside id="side" aria-label="Files">
  <div id="sidebar"><span id="sidename">Files</span><button id="refresh" class="iconbtn" type="button" title="Refresh the tree" aria-label="Refresh the tree">&#8635;</button></div>
  <div id="tree" role="tree"></div>
  <p id="treenote" class="hidden"></p>
</aside>
<div id="work">
<nav id="tabs" class="strip"></nav>
<main>
  <section id="docsec">
    <div id="docbar"><span id="docpath"></span><button id="modebtn" class="hidden" type="button"></button></div>
    <div id="code"><div id="selection"></div><div id="lines"></div><div id="caret" class="hidden"></div><textarea id="input" aria-label="Editor" autocapitalize="off" autocomplete="off" autocorrect="off" spellcheck="false" wrap="off"></textarea></div>
    <iframe id="md" class="hidden" sandbox="" title="Markdown page"></iframe>
    <div id="note" class="hidden"></div>
  </section>
  <section id="termsec" class="none">
    <nav id="termtabs" class="strip"></nav>
    <div id="term" tabindex="0"><div id="hist"></div><div id="screen"><div id="cursor" class="hidden"></div></div></div>
  </section>
</main>
<p id="termnote" class="hidden"></p>
</div>
</div>
<div id="toast" class="hidden" role="status"></div>
<script nonce="{{NONCE}}">
"use strict";
(function () {
  const $ = (id) => document.getElementById(id);
  const token = new URLSearchParams(location.search).get("t") || "";
  const mac = /Mac|iPhone|iPad|iPod/.test(navigator.platform || "");
  let ws = null, palette = null, state = null, ended = false, folder = "", termNote = "";
  let editable = false, canSave = false;
  const want = { doc: 0, term: 0 };
  let followDoc = true, followTerm = true, termInfo = null;
  const kHist = 5000;

  function base(p) { const i = p.lastIndexOf("/"); return i < 0 ? p : p.slice(i + 1); }
  function inFolder(p) { return folder && p.startsWith(folder + "/") ? p.slice(folder.length + 1) : p; }
  function color(n) { return "#" + (n >>> 0).toString(16).padStart(6, "0"); }

  // The host's colors and sizes (paletteJson): CSS variables for the page,
  // a rule per syntax style, and the Markdown frame's colors.
  function applyPalette(p) {
    palette = p;
    const root = document.documentElement.style;
    const set = (k, v) => { if (v !== undefined && v !== null && v !== "") root.setProperty(k, v); };
    const sf = p.surfaces || {};
    const title = sf.titlebar || {};
    set("--bg", p.background); set("--fg", p.text);
    set("--tbg", p.terminalBackground); set("--tfg", p.terminalText);
    set("--hbg", title.background); set("--hfg", title.text);
    const side = sf.sidebar || {};
    set("--sbg", side.background); set("--sfg", side.text);
    set("--ign", p.treeIgnored);
    set("--gutter", p.gutter); set("--ln", p.lineNumber); set("--lnact", p.lineNumberActive);
    set("--muted", p.dark ? "#9d9d9d" : "#616161");
    set("--line", p.dark ? "#2b2b2b" : "#e5e5e5");
    set("--selbg", p.dark ? "rgba(38, 79, 120, .9)" : "rgba(173, 214, 255, .9)");
    if (p.editorFontSize) set("--efs", p.editorFontSize + "px");
    if (p.terminalFontSize) set("--tfs", p.terminalFontSize + "px");
    if (p.tabsFontSize) set("--sfs", p.tabsFontSize + "px");
    root.colorScheme = p.dark ? "dark" : "light";
    let css = "";
    (p.syntax || []).forEach((c, i) => { if (i) css += "#code .s" + i + "{color:" + c + "}\n"; });
    // The ruler's columns, past the line numbers' 4.5em.
    const rulers = (p.rulers || []).map((n) => {
      const x = "calc(4.5em + " + n + "ch)";
      return "linear-gradient(to right, transparent " + x + ", " + p.ruler + " " + x + ", " +
             p.ruler + " calc(" + x + " + 1px), transparent calc(" + x + " + 1px))";
    });
    if (rulers.length) css += "#code .l{background-image:" + rulers.join(",") + "}\n";
    $("theme").textContent = css;
    $("code").classList.toggle("wrap", !!p.wordWrap);
    if (mdHtml !== null) setMarkdown(mdHtml);
    drawSelection();
  }
  // A Markdown page in the frame, in the host's colors.
  let mdHtml = null;
  function markdownCss(p) {
    const m = (p && p.markdown) || {};
    let css = "body{background:" + p.background + ";color:" + p.text + ";color-scheme:" +
              (p.dark ? "dark" : "light") + "}";
    if (m.heading) css += "h1,h2,h3,h4,h5,h6{color:" + m.heading + "}";
    if (m.link) css += "a{color:" + m.link + "}";
    if (m.code) css += "code{color:" + m.code + "}";
    if (m.codeBackground) css += "code,pre,th{background:" + m.codeBackground + "}pre code{background:none}";
    if (m.quote) css += "blockquote{color:" + m.quote + ";border-left-color:" + m.quote + "}";
    if (m.maxWidth) css += ".markdown-body{max-width:" + m.maxWidth + "px}";
    return css;
  }
  function setMarkdown(html) {
    mdHtml = html;
    const style = palette ? "<style>" + markdownCss(palette) + "</style>" : "";
    const at = html.lastIndexOf("</head>");
    $("md").srcdoc = at < 0 ? style + html : html.slice(0, at) + style + html.slice(at);
  }

  // ---- messages out
  function post(m) {
    if (!ws || ws.readyState !== 1) return false;
    ws.send(JSON.stringify(m));
    return true;
  }
  // Says what this page shows, when that changed since it last said.
  let said = "";
  function sendShow() {
    const text = JSON.stringify({ t: "show", doc: want.doc, term: want.term });
    if (!ws || ws.readyState !== 1 || text === said) return;
    said = text;
    ws.send(text);
  }
  let toastTimer = 0;
  function toast(text) {
    const t = $("toast");
    t.textContent = text;
    t.classList.remove("hidden");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.add("hidden"), 4000);
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
    if (!state) return;
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
    if (!state) return;
    $("termsec").classList.toggle("none", !state.terms.length);
    $("termnote").textContent = termNote;
    $("termnote").classList.toggle("hidden", !termNote || state.terms.length > 0);
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
  // A document with typing not yet confirmed waits for it before another
  // shows, so nothing typed is left behind.
  function showDoc(id) {
    if (id !== want.doc) {
      if (!synced()) { D.switchTo = id; return; }
      want.doc = id;
      sendShow();
    }
    drawTabs();
    markCurrent();
  }
  function showTerm(id) { if (id !== want.term) { want.term = id; sendShow(); } drawTermTabs(); }


  // ---- the tree: one folder's rows at a time, read by the host each time
  // a folder opens (list), folders first as the apps list them, dotfiles as
  // the window shows them, what the ignore files ignore in tree.git-ignored's
  // color. A click on a file opens it as a tab in the minicode window (open),
  // as a click in the window's own tree would (a double-click keeps it), and
  // this page shows that tab once the session's state has it. Read-only
  // pages may open files too: a tab opened or shown changes no text.
  const T = { root: "", open: new Set([""]), kids: new Map(), more: new Map(), err: new Map(),
              pending: null, timer: 0 };
  const kViewerKinds = new Set(["image", "video", "audio", "pdf", "database", "archive", "bookmark"]);
  const narrow = window.matchMedia("(max-width: 700px)");
  function listFolder(rel) { post({ t: "list", path: rel }); }
  function refreshTree() { for (const rel of T.open) listFolder(rel); }
  function treeNote(text) {
    const n = $("treenote");
    n.textContent = text || "";
    n.classList.toggle("hidden", !text);
  }
  function joinRel(dir, name) { return dir ? dir + "/" + name : name; }
  function currentRel() {
    if (!state || !T.root || !want.doc) return null;
    const d = state.docs.find((x) => x.id === want.doc);
    return d && d.path.startsWith(T.root + "/") ? d.path.slice(T.root.length + 1) : null;
  }
  function treeRow(e, rel, depth) {
    const r = document.createElement("div");
    r.className = "row" + (e.dir ? " dir" : "") + (e.ignored ? " ignored" : "") +
                  (e.outside ? " outside" : "") + (e.link ? " link" : "");
    r.dataset.path = rel;
    r.setAttribute("role", "treeitem");
    r.style.paddingLeft = (4 + depth * 12) + "px";
    const chev = document.createElement("span");
    chev.className = "chev";
    if (e.dir && !e.outside) {
      chev.textContent = T.open.has(rel) ? "▾" : "▸";
      r.setAttribute("aria-expanded", T.open.has(rel) ? "true" : "false");
    }
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = e.name;
    r.title = e.outside ? e.name + " leads out of this folder" : rel;
    r.append(chev, name);
    r._entry = e;
    return r;
  }
  function treeLine(cls, text, depth) {
    const d = document.createElement("div");
    d.className = cls;
    d.style.paddingLeft = (20 + depth * 12) + "px";
    d.textContent = text;
    return d;
  }
  function drawTree() {
    const tree = $("tree"), top = tree.scrollTop;
    const frag = document.createDocumentFragment();
    const cur = currentRel();
    const add = (dir, depth) => {
      if (T.err.has(dir)) { frag.appendChild(treeLine("err", T.err.get(dir), depth)); return; }
      const kids = T.kids.get(dir);
      if (!kids) return;
      for (const e of kids) {
        const rel = joinRel(dir, e.name);
        const row = treeRow(e, rel, depth);
        if (rel === cur) row.classList.add("current");
        frag.appendChild(row);
        if (e.dir && !e.outside && T.open.has(rel)) add(rel, depth + 1);
      }
      if (T.more.get(dir)) frag.appendChild(treeLine("more", "… " + T.more.get(dir) + " more", depth));
    };
    add("", 0);
    tree.textContent = "";
    tree.appendChild(frag);
    tree.scrollTop = top;
  }
  // The row of the document on show, marked as the window's tree marks it.
  function markCurrent() {
    const cur = currentRel();
    for (const r of $("tree").querySelectorAll(".row"))
      r.classList.toggle("current", r.dataset.path === cur);
  }
  function onTree(m) {
    if (m.error !== undefined) {
      T.kids.delete(m.path);
      T.more.delete(m.path);
      if (m.path) {
        // Gone, or no longer the folder's: closed, with what was open in it.
        for (const k of [...T.open]) if (k === m.path || k.startsWith(m.path + "/")) T.open.delete(k);
      } else {
        T.err.set("", m.error === "no folder to read in" ? "This window has no folder." : m.error);
      }
    } else {
      T.err.delete(m.path);
      if (m.root) T.root = m.root;
      T.kids.set(m.path, m.entries || []);
      if (m.more) T.more.set(m.path, m.more); else T.more.delete(m.path);
      if (!m.path) $("sidename").textContent = base(T.root) || "Files";
    }
    drawTree();
  }
  function openFile(rel, e, keep) {
    treeNote("");
    T.pending = { rel: rel, kind: e.kind || "text", abs: "" };
    if (!post({ t: "open", path: rel, keep: !!keep })) { T.pending = null; return; }
    if (narrow.matches) setSide(false);
  }
  function onOpening(m) {
    const p = T.pending;
    if (!p || p.rel !== m.path) return;
    if (!m.ok) { T.pending = null; treeNote(m.why); return; }
    if (kViewerKinds.has(p.kind)) {
      T.pending = null;
      treeNote(base(m.path) + " opens in the minicode window.");
      return;
    }
    p.abs = m.abs;
    clearTimeout(T.timer);
    // A file the window shows in a viewer of its own has no document here.
    T.timer = setTimeout(() => {
      if (T.pending !== p) return;
      T.pending = null;
      treeNote(base(p.rel) + " opened in the minicode window.");
    }, 4000);
    takePending();
  }
  // The clicked file's tab, once the session's state has it.
  function takePending() {
    const p = T.pending;
    if (!p || !p.abs || !state) return;
    const d = state.docs.find((x) => x.path === p.abs);
    if (!d) return;
    T.pending = null;
    clearTimeout(T.timer);
    followDoc = d.id === sessionDoc();
    showDoc(d.id);
  }
  function sideOpen() {
    return narrow.matches ? document.body.classList.contains("sideopen")
                          : !document.body.classList.contains("sideclosed");
  }
  // The sidebar beside the page; at phone width, over it behind the button.
  function setSide(open) {
    if (narrow.matches) document.body.classList.toggle("sideopen", open);
    else document.body.classList.toggle("sideclosed", !open);
    $("sidebtn").setAttribute("aria-expanded", open ? "true" : "false");
    drawSelection();
  }

  function onState(m) {
    state = m;
    const docs = new Set(m.docs.map((d) => d.id));
    if ((followDoc || !docs.has(want.doc)) && (synced() || !docs.has(want.doc))) {
      followDoc = true; want.doc = sessionDoc();
    }
    const terms = m.terms.map((t) => t.id);
    if (followTerm && terms.length && !terms.includes(want.term)) want.term = terms[0];
    if (!terms.includes(want.term)) { followTerm = true; want.term = terms.length ? terms[0] : 0; }
    sendShow();
    drawTabs(); drawTermTabs();
    if (!want.doc) showNote("No document is on show.");
    takePending();
    markCurrent();
  }

  // ---- a document. `shadow` is the server's lines at `version`; `flight`
  // this page's edit sent and not yet confirmed, `queue` those waiting to
  // be sent; `lines` what is on show: the shadow with those edits made.
  // Each line is {t: text, s: spans, b: its break}. Offsets count UTF-16
  // units, as the server's do; a: the selection's anchor, h: its caret.
  const kBreaks = ["", "\n", "\r\n", "\r", " "];
  const D = { id: 0, kind: "", path: "", version: 0, editable: false,
              indent: { tabs: false, width: 4 }, shadow: [], lines: [], starts: [0],
              flight: null, queue: [], a: 0, h: 0, goal: null, moved: true,
              undo: 0, save: false, switchTo: 0, mode: "code", html: "" };
  let seq = 0;
  function lineOf(l) { return { t: l[0], s: l[1] || [], b: l.length > 2 ? kBreaks[l[2]] : "\n" }; }
  function pending() { return D.flight ? [D.flight].concat(D.queue) : D.queue; }
  function synced() { return !D.flight && !D.queue.length; }
  function reindex() {
    const L = D.lines, st = new Array(L.length + 1);
    let at = 0;
    for (let i = 0; i < L.length; i++) { st[i] = at; at += L[i].t.length + L[i].b.length; }
    st[L.length] = at;
    D.starts = st;
  }
  function total() { return D.starts[D.lines.length] || 0; }
  function lineAt(off) {
    let lo = 0, hi = Math.max(0, D.lines.length - 1);
    while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (D.starts[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo;
  }
  function colOf(off) { const i = lineAt(off); return Math.min(off - D.starts[i], D.lines[i] ? D.lines[i].t.length : 0); }
  function clampOff(off) {
    off = Math.max(0, Math.min(off, total()));
    const i = lineAt(off), end = D.starts[i] + (D.lines[i] ? D.lines[i].t.length : 0);
    return off > end ? end : off;   // never inside a line break
  }
  function textOf(a, b) {
    const la = lineAt(a), lb = lineAt(b);
    let s = "";
    for (let i = la; i <= lb; i++) s += D.lines[i].t + D.lines[i].b;
    return s.slice(a - D.starts[la], b - D.starts[la]);
  }
  // A text as lines, split where the highlighter splits them: after "\n",
  // "\r\n", a "\r" not before "\n", and U+2029. The last holds what follows
  // the last break.
  function splitText(s) {
    const out = [];
    let i0 = 0;
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c === 10) { out.push({ t: s.slice(i0, i), b: "\n" }); i0 = i + 1; }
      else if (c === 13) {
        if (s.charCodeAt(i + 1) === 10) { out.push({ t: s.slice(i0, i), b: "\r\n" }); i++; }
        else out.push({ t: s.slice(i0, i), b: "\r" });
        i0 = i + 1;
      } else if (c === 0x2029) { out.push({ t: s.slice(i0, i), b: " " }); i0 = i + 1; }
    }
    out.push({ t: s.slice(i0), b: "" });
    return out;
  }
  // One operation made on a list of lines. Lines it leaves as they were
  // keep their objects (and so their elements); a changed line keeps the
  // colors of what lies outside the change until the server's come.
  function applyOp(L, op) {
    const end = op.pos + op.del;
    let at = 0, la = -1, lb = -1, sa = 0;
    for (let i = 0; i < L.length; i++) {
      const n = L[i].t.length + L[i].b.length, last = i === L.length - 1;
      if (la < 0 && (op.pos < at + n || last)) { la = i; sa = at; }
      if (la >= 0 && (end < at + n || last)) { lb = i; break; }
      at += n;
    }
    if (la < 0) return;
    let first = la, lastI = lb;
    if (first > 0 && L[first - 1].b === "\r") { first--; sa -= L[first].t.length + 1; }
    const old = L.slice(first, lastI + 1);
    let win = old.map((x) => x.t + x.b).join("");
    const rel = op.pos - sa;
    win = win.slice(0, rel) + op.ins + win.slice(rel + op.del);
    // The window ends at a break that nothing after it can join.
    while (lastI + 1 < L.length) {
      const c = win.charCodeAt(win.length - 1);
      if (win.length && (c === 10 || c === 0x2029)) break;
      lastI++;
      old.push(L[lastI]);
      win += L[lastI].t + L[lastI].b;
    }
    const pieces = splitText(win);
    if (lastI < L.length - 1) pieces.pop();
    let p = 0;
    while (p < pieces.length && p < old.length && pieces[p].t === old[p].t && pieces[p].b === old[p].b) p++;
    let q = 0;
    while (q < pieces.length - p && q < old.length - p &&
           pieces[pieces.length - 1 - q].t === old[old.length - 1 - q].t &&
           pieces[pieces.length - 1 - q].b === old[old.length - 1 - q].b) q++;
    const out = pieces.map((x, k) => {
      if (k < p) return old[k];
      if (k >= pieces.length - q) return old[old.length - (pieces.length - k)];
      return { t: x.t, s: [], b: x.b };
    });
    if (p < pieces.length - q && p < old.length) {
      const o = old[p], n = out[p];
      let pre = 0;
      const m = Math.min(o.t.length, n.t.length);
      while (pre < m && o.t.charCodeAt(pre) === n.t.charCodeAt(pre)) pre++;
      n.s = o.s.filter((s) => s[0] + s[1] <= pre);
      const oi = old.length - 1 - q, ni = pieces.length - 1 - q;
      if (oi >= p) {
        const ol = old[oi], nl = out[ni];
        let suf = 0;
        const m2 = Math.min(ol.t.length, nl.t.length) - (nl === n ? pre : 0);
        while (suf < m2 && ol.t.charCodeAt(ol.t.length - 1 - suf) === nl.t.charCodeAt(nl.t.length - 1 - suf)) suf++;
        const shift = nl.t.length - ol.t.length;
        const keep = ol.s.filter((s) => s[0] >= ol.t.length - suf).map((s) => [s[0] + shift, s[1], s[2]]);
        nl.s = (nl === n ? n.s : []).concat(keep);
      }
    }
    L.splice(first, lastI - first + 1, ...out);
  }
  // The lines on show again: the server's, with this page's edits made.
  function rebuild() {
    const L = D.shadow.slice();
    for (const m of pending()) for (const op of m.ops) applyOp(L, op);
    D.lines = L;
    reindex();
    render();
  }

  // ---- Ot (src/Ot.cpp) in UTF-16 units: the same rules, so this page and
  // the server move one edit past another alike.
  function prims(ops) {
    const out = [];
    for (const o of ops) {
      if (o.del > 0) out.push({ pos: o.pos, del: o.del, ins: "" });
      if (o.ins.length) out.push({ pos: o.pos, del: 0, ins: o.ins });
    }
    return out;
  }
  function pastDelete(at, q, n) { return at <= q ? at : at >= q + n ? at - n : q; }
  function prim(x, y, xo, yo) {
    const xIns = x.del === 0, yIns = y.del === 0;
    if (xIns && yIns) {
      xo.push({ pos: x.pos < y.pos ? x.pos : x.pos + y.ins.length, del: 0, ins: x.ins });
      yo.push({ pos: y.pos <= x.pos ? y.pos : y.pos + x.ins.length, del: 0, ins: y.ins });
      return;
    }
    if (xIns || yIns) {
      const i = xIns ? x : y, d = xIns ? y : x, io = xIns ? xo : yo, dout = xIns ? yo : xo;
      const il = i.ins.length, p = i.pos, q = d.pos, n = d.del;
      if (p <= q) { io.push(i); dout.push({ pos: q + il, del: n, ins: "" }); }
      else if (p >= q + n) { io.push({ pos: p - n, del: 0, ins: i.ins }); dout.push(d); }
      else {
        io.push({ pos: q, del: 0, ins: i.ins });
        dout.push({ pos: q, del: p - q, ins: "" });
        dout.push({ pos: q + il, del: q + n - p, ins: "" });
      }
      return;
    }
    const xa = pastDelete(x.pos, y.pos, y.del), xb = pastDelete(x.pos + x.del, y.pos, y.del);
    const ya = pastDelete(y.pos, x.pos, x.del), yb = pastDelete(y.pos + y.del, x.pos, x.del);
    if (xb > xa) xo.push({ pos: xa, del: xb - xa, ins: "" });
    if (yb > ya) yo.push({ pos: ya, del: yb - ya, ins: "" });
  }
  function xf(x, y) {
    if (!x.length) return [[], y.slice()];
    if (!y.length) return [x.slice(), []];
    if (x.length === 1 && y.length === 1) { const xo = [], yo = []; prim(x[0], y[0], xo, yo); return [xo, yo]; }
    if (x.length >= y.length) {
      const h = x.length >> 1;
      const [x1, y1] = xf(x.slice(0, h), y);
      const [x2, y2] = xf(x.slice(h), y1);
      return [x1.concat(x2), y2];
    }
    const h = y.length >> 1;
    const [xa, y1] = xf(x, y.slice(0, h));
    const [xo, y2] = xf(xa, y.slice(h));
    return [xo, y1.concat(y2)];
  }
  // [a past b, b past a]; b wins ties (its insertion goes first).
  function transform(a, b) { return xf(prims(a), prims(b)); }
  function mapOffset(at, ops, afterInsert) {
    for (const o of ops) {
      if (o.del > 0) at = pastDelete(at, o.pos, o.del);
      if (o.ins.length && (at > o.pos || (at === o.pos && afterInsert))) at += o.ins.length;
    }
    return at;
  }
  function opsOf(j) { return (j || []).map((o) => ({ pos: o[0], del: o[1], ins: o[2] })); }
  // Someone else's change `f` (on the shadow before it): this page's edits
  // move past it in order, and the selection past what is left of it.
  function movePending(f, afterInsert) {
    for (const m of pending()) {
      if (m.at) m.at = m.at.map((x) => mapOffset(x, f, false));
      m.before = m.before.map((x) => mapOffset(x, f, afterInsert));
      const r = transform(m.ops, f);
      m.ops = r[0];
      f = r[1];
      m.after = m.after.map((x) => mapOffset(x, f, afterInsert));
    }
    D.a = mapOffset(D.a, f, afterInsert);
    D.h = mapOffset(D.h, f, afterInsert);
  }

  // ---- drawing the lines
  function lineEl(l) {
    const d = document.createElement("div");
    d.className = "l";
    d._l = l;
    const text = l.t;
    let at = 0;
    for (const s of l.s) {
      if (s[0] < at || s[0] + s[1] > text.length) continue;
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
  // The elements follow the lines: only those whose line changed are made
  // again.
  function render() {
    const box = $("lines"), L = D.lines, kids = box.children;
    const nk = kids.length, nl = L.length;
    let a = 0;
    while (a < nk && a < nl && kids[a]._l === L[a]) a++;
    let z = 0;
    while (z < nk - a && z < nl - a && kids[nk - 1 - z]._l === L[nl - 1 - z]) z++;
    for (let i = nk - z - 1; i >= a; i--) kids[i].remove();
    const f = document.createDocumentFragment();
    for (let i = a; i < nl - z; i++) f.appendChild(lineEl(L[i]));
    box.insertBefore(f, box.children[a] || null);
    $("code").dataset.version = D.version;
    drawSelection();
  }

  // ---- places on screen
  // The text node and offset of column `col` in a line's element.
  function nodeAt(div, col, forward) {
    const w = document.createTreeWalker(div, NodeFilter.SHOW_TEXT);
    let n, at = 0, last = null;
    while ((n = w.nextNode())) {
      const len = n.data.length;
      if (col < at + len || (!forward && col === at + len)) return [n, col - at];
      at += len;
      last = n;
    }
    return last ? [last, last.data.length] : null;
  }
  function lineHeight(div) { return parseFloat(getComputedStyle(div).lineHeight) || 18; }
  function gutterWidth(div) {
    const w = parseFloat(getComputedStyle(div, "::before").width);
    return isNaN(w) ? 4.5 * (parseFloat(getComputedStyle(div).fontSize) || 13) : w;
  }
  // Where offset `off` is: {x, y, h} in the scroller's content.
  function caretBox(off) {
    if (!D.lines.length) return null;
    const li = lineAt(off), div = $("lines").children[li];
    if (!div) return null;
    const code = $("code"), host = code.getBoundingClientRect();
    const t = D.lines[li].t, col = Math.min(off - D.starts[li], t.length);
    const lh = lineHeight(div);
    let x, top;
    if (t.length) {
      const r = document.createRange();
      const atEnd = col >= t.length;
      let c = atEnd ? col - 1 : col;
      if (c > 0 && (t.charCodeAt(c) & 0xFC00) === 0xDC00) c--;
      const p = nodeAt(div, c, true);
      const span = (t.charCodeAt(c) & 0xFC00) === 0xD800 ? 2 : 1;
      if (p) {
        r.setStart(p[0], p[1]);
        r.setEnd(p[0], Math.min(p[0].data.length, p[1] + span));
        const rects = r.getClientRects();
        const b = rects.length ? rects[atEnd ? rects.length - 1 : 0] : r.getBoundingClientRect();
        x = atEnd ? b.right : b.left;
        top = b.top - (lh - b.height) / 2;
      }
    }
    if (x === undefined) {
      const dr = div.getBoundingClientRect();
      x = dr.left + gutterWidth(div);
      top = dr.top;
    }
    return { x: x - host.left + code.scrollLeft, y: top - host.top + code.scrollTop, h: lh };
  }
  // The offset under a point on screen, or null.
  function offsetAt(cx, cy) {
    const box = $("lines"), kids = box.children;
    if (!kids.length) return null;
    let node = null, off = 0;
    if (document.caretRangeFromPoint) {
      const r = document.caretRangeFromPoint(cx, cy);
      if (r) { node = r.startContainer; off = r.startOffset; }
    } else if (document.caretPositionFromPoint) {
      const p = document.caretPositionFromPoint(cx, cy);
      if (p) { node = p.offsetNode; off = p.offset; }
    }
    let el = node && (node.nodeType === 3 ? node.parentNode : node);
    while (el && el !== box && !(el.classList && el.classList.contains("l"))) el = el.parentNode;
    if (!el || el === box || !el.classList.contains("l")) {
      // Not on a line: the nearest by height.
      const top = kids[0].getBoundingClientRect().top;
      if (cy < top) return 0;
      let lo = 0, hi = kids.length - 1;
      while (lo < hi) {
        const mid = (lo + hi + 1) >> 1;
        if (kids[mid].getBoundingClientRect().top <= cy) lo = mid; else hi = mid - 1;
      }
      return D.starts[lo] + D.lines[lo].t.length;
    }
    const li = Array.prototype.indexOf.call(kids, el);
    if (li < 0) return null;
    let col = 0;
    if (node.nodeType === 3) {
      const w = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
      let n;
      while ((n = w.nextNode()) && n !== node) col += n.data.length;
      col += off;
    } else {
      for (let i = 0; i < off && i < node.childNodes.length; i++) col += node.childNodes[i].textContent.length;
    }
    const t = D.lines[li].t;
    col = Math.min(col, t.length);
    if (col > 0 && col < t.length && (t.charCodeAt(col) & 0xFC00) === 0xDC00) col--;
    return D.starts[li] + col;
  }
  function visibleLines() {
    const box = $("lines"), kids = box.children, n = kids.length;
    if (!n) return [0, -1];
    const code = $("code");
    const top = code.scrollTop - box.offsetTop, bottom = top + code.clientHeight;
    let lo = 0, hi = n - 1;
    while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (kids[mid].offsetTop <= top) lo = mid; else hi = mid - 1; }
    let end = lo;
    while (end + 1 < n && kids[end + 1].offsetTop < bottom) end++;
    return [lo, end];
  }
  function selRange() { return [Math.min(D.a, D.h), Math.max(D.a, D.h)]; }
  // The selection and the caret, drawn over the lines in view.
  function drawSelection() {
    const layer = $("selection"), caret = $("caret"), code = $("code");
    layer.textContent = "";
    if (!D.id || D.kind === "large" || D.mode === "page" || !D.lines.length || code.classList.contains("hidden")) {
      caret.classList.add("hidden");
      return;
    }
    const [s, e] = selRange();
    const host = code.getBoundingClientRect();
    if (s !== e) {
      const [v0, v1] = visibleLines();
      const l0 = Math.max(lineAt(s), v0), l1 = Math.min(lineAt(e), v1);
      const f = document.createDocumentFragment();
      const add = (left, top, width, height) => {
        const d = document.createElement("div");
        d.style.left = left + "px"; d.style.top = top + "px";
        d.style.width = Math.max(width, 2) + "px"; d.style.height = height + "px";
        f.appendChild(d);
      };
      for (let li = l0; li <= l1; li++) {
        const div = $("lines").children[li];
        if (!div) break;
        const st = D.starts[li], t = D.lines[li].t, lh = lineHeight(div);
        const a = Math.max(s, st) - st, b = Math.min(e, st + t.length) - st;
        if (b > a) {
          const pa = nodeAt(div, a, true), pb = nodeAt(div, b, false);
          if (pa && pb) {
            const r = document.createRange();
            r.setStart(pa[0], pa[1]);
            r.setEnd(pb[0], pb[1]);
            const rs = r.getClientRects();
            for (let i = 0; i < rs.length; i++) {
              const c = rs[i];
              add(c.left - host.left + code.scrollLeft, c.top - (lh - c.height) / 2 - host.top + code.scrollTop, c.width, lh);
            }
          }
        }
        if (e > st + t.length && li < D.lines.length - 1) {
          const k = caretBox(st + t.length);
          if (k) add(k.x, k.y, lh / 3, k.h);
        }
      }
      layer.appendChild(f);
    }
    const k = caretBox(D.h);
    caret.classList.toggle("hidden", !k);
    if (k) {
      caret.style.left = k.x + "px"; caret.style.top = k.y + "px"; caret.style.height = k.h + "px";
      caret.style.animation = "none";
      void caret.offsetWidth;
      caret.style.animation = "";
    }
  }
  // The caret in view.
  function reveal() {
    const code = $("code"), k = caretBox(D.h);
    if (!k) return;
    if (k.y < code.scrollTop) code.scrollTop = k.y;
    else if (k.y + k.h > code.scrollTop + code.clientHeight) code.scrollTop = k.y + k.h - code.clientHeight;
    const gutter = D.lines.length ? gutterWidth($("lines").children[0] || $("lines")) : 0;
    if (k.x - gutter < code.scrollLeft) code.scrollLeft = Math.max(0, k.x - gutter - 20);
    else if (k.x > code.scrollLeft + code.clientWidth - 8) code.scrollLeft = k.x - code.clientWidth + 40;
    drawSelection();
  }

  // ---- editing
  function flight(m) {
    m.seq = ++seq;
    D.flight = m;
    post({ t: "edit", doc: D.id, base: D.version, seq: m.seq,
           ops: m.ops.map((o) => [o.pos, o.del, o.ins]), step: m.step, before: m.before,
           after: m.after, cmd: m.cmd || "", at: m.at || null });
  }
  // One edit in flight, as SessionClient's: the next goes once it is
  // confirmed, typing that waited meanwhile joined into one.
  function sendNext() {
    if (D.flight || !D.queue.length || !ws || ws.readyState !== 1) return;
    let m = D.queue.shift();
    while (D.queue.length && !m.cmd && !D.queue[0].cmd && m.step.coalesce && D.queue[0].step.coalesce &&
           !D.queue[0].step.fresh && D.queue[0].step.name === m.step.name) {
      const n = D.queue.shift();
      m = { ops: m.ops.concat(n.ops), step: m.step, before: m.before, after: n.after };
    }
    flight(m);
  }
  function afterSync() {
    if (!synced()) return;
    if (D.undo) { post({ t: "undo", doc: D.id, redo: D.undo === 2 }); D.undo = 0; D.moved = true; }
    if (D.save) { post({ t: "save", doc: D.id }); D.save = false; }
    if (D.switchTo) { const id = D.switchTo; D.switchTo = 0; showDoc(id); }
  }
  // An edit of this page's: made on its lines at once, then sent.
  function edit(ops, step, a, h, cmd, at) {
    if (!D.editable) { toast(editable ? "This document cannot be edited here." : "This page only reads."); return; }
    D.queue.push({ ops: ops, step: { name: step.name, coalesce: !!step.coalesce, fresh: D.moved },
                   before: [D.a, D.h], after: [a, h], cmd: cmd || "", at: at || null });
    D.moved = false;
    for (const op of ops) applyOp(D.lines, op);
    reindex();
    render();
    D.a = a; D.h = h; D.goal = null;
    reveal();
    syncInput();
    sendNext();
  }
  const kTyping = { name: "Typing", coalesce: true };
  function replaceSel(text, step, cmd) {
    const [s, e] = selRange();
    if (s === e && !text.length) return;
    edit([{ pos: s, del: e - s, ins: text }], step, s + text.length, s + text.length, cmd, cmd ? [s, e] : null);
  }
  // The units before and after an offset that one key deletes: a whole
  // surrogate pair, or a whole line break.
  function unitBefore(off) {
    const li = lineAt(off);
    if (off === D.starts[li]) return li > 0 ? D.lines[li - 1].b.length : 0;
    const t = D.lines[li].t, c = off - D.starts[li];
    return c >= 2 && (t.charCodeAt(c - 1) & 0xFC00) === 0xDC00 && (t.charCodeAt(c - 2) & 0xFC00) === 0xD800 ? 2 : 1;
  }
  function unitAfter(off) {
    const li = lineAt(off), t = D.lines[li].t, c = off - D.starts[li];
    if (c >= t.length) return D.lines[li].b.length;
    return (t.charCodeAt(c) & 0xFC00) === 0xD800 && (t.charCodeAt(c + 1) & 0xFC00) === 0xDC00 ? 2 : 1;
  }
  function kind(ch) { return /\s/.test(ch) ? 0 : /[\p{L}\p{N}_]/u.test(ch) ? 1 : 2; }
  function wordLeft(off) {
    const li = lineAt(off), st = D.starts[li], t = D.lines[li].t;
    let c = off - st;
    if (c === 0) return off - unitBefore(off);
    while (c > 0 && kind(t[c - 1]) === 0) c--;
    if (c > 0) { const k = kind(t[c - 1]); while (c > 0 && kind(t[c - 1]) === k) c--; }
    return st + c;
  }
  function wordRight(off) {
    const li = lineAt(off), st = D.starts[li], t = D.lines[li].t;
    let c = off - st;
    if (c >= t.length) return off + unitAfter(off);
    while (c < t.length && kind(t[c]) === 0) c++;
    if (c < t.length) { const k = kind(t[c]); while (c < t.length && kind(t[c]) === k) c++; }
    return st + c;
  }
  function wordAt(off) {
    const li = lineAt(off), st = D.starts[li], t = D.lines[li].t;
    let c = Math.min(off - st, t.length);
    if (!t.length) return [off, off];
    if (c === t.length) c--;
    const k = kind(t[c]);
    let a = c, b = c;
    while (a > 0 && kind(t[a - 1]) === k) a--;
    while (b < t.length && kind(t[b]) === k) b++;
    return [st + a, st + b];
  }
  function backspace(word, toLineStart) {
    const [s, e] = selRange();
    if (s !== e) return replaceSel("", kTyping);
    if (s === 0) return;
    const li = lineAt(s);
    let from;
    if (toLineStart) from = s === D.starts[li] ? s - unitBefore(s) : D.starts[li];
    else if (word) from = wordLeft(s);
    else from = s - unitBefore(s);
    const plain = !word && !toLineStart;
    edit([{ pos: from, del: s - from, ins: "" }], kTyping, from, from, plain ? "backspace" : "",
         plain ? [s, s] : null);
  }
  function deleteForward(word) {
    const [s, e] = selRange();
    if (s !== e) return replaceSel("", kTyping);
    if (s >= total()) return;
    const to = word ? wordRight(s) : s + unitAfter(s);
    edit([{ pos: s, del: to - s, ins: "" }], kTyping, s, s);
  }
  // Return: the line's indentation, then whatever AutoIndent makes of it
  // on the server.
  function newline() {
    const [s, e] = selRange();
    const li = lineAt(s), t = D.lines[li].t, col = s - D.starts[li];
    let ind = 0;
    while (ind < col && (t[ind] === " " || t[ind] === "\t")) ind++;
    replaceSel("\n" + t.slice(0, ind), kTyping, "newline");
  }
  function tab(back) {
    const [s, e] = selRange();
    if (back || lineAt(s) !== lineAt(e))
      return edit([], { name: back ? "Outdent" : "Indent", coalesce: false }, D.a, D.h,
                  back ? "backtab" : "tab", [s, e]);
    const col = s - D.starts[lineAt(s)], w = D.indent.width || 4;
    replaceSel(D.indent.tabs ? "\t" : " ".repeat(w - (col % w)), kTyping, "tab");
  }
  function undo(redo) {
    if (!D.editable) return toast("This page only reads.");
    D.undo = redo ? 2 : 1;
    afterSync();
  }
  function save() {
    if (!editable) return toast("This page only reads.");
    if (!canSave) return toast("Nothing here saves to disk yet: save from the minicode window.");
    D.save = true;
    afterSync();
  }

  // ---- moving the caret
  function moveTo(off, extend) {
    D.h = clampOff(off);
    if (!extend) D.a = D.h;
    D.moved = true;
    reveal();
    syncInput();
  }
  function home(extend) {
    const li = lineAt(D.h), t = D.lines[li].t, col = D.h - D.starts[li];
    let f = 0;
    while (f < t.length && (t[f] === " " || t[f] === "\t")) f++;
    moveTo(D.starts[li] + (col === f ? 0 : f), extend);
    D.goal = null;
  }
  function lineEnd(extend) {
    const li = lineAt(D.h);
    moveTo(D.starts[li] + D.lines[li].t.length, extend);
    D.goal = null;
  }
  function horizontal(dir, extend, word) {
    const [s, e] = selRange();
    D.goal = null;
    if (!extend && !word && s !== e) return moveTo(dir < 0 ? s : e, false);
    if (dir < 0) moveTo(word ? wordLeft(D.h) : D.h - unitBefore(D.h), extend);
    else moveTo(word ? wordRight(D.h) : D.h + unitAfter(D.h), extend);
  }
  // Up and Down by rows on screen, keeping the column the caret had (a
  // page at a time for Page Up and Page Down).
  function vertical(dir, extend, page) {
    const code = $("code"), k = caretBox(D.h);
    if (!k) return;
    if (D.goal === null) D.goal = k.x;
    const step = page ? Math.max(k.h, code.clientHeight - k.h) : k.h;
    const y = k.y + k.h / 2 + dir * step;
    const goal = D.goal;
    if (y < 0) { moveTo(0, extend); D.goal = goal; return; }
    if (y > $("lines").offsetTop + $("lines").offsetHeight) { moveTo(total(), extend); D.goal = goal; return; }
    if (page) code.scrollTop += dir * step;
    else if (y < code.scrollTop) code.scrollTop = y - k.h / 2;
    else if (y > code.scrollTop + code.clientHeight) code.scrollTop = y + k.h / 2 - code.clientHeight;
    const host = code.getBoundingClientRect();
    const cx = Math.min(Math.max(goal - code.scrollLeft + host.left, host.left + 1), host.right - 2);
    const off = offsetAt(cx, y - code.scrollTop + host.top);
    if (off !== null) moveTo(off, extend);
    D.goal = goal;
  }

  // ---- the keyboard: a hidden field at the caret takes keys, input
  // methods and the clipboard; the editor does the rest.
  let composing = false;
  function syncInput() {
    const ta = $("input"), k = caretBox(D.h);
    if (k) { ta.style.left = k.x + "px"; ta.style.top = k.y + "px"; ta.style.height = k.h + "px"; }
    if (composing) return;
    const [s, e] = selRange();
    const text = s !== e && e - s <= (1 << 20) ? textOf(s, e) : "";
    if (ta.value !== text) ta.value = text;
    if (document.activeElement === ta) ta.select();
  }
  function focusEditor() {
    const ta = $("input");
    if (document.activeElement !== ta) ta.focus({ preventScroll: true });
    syncInput();
  }
  function onKey(e) {
    if (e.isComposing || e.keyCode === 229 || composing) return;
    if (!D.id || D.kind === "large" || D.mode === "page") return;
    const cmd = mac ? e.metaKey : e.ctrlKey;
    const word = mac ? e.altKey : e.ctrlKey;
    const shift = e.shiftKey;
    let done = true;
    switch (e.key) {
      case "ArrowLeft": if (mac && e.metaKey) home(shift); else horizontal(-1, shift, word); break;
      case "ArrowRight": if (mac && e.metaKey) lineEnd(shift); else horizontal(1, shift, word); break;
      case "ArrowUp": if (cmd) moveTo(0, shift); else vertical(-1, shift, false); break;
      case "ArrowDown": if (cmd) moveTo(total(), shift); else vertical(1, shift, false); break;
      case "Home": if (e.ctrlKey || e.metaKey) moveTo(0, shift); else home(shift); break;
      case "End": if (e.ctrlKey || e.metaKey) moveTo(total(), shift); else lineEnd(shift); break;
      case "PageUp": vertical(-1, shift, true); break;
      case "PageDown": vertical(1, shift, true); break;
      case "Backspace": backspace(word, mac && e.metaKey); break;
      case "Delete": deleteForward(word); break;
      case "Enter": if (cmd || e.altKey) done = false; else newline(); break;
      case "Tab": if (cmd || e.altKey || e.ctrlKey) done = false; else tab(shift); break;
      case "Escape": if (D.a !== D.h) moveTo(D.h, false); else done = false; break;
      default:
        if (cmd && !e.altKey) {
          const k = e.key.toLowerCase();
          if (k === "a") { D.a = 0; D.h = total(); D.moved = true; drawSelection(); syncInput(); }
          else if (k === "z") undo(shift);
          else if (k === "y" && !mac) undo(true);
          else if (k === "s") save();
          else done = false;   // copy, cut and paste: the events below
        } else if (!e.ctrlKey && !e.metaKey && [...e.key].length === 1) {
          replaceSel(e.key, kTyping);
        } else {
          done = false;
        }
    }
    if (done) e.preventDefault();
  }

  // ---- the mouse
  let dragging = false;
  function onMouseDown(e) {
    if (e.button !== 0 || !D.id || D.kind === "large" || D.mode === "page") return;
    if (e.target && e.target.closest && e.target.closest("#docbar")) return;
    const off = offsetAt(e.clientX, e.clientY);
    if (off === null) return;
    e.preventDefault();
    if (e.detail === 2) { const w = wordAt(off); D.a = w[0]; D.h = w[1]; }
    else if (e.detail >= 3) { const li = lineAt(off); D.a = D.starts[li]; D.h = D.starts[li + 1]; }
    else { D.h = off; if (!e.shiftKey) D.a = off; }
    D.moved = true;
    D.goal = null;
    dragging = e.detail < 2;
    drawSelection();
    focusEditor();
  }

  // ---- documents on show
  function showNote(text) {
    $("code").classList.add("hidden"); $("md").classList.add("hidden"); mdHtml = null;
    $("modebtn").classList.add("hidden");
    const n = $("note"); n.textContent = text; n.classList.remove("hidden");
    $("docpath").textContent = "";
    D.kind = "";
    drawSelection();
  }
  function showMode() {
    const code = $("code"), md = $("md");
    const page = D.kind === "markdown" && D.mode === "page";
    $("modebtn").classList.toggle("hidden", D.kind !== "markdown");
    $("modebtn").textContent = page ? "Edit Source" : "Show Page";
    $("note").classList.add("hidden");
    code.classList.toggle("hidden", page);
    md.classList.toggle("hidden", !page);
    if (page) setMarkdown(D.html);
    else { md.removeAttribute("srcdoc"); mdHtml = null; }
    drawSelection();
  }
  function onDoc(m) {
    if (m.id !== want.doc) return;
    const same = m.id === D.id;
    if (same && !synced()) toast("The page was out of step with the session; it has the text again.");
    D.id = m.id; D.kind = m.kind; D.path = m.path; D.version = m.version;
    D.editable = editable && !!m.editable;
    D.indent = m.indent || D.indent;
    D.flight = null; D.queue = []; D.undo = 0; D.save = false;
    D.html = m.html || "";
    if (!same) {
      D.a = D.h = 0; D.goal = null; D.moved = true;
      D.mode = m.kind === "markdown" && !D.editable ? "page" : "code";
      $("code").scrollTop = 0;
    }
    $("docpath").textContent = inFolder(m.path);
    if (m.kind === "large") {
      D.shadow = []; D.lines = []; reindex(); render();
      $("code").classList.add("hidden"); $("md").classList.add("hidden"); $("modebtn").classList.add("hidden");
      const n = $("note"); n.textContent = m.note; n.classList.remove("hidden");
      drawSelection();
      return;
    }
    D.shadow = (m.lines || []).map(lineOf);
    rebuild();
    D.a = clampOff(D.a); D.h = clampOff(D.h);
    showMode();
  }
  function onLines(m) {
    if (m.id !== D.id || D.kind === "large" || !D.kind) return;
    if (m.all) D.shadow = m.all.map(lineOf);
    else for (const e of m.edits || []) D.shadow.splice(e[0], e[1], ...e[2].map(lineOf));
    D.version = m.version;
    if ("seq" in m) {
      // This page's own edit, made: the next may go. A command came out
      // otherwise than guessed: what is waiting moves past the difference.
      if (D.flight && D.flight.seq === m.seq) D.flight = null;
      if (m.fix && m.fix.length) movePending(opsOf(m.fix), true);
      if (m.sel && !D.queue.length) { D.a = m.sel[0]; D.h = m.sel[1]; }
    } else {
      movePending(opsOf(m.ops), false);
      if (m.sel) {
        let a = m.sel[0], h = m.sel[1];
        for (const q of pending()) { a = mapOffset(a, q.ops, false); h = mapOffset(h, q.ops, false); }
        D.a = a; D.h = h; D.moved = true;
      }
    }
    rebuild();
    D.a = clampOff(D.a); D.h = clampOff(D.h);
    if (m.sel) reveal();
    syncInput();
    sendNext();
    afterSync();
  }
  function onRefused(m) {
    if (m.id === D.id) {
      D.flight = null; D.queue = [];
      rebuild();
      D.a = clampOff(D.a); D.h = clampOff(D.h);
      drawSelection();
    }
    toast(m.why);
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
  // Keys for the terminal on show: named keys and characters, which the
  // server encodes as the program's modes ask.
  const kTermKeys = new Set(["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Home", "End",
    "PageUp", "PageDown", "Insert", "Delete", "Enter", "Backspace", "Tab", "Escape",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"]);
  function onTermKey(e) {
    if (!want.term || e.isComposing || e.keyCode === 229) return;
    if (mac && e.metaKey) return;   // the browser's: copy, paste and the rest
    if (!editable) return;
    const mods = (e.shiftKey ? 1 : 0) | (e.altKey && !mac ? 2 : 0) | (e.ctrlKey ? 4 : 0);
    if (kTermKeys.has(e.key)) post({ t: "tkey", term: want.term, key: e.key, mods: mods });
    else if ([...e.key].length === 1) post({ t: "ttext", term: want.term, text: e.key, mods: mods & 6 });
    else return;
    e.preventDefault();
    $("term").scrollTop = $("term").scrollHeight;
  }

  // ---- the socket
  function setStatus(text, live) { const s = $("status"); s.textContent = text; s.classList.toggle("live", !!live); }
  function banner(text) { const b = $("banner"); b.textContent = text; b.classList.toggle("hidden", !text); }
  function connect() {
    const scheme = location.protocol === "https:" ? "wss://" : "ws://";
    ws = new WebSocket(scheme + location.host + "/ws?t=" + encodeURIComponent(token));
    ws.onopen = () => { said = ""; banner(""); };
    ws.onmessage = (ev) => {
      const m = JSON.parse(ev.data);
      switch (m.t) {
        case "hello":
          $("machine").textContent = m.machine || "minicode";
          folder = m.folder;
          termNote = m.termNote || "";
          editable = !!m.editable;
          canSave = !!m.canSave;
          setStatus(editable ? "live, editing" : "live, read-only", true);
          document.body.classList.toggle("editable", editable);
          // Marks keep the path's slashes where they belong in a field
          // that cuts from the left.
          $("folder").textContent = "‎" + m.folder + "‎";
          document.title = (m.machine ? m.machine + ": " : "") + base(m.folder) + " (minicode)";
          applyPalette(m.palette);
          // The tree afresh: every folder that was open, read again (the
          // rows on show stay until the answers replace them).
          T.err.clear();
          refreshTree();
          break;
        case "palette": applyPalette(m.palette); break;
        case "state": onState(m); break;
        case "doc": onDoc(m); break;
        case "lines": onLines(m); break;
        case "md":
          if (m.id === D.id) { D.html = m.html; if (D.mode === "page") setMarkdown(m.html); }
          break;
        case "refused": onRefused(m); break;
        case "note": toast(m.text); break;
        case "gone":
          if (m.id === want.doc) { D.id = 0; D.flight = null; D.queue = []; showNote("This document was closed."); }
          break;
        case "term": onTerm(m); break;
        case "termd": onTermDelta(m); break;
        case "ended": ended = true; banner(m.why); setStatus("ended"); break;
        case "tree": onTree(m); break;
        case "opening": onOpening(m); break;
      }
    };
    ws.onclose = () => {
      ws = null;
      if (!synced()) toast("Typing the session had not confirmed was lost when the connection dropped.");
      D.flight = null; D.queue = [];
      if (ended) return;
      setStatus("reconnecting");
      termInfo = null;
      setTimeout(connect, 2000);
    };
  }

  const ta = $("input");
  ta.addEventListener("keydown", onKey);
  ta.addEventListener("compositionstart", () => { composing = true; ta.classList.add("composing"); });
  ta.addEventListener("compositionend", (e) => {
    composing = false;
    ta.classList.remove("composing");
    const text = e.data || "";
    ta.value = "";
    if (text) replaceSel(text, kTyping);
    syncInput();
  });
  // What arrives without a key this page handles (dictation, a phone's
  // keyboard, the character viewer): only committed text.
  ta.addEventListener("input", (e) => {
    if (composing || e.isComposing) return;
    const v = ta.value;
    ta.value = "";
    if (v && e.inputType && e.inputType.indexOf("insert") === 0) replaceSel(v, kTyping);
    syncInput();
  });
  ta.addEventListener("copy", (e) => {
    const [s, f] = selRange();
    if (s === f) return;
    e.clipboardData.setData("text/plain", textOf(s, f));
    e.preventDefault();
  });
  ta.addEventListener("cut", (e) => {
    const [s, f] = selRange();
    if (s === f) return;
    e.clipboardData.setData("text/plain", textOf(s, f));
    e.preventDefault();
    replaceSel("", { name: "Cut", coalesce: false });
  });
  ta.addEventListener("paste", (e) => {
    e.preventDefault();
    const text = e.clipboardData.getData("text/plain");
    if (text) replaceSel(text, { name: "Paste", coalesce: false });
  });
  ta.addEventListener("focus", () => $("code").classList.add("focused"));
  ta.addEventListener("blur", () => $("code").classList.remove("focused"));
  $("code").addEventListener("mousedown", onMouseDown);
  document.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    const off = offsetAt(e.clientX, e.clientY);
    if (off !== null && off !== D.h) { D.h = off; drawSelection(); }
  });
  document.addEventListener("mouseup", () => { if (dragging) { dragging = false; syncInput(); } });
  let drawQueued = false;
  $("code").addEventListener("scroll", () => {
    if (drawQueued) return;
    drawQueued = true;
    requestAnimationFrame(() => { drawQueued = false; drawSelection(); });
  });
  window.addEventListener("resize", () => drawSelection());
  $("modebtn").addEventListener("click", () => {
    D.mode = D.mode === "page" ? "code" : "page";
    showMode();
    if (D.mode === "code") focusEditor();
  });
  $("tree").addEventListener("click", (ev) => {
    const row = ev.target.closest(".row");
    if (!row || !row._entry) return;
    const e = row._entry, rel = row.dataset.path;
    if (e.outside) return;
    if (e.dir) {
      if (T.open.has(rel)) T.open.delete(rel);
      else { T.open.add(rel); listFolder(rel); }   // read afresh as it opens
      drawTree();
      return;
    }
    // The second click of a double-click, or Command (Control) with it,
    // keeps the tab, as in the window's tree.
    openFile(rel, e, ev.detail >= 2 || ev.metaKey || ev.ctrlKey);
  });
  $("refresh").addEventListener("click", refreshTree);
  $("sidebtn").addEventListener("click", () => setSide(!sideOpen()));
  // At phone width a tap beside the open tree closes it.
  $("work").addEventListener("pointerdown", () => { if (narrow.matches && sideOpen()) setSide(false); });
  // Back to the page after a while: the folders may have changed meanwhile.
  document.addEventListener("visibilitychange", () => { if (!document.hidden) refreshTree(); });
  $("term").addEventListener("keydown", onTermKey);
  $("term").addEventListener("paste", (e) => {
    if (!editable || !want.term) return;
    e.preventDefault();
    const text = e.clipboardData.getData("text/plain");
    if (text) post({ t: "tpaste", term: want.term, text: text });
  });
  window.mcSessionView = {
    want: want, state: () => state, palette: () => palette,
    text: () => D.lines.map((l) => l.t + l.b).join(""),
    selection: () => [D.a, D.h], synced: () => synced(), version: () => D.version,
    focus: focusEditor,
    tree: () => [...$("tree").querySelectorAll(".row")].map((r) => r.dataset.path),
    treeNote: () => $("treenote").textContent,
  };
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
