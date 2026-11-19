// FileTypeIcons.mm — see FileTypeIcons.h.
#import "FileTypeIcons.h"
#include <vector>

namespace {

using FileIcon::Type;

// The families' colors as a dark tree shows them: muted, a little below
// Seti's saturation, so a column of icons reads as color beside the names
// without shouting over them. A light tree takes each one deeper
// (MCDeeper).
enum : unsigned {
    kBlue = 0x6CA6D9, kYellow = 0xD4BF5A, kOrange = 0xDE8C55, kRed = 0xD8686E,
    kGreen = 0x8DBF63, kPurple = 0xA88BD0, kPink = 0xDB7FA6, kTeal = 0x5DB8B0,
    kBrown = 0xB8946A, kGray = 0x8A99A8, kSteel = 0x7F9FB8,
};

// A type's look: SF Symbols to try in order (space-separated; the first
// this macOS has wins), else `label` drawn, else doc.text.
struct Look {
    Type type;
    const char *symbols;
    const char *label;
    unsigned color;
};

const Look kLooks[] = {
    {Type::Text, "doc.text", nullptr, kGray},
    {Type::Code, "chevron.left.forwardslash.chevron.right", nullptr, kSteel},
    // source: the language's short name, as Seti draws most of them
    {Type::C, "", "C", kBlue},
    {Type::CHeader, "", "h", kPurple},
    {Type::Cpp, "", "C++", kBlue},
    {Type::CppHeader, "", "h++", kPurple},
    {Type::ObjC, "", "m", kSteel},
    {Type::CSharp, "", "C#", kGreen},
    {Type::Python, "", "py", kBlue},
    {Type::JavaScript, "", "JS", kYellow},
    {Type::TypeScript, "", "TS", kBlue},
    {Type::Rust, "", "rs", kOrange},
    {Type::Go, "", "go", kTeal},
    {Type::Java, "cup.and.saucer", "J", kRed},
    {Type::Kotlin, "", "kt", kPurple},
    {Type::Swift, "swift", "sw", kOrange},
    {Type::Ruby, "", "rb", kRed},
    {Type::Php, "", "php", kPurple},
    {Type::Perl, "", "pl", kSteel},
    {Type::Lua, "", "lua", kBlue},
    {Type::Shell, "terminal", "sh", kGreen},
    {Type::Haskell, "", "hs", kPurple},
    {Type::Scala, "", "sc", kRed},
    {Type::Dart, "", "dt", kTeal},
    {Type::R, "", "R", kBlue},
    {Type::Julia, "", "jl", kPurple},
    {Type::Elixir, "", "ex", kPurple},
    {Type::Erlang, "", "erl", kRed},
    {Type::Zig, "", "zig", kOrange},
    {Type::Nim, "", "nim", kYellow},
    {Type::Vala, "", "va", kPurple},
    {Type::Fortran, "", "F", kPurple},
    {Type::OCaml, "", "ml", kOrange},
    {Type::Lisp, "", "λ", kPurple},
    {Type::Qml, "", "qml", kGreen},
    {Type::Vue, "", "V", kGreen},
    // markup and data
    {Type::Markdown, "", "M↓", kBlue},
    {Type::Html, "globe", nullptr, kOrange},
    {Type::Css, "", "#", kBlue},
    {Type::Sass, "", "S", kPink},
    {Type::Less, "", "le", kSteel},
    {Type::Json, "curlybraces", "{}", kYellow},
    {Type::Yaml, "", "yml", kPurple},
    {Type::Toml, "", "tml", kGray},
    {Type::Xml, "chevron.left.forwardslash.chevron.right", nullptr, kOrange},
    {Type::Csv, "tablecells", nullptr, kGreen},
    {Type::Sql, "cylinder.split.1x2", "sql", kPink},
    {Type::Latex, "", "TeX", kGreen},
    {Type::Notebook, "book", nullptr, kOrange},
    // media, documents and binaries
    {Type::Image, "photo", nullptr, kPurple},
    {Type::Svg, "scribble.variable scribble", nullptr, kYellow},
    {Type::Audio, "waveform", nullptr, kPink},
    {Type::Video, "film", nullptr, kRed},
    {Type::Pdf, "doc.richtext", nullptr, kRed},
    {Type::Archive, "doc.zipper", nullptr, kBrown},
    {Type::Font, "textformat", nullptr, kSteel},
    {Type::Executable, "gearshape.2", nullptr, kGreen},
    {Type::Library, "books.vertical", nullptr, kBrown},
    {Type::Database, "cylinder internaldrive", nullptr, kTeal},
    {Type::Document, "doc.plaintext", nullptr, kBlue},
    {Type::Spreadsheet, "tablecells.fill tablecells", nullptr, kGreen},
    {Type::Presentation, "chart.bar.doc.horizontal", nullptr, kOrange},
    {Type::Ebook, "book.closed", nullptr, kBrown},
    // by name, and the rest
    {Type::Makefile, "hammer", nullptr, kOrange},
    {Type::CMake, "hammer", nullptr, kBlue},
    {Type::Meson, "hammer", nullptr, kGreen},
    {Type::Dockerfile, "shippingbox", nullptr, kBlue},
    {Type::License, "checkmark.seal", nullptr, kYellow},
    {Type::Readme, "info.circle", nullptr, kBlue},
    {Type::Authors, "person.2", nullptr, kSteel},
    {Type::Lockfile, "lock", nullptr, kGray},
    {Type::Git, "arrow.triangle.branch", nullptr, kRed},
    {Type::Config, "gearshape", nullptr, kGray},
    {Type::Patch, "plus.forwardslash.minus", nullptr, kGreen},
    {Type::Log, "list.bullet.rectangle", nullptr, kGray},
    {Type::Key, "key", nullptr, kYellow},
    {Type::Bookmark, "bookmark", nullptr, kBlue},
    {Type::Translation, "character.bubble", nullptr, kTeal},
};

const std::size_t kTypeCount = FileIcon::allTypes().size();

const Look &LookOf(Type t) {
    static std::vector<const Look *> byType = [] {
        std::vector<const Look *> v(kTypeCount, &kLooks[0]);
        for (const Look &l : kLooks) v[(std::size_t)l.type] = &l;
        return v;
    }();
    const std::size_t i = (std::size_t)t;
    return *byType[i < byType.size() ? i : 0];
}

NSColor *RGB(unsigned rgb, CGFloat scale) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0 * scale
                               green:((rgb >> 8) & 0xFF) / 255.0 * scale
                                blue:(rgb & 0xFF) / 255.0 * scale
                               alpha:1];
}

// The face a label is drawn in: the system's bold, rounded where the
// system has the rounded design.
NSFont *LabelFont(CGFloat size) {
    NSFont *f = [NSFont systemFontOfSize:size weight:NSFontWeightBold];
    NSFontDescriptor *d = [f.fontDescriptor fontDescriptorWithDesign:NSFontDescriptorSystemDesignRounded];
    return (d ? [NSFont fontWithDescriptor:d size:size] : nil) ?: f;
}

// A label drawn into the 16-point square the tree gives an icon: as large
// as fits across, centered on its letters (a lower-case label on
// the band between its x-height and cap height, so "py" sits where "JS"
// does). Drawn when shown, at the screen's scale, so it stays sharp.
NSImage *DrawnLabel(NSString *label) {
    BOOL lower = YES;
    for (NSUInteger i = 0; i < label.length; i++) {
        const unichar c = [label characterAtIndex:i];
        if (c < 'a' || c > 'z') lower = NO;
    }
    NSImage *image = [NSImage imageWithSize:NSMakeSize(16, 16) flipped:NO
                             drawingHandler:^BOOL(NSRect rect) {
        CGFloat size = lower ? 11.5 : 10.5;
        // Three letters set a little tighter, so they keep a size to read.
        const CGFloat kern = label.length > 2 ? -0.6 : 0;
        auto attrsFor = ^NSDictionary *(NSFont *f) {
            return @{NSFontAttributeName: f, NSKernAttributeName: @(kern),
                     NSForegroundColorAttributeName: NSColor.blackColor};
        };
        NSFont *font = LabelFont(size);
        NSDictionary *attrs = attrsFor(font);
        NSSize s = [label sizeWithAttributes:attrs];
        const CGFloat room = NSWidth(rect) + 0.5;
        if (s.width > room) {
            size = MAX(7, floor(size * room / s.width * 2) / 2);
            font = LabelFont(size);
            attrs = attrsFor(font);
            s = [label sizeWithAttributes:attrs];
        }
        const CGFloat band = lower ? (font.xHeight + font.capHeight) / 2 : font.capHeight;
        const CGFloat baseline = round((NSHeight(rect) - band) / 2 * 2) / 2;
        const CGFloat x = (NSWidth(rect) - s.width) / 2;
        // Without NSStringDrawingUsesLineFragmentOrigin the rect's origin
        // is the first line's baseline.
        [label drawWithRect:NSMakeRect(x, baseline, s.width + 2, NSHeight(rect))
                    options:0 attributes:attrs context:nil];
        return YES;
    }];
    [image setTemplate:YES];
    return image;
}

}  // namespace

FileIcon::Type MCFileTypeOf(NSString *path, BOOL executable) {
    return FileIcon::typeOf(path.fileSystemRepresentation ?: "", executable);
}

NSImage *MCFileTypeImage(FileIcon::Type type) {
    static std::vector<NSImage *> images(kTypeCount, nil);
    std::size_t i = (std::size_t)type;
    if (i >= images.size()) i = 0;
    if (images[i]) return images[i];
    const Look &look = LookOf(type);
    NSString *what = @(FileIcon::typeName(type));
    NSImage *image = nil;
    for (NSString *name in [@(look.symbols) componentsSeparatedByString:@" "]) {
        if (!name.length) continue;
        image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:what];
        if (image) break;
    }
    if (!image && look.label) image = DrawnLabel(@(look.label));
    if (!image) image = [NSImage imageWithSystemSymbolName:@"doc.text" accessibilityDescription:what];
    image.accessibilityDescription = what;
    images[i] = image;
    return image;
}

NSColor *MCFileTypeTint(FileIcon::Type type, BOOL dark) {
    static std::vector<NSColor *> shades[2] = {std::vector<NSColor *>(kTypeCount, nil),
                                               std::vector<NSColor *>(kTypeCount, nil)};
    std::size_t i = (std::size_t)type;
    if (i >= kTypeCount) i = 0;
    std::vector<NSColor *> &v = shades[dark ? 1 : 0];
    // On a light tree the same hue, deeper, so it keeps 3:1 or more
    // against a near-white background (the yellow is the lightest).
    if (!v[i]) v[i] = RGB(LookOf(type).color, dark ? 1.0 : 0.72);
    return v[i];
}

BOOL MCFileTypeTextIsLight(NSColor *text) {
    NSColor *c = [text colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (!c) return YES;
    return 0.299 * c.redComponent + 0.587 * c.greenComponent + 0.114 * c.blueComponent > 0.5;
}
