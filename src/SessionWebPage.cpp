// SessionWebPage.cpp — the one page a browser's view of a session loads
// (SessionWeb.h): its CSS and script, no third-party code. The script draws
// only what the server describes (tabs, lines with the highlighter's spans,
// a Markdown page, terminal rows of styled runs, the folder's tree in a
// sidebar, listed by the host one folder at a time, and the window's
// browser tabs in a pane of their own, the one picked shown in a frame),
// laid out as the window is: the editor, the browser to its right, the
// terminal below both, each pane shown while the window shows it unless
// this page hid or showed it itself. Read-only, it sends only which
// document and terminal it shows, which folders to list, and which file to
// read for a tab of its own (closed here, the window untouched). Editable,
// it opens files as the window's tabs and closes them, may ask the window's
// browser pane to open an address, and it is a small editor of
// its own: a caret and selection drawn over the lines, a hidden field at the
// caret for keys, input methods and the clipboard, edits made on its lines
// at once and sent with the version they were made on (moved past others'
// with the same rules as src/Ot.cpp), undo and save asked of the server,
// and keys for the terminal on show. A Markdown page goes into a sandboxed
// frame with no scripts. A picture, PDF, audio, video or HTML file clicked
// in the tree shows in a tab of this page's own from the server's /file/
// address (an HTML page in a frame sandboxed with no scripts), and a CSV
// or TSV file as a table of the rows the server parsed (R3); a notebook as
// the page the server rendered, in a frame sandboxed with no scripts, and a
// SQLite database as its tables and a page of rows at a time, read-only
// (R3's second slice); an editable page opens it in the window too. A
// LaTeX document shows as the PDF the app's own LaTeX preview typeset,
// served at the server's /typeset/ address and drawn again each time the
// app typesets it, Show Source flipping to its lines (R3's third slice).
// The sidebar's Source Control lists the folder's uncommitted changes as
// the window's does, and a file clicked shows the review page of those
// changes in a tab of the page's own, inline or side by side, read only
// (R3's fourth slice, SessionWebGit.h).
// "{{NONCE}}" is the response's script nonce. Every
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
<title>mutible</title>
<style id="theme"></style>
<style>
:root { --bg: #1f1f1f; --fg: #cccccc; --tbg: #181818; --tfg: #cccccc; --muted: #9d9d9d;
        --sbg: #181818; --sfg: #cccccc; --ign: #8c8c8c;
        --line: #2b2b2b; --accent: #4ea1f7; --lh: 1.4em; --hbg: #181818; --hfg: #cccccc;
        --gutter: #1f1f1f; --ln: #6e7681; --lnact: #cccccc; --efs: 13px; --tfs: 12px;
        --sfs: 13px; --selbg: rgba(38, 79, 120, .9);
        --gmod: #e2c08d; --gadd: #81b88b; --gunt: #73c991; --gdel: #c74e39; --gcon: #e4676b; }
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
#tabrow { display: flex; align-items: center; flex: none; background: var(--hbg);
          border-bottom: 1px solid var(--line); }
#tabrow .strip { flex: 1; min-width: 0; border-bottom: 0; align-self: stretch; }
.strip button .x { margin-left: 8px; padding: 0 3px; border-radius: 3px; opacity: .55; }
.strip button .x:hover { opacity: 1; background: rgba(127, 127, 127, .25); }
.strip button.local { font-style: italic; }
#browse { flex: 0 1 17em; min-width: 6em; margin: 3px 6px; padding: 2px 6px; font: inherit;
          font-size: 12px; background: var(--bg); color: var(--fg);
          border: 1px solid var(--line); border-radius: 3px; }
.panebtn { background: none; border: 1px solid var(--line); color: var(--muted); font: inherit;
           font-size: 12px; padding: 0 8px; border-radius: 3px; cursor: pointer; align-self: center; }
.panebtn[aria-pressed="true"] { color: var(--hfg); border-color: var(--accent); }
.panerow { display: flex; align-items: center; flex: none; background: var(--hbg);
           border-bottom: 1px solid var(--line); }
.panerow .strip { flex: 1; min-width: 0; border-bottom: 0; align-self: stretch; }
#top { flex: 3; display: flex; min-height: 0; }
#websec { flex: 0 0 var(--webshare, 50%); display: flex; flex-direction: column; min-width: 0;
          min-height: 0; border-left: 1px solid var(--line); }
#webbar { display: flex; gap: 10px; align-items: baseline; flex: none; padding: 3px 10px;
          font-size: 12px; }
#webtitle { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
#weburl { color: var(--muted); margin-left: 8px; }
#webopen { color: var(--accent); flex: none; }
#websec p { margin: 0; padding: 0 10px 4px; color: var(--muted); font-size: 12px; flex: none; }
#web { flex: 1; border: 0; width: 100%; min-height: 0; background: #ffffff; }
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
#sidemodes { display: flex; gap: 4px; padding: 0 6px 5px 8px; flex: none; }
#sidemodes button { flex: 1; min-width: 0; font: inherit; font-size: 12px; background: none; color: var(--muted);
                    border: 1px solid var(--line); border-radius: 3px; padding: 2px 6px; cursor: pointer;
                    white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#sidemodes button[aria-pressed="true"] { color: var(--sfg); border-color: var(--accent); }
#git { flex: 1; overflow: auto; min-height: 0; padding-bottom: 1em; }
#gitsum { margin: 0; padding: 2px 10px 6px; color: var(--muted); font-size: 12px; overflow-wrap: anywhere; }
#gitlist .gsec { padding: 6px 10px 2px; font-size: 11px; text-transform: uppercase; letter-spacing: .04em;
                 font-weight: 600; opacity: .8; white-space: nowrap; }
#gitlist .row { display: flex; gap: 6px; align-items: baseline; white-space: nowrap; cursor: pointer;
                line-height: 22px; padding: 0 8px 0 14px; user-select: none; }
#gitlist .row:hover { background: rgba(127, 127, 127, .12); }
#gitlist .row.current { background: rgba(127, 127, 127, .24); }
#gitlist .gname { flex: 0 1 auto; min-width: 0; overflow: hidden; text-overflow: ellipsis; }
#gitlist .gdir { flex: 1 1 0; min-width: 0; overflow: hidden; text-overflow: ellipsis; color: var(--muted);
                 font-size: 12px; }
.gl { flex: none; width: 1.2em; text-align: center; font-weight: 600; color: var(--gmod); }
.gl.A { color: var(--gadd); } .gl.U { color: var(--gunt); } .gl.D { color: var(--gdel); }
.gl.C { color: var(--gcon); }
#fileview.review { display: block; position: relative; }
#review { padding-bottom: 2em; font-size: var(--efs); }
#review .rsec { padding: 10px 12px 4px; font-size: 11px; font-weight: 600; text-transform: uppercase;
                letter-spacing: .04em; color: var(--muted); }
#review .rhead { display: flex; align-items: center; gap: 8px; padding: 4px 12px; background: var(--hbg);
                 color: var(--hfg); border-top: 1px solid var(--line); border-bottom: 1px solid var(--line);
                 position: sticky; top: 0; z-index: 1; font-size: 13px; }
#review .rpath { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
#review .rhead button { flex: none; font: inherit; font-size: 12px; color: var(--fg); background: none;
                        border: 1px solid var(--line); border-radius: 3px; padding: 1px 8px; cursor: pointer; }
#review .rbody { overflow-x: auto; }
#review table { border-collapse: collapse; min-width: 100%;
                font: var(--efs)/var(--lh) ui-monospace, SFMono-Regular, Menlo, "DejaVu Sans Mono", monospace; }
#review td { padding: 0 8px; white-space: pre; vertical-align: top; border: 0; }
#review td.n { color: var(--ln); background: var(--gutter); text-align: right; user-select: none;
               min-width: 3.2em; width: 1px; }
#review table.side td.t { width: 50%; }
#review td.t.r, #review tr.r td.t { background: rgba(241, 76, 76, .16); }
#review td.t.a, #review tr.a td.t { background: rgba(115, 201, 145, .16); }
#review td.fill { background: rgba(127, 127, 127, .07); }
#review .wr { background: rgba(241, 76, 76, .38); }
#review .wa { background: rgba(115, 201, 145, .36); }
#review tr.gap td { color: var(--muted); cursor: pointer; text-align: center; background: rgba(127, 127, 127, .08);
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Ubuntu, sans-serif; font-size: 12px; }
#review tr.gap:hover td { color: var(--fg); }
#review tr.rnote td { color: var(--muted); font-style: italic; white-space: normal; padding: 4px 12px;
                      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Ubuntu, sans-serif; font-size: 12px; }
#review tr.x td.t { background: rgba(127, 127, 127, .10); }
#review tr.x.p1 td.t, #review tr.x.p2 td.t { background: rgba(64, 166, 255, .18); }
#review tr.x.p6 td.t, #review tr.x.p7 td.t { background: rgba(115, 201, 145, .18); }
#review tr.x.p1 td.t, #review tr.x.p3 td.t, #review tr.x.p5 td.t, #review tr.x.p7 td.t { font-weight: 600; }
#treenote { margin: 0; padding: 6px 10px; color: var(--muted); font-size: 12px; flex: none;
            border-top: 1px solid var(--line); }
#work { flex: 1; display: flex; flex-direction: column; min-width: 0; min-height: 0; }
main { flex: 1; display: flex; flex-direction: column; min-height: 0; }
#docsec { flex: 1; display: flex; flex-direction: column; min-width: 0; min-height: 0; }
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
#termsec .panerow, #termsec .strip { background: var(--tbg); }
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
#tex { flex: 1; display: flex; flex-direction: column; min-height: 0; }
#texnote { flex: none; margin: 0; padding: 4px 10px; color: var(--muted); font-size: 12px; }
#texframe { flex: 1; border: 0; width: 100%; min-height: 0; background: #ffffff; }
#texlog { flex: 1; min-height: 0; margin: 0; overflow: auto; padding: 8px 12px; white-space: pre-wrap;
          font: 12px/1.45 ui-monospace, SFMono-Regular, Menlo, "DejaVu Sans Mono", monospace; }
#note { padding: 2em 1em; color: var(--muted); }
#fileopen { color: var(--accent); flex: none; font-size: 12px; }
#fileview { flex: 1; min-height: 0; overflow: auto; display: flex; }
#fileview.center { align-items: center; justify-content: center; padding: 12px; }
#fileview img { max-width: 100%; max-height: 100%; object-fit: contain;
                background: repeating-conic-gradient(rgba(127, 127, 127, .18) 0 25%, transparent 0 50%) 0 0 / 16px 16px; }
#fileview video { max-width: 100%; max-height: 100%; }
#fileview audio { width: min(600px, 90%); }
#fileview iframe { flex: 1; border: 0; width: 100%; min-height: 0; background: #ffffff; }
#fileview table { border-collapse: collapse; align-self: flex-start; font-size: var(--efs); }
#fileview td, #fileview th { border: 1px solid var(--line); padding: 2px 8px; text-align: left;
                             vertical-align: top; white-space: pre; max-width: 40ch; overflow: hidden;
                             text-overflow: ellipsis; }
#fileview th.rn { color: var(--ln); background: var(--gutter); text-align: right; font-weight: normal;
                  user-select: none; position: sticky; left: 0; }
#fileview tr.head td { font-weight: 600; position: sticky; top: 0; background: var(--hbg); }
#filenote { padding: 2em 1em; color: var(--muted); }
#fileview iframe.nb { background: var(--bg); }
#fileview.db { flex-direction: column; overflow: hidden; }
#dbbar { flex: none; display: flex; flex-wrap: wrap; align-items: center; gap: 8px; padding: 6px 10px;
         border-bottom: 1px solid var(--line); font-size: 12px; color: var(--muted); }
#dbbar select { font: inherit; color: var(--fg); background: var(--bg); border: 1px solid var(--line);
                max-width: 50%; }
#dbbar button { font: inherit; color: var(--fg); background: none; border: 1px solid var(--line);
                border-radius: 3px; padding: 1px 8px; cursor: pointer; }
#dbbar span { white-space: nowrap; }
#dbbar button:disabled { opacity: .4; cursor: default; }
#dbscroll { flex: 1; min-height: 0; overflow: auto; display: flex; flex-direction: column; }
#fileview thead th { position: sticky; top: 0; background: var(--hbg); font-weight: 600; z-index: 1; }
#fileview td.null, #fileview td.blob { color: var(--muted); font-style: italic; }
#filemore { margin: 0; padding: 6px 10px; color: var(--muted); font-size: 12px; flex: none; }
#termnote { margin: 0; padding: 6px 10px; color: var(--muted); font-size: 12px;
            border-top: 1px solid #333333; }
.hidden { display: none !important; }
#term { padding: 4px 6px; }
#term .r { white-space: pre; min-height: var(--lh); }
#screen { position: relative; }
#cursor { position: absolute; width: 1ch; height: var(--lh); background: var(--tfg);
          opacity: .55; pointer-events: none; }
#banner { padding: 6px 10px; background: #5a1d1d; color: #fff; }
/* Waiting for the app's answer, refused, or expired: one plain sentence
   over the whole page. */
#gate { position: fixed; inset: 0; z-index: 50; display: flex; align-items: center;
        justify-content: center; padding: 24px; background: var(--bg); color: var(--fg);
        font-size: 17px; text-align: center; line-height: 1.5; }
@media (max-width: 700px) {
  body { font-size: 14px; }
  #code, #term { font-size: calc(var(--efs) - 1px); }
  #code .l { white-space: pre-wrap; word-break: break-all; padding-left: 3.4em;
             text-indent: -3.4em; }
  #code .l::before { width: 3.4em; padding-right: .6em; text-indent: 0; }
  /* No room beside the editor: the browser goes below it. */
  #top { flex: 1.3; flex-direction: column; }
  #websec { flex: 1 1 0; border-left: 0; border-top: 1px solid var(--line); }
  #browse { flex-basis: 9em; }
  #termsec { flex: 1; }
  /* The tree is behind the header's button, over the page while open. */
  #side { display: none; position: absolute; left: 0; top: 0; bottom: 0; z-index: 20;
          width: min(85vw, 340px); box-shadow: 2px 0 12px rgba(0, 0, 0, .35); }
  body.sideopen #side { display: flex; }
  #tree .row, #gitlist .row { line-height: 30px; }
}
</style>
</head>
<body>
<header><button id="sidebtn" class="iconbtn" type="button" title="Files" aria-label="Files" aria-expanded="true">&#9776;</button><span id="machine">mutible</span><span id="folder"></span><button id="webbtn" class="panebtn hidden" type="button" aria-pressed="false" title="Show or hide the browser on this page">Browser</button><button id="termbtn" class="panebtn hidden" type="button" aria-pressed="false" title="Show or hide the terminal on this page">Terminal</button><span id="status">connecting</span></header>
<div id="banner" class="hidden"></div>
<div id="gate" class="hidden" role="status"></div>
<div id="wrap">
<aside id="side" aria-label="Files">
  <div id="sidebar"><span id="sidename">Files</span><button id="refresh" class="iconbtn" type="button" title="Refresh the tree" aria-label="Refresh the tree">&#8635;</button></div>
  <div id="sidemodes"><button id="filesbtn" type="button" aria-pressed="true" title="The folder's files">Files</button><button id="scmbtn" type="button" aria-pressed="false" title="The folder's uncommitted changes, as the window's Source Control lists them (read-only)">Source Control</button></div>
  <div id="tree" role="tree"></div>
  <div id="git" class="hidden"><p id="gitsum"></p><div id="gitlist" role="list"></div></div>
  <p id="treenote" class="hidden"></p>
</aside>
<div id="work">
<main>
 <div id="top">
  <section id="docsec">
    <div id="tabrow"><nav id="tabs" class="strip"></nav></div>
    <div id="docbar"><span id="docpath"></span><a id="fileopen" class="hidden" target="_blank" rel="noopener noreferrer">Open in a new tab</a><button id="modebtn" class="hidden" type="button"></button></div>
    <div id="code"><div id="selection"></div><div id="lines"></div><div id="caret" class="hidden"></div><textarea id="input" aria-label="Editor" autocapitalize="off" autocomplete="off" autocorrect="off" spellcheck="false" wrap="off"></textarea></div>
    <iframe id="md" class="hidden" sandbox="" title="Markdown page"></iframe>
    <div id="tex" class="hidden"><p id="texnote" class="hidden"></p><iframe id="texframe" class="hidden" title="Typeset pages"></iframe><pre id="texlog" class="hidden"></pre></div>
    <div id="note" class="hidden"></div>
    <div id="fileview" class="hidden"></div>
    <p id="filemore" class="hidden"></p>
  </section>
  <section id="websec" class="hidden" aria-label="Browser">
    <div id="webrow" class="panerow"><nav id="webtabs" class="strip"></nav><input id="browse" class="hidden" type="text" inputmode="url" placeholder="Open an address in the window" title="Opens in the mutible window's browser pane" aria-label="Open an address in the mutible window's browser pane" autocapitalize="off" autocomplete="off" autocorrect="off" spellcheck="false"><button id="webclose" class="iconbtn" type="button" title="Hide the browser on this page (the window keeps it)" aria-label="Hide the browser on this page">&#215;</button></div>
    <div id="webbar"><span id="webtitle"></span><a id="webopen" target="_blank" rel="noopener noreferrer">Open in a new tab</a></div>
    <p id="webhint">Some sites refuse to be shown inside another page and stay blank here: open those in a new tab.</p>
    <p id="webloop" class="hidden">This address is on the machine running mutible: it shows here only when this browser runs on that machine too, not through a tunnel from elsewhere.</p>
    <p id="webnote" class="hidden"></p>
    <iframe id="web" title="Browser tab" sandbox="allow-scripts allow-same-origin allow-forms allow-popups allow-popups-to-escape-sandbox allow-modals allow-downloads" referrerpolicy="no-referrer" allow="fullscreen"></iframe>
  </section>
 </div>
  <section id="termsec" class="none" aria-label="Terminal">
    <div id="termrow" class="panerow"><nav id="termtabs" class="strip"></nav><button id="termclose" class="iconbtn" type="button" title="Hide the terminal on this page (the window keeps it)" aria-label="Hide the terminal on this page">&#215;</button></div>
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
  // The view's own path. The link's token was spent on this page's load
  // for the credential's cookie: out of the address bar and the history
  // entry it goes, before anything else runs.
  const viewBase = "{{BASE}}";
  if (location.search || location.pathname !== viewBase) history.replaceState(null, "", viewBase);
  const mac = /Mac|iPhone|iPad|iPod/.test(navigator.platform || "");
  let ws = null, palette = null, state = null, ended = false, folder = "", termNote = "";
  let editable = false, canSave = false, canBrowse = false, canOpen = false, canClose = false;
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
    const g = p.git || {};
    set("--gmod", g.modified); set("--gadd", g.added); set("--gunt", g.untracked); set("--gdel", g.deleted);
    set("--gcon", g.conflict);
    set("--gutter", p.gutter); set("--ln", p.lineNumber); set("--lnact", p.lineNumberActive);
    set("--muted", p.dark ? "#9d9d9d" : "#616161");
    set("--line", p.dark ? "#2b2b2b" : "#e5e5e5");
    set("--selbg", p.dark ? "rgba(38, 79, 120, .9)" : "rgba(173, 214, 255, .9)");
    if (p.editorFontSize) set("--efs", p.editorFontSize + "px");
    if (p.terminalFontSize) set("--tfs", p.terminalFontSize + "px");
    if (p.tabsFontSize) set("--sfs", p.tabsFontSize + "px");
    root.colorScheme = p.dark ? "dark" : "light";
    let css = "";
    (p.syntax || []).forEach((c, i) => { if (i) css += "#code .s" + i + ",#review .s" + i + "{color:" + c + "}\n"; });
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
    const nb = V.on ? L.tabs.find((x) => x.viewer && x.path === V.on && x.nb) : null;
    if (nb) drawNotebook(nb.nb);
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
  // A rendered page (Markdown, a notebook) with the host's colors in a
  // <style> before its </head>, so the frame keeps no script.
  function withPalette(html) {
    const style = palette ? "<style>" + markdownCss(palette) + "</style>" : "";
    const at = html.lastIndexOf("</head>");
    return at < 0 ? style + html : html.slice(0, at) + style + html.slice(at);
  }
  function setMarkdown(html) {
    mdHtml = html;
    $("md").srcdoc = withPalette(html);
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
  function tabButton(label, title, cls, onclick, onclose, closeTitle) {
    const b = document.createElement("button");
    b.textContent = label; b.title = title; b.className = cls; b.onclick = onclick;
    if (onclose) {
      // A close button in the tab, as the window's tabs have.
      const x = document.createElement("span");
      x.className = "x"; x.textContent = "\u00d7";
      x.setAttribute("role", "button");
      x.setAttribute("aria-label", closeTitle);
      x.title = closeTitle;
      x.onclick = (e) => { e.stopPropagation(); onclose(); };
      b.appendChild(x);
    }
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
      let cls = id === want.doc && !L.on ? "shown" : "";
      if (id === onShow) cls += " onshow";
      if (dirty.get(id)) cls += " dirty";
      const name = base(path) || "untitled";
      // An editable page closes the window's tab as the window does; a
      // page that only reads leaves the window's tabs to the window.
      strip.appendChild(tabButton(name, path, cls, () => {
        followDoc = id === sessionDoc();
        showDoc(id);
      }, canClose ? () => post({ t: "close", doc: id }) : null, "Close " + name + " in the mutible window"));
    };
    for (const w of state.windows)
      for (const g of w.groups || []) for (const t of g.tabs || []) add(t.doc, t.path);
    for (const d of state.docs) add(d.id, d.path);
    // The files this page opened itself, which only it shows and closes.
    for (const f of L.tabs) {
      const name = f.label || base(f.path);
      strip.appendChild(tabButton(name, f.abs + " (on this page only)", "local" + (L.on === f.path ? " shown" : ""),
                                  () => showLocal(f.path), () => closeLocal(f.path), "Close " + name + " on this page"));
    }
    if (!seen.size && !L.tabs.length) {
      const e = document.createElement("span");
      e.className = "empty"; e.textContent = "No documents are open in this session.";
      strip.appendChild(e);
    }
  }

  // ---- the panes: the editor; the browser, to its right at the share the
  // window gives it; the terminal, below both. Each shows while the window
  // shows it, unless this page hid or showed it itself (a choice kept until
  // the window shows or hides that pane again). Hiding a pane here never
  // hides the window's: this screen's room is its own.
  const P = { browser: { mine: null, window: null }, terminal: { mine: null, window: null } };
  function windowPanes() {
    if (!state) return null;
    for (const w of state.windows) if (w.panes) return w.panes;
    return null;
  }
  // Whether the page shows `name` ("browser", "terminal"), given that it
  // has something to show in it.
  function paneOn(name, has) {
    if (!has) return false;
    const p = P[name], panes = windowPanes();
    const win = panes ? !!panes[name] : true;   // a host that says nothing: shown
    return p.mine !== null ? p.mine : win;
  }
  // The window's own choice, as last seen: a change of it ends the page's.
  function takeWindowPanes() {
    const panes = windowPanes();
    for (const name of ["browser", "terminal"]) {
      const win = panes ? !!panes[name] : null;
      if (win !== P[name].window) P[name].mine = null;
      P[name].window = win;
    }
    $("top").style.setProperty("--webshare", Math.round(((panes && panes.browserShare) || 0.5) * 1000) / 10 + "%");
  }
  function setPane(name, on) {
    P[name].mine = on;
    if (name === "browser") drawWeb(); else drawTermTabs();
    drawSelection();
  }
  function paneButton(id, has, on) {
    const b = $(id);
    b.classList.toggle("hidden", !has);
    b.setAttribute("aria-pressed", on ? "true" : "false");
  }

  // ---- the window's browser tabs, in their own pane: the one picked in a
  // frame, with a link to open it in a tab of this browser (a site that
  // refuses frames stays blank, which the page cannot always tell), and a
  // line when the address is on the machine running mutible. Only http and
  // https are framed. The pane follows the window's tab on show until a
  // tab is picked here.
  const W = { url: "", follow: true, pending: "", pendingAt: 0 };
  function hostOf(url) { try { return new URL(url).host || url; } catch (e) { return url; } }
  function browserTabs() {
    const out = [];
    if (!state) return out;
    for (const w of state.windows) {
      const b = w.browser;
      if (b && b.tabs) b.tabs.forEach((tab, i) => out.push({ tab: tab, onShow: i === b.active }));
    }
    return out;
  }
  // The tab the pane shows: the one picked here while the window has it,
  // else the window's own.
  function webTab(tabs) {
    if (!W.follow) for (const t of tabs) if (t.tab.url === W.url) return t.tab;
    W.follow = true;
    for (const t of tabs) if (t.onShow) return t.tab;
    return tabs.length ? tabs[0].tab : null;
  }
  function showWeb(tab) {
    W.url = tab.url;
    W.follow = false;
    for (const t of browserTabs()) if (t.onShow && t.tab.url === tab.url) W.follow = true;
    drawWeb();
  }
  // The browser pane as the window and this page say: its tabs, and the
  // tab on show framed; hidden, the frame lets its page go (no sound or
  // work left running behind it).
  function drawWeb() {
    const tabs = browserTabs(), on = paneOn("browser", tabs.length > 0);
    paneButton("webbtn", tabs.length > 0, on);
    const sec = $("websec"), f = $("web");
    sec.classList.toggle("hidden", !on);
    const strip = $("webtabs");
    strip.textContent = "";
    if (!on) {
      if (f.getAttribute("src") && f.getAttribute("src") !== "about:blank") f.src = "about:blank";
      return;
    }
    const tab = webTab(tabs);
    W.url = tab ? tab.url : "";
    for (const { tab: t, onShow: shownThere } of tabs) {
      let cls = "web";
      if (t.url === W.url) cls += " shown";
      if (shownThere) cls += " onshow";
      strip.appendChild(tabButton(t.title || hostOf(t.url), t.url, cls, () => showWeb(t)));
    }
    if (!tab) return;
    const title = $("webtitle");
    title.textContent = tab.title || hostOf(tab.url);
    const u = document.createElement("span");
    u.id = "weburl"; u.textContent = tab.url;
    title.appendChild(u);
    const open = $("webopen");
    if (tab.web) {
      if (f.getAttribute("src") !== tab.url) f.src = tab.url;
      open.href = tab.url;
    } else {
      f.src = "about:blank";
      open.removeAttribute("href");
    }
    f.classList.toggle("hidden", !tab.web);
    open.classList.toggle("hidden", !tab.web);
    $("webhint").classList.toggle("hidden", !tab.web);
    $("webnote").textContent = tab.web ? "" : "This tab is not an http or https page; it shows only in the mutible window.";
    $("webnote").classList.toggle("hidden", !!tab.web);
    $("webloop").classList.toggle("hidden", !tab.loopback);
  }
  // Addresses a person types: a scheme kept as typed (the server refuses
  // all but http and https), else http for this machine and https for the
  // rest.
  function typedAddress(s) {
    s = s.trim();
    if (!s || /^[a-z][a-z0-9+.-]*:[/][/]/i.test(s) ||
        /^(javascript|data|file|about|blob|vbscript|mailto|view-source):/i.test(s)) return s;
    const local = /^(localhost|127[.]|\[::1\]|0[.]0[.]0[.]0)/i.test(s);
    return (local ? "http:" : "https:") + "//" + s;
  }
  function sameAddress(a, b) { return a.replace(/[/]+$/, "") === b.replace(/[/]+$/, ""); }
  function onBrowsing(m) {
    if (!m.ok) { toast(m.why || "That address was not opened."); return; }
    $("browse").value = "";
    toast("Opening in the mutible window's browser pane.");
    W.pending = m.url; W.pendingAt = Date.now();
    takeWebPending();
  }
  // The address asked for shows here once the window's tabs have it.
  function takeWebPending() {
    if (!W.pending) return;
    if (Date.now() - W.pendingAt > 15000) { W.pending = ""; return; }
    for (const { tab } of browserTabs())
      if (sameAddress(tab.url, W.pending)) {
        W.pending = "";
        if (!paneOn("browser", true)) P.browser.mine = true;   // asked for here: shown here
        showWeb(tab);
        return;
      }
  }
  function drawTermTabs() {
    const strip = $("termtabs");
    strip.textContent = "";
    if (!state) return;
    // Only the terminals the session has now: one its window closed, or
    // that ended, is gone from the state and from here.
    const on = paneOn("terminal", state.terms.length > 0);
    paneButton("termbtn", state.terms.length > 0, on);
    $("termsec").classList.toggle("none", !on);
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
    hideViewer();
    if (L.on) {
      L.on = "";
      if (id === want.doc) { want.doc = 0; D.id = 0; }   // asked for again: the server sends it whole
    }
    if (id !== want.doc) {
      if (!synced()) { D.switchTo = id; return; }
      want.doc = id;
      sendShow();
    }
    drawTabs();
    markCurrent();
  }
  function showTerm(id) { if (id !== want.term) { want.term = id; sendShow(); } drawTermTabs(); }

  // ---- files this page opened itself: read from the session's folder as
  // they are on disk (readfile), shown here only, read-only, and closed
  // here; the window's tabs stay as they are. A page that only reads opens
  // every file this way; an editable one when the session has no window to
  // open it in.
  const L = { tabs: [], on: "" };   // tabs: the filetext messages, by path
  function showLocal(rel) {
    const f = L.tabs.find((x) => x.path === rel);
    if (!f) return;
    if (!synced()) { toast("Waiting for the session to confirm typing before showing another file."); return; }
    if (f.viewer) { showViewer(f); return; }
    hideViewer();
    L.on = rel;
    if (want.doc) { want.doc = 0; sendShow(); }   // the session's document stops coming
    followDoc = false;
    D.id = -1; D.kind = f.kind; D.path = f.abs; D.version = f.version || 0;
    D.editable = false;
    D.indent = f.indent || D.indent;
    D.flight = null; D.queue = []; D.undo = 0; D.save = false;
    D.html = f.html || "";
    D.latex = !!f.latex;
    D.a = D.h = 0; D.goal = null; D.moved = true;
    D.mode = f.kind === "markdown" || D.latex ? "page" : "code";
    $("code").scrollTop = 0;
    $("docpath").textContent = inFolder(f.abs) + " (on this page only)";
    if (f.kind === "large") {
      D.shadow = []; D.lines = []; reindex(); render();
      $("code").classList.add("hidden"); $("md").classList.add("hidden"); $("modebtn").classList.add("hidden");
      hideTex();
      const n = $("note"); n.textContent = f.note; n.classList.remove("hidden");
      drawSelection();
    } else {
      D.shadow = (f.lines || []).map(lineOf);
      rebuild();
      showMode();
    }
    drawTabs();
    markCurrent();
  }
  function closeLocal(rel) {
    L.tabs = L.tabs.filter((x) => x.path !== rel);
    if (rel === kReview) { G.review = null; G.cur = ""; drawGit(); watchGit(); }
    if (L.on === rel) {
      hideViewer();
      // Back to the window's tab on show.
      L.on = "";
      D.id = 0;
      followDoc = true;
      want.doc = sessionDoc();
      sendShow();
      if (!want.doc) showNote("No document is on show.");
    }
    drawTabs();
    markCurrent();
  }
  // ---- a file the browser shows itself (R3), in a tab of this page's own:
  // served by the session's server at /v/ID/file/PATH, read from the
  // host's folder, in the browser's own viewers; a CSV or TSV as rows the
  // server parsed (`file`, answered `table`).
  const V = { on: "" };   // the viewer tab on show, by path
  function fileUrl(rel) {
    return viewBase + "file/" + rel.split("/").map(encodeURIComponent).join("/");
  }
  function fileNote(text) {
    const box = $("fileview");
    box.textContent = ""; box.className = "";
    const n = document.createElement("div");
    n.id = "filenote"; n.textContent = text;
    box.appendChild(n);
  }
  function openViewer(rel, view) {
    if (!synced()) { toast("Waiting for the session to confirm typing before showing another file."); return; }
    if (!L.tabs.find((x) => x.path === rel))
      L.tabs.push({ path: rel, abs: T.root ? T.root + "/" + rel : rel, view: view, viewer: true });
    showLocal(rel);
  }
  function showViewer(f) {
    const rel = f.path, view = f.view;
    L.on = rel;
    if (want.doc) { want.doc = 0; sendShow(); }   // the session's document stops coming
    followDoc = false;
    D.id = 0; D.kind = ""; D.editable = false; D.flight = null; D.queue = [];
    $("code").classList.add("hidden"); $("md").classList.add("hidden"); mdHtml = null;
    $("modebtn").classList.add("hidden"); $("note").classList.add("hidden");
    hideTex();
    $("docpath").textContent = f.label ? f.abs : rel + " (on this page only)";
    drawSelection();
    const box = $("fileview"), url = fileUrl(rel), open = $("fileopen");
    box.classList.remove("hidden");
    $("filemore").classList.add("hidden");
    open.href = url;
    // Only what the server serves as bytes has an address of its own.
    open.classList.toggle("hidden", view === "table" || view === "notebook" || view === "database" ||
                          view === "review");
    if (V.on !== rel) {
      V.on = rel;
      box.textContent = ""; box.className = "";
      let el = null;
      if (view === "image") {
        el = document.createElement("img"); el.alt = base(rel); box.className = "center";
      } else if (view === "audio" || view === "video") {
        el = document.createElement(view); el.controls = true; el.preload = "metadata"; box.className = "center";
      } else if (view === "pdf") {
        el = document.createElement("iframe"); el.title = base(rel);
      } else if (view === "html") {
        // Sandboxed with no scripts and an origin of its own, as the
        // server's CSP serves it too: the page is drawn, nothing in it runs.
        el = document.createElement("iframe"); el.setAttribute("sandbox", "");
        el.referrerPolicy = "no-referrer"; el.title = base(rel);
      } else if (view === "table") {
        if (f.table) drawTable(f.table);
        else { fileNote("Reading " + base(rel) + "…"); post({ t: "file", path: rel }); }
      } else if (view === "notebook") {
        if (f.nb) drawNotebook(f.nb);
        else { fileNote("Reading " + base(rel) + "…"); post({ t: "notebook", path: rel }); }
      } else if (view === "database") {
        if (f.db) drawDb(f.db);
        else { fileNote("Reading " + base(rel) + "…"); askDb(rel, "", 0); }
      } else if (view === "review") {
        drawReview();
      }
      if (el) {
        el.addEventListener("error", () => { if (V.on === rel) fileNote(base(rel) + " could not be shown here."); });
        el.src = url;
        box.appendChild(el);
      }
    } else if (view === "table" && f.table) {
      drawTable(f.table);
    } else if (view === "review") {
      scrollReview();
    }
    drawTabs();
    markCurrent();
  }
  // The file's view goes (no sound left playing), and the editor's area is
  // the editor's again.
  function hideViewer() {
    if (!V.on) return;
    V.on = "";
    $("fileview").textContent = "";
    $("fileview").classList.add("hidden");
    $("filemore").classList.add("hidden");
    $("fileopen").classList.add("hidden");
  }
  function drawTable(m) {
    const box = $("fileview");
    box.textContent = ""; box.className = "";
    if (m.error !== undefined) { fileNote(m.error); return; }
    if (!(m.rows || []).length) { fileNote(base(m.path) + " is empty."); return; }
    const table = document.createElement("table"), body = document.createElement("tbody");
    m.rows.forEach((row, i) => {
      const tr = document.createElement("tr");
      if (i === 0) tr.className = "head";   // the first row is usually the header, as the window shows it
      const n = document.createElement("th");
      n.className = "rn"; n.textContent = String(i + 1);
      tr.appendChild(n);
      for (let c = 0; c < (m.columns || 0); c++) {
        const td = document.createElement("td");
        td.textContent = c < row.length ? row[c] : "";
        tr.appendChild(td);
      }
      body.appendChild(tr);
    });
    table.appendChild(body);
    box.appendChild(table);
    const more = $("filemore");
    more.textContent = m.truncated ? "The first " + m.rows.length + " rows; the rest shows in the mutible window." : "";
    more.classList.toggle("hidden", !m.truncated);
  }
  // A notebook: the server's page in a frame sandboxed with no scripts and
  // an origin of its own; its pictures are data: images, which the page's
  // CSP (inherited by the frame) allows, and nothing else is fetched.
  function drawNotebook(m) {
    const box = $("fileview");
    box.textContent = ""; box.className = "";
    if (m.error !== undefined) { fileNote(m.error); return; }
    const el = document.createElement("iframe");
    el.setAttribute("sandbox", "");
    el.className = "nb"; el.title = base(m.path);
    el.srcdoc = withPalette(m.html || "");
    box.appendChild(el);
  }
  function onNotebook(m) {
    const f = L.tabs.find((x) => x.viewer && x.path === m.path);
    if (!f) return;
    f.nb = m;
    if (V.on === m.path) drawNotebook(m);
  }
  // A database: its tables in a menu, a page of the one picked's rows, and
  // the pages before and after it, each asked of the server (db), which
  // opens the file read-only every time.
  function askDb(path, table, offset) {
    post({ t: "db", path: path, table: table, offset: offset });
  }
  function sizeText(n) {
    return n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : n >= 1024 ? (n / 1024).toFixed(1) + " KB" : n + " bytes";
  }
  function drawDb(m) {
    const box = $("fileview");
    box.textContent = ""; box.className = "db";
    const bar = document.createElement("div");
    bar.id = "dbbar";
    const tables = m.tables || [];
    if (tables.length) {
      const sel = document.createElement("select");
      sel.id = "dbtable"; sel.setAttribute("aria-label", "Table");
      tables.forEach((t) => {
        const o = document.createElement("option");
        o.value = t.name;
        o.textContent = t.type === "view" ? t.name + " (view)" : t.name;
        sel.appendChild(o);
      });
      if (m.moreTables) {
        const o = document.createElement("option");
        o.disabled = true; o.textContent = m.moreTables + " more not listed";
        sel.appendChild(o);
      }
      sel.value = m.table || "";
      sel.addEventListener("change", () => askDb(m.path, sel.value, 0));
      bar.appendChild(sel);
    }
    const rows = m.rows || [], off = m.offset || 0, page = Math.max(rows.length, 100);
    const where = document.createElement("span");
    where.id = "dbwhere";
    where.textContent = m.error !== undefined || !tables.length ? "" :
      rows.length ? "Rows " + (off + 1) + "–" + (off + rows.length) + (m.more ? ", more after" : "") :
      off ? "No rows past " + off : "No rows";
    bar.appendChild(where);
    const prev = document.createElement("button"), next = document.createElement("button");
    prev.type = next.type = "button";
    prev.textContent = "Previous"; next.textContent = "Next";
    prev.disabled = m.error !== undefined || off <= 0;
    next.disabled = m.error !== undefined || !m.more;
    prev.addEventListener("click", () => askDb(m.path, m.table || "", Math.max(0, off - page)));
    next.addEventListener("click", () => askDb(m.path, m.table || "", off + rows.length));
    if (tables.length) { bar.appendChild(prev); bar.appendChild(next); }
    const ro = document.createElement("span");
    ro.textContent = "Read-only"; ro.title = "Opened read-only: nothing on this page can change the database.";
    bar.appendChild(ro);
    box.appendChild(bar);
    const note = (text) => {
      const n = document.createElement("div");
      n.id = "filenote"; n.textContent = text;
      box.appendChild(n);
    };
    if (m.error !== undefined) { note(m.error); return; }
    if (!tables.length) { note(base(m.path) + " has no tables."); return; }
    const scroll = document.createElement("div");
    scroll.id = "dbscroll";
    const table = document.createElement("table"), head = document.createElement("thead"),
          body = document.createElement("tbody"), hr = document.createElement("tr");
    const corner = document.createElement("th");
    corner.className = "rn";
    hr.appendChild(corner);
    (m.columns || []).forEach((c) => {
      const th = document.createElement("th");
      th.className = "col"; th.textContent = c;
      hr.appendChild(th);
    });
    head.appendChild(hr);
    rows.forEach((row, i) => {
      const tr = document.createElement("tr");
      const n = document.createElement("th");
      n.className = "rn"; n.textContent = String(off + i + 1);
      tr.appendChild(n);
      row.forEach((v) => {
        const td = document.createElement("td");
        if (v === null) { td.textContent = "NULL"; td.className = "null"; }
        else if (typeof v === "object" && v.blob !== undefined) { td.textContent = "blob, " + sizeText(v.blob); td.className = "blob"; }
        else if (typeof v === "object") { td.textContent = (v.text || "") + "…"; td.title = "Cut here: the value is " + sizeText(v.bytes || 0) + "."; }
        else td.textContent = String(v);
        tr.appendChild(td);
      });
      body.appendChild(tr);
    });
    table.appendChild(head);
    table.appendChild(body);
    scroll.appendChild(table);
    box.appendChild(scroll);
  }
  function onDb(m) {
    const f = L.tabs.find((x) => x.viewer && x.path === m.path);
    if (!f) return;
    f.db = m;
    if (V.on === m.path) drawDb(m);
  }
  function onTable(m) {
    const f = L.tabs.find((x) => x.viewer && x.path === m.path);
    if (!f) return;
    f.table = m;
    if (V.on === m.path) drawTable(m);
  }
  function onFileText(m) {
    const p = T.pending;
    if (p && p.rel === m.path) { T.pending = null; clearTimeout(T.timer); }
    if (m.error !== undefined) { treeNote(m.error); return; }
    const at = L.tabs.findIndex((x) => x.path === m.path);
    if (at >= 0) L.tabs[at] = m; else L.tabs.push(m);
    showLocal(m.path);
  }


  // ---- Source Control (R3's fourth slice): the folder's uncommitted
  // changes as the window's Source Control lists them, read by the server
  // with the core's fixed git commands (git), and their review page in a
  // tab of this page's own (review): each file's blocks, unchanged lines
  // folded (a click shows them), inline or side by side. Read only: no
  // stage, unstage, discard or commit, on any page. The server says again
  // when the changes move, as far as its host watches the folder (the Mac
  // does); otherwise ↻, a reconnect, or coming back to the page reads again.
  const kReview = "/changes";   // the review's tab, by a path no file of the folder has
  const G = { mode: false, status: null, review: null, opened: new Map(), side: new Set(), goto: "", cur: "" };
  function gitWanted() { return G.mode || L.tabs.some((x) => x.path === kReview); }
  function askGit() {
    if (!G.status) drawGit();
    post({ t: "git", watch: true });
  }
  function watchGit() { if (!gitWanted()) post({ t: "git", watch: false }); }
  function setSideMode(scm) {
    G.mode = scm;
    $("tree").classList.toggle("hidden", scm);
    $("git").classList.toggle("hidden", !scm);
    $("treenote").classList.add("hidden");
    $("filesbtn").setAttribute("aria-pressed", String(!scm));
    $("scmbtn").setAttribute("aria-pressed", String(scm));
    const r = $("refresh"), what = scm ? "Read the changes again" : "Refresh the tree";
    r.title = what; r.setAttribute("aria-label", what);
    if (scm) askGit(); else watchGit();
  }
  function dirOf(p) { const i = p.lastIndexOf("/"); return i < 0 ? "" : p.slice(0, i); }
  function letterEl(letter) {
    const l = document.createElement("span");
    l.className = "gl " + letter; l.textContent = letter;
    l.title = { M: "Modified", A: "Added", D: "Deleted", R: "Renamed", T: "Type changed", U: "Untracked",
                C: "Conflict" }[letter] || "";
    return l;
  }
  function sectionText(section, n) { return (section === "staged" ? "Staged Changes" : "Changes") + "  " + n; }
  function drawGit() {
    const sum = $("gitsum"), list = $("gitlist"), m = G.status;
    list.textContent = "";
    if (!m) { sum.textContent = "Reading the changes…"; return; }
    if (m.error) { sum.textContent = m.error; return; }
    const n = m.count || 0;
    sum.textContent = (m.branch ? m.branch + ": " : "") +
                      (n ? n + (n === 1 ? " file changed" : " files changed") : "No changes.");
    const counts = {};
    for (const f of m.files) counts[f.section] = (counts[f.section] || 0) + 1;
    let section = "";
    for (const f of m.files) {
      if (f.section !== section) {
        section = f.section;
        const h = document.createElement("div");
        h.className = "gsec"; h.textContent = sectionText(section, counts[section]);
        list.appendChild(h);
      }
      const row = document.createElement("div");
      row.className = "row" + (f.key === G.cur && L.on === kReview ? " current" : "");
      row.setAttribute("role", "listitem");
      row.dataset.key = f.key; row.dataset.path = f.path;
      row.title = f.header;
      const name = document.createElement("span"), dir = document.createElement("span");
      name.className = "gname"; name.textContent = base(f.path);
      dir.className = "gdir"; dir.textContent = dirOf(f.path);
      row.appendChild(name); row.appendChild(dir); row.appendChild(letterEl(f.letter));
      list.appendChild(row);
    }
  }
  function onGit(m) {
    G.status = m;
    drawGit();
    // The review on show follows a read that shows something new.
    if (L.tabs.some((x) => x.path === kReview) && (!G.review || G.review.version !== m.version)) askReview();
  }
  function openedJson() {
    const o = {};
    for (const [k, v] of G.opened) o[k] = [...v];
    return o;
  }
  function askReview() { post({ t: "review", opened: openedJson(), side: [...G.side] }); }
  function askReviewFile(key) {
    post({ t: "review", file: key, opened: [...(G.opened.get(key) || [])], side: G.side.has(key) });
  }
  function openReview(key) {
    if (!synced()) { toast("Waiting for the session to confirm typing before showing another file."); return; }
    G.goto = key; G.cur = key;
    if (!L.tabs.find((x) => x.path === kReview))
      L.tabs.push({ path: kReview, abs: "Uncommitted changes (read-only)", label: "Changes", view: "review",
                    viewer: true });
    if (!G.review) askReview();
    showLocal(kReview);
    drawGit();
  }
  // Text with its syntax colors and the words that changed, each piece a
  // span made by textContent.
  function spansInto(td, text, syn, words) {
    const cuts = new Set([0, text.length]);
    for (const s of syn || []) { cuts.add(s[0]); cuts.add(Math.min(text.length, s[0] + s[1])); }
    for (const s of words || []) { cuts.add(s[0]); cuts.add(Math.min(text.length, s[0] + s[1])); }
    const at = [...cuts].filter((x) => x >= 0 && x <= text.length).sort((a, b) => a - b);
    const inside = (list, a) => (list || []).find((s) => s[0] <= a && a < s[0] + s[1]);
    for (let i = 0; i + 1 < at.length; i++) {
      const a = at[i], b = at[i + 1];
      if (b <= a) continue;
      const sy = inside(syn, a), w = inside(words, a);
      if (!sy && !w) { td.appendChild(document.createTextNode(text.slice(a, b))); continue; }
      const e = document.createElement("span");
      e.className = (sy ? "s" + sy[2] : "") + (w ? (w[2] ? " wa" : " wr") : "");
      e.textContent = text.slice(a, b);
      td.appendChild(e);
    }
  }
  function cell(cls, text) {
    const td = document.createElement("td");
    td.className = cls;
    if (text !== undefined) td.textContent = text;
    return td;
  }
  function reviewRows(f) {
    const table = document.createElement("table"), body = document.createElement("tbody");
    if (f.side) table.className = "side";
    const cols = f.side ? 4 : 3;
    for (const r of f.rows || []) {
      const tr = document.createElement("tr");
      if (r[0] === "g" || r[0] === "n") {
        tr.className = r[0] === "g" ? "gap" : "rnote";
        const td = cell("", r[3]);
        td.colSpan = cols;
        tr.appendChild(td);
        if (r[0] === "g") {
          tr.title = "Show these lines";
          tr.addEventListener("click", () => {
            if (!G.opened.has(f.key)) G.opened.set(f.key, new Set());
            G.opened.get(f.key).add(r[1]);
            askReviewFile(f.key);
          });
        }
      } else if (r[0] === "s") {
        // [s, kind, old, new, oldText, newText, oldSyn, newSyn, oldWords, newWords]
        const k = r[1];
        tr.className = "s" + k;
        const lt = cell("t" + (r[2] ? (k === "m" || k === "r" ? " r" : "") : " fill"));
        const rt = cell("t" + (r[3] ? (k === "m" || k === "a" ? " a" : "") : " fill"));
        if (r[2]) spansInto(lt, r[4], r[6], r[8]);
        if (r[3]) spansInto(rt, r[5], r[7], r[9]);
        tr.appendChild(cell("n", r[2] ? String(r[2]) : ""));
        tr.appendChild(lt);
        tr.appendChild(cell("n", r[3] ? String(r[3]) : ""));
        tr.appendChild(rt);
      } else {
        // [kind, old, new, text, syntax, words | part]
        tr.className = r[0] + (r[0] === "x" ? " p" + r[5] : "");
        tr.appendChild(cell("n", r[1] ? String(r[1]) : ""));
        tr.appendChild(cell("n", r[2] ? String(r[2]) : ""));
        const td = cell("t");
        spansInto(td, r[3], r[4], r[0] === "r" || r[0] === "a" ? r[5] : null);
        tr.appendChild(td);
      }
      body.appendChild(tr);
    }
    table.appendChild(body);
    return table;
  }
  function reviewFile(f) {
    const box = document.createElement("div");
    box.className = "rfile"; box.dataset.key = f.key;
    const head = document.createElement("div");
    head.className = "rhead";
    head.appendChild(letterEl(f.letter));
    const path = document.createElement("span");
    path.className = "rpath"; path.textContent = f.header; path.title = f.header;
    head.appendChild(path);
    const button = (text, title, onclick) => {
      const b = document.createElement("button");
      b.type = "button"; b.textContent = text; b.title = title;
      b.addEventListener("click", onclick);
      head.appendChild(b);
    };
    if (f.more) button("Show Changes", "This file's changes, which the page left out to stay quick",
                       () => askReviewFile(f.key));
    else if (f.canSide)
      button(f.side ? "Inline" : "Side by Side", f.side ? "Removed lines above added ones" : "The two versions side by side",
             () => { if (f.side) G.side.delete(f.key); else G.side.add(f.key); askReviewFile(f.key); });
    box.appendChild(head);
    if (!f.more) {
      const body = document.createElement("div");
      body.className = "rbody";
      body.appendChild(reviewRows(f));
      box.appendChild(body);
    }
    return box;
  }
  // Where the eye is: the file at the top of the view, and how far into it.
  function reviewAnchor() {
    const view = $("fileview"), box = $("review");
    if (!box) return null;
    for (const f of box.querySelectorAll(".rfile"))
      if (f.offsetTop + f.offsetHeight > view.scrollTop) return { key: f.dataset.key, off: view.scrollTop - f.offsetTop };
    return null;
  }
  function fileEl(key) {
    const box = $("review");
    if (!box) return null;
    for (const f of box.querySelectorAll(".rfile")) if (f.dataset.key === key) return f;
    return null;
  }
  function scrollReview(keep) {
    const view = $("fileview");
    if (G.goto) {
      const f = fileEl(G.goto);
      if (f) { view.scrollTop = f.offsetTop; G.goto = ""; }
      return;
    }
    if (keep) {
      const f = fileEl(keep.key);
      if (f) view.scrollTop = f.offsetTop + keep.off;
    }
  }
  function drawReview() {
    if (V.on !== kReview) return;
    const box = $("fileview"), m = G.review, keep = reviewAnchor();
    if (!m || m.pending) { fileNote("Reading the changes…"); return; }
    if (m.error) { fileNote(m.error); return; }
    if (!(m.files || []).length) { fileNote("No changes."); return; }
    box.textContent = ""; box.className = "review";
    const wrap = document.createElement("div");
    wrap.id = "review";
    const counts = {};
    for (const f of m.files) counts[f.section] = (counts[f.section] || 0) + 1;
    let section = "";
    for (const f of m.files) {
      if (f.section !== section) {
        section = f.section;
        const h = document.createElement("div");
        h.className = "rsec"; h.textContent = sectionText(section, counts[section]);
        wrap.appendChild(h);
      }
      wrap.appendChild(reviewFile(f));
    }
    box.appendChild(wrap);
    scrollReview(keep);
  }
  function onReview(m) {
    if (m.pending) { if (!G.review) G.review = m; drawReview(); return; }
    G.review = m;
    drawReview();
  }
  function onReviewFile(m) {
    if (!G.review || !G.review.files) return;
    const key = m.file ? m.file.key : m.key, at = G.review.files.findIndex((f) => f.key === key);
    if (at < 0) return;
    if (m.gone) { askReview(); return; }   // the changes moved: the whole review again
    G.review.files[at] = m.file;
    const old = fileEl(key);
    if (old && V.on === kReview) old.replaceWith(reviewFile(m.file));
  }

  // ---- the tree: one folder's rows at a time, read by the host each time
  // a folder opens (list), folders first as the apps list them, dotfiles as
  // the window shows them, what the ignore files ignore in tree.git-ignored's
  // color. On an editable page a click on a file opens it as a tab in the
  // mutible window (open), as a click in the window's own tree would (a
  // double-click keeps it), and this page shows that tab once the session's
  // state has it. A page that only reads changes nothing in the window: it
  // reads the file for a tab of its own (readfile).
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
    if (L.on) return L.on;
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
    if (narrow.matches) setSide(false);
    if (e.view) {
      // A file the browser shows itself: in a tab of this page's own; an
      // editable page opens it in the window too, as for any file, and
      // stays on it here.
      openViewer(rel, e.view);
      if (canOpen) {
        T.pending = { rel: rel, kind: e.kind || "text", abs: "", shown: true };
        if (!post({ t: "open", path: rel, keep: !!keep })) T.pending = null;
      }
      return;
    }
    if (!canOpen) {
      // This page's own tab: the window's tabs stay as they are. A file
      // the window has open already is shown from its tab, live.
      const abs = T.root ? T.root + "/" + rel : "";
      const d = state && abs ? state.docs.find((x) => x.path === abs) : null;
      if (d) { followDoc = d.id === sessionDoc(); showDoc(d.id); return; }
      if (kViewerKinds.has(e.kind || "text")) {
        treeNote(base(rel) + " is not a file this page can show; it opens in the mutible window.");
        return;
      }
      T.pending = { rel: rel, kind: e.kind || "text", abs: "" };
      if (!post({ t: "readfile", path: rel })) T.pending = null;
      return;
    }
    T.pending = { rel: rel, kind: e.kind || "text", abs: "" };
    if (!post({ t: "open", path: rel, keep: !!keep })) { T.pending = null; return; }
  }
  function onOpening(m) {
    const p = T.pending;
    if (!p || p.rel !== m.path) return;
    if (p.shown) { T.pending = null; return; }
    if (!m.ok) { T.pending = null; treeNote(m.why); return; }
    if (kViewerKinds.has(p.kind)) {
      T.pending = null;
      treeNote(base(m.path) + " opens in the mutible window.");
      return;
    }
    p.abs = m.abs;
    clearTimeout(T.timer);
    // A file the window shows in a viewer of its own has no document here.
    T.timer = setTimeout(() => {
      if (T.pending !== p) return;
      T.pending = null;
      treeNote(base(p.rel) + " opened in the mutible window.");
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
    // A file of this page's own stays on show until it is closed here.
    if (!L.on && (followDoc || !docs.has(want.doc)) && (synced() || !docs.has(want.doc))) {
      followDoc = true; want.doc = sessionDoc();
    }
    const terms = m.terms.map((t) => t.id);
    if (followTerm && terms.length && !terms.includes(want.term)) want.term = terms[0];
    if (!terms.includes(want.term)) { followTerm = true; want.term = terms.length ? terms[0] : 0; }
    sendShow();
    takeWindowPanes();
    drawTabs(); drawTermTabs(); drawWeb();
    if (!want.doc && !L.on) showNote("No document is on show.");
    takePending();
    takeWebPending();
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
              undo: 0, save: false, switchTo: 0, mode: "code", html: "", latex: false };
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
    if (!canSave) return toast("Nothing here saves to disk yet: save from the mutible window.");
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
    hideViewer();
    hideTex();
    $("code").classList.add("hidden"); $("md").classList.add("hidden"); mdHtml = null;
    $("modebtn").classList.add("hidden");
    const n = $("note"); n.textContent = text; n.classList.remove("hidden");
    $("docpath").textContent = "";
    D.kind = "";
    drawSelection();
  }
  function showMode() {
    const code = $("code"), md = $("md");
    // A Markdown document's page is the core's HTML; a LaTeX document's
    // the app's typeset PDF.
    const tex = D.kind === "code" && D.latex;
    const page = (D.kind === "markdown" || tex) && D.mode === "page";
    $("modebtn").classList.toggle("hidden", D.kind !== "markdown" && !tex);
    $("modebtn").textContent = page ? (D.editable ? "Edit Source" : "Show Source") : "Show Page";
    $("note").classList.add("hidden");
    code.classList.toggle("hidden", page);
    md.classList.toggle("hidden", !page || tex);
    if (page && !tex) setMarkdown(D.html);
    else { md.removeAttribute("srcdoc"); mdHtml = null; }
    if (page && tex) showTex(L.on || D.path);
    else hideTex();
    drawSelection();
  }
  // ---- a LaTeX document's pages (R3's third slice): the PDF the app's own
  // LaTeX preview typeset, at the server's /typeset/ address. Asked for
  // each time the page shows it (`latex`, the path as this page knows it:
  // a tab of its own by its path from the folder, a document by the
  // session's path), and drawn again when the server says the app
  // typeset it anew (a new address, so the frame loads the new PDF).
  const X = { asked: "", m: null };
  function texNote(text) { const n = $("texnote"); n.textContent = text || ""; n.classList.toggle("hidden", !text); }
  function showTex(path) {
    $("tex").classList.remove("hidden");
    if (X.asked !== path) {
      X.asked = path; X.m = null;
      $("texframe").removeAttribute("src"); $("texframe").classList.add("hidden");
      $("texlog").textContent = ""; $("texlog").classList.add("hidden");
      texNote("Typesetting…");
    }
    post({ t: "latex", path: path });
  }
  function hideTex() {
    X.asked = ""; X.m = null;
    $("tex").classList.add("hidden");
    $("texframe").removeAttribute("src");   // nothing left loading
    $("texlog").textContent = "";
  }
  function onLatex(m) {
    if (!X.asked || m.path !== X.asked) return;
    X.m = m;
    const frame = $("texframe"), log = $("texlog");
    const failed = m.state === "failed" || m.state === "refused";
    if (m.url && !failed) {
      if (frame.getAttribute("src") !== m.url) frame.setAttribute("src", m.url);
      frame.classList.remove("hidden");
    } else {
      frame.removeAttribute("src"); frame.classList.add("hidden");
    }
    log.textContent = m.state === "failed" ? m.text || "" : "";
    log.classList.toggle("hidden", m.state !== "failed");
    if (m.state === "ready") texNote("");
    else if (m.state === "working") texNote(m.url ? "Typesetting again…" : "Typesetting…");
    else if (m.state === "failed") texNote("The document did not typeset. tectonic said:");
    else texNote(m.text || "");
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
    D.latex = !!m.latex;
    if (!same) {
      D.a = D.h = 0; D.goal = null; D.moved = true;
      // A LaTeX document opens on its pages, as the window's does.
      D.mode = (m.kind === "markdown" && !D.editable) || (m.kind === "code" && D.latex) ? "page" : "code";
      $("code").scrollTop = 0;
    }
    $("docpath").textContent = inFolder(m.path);
    if (m.kind === "large") {
      D.shadow = []; D.lines = []; reindex(); render();
      $("code").classList.add("hidden"); $("md").classList.add("hidden"); $("modebtn").classList.add("hidden");
      hideTex();
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
    sendTermSize();
  }
  // The cells this page's terminal pane holds, said to the server: the pty
  // takes them only while this page is the one typing there (the server
  // sends the size of the page that typed last), so a narrow page that
  // only looks shrinks nothing. The terminal is drawn at the pty's size
  // from "term", scrolled sideways when wider than the pane.
  let sentSize = "";
  function sendTermSize() {
    if (!editable || !want.term || !ws || ws.readyState !== 1) return;
    const t = $("term");
    if (!t.clientWidth || !t.clientHeight) return;   // hidden
    let probe = $("probe");
    if (!probe) {
      // A hundred cells in the terminal's font, in a box of no size, so
      // it adds nothing to what the pane scrolls.
      const hold = document.createElement("div");
      hold.style.cssText = "position: absolute; width: 0; height: 0; overflow: hidden; visibility: hidden";
      probe = document.createElement("span");
      probe.id = "probe";
      probe.style.cssText = "display: inline-block; white-space: pre; line-height: var(--lh)";
      probe.textContent = "0".repeat(100);
      hold.appendChild(probe);
      t.appendChild(hold);
    }
    const box = probe.getBoundingClientRect(), cs = getComputedStyle(t);
    if (!box.width || !box.height) return;
    const padX = parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight);
    const padY = parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
    const cols = Math.max(2, Math.floor((t.clientWidth - padX) / (box.width / 100)));
    const rows = Math.max(1, Math.floor((t.clientHeight - padY) / box.height));
    const text = JSON.stringify({ t: "tsize", term: want.term, cols: cols, rows: rows });
    if (text === sentSize) return;
    sentSize = text;
    ws.send(text);
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
  // Waiting for the app's answer, refused, or expired: the sentence alone.
  function gate(text) { const g = $("gate"); g.textContent = text; g.classList.toggle("hidden", !text); }
  // Someone using the page keeps its credential alive: at most once a
  // minute, while the page shows and was used since.
  let usedAt = 0, aliveAt = 0;
  function used() {
    usedAt = Date.now();
    if (usedAt - aliveAt >= 60000 && !document.hidden && post({ t: "alive" })) aliveAt = usedAt;
  }
  for (const type of ["keydown", "pointerdown", "wheel", "touchstart"])
    window.addEventListener(type, used, { capture: true, passive: true });
  setInterval(() => {
    if (!document.hidden && usedAt > aliveAt && post({ t: "alive" })) aliveAt = Date.now();
  }, 60000);
  function connect() {
    const scheme = location.protocol === "https:" ? "wss://" : "ws://";
    ws = new WebSocket(scheme + location.host + viewBase + "ws");
    ws.onopen = () => { said = ""; sentSize = ""; banner(""); };
    ws.onmessage = (ev) => {
      const m = JSON.parse(ev.data);
      switch (m.t) {
        case "waiting": gate(m.text); setStatus("waiting"); break;
        case "denied": case "expired":
          ended = true; gate(m.text); setStatus(m.t === "denied" ? "refused" : "expired");
          hideViewer(); hideTex(); $("web").setAttribute("src", "about:blank");
          break;
        case "hello":
          gate("");
          $("machine").textContent = m.machine || "mutible";
          folder = m.folder;
          termNote = m.termNote || "";
          editable = !!m.editable;
          canSave = !!m.canSave;
          canBrowse = !!m.canBrowse;
          canOpen = !!m.canOpen;
          canClose = !!m.canClose;
          $("browse").classList.toggle("hidden", !canBrowse);
          setStatus(editable ? "live, editing" : "live, read-only", true);
          document.body.classList.toggle("editable", editable);
          // Marks keep the path's slashes where they belong in a field
          // that cuts from the left.
          $("folder").textContent = "‎" + m.folder + "‎";
          document.title = (m.machine ? m.machine + ": " : "") + base(m.folder) + " (mutible)";
          applyPalette(m.palette);
          // The tree afresh: every folder that was open, read again (the
          // rows on show stay until the answers replace them).
          T.err.clear();
          refreshTree();
          // A LaTeX document's pages on show: asked for again on the new
          // connection, which knows nothing of the last one's.
          if (X.asked) post({ t: "latex", path: X.asked });
          // Source Control on show, or its review: read again.
          if (gitWanted()) askGit();
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
        case "browsing": onBrowsing(m); break;
        case "filetext": onFileText(m); break;
        case "table": onTable(m); break;
        case "notebook": onNotebook(m); break;
        case "db": onDb(m); break;
        case "latex": onLatex(m); break;
        case "git": onGit(m); break;
        case "review": onReview(m); break;
        case "reviewfile": onReviewFile(m); break;
        case "closing": if (!m.ok) toast(m.why || "That tab was not closed."); break;
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
  window.addEventListener("resize", () => { drawSelection(); sendTermSize(); });
  if (window.ResizeObserver) new ResizeObserver(() => sendTermSize()).observe($("term"));
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
  $("refresh").addEventListener("click", () => { if (G.mode) askGit(); else refreshTree(); });
  $("filesbtn").addEventListener("click", () => setSideMode(false));
  $("scmbtn").addEventListener("click", () => setSideMode(true));
  $("gitlist").addEventListener("click", (ev) => {
    const row = ev.target.closest(".row");
    if (row && row.dataset.key) openReview(row.dataset.key);
  });
  $("browse").addEventListener("keydown", (e) => {
    if (e.key === "Escape") { e.target.value = ""; e.target.blur(); return; }
    if (e.key !== "Enter" || e.isComposing) return;
    e.preventDefault();
    const url = typedAddress(e.target.value);
    if (url && !post({ t: "browse", url: url })) toast("Not connected to the session.");
  });
  $("sidebtn").addEventListener("click", () => setSide(!sideOpen()));
  $("webclose").addEventListener("click", () => setPane("browser", false));
  $("termclose").addEventListener("click", () => setPane("terminal", false));
  $("webbtn").addEventListener("click", () => setPane("browser", $("websec").classList.contains("hidden")));
  $("termbtn").addEventListener("click", () => setPane("terminal", $("termsec").classList.contains("none")));
  // At phone width a tap beside the open tree closes it.
  $("work").addEventListener("pointerdown", () => { if (narrow.matches && sideOpen()) setSide(false); });
  // Back to the page after a while: the folders may have changed meanwhile.
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) return;
    refreshTree();
    if (gitWanted()) askGit();   // a host that watches nothing has not said
  });
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
    browser: () => [...$("webtabs").querySelectorAll("button.web")].map((b) => b.title),
    web: () => ($("websec").classList.contains("hidden") ? null
                : { src: $("web").getAttribute("src") || "", open: $("webopen").getAttribute("href") || "",
                    loopback: !$("webloop").classList.contains("hidden") }),
    // Which panes show, and where the browser is: beside the editor
    // (its left edge past the editor's) or below it.
    panes: () => ({ browser: !$("websec").classList.contains("hidden"),
                    terminal: !$("termsec").classList.contains("none"),
                    beside: $("websec").getBoundingClientRect().left > $("docsec").getBoundingClientRect().left + 10 }),
    tabs: () => [...$("tabs").querySelectorAll("button")].map((b) => (b.classList.contains("local") ? "+" : "") +
                                                                      b.firstChild.textContent),
    local: () => L.on,
    file: () => {
      if (!V.on) return null;
      const f = L.tabs.find((x) => x.path === V.on) || {};
      const box = $("fileview"), el = box.firstElementChild, rows = [...box.querySelectorAll("tbody tr")];
      return { view: f.view, path: V.on, src: (el && el.getAttribute("src")) || "",
               width: el && el.tagName === "IMG" ? el.naturalWidth : 0,
               sandbox: el && el.hasAttribute("sandbox") ? el.getAttribute("sandbox") : null,
               rows: rows.map((r) => [...r.querySelectorAll("td")].map((c) => c.textContent).join(",")),
               columns: [...box.querySelectorAll("thead th.col")].map((c) => c.textContent),
               tables: [...box.querySelectorAll("#dbtable option")].filter((o) => !o.disabled).map((o) => o.value),
               table: ($("dbtable") || {}).value || "",
               where: ($("dbwhere") || {}).textContent || "",
               html: el && el.tagName === "IFRAME" ? el.srcdoc || "" : "",
               note: ($("filenote") || {}).textContent || "" };
    },
    // A LaTeX document's pages: what the page asked for, the server's
    // last answer, and what is drawn.
    typeset: () => ({ shown: !$("tex").classList.contains("hidden"), asked: X.asked,
                      state: X.m ? X.m.state : "", url: X.m && X.m.url ? X.m.url : "",
                      src: $("texframe").getAttribute("src") || "",
                      frame: !$("texframe").classList.contains("hidden"),
                      note: $("texnote").textContent, log: $("texlog").textContent }),
    // Source Control: the list as drawn, and the review on show.
    git: () => ({ mode: G.mode, summary: $("gitsum").textContent, version: G.status ? G.status.version : 0,
                  rows: [...$("gitlist").querySelectorAll(".row")].map((r) => r.querySelector(".gl").textContent + " " +
                                                                           r.dataset.path),
                  current: [...$("gitlist").querySelectorAll(".row.current")].map((r) => r.dataset.path) }),
    review: () => {
      const box = $("review");
      return { shown: V.on === kReview && !!box, version: G.review ? G.review.version || 0 : 0,
               note: ($("filenote") || {}).textContent || "",
               buttons: box ? [...box.querySelectorAll("button")].map((b) => b.textContent) : [],
               words: box ? box.querySelectorAll(".wa, .wr").length : 0,
               colored: box ? box.querySelectorAll("td.t span[class^='s']").length : 0,
               top: $("fileview").scrollTop,
               files: box ? [...box.querySelectorAll(".rfile")].map((f) => ({
                 key: f.dataset.key, head: f.querySelector(".rpath").textContent, offset: f.offsetTop,
                 side: !!f.querySelector("table.side"),
                 rows: [...f.querySelectorAll("tr")].map((r) => r.className + "|" +
                         [...r.querySelectorAll("td")].map((c) => c.textContent).join("|")) })) : [] };
    },
    windowPanes: () => windowPanes(),
    gate: () => ($("gate").classList.contains("hidden") ? "" : $("gate").textContent),
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
