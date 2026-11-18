#!/usr/bin/env python3
"""Builds the homepage, https://mut.ible.ai, from the README.

The README is rendered to HTML by GitHub's own Markdown API (the Pages
workflow, .github/workflows/pages.yml, runs `gh api markdown`), so the page
reads as the repository's front page does. This script, standard library
only, puts that HTML into site/template.html and writes the folder Pages
serves:

    python3 site/build.py <readme.html> <out-dir>

- The README's own title (its <h1> with the mark) is dropped: the template's
  header shows the mark.
- Headings get the ids GitHub gives them on github.com, so #get-started
  works.
- A relative link or picture whose file is copied into the site (the demo
  GIFs, docs/brand) keeps its path; any other relative link goes to the
  file's page on GitHub, so docs/REFERENCE.md opens there.
- favicons, the social card and CNAME (mut.ible.ai) go in the folder too.

Every page later added to the site (the browser version's landing page, for
one) is a template of its own beside this one.
"""

import html
import os
import re
import shutil
import sys

REPO = "https://github.com/ible-ai/mutible"
BLOB = REPO + "/blob/main/"
DOMAIN = "mut.ible.ai"

# What the site carries from the repository, at the same paths.
COPIED = ["docs/demos", "docs/brand"]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def slug(text, seen):
    """GitHub's heading anchor: lowercase, punctuation dropped, spaces to
    hyphens, -1, -2 and so on for a repeat."""
    text = re.sub(r"<[^>]+>", "", text)
    text = html.unescape(text).strip().lower()
    s = re.sub(r"[^\w\- ]", "", text).replace(" ", "-")
    base, n = s, 1
    while s in seen:
        s = "%s-%d" % (base, n)
        n += 1
    seen.add(s)
    return s


def is_relative(url):
    return not re.match(r"^([a-z][a-z0-9+.-]*:|//|#)", url, re.I)


def rewrite_url(url, out):
    if not is_relative(url):
        return url
    path, _, frag = url.partition("#")
    if path.startswith("./"):
        path = path[2:]
    if path and os.path.exists(os.path.join(out, path)):
        return url
    return BLOB + path + ("#" + frag if frag else "")


def render(body, out):
    # The README's title, the mark in a <picture>: the header has it.
    body = re.sub(r"\A\s*<h1[^>]*>.*?</h1>\s*", "", body, count=1, flags=re.S)

    seen = set()

    def heading(m):
        tag, attrs, inner = m.group(1), m.group(2), m.group(3)
        if "id=" in attrs:
            return m.group(0)
        hid = slug(inner, seen)
        return '<%s id="%s"%s><a class="anchor" href="#%s" aria-hidden="true">#</a>%s</%s>' % (
            tag, hid, attrs, hid, inner, tag)

    body = re.sub(r"<(h[1-6])([^>]*)>(.*?)</\1>", heading, body, flags=re.S)

    def attr(m):
        name, url = m.group(1), m.group(2)
        if name == "srcset":
            parts = [p.strip().split(" ", 1) for p in url.split(",")]
            url = ", ".join(" ".join([rewrite_url(p[0], out)] + p[1:]) for p in parts)
        else:
            url = rewrite_url(url, out)
        return '%s="%s"' % (name, url)

    return re.sub(r'\b(href|src|srcset)="([^"]*)"', attr, body)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: build.py <readme.html> <out-dir>")
    rendered, out = sys.argv[1], sys.argv[2]
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for folder in COPIED:
        shutil.copytree(os.path.join(ROOT, folder), os.path.join(out, folder),
                        ignore=shutil.ignore_patterns("linux"))
    shutil.copy(os.path.join(ROOT, "site", "site.css"), out)
    for name in ["favicon.ico", "favicon.svg", "apple-touch-icon.png"]:
        shutil.copy(os.path.join(ROOT, "docs", "brand", name), out)
    with open(os.path.join(out, "CNAME"), "w") as f:
        f.write(DOMAIN + "\n")

    with open(rendered, encoding="utf-8") as f:
        body = f.read()
    if "<p" not in body:
        sys.exit("build.py: %s does not look like the rendered README" % rendered)
    with open(os.path.join(ROOT, "site", "template.html"), encoding="utf-8") as f:
        page = f.read()
    marker = "<!-- README -->"
    if page.count(marker) != 1:
        sys.exit("build.py: the template needs exactly one %s" % marker)
    page = page.replace(marker, render(body, out))
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as f:
        f.write(page)


if __name__ == "__main__":
    main()
