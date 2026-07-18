# MiniCode — native macOS editor. No third-party dependencies.
# Only Apple system frameworks + the C++ standard library.

APP      := MiniCode
BUNDLE   := $(APP).app
BIN      := build/$(APP)
SRC      := $(wildcard src/*.mm) $(wildcard src/*.cpp)

CXX      := clang++
CXXFLAGS := -std=c++17 -fobjc-arc -Wall -Wextra -O2 -Isrc
LDFLAGS  := -framework Cocoa -framework WebKit -framework CoreServices

.PHONY: all app run clean

all: app

$(BIN): $(SRC)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $(BIN)

# Assemble a minimal .app bundle so Cocoa treats it as a real GUI app.
app: $(BIN)
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS
	@cp $(BIN) $(BUNDLE)/Contents/MacOS/$(APP)
	@cp Info.plist $(BUNDLE)/Contents/Info.plist
	@echo "APPL????" > $(BUNDLE)/Contents/PkgInfo
	@echo "Built $(BUNDLE)"

# Launch, opening the given folder (defaults to current directory).
run: app
	./$(BUNDLE)/Contents/MacOS/$(APP) $(or $(DIR),.)

clean:
	rm -rf build $(BUNDLE)
