# Math in Markdown

Every way of writing math that the preview reads, for eyes and for the
`md:math-corpus` unit test. On the Mac the formulas are typeset with
tectonic; elsewhere they show as their TeX in the code style.

## Inline

The norm $\lVert x \rVert_2 = \sqrt{\sum_i x_i^2}$ sits on the text's
baseline, and so do $e^{i\pi} + 1 = 0$, $\alpha_t$, $x^{(\ell)}$, $y_{t-1}$
and $\mathbb{R}^{n \times d}$. Tall ones such as $\frac{a}{b}$,
$\binom{n}{k}$ and $\left(\frac{1}{2}\right)^2$ open the line up a little.
Descenders: $p(y \mid x)$, $g_j$, $\mathcal{Q}$.

Escapes and underscores inside math are TeX's, not Markdown's:
$\{a, b\} \cup \{c\}$, $\mathrm{max\_len}$, $a_i b_j c_k$, $2 * 3 * 4$.

Not math: it costs $5 and $10, a price of $20,000, \$x\$ written with
escaped dollars, `$HOME` in code, and a lone $ sign.

Parentheses work too: \(a^2 + b^2 = c^2\).

## Display

$$
\mathcal{L}(\theta) = -\frac{1}{N}\sum_{n=1}^{N} \log p_\theta(y_n \mid x_n)
$$

$$
\begin{aligned}
h_t &= \tanh(W_h h_{t-1} + W_x x_t + b) \\
y_t &= W_y h_t
\end{aligned}
$$

A formula inside a paragraph, $$\int_0^1 x^2\,dx = \tfrac{1}{3},$$ stands
on a line of its own, and the text goes on after it.

$$
f(x) = \begin{cases} x & x \ge 0 \\ 0 & \text{otherwise} \end{cases}
\qquad
R_\theta = \begin{bmatrix} \cos\theta & -\sin\theta \\ \sin\theta & \cos\theta \end{bmatrix}
$$

$$ \underbrace{a_1 + a_2 + \cdots + a_n}_{n\ \text{terms}} \quad \mathbb{1}[i \in S] \quad \mathds{1}_A $$

```math
\operatorname*{arg\,max}_{k} \; s_k = \frac{\exp(q^\top k_s / \sqrt{d})}{\sum_{s'} \exp(q^\top k_{s'} / \sqrt{d})}
```

\[
\sum_{k=1}^{\infty} \frac{1}{k^2} = \frac{\pi^2}{6}
\]

A formula wider than the pane is scaled down to fit:

$$
\Pr(x_1, \ldots, x_T) = \prod_{t=1}^{T} p(x_t \mid x_1, \ldots, x_{t-1}) = \prod_{t=1}^{T} \mathrm{softmax}\big(W_o h_t + b_o\big)_{x_t} \qquad h_t = \mathrm{Transformer}(x_1, \ldots, x_{t-1}) \in \mathbb{R}^{d_{\text{model}}}
$$

## Around other blocks

- the forget gate $f_t = \sigma(W_f [h_{t-1}; x_t] + b_f)$
- the cell update, in the item:

  $$
  c_t = f_t \odot c_{t-1} + i_t \odot g_t
  $$

- and a last item with $o_t$.

> A quote with $\Delta W = BA$ inside, and a display formula:
>
> $$ \mathrm{rank}(BA) \le r $$

| Symbol | Meaning | Shape |
|---|---|---|
| $\sigma$ | sigmoid | scalar |
| $\odot$ | elementwise product | $\mathbb{R}^{d}$ |
| $W_Q$ | query weights | $\mathbb{R}^{d \times d_h}$ |

### A heading with $O(n \log n)$ in it

**Bold text with $x^2$** and *italic text with $y^2$*, and a [link with $z$](#inline).

## Failures

A formula TeX cannot read stays as written, with TeX's error on hover:
$\notacommand{x}$ and $\frac{a}$, while its neighbours $a + b$ typeset.

$$
\begin{aligned} a &= b \end{cases}
$$
