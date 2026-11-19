// MarkdownEmoji.h — pure C++. GitHub's emoji shortcodes (":tada:",
// ":white_check_mark:") as the emoji they stand for, from a compact table
// of the common few hundred of GitHub's names (gemoji's), not all of them.
// MarkdownParser replaces them in a page's text, outside code and math; a
// name the table does not know is left as written, as GitHub leaves one.
#pragma once
#include <cstddef>
#include <string>

namespace MarkdownEmoji {

// The emoji for a shortcode's name ("tada", without the colons) as UTF-8,
// with the variation selector that asks for the colored form where the
// character's default is text; "" for a name the table does not know.
std::string lookup(const std::string& name);

// `text` with every known ":name:" replaced. A name is letters, digits,
// '_', '+' and '-'; a code counts only where the character before its first
// colon and after its last is not a letter or a digit, so "12:100:30" and
// "a:b:c" stay as they are.
std::string replace(const std::string& text);

// How many names the table holds (for the tests).
size_t count();

}  // namespace MarkdownEmoji
