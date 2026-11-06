// JsonOutline.h — pure C++. A JSON document as a tree of nodes that know
// where they are in the text, for an outline beside the editor: selecting a
// node selects its text. Strict JSON (RFC 8259); on an error the nodes read
// so far are kept and the error's place is given.
//
// Every node has its byte range in the UTF-8 text and the same range in
// UTF-16 units (what NSString and NSTextView count). A member of an object
// also has the range from its key's opening quote, which is what selecting
// it selects.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace JsonOutline {

enum class Kind { Object, Array, String, Number, Bool, Null };

struct Node {
    Kind kind = Kind::Null;
    std::string key;            // a member's name, decoded; "" otherwise
    bool isMember = false;      // in an object (key is meaningful)
    long index = -1;            // an array element's index; -1 otherwise
    std::string preview;        // a scalar's value as text (strings decoded,
                                // cut at kPreviewChars), "" for containers
    size_t start = 0, end = 0;  // the value's bytes, [start, end)
    size_t memberStart = 0;     // the key's opening quote (== start unless isMember)
    size_t start16 = 0, end16 = 0, memberStart16 = 0;   // the same in UTF-16 units
    long parent = -1;
    std::vector<size_t> children;
};

constexpr size_t kPreviewChars = 120;

struct Outline {
    std::vector<Node> nodes;    // nodes[0] is the root, when there is one
    bool ok = false;
    std::string error;          // "Expected ',' or '}'", ...
    size_t errorOffset = 0;     // bytes
    size_t errorLine = 0, errorColumn = 0;   // 1-based; column in characters
    bool truncated = false;     // stopped at maxNodes
};

Outline parse(const std::string &text, size_t maxNodes = 2000000);

// The deepest node whose value (or member, key included) holds the UTF-16
// offset, or -1.
long nodeAt16(const Outline &o, size_t offset16);

// What the outline shows for a node: "name", "[3]" or "root", then for a
// container its size ("{4}", "[12]") and for a scalar its value.
std::string label(const Node &n);
std::string summary(const Node &n);

}  // namespace JsonOutline
