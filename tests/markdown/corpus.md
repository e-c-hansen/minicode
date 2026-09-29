---
title: Markdown corpus
purpose: every construct the preview draws, for tests and for eyes
---

# Markdown corpus

Open this file in MiniCode and compare the preview with GitHub's rendering of
the same file. `tests/run_tests.cpp` parses it too, and checks what it can
without a screen. Each section names what it exercises.

## Spacing between blocks

One paragraph.
Its second source line joins the first.

Another paragraph, after one blank line.


A third, after two blank lines: the space above it is the same.
A list follows with no blank line:
- the list is not glued to the text above
- and a table follows the list

| Column | Other |
| --- | --- |
| a | b |

Text right after the table.

## Emphasis

*Italic*, **bold**, ***bold italic***, **bold with *italic* inside**, and
*italic with **bold** inside*. Underscores: _italic_ and __bold__.

Names keep their underscores: snake_case_name, max_len, load_config and
save_state, x_1 and a_b_c. Arithmetic keeps its stars: 2 * 3 * 4.

Stars inside a word do emphasise, as on GitHub: un*frigging*believable, and
5*6*7 puts the 6 in italics. So does a double underscore at a word's start:
__init__.py shows a bold "init". A bold label glued to its sentence,
**Label**Text after it, stays bold as far as the label.

~~Struck through~~ and ~single tilde~. An unmatched ** stays as written, and
so does a lone * or _.

## Escapes

\*not italic\*, \_not italic\_, \# not a heading, \[not a link\](nowhere),
\`not code\`, a backslash before a letter \q stays, and f′\_n keeps its
underscore.

## Inline code

Plain `code`, ``code with a ` backtick``, `` `backticks` `` with spaces
trimmed, and `code with **stars** and _underscores_` left alone.

## Links

Several on one line: [first](https://example.com/one) [second](#links)
[third](#emphasis) and [fourth](corpus.md).

A link with a title [titled](https://example.com "The title"), one with
parentheses in it [wiki](https://en.wikipedia.org/wiki/Markdown_(disambiguation)),
and one whose text is styled: [**bold** and `code`](https://example.com/styled).

Reference links: [full][ref-one], [collapsed][], and [shortcut]. The
definitions below them do not show.

[ref-one]: https://example.com/reference "Reference"
[collapsed]: https://example.com/collapsed
[shortcut]: <https://example.com/shortcut>

Autolinks: <https://example.com/auto>, <someone@example.org>, and bare ones,
https://example.com/bare_path_here and www.example.com, followed by a full
stop. A picture: ![alt text of a missing picture](missing.png).

## Entities

&amp; &lt;tag&gt; &quot;quoted&quot; non&nbsp;breaking &copy; 2026 &mdash;
&#8212; &#x2192; &frac12; &alpha;&beta;&gamma; and &unknown; stays.

## Hard line breaks

Two spaces end this line  
so this is a new line.
A backslash ends this one\
and this is a new line too.<br>After a br tag.

## Lists

- **Bold label**: text after the label.
- A long item that wraps: the second and later lines of it line up with the
  first line's text rather than with the bullet, as a hanging indent.
- Nested with two spaces:
  - second level
    - third level
- Nested with four spaces:
    - second level again

3. An ordered list can start at three.
4. The next item.
10. A wider number lines up with the others.

1) Parentheses work too.
2) Second.

- [ ] A task still to do
- [x] A task done

- A loose list

- has blank lines between its items

- Item one
continues lazily on an unindented line.
- Item two

  has a second paragraph, indented under it.

  ```
  and a code block
  ```

## Quotes

> A quote written over
> several source lines is one paragraph.
>
> A second paragraph in the same quote.
Lazy continuation joins the paragraph above.

> Outer
>> Nested, a paragraph of its own

> - a list
> - inside a quote

> [!NOTE]
> An alert, as GitHub writes them.

> [!WARNING]
> Another kind.

## Code blocks

```python
def f(x_1, x_2):
    return x_1 * x_2   # stars and underscores stay
```

~~~
Tildes fence a block too.
~~~

````markdown
```
A fence inside a longer fence.
```
````

    An indented code block,
    four spaces in.

## Rules

Above a rule.

---

***

___

Setext headings
===============

A second-level one
------------------

## Headings: closing hashes ##

### §3 Section sign, café, and 🚀 rocket

Links to them: [closing hashes](#headings-closing-hashes) and
[the third](#3-section-sign-café-and--rocket).

## Tables

| Left | Centre | Right |
| :--- | :---: | ---: |
| **bold** | `code` | [link](#tables) |
|  | empty cells |  |
| a \| b | ~~struck~~ | 12 |

A paragraph directly above a table:
| k | v |
|---|---|
| x | y |

## HTML

<!-- A comment on its own line does not show. -->

<!--
A comment over
several lines does not show either.
-->

Press <kbd>Ctrl</kbd>+<kbd>C</kbd>, a <b>bold</b> tag, an <i>italic</i> one,
a <a href="https://example.com/html">link by tag</a>, and an inline
<!-- hidden --> comment.

<p align="center">
  <img src="missing-logo.png" alt="A logo that is not there">
</p>

<h3 align="center">A heading written in HTML</h3>

## Unicode

Symbols render as they are: x ∈ ℝ^n, A ∈ ℝ^{n × m}, a ⊙ b, f′(x), α → β,
≤ ≥ ≠ ≈ ∞ ∑ ∏ √, and ½ ¼ ¾. Wide text: 日本語のテキスト and 한국어.

## Long lines

A very long line that goes on and on to show that wrapping happens at the pane's edge rather than at the source's line length, with nothing lost or clipped at the right side, however narrow the window is made, and however many words are added to it after this point.
