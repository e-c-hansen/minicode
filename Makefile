# MiniCode — native macOS editor. No third-party dependencies.
# Only Apple system frameworks + the C++ standard library.

APP      := MiniCode
BUNDLE   := $(APP).app
BIN      := build/$(APP)
SRC      := $(wildcard src/*.mm) $(wildcard src/*.cpp)

CXX      := clang++
CXXFLAGS := -std=c++17 -fobjc-arc -Wall -Wextra -O2 -Isrc
LDFLAGS  := -framework Cocoa -framework WebKit -framework CoreServices

# The pure-C++ core, testable on its own (no frameworks, no Objective-C).
CORE_SRC := src/SyntaxHighlighter.cpp src/MarkdownParser.cpp

.PHONY: all app run test dmg clean

all: app

$(BIN): $(SRC)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $(BIN)

# Assemble a minimal .app bundle so Cocoa treats it as a real GUI app.
app: $(BIN)
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	@cp $(BIN) $(BUNDLE)/Contents/MacOS/$(APP)
	@cp Info.plist $(BUNDLE)/Contents/Info.plist
	@echo "APPL????" > $(BUNDLE)/Contents/PkgInfo
	@cp resources/AppIcon.icns $(BUNDLE)/Contents/Resources/AppIcon.icns
	@cp scripts/minicode-launcher $(BUNDLE)/Contents/Resources/minicode
	@chmod +x $(BUNDLE)/Contents/Resources/minicode
	@# Ad-hoc signature (free, no Apple account). Required to run on Apple
	@# Silicon and keeps granted permissions stable across launches. It does
	@# NOT remove the Gatekeeper download warning — only notarization does.
	@codesign --force --sign - $(BUNDLE) 2>/dev/null || true
	@echo "Built $(BUNDLE)"

# Launch, opening the given folder (defaults to current directory).
run: app
	./$(BUNDLE)/Contents/MacOS/$(APP) $(or $(DIR),.)

# Unit-test the pure-C++ core (syntax highlighter + markdown parser).
test:
	@mkdir -p build
	$(CXX) -std=c++17 -Wall -Wextra -Isrc tests/run_tests.cpp $(CORE_SRC) \
		-o build/run_tests
	@./build/run_tests

# Regenerate the app icon (resources/AppIcon.icns) from tools/makeicon.m.
icon:
	@mkdir -p build
	@clang -framework Cocoa tools/makeicon.m -o build/makeicon
	@./build/makeicon /tmp/mc_icon.png
	@rm -rf /tmp/mc.iconset && mkdir -p /tmp/mc.iconset
	@for sz in 16 32 64 128 256 512; do \
		sips -z $$sz $$sz /tmp/mc_icon.png --out /tmp/mc.iconset/icon_$${sz}x$${sz}.png >/dev/null; \
		d=$$((sz*2)); \
		sips -z $$d $$d /tmp/mc_icon.png --out /tmp/mc.iconset/icon_$${sz}x$${sz}@2x.png >/dev/null; \
	done
	@cp /tmp/mc_icon.png /tmp/mc.iconset/icon_512x512@2x.png
	@iconutil -c icns /tmp/mc.iconset -o resources/AppIcon.icns
	@echo "Regenerated resources/AppIcon.icns"

# Package the app into a distributable (unsigned) disk image.
dmg:
	./scripts/make-dmg.sh

# Zip the app for a GitHub Release (preserves the bundle correctly).
dist-zip: app
	@ditto -c -k --sequesterRsrc --keepParent $(BUNDLE) $(APP).zip
	@echo "Created $(APP).zip"

clean:
	rm -rf build $(BUNDLE) $(APP).dmg $(APP).zip
