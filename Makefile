CXX      := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -DGL_SILENCE_DEPRECATION -Isrc/engine -Isrc/game
SDL2     := $(shell sdl2-config --cflags --libs)
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
GL_FLAGS := -framework OpenGL
else
GL_FLAGS := -lGL -lGLEW
endif

# Emscripten — invoked via subshell so emsdk_env.sh is sourced per call.
EMSDK_ENV := source $(HOME)/emsdk/emsdk_env.sh 2>/dev/null || true
WASM_OUT  := build/web
WASM_FLAGS := -std=c++17 -O2 -Isrc/engine -Isrc/game \
              -sUSE_SDL=2 -sUSE_WEBGL2=1 -sFULL_ES3=1 \
              -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
              -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 \
              --emrun \
              --preload-file art \
              --preload-file src/levels@scenes \
              --pre-js web/pre.js

SRC      := $(wildcard src/engine/*.cpp) $(wildcard src/game/*.cpp)

BIN      := build/clarevoyance
APP       := build/Clarevoyance.app
EXE       := $(APP)/Contents/MacOS/clarevoyance

TEST_DIR  := build/test
IMGDIFF   := tools/imgdiff

# Test parameters — deterministic fixed frame, enough frames to get past upload.
TEST_FRAMES := 120
TEST_TIME   := 2.0

.PHONY: all build bundle run demo demo-controls demo-events demo-menu clean \
        build-wasm run-wasm \
        deploy \
        test test-wasm test-parity

# Scene to run with `make demo` — override on the command line:
#   make demo SCENE=src/levels/other.json
SCENE ?= src/levels/demo.json

all: bundle

build: $(BIN)

# Run the data-driven scene demo (the script layer) in an interactive window.
# The player penguin walks toward Mochi; a proximity event fires, printing
# dialogue and making Mochi leap away. Watch the terminal for CV_DIALOGUE.
demo: $(BIN)
	CV_SCENE=$(SCENE) $(BIN)

# Keyboard-controls demo: WASD / arrows move the player penguin freely; press
# space to toggle the buddy penguin into the controlled set so both move together.
demo-controls: $(BIN)
	CV_SCENE=src/levels/controls.json $(BIN)

# Events demo: the player walks toward Mochi; a proximity event fires dialogue
# and sends Mochi leaping away.
demo-events: $(BIN)
	CV_SCENE=src/levels/demo.json $(BIN)

# Menu demo: Up/Down (or W/S) navigate between two options; selection state
# is tracked via flags and expressed through sprite animation.
demo-menu: $(BIN)
	CV_SCENE=src/levels/menu.json $(BIN)

$(BIN): $(SRC) $(wildcard src/engine/*.h) $(wildcard src/game/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SDL2) $(GL_FLAGS) $(SRC) -o $@

bundle: $(BIN)
	@mkdir -p $(APP)/Contents/MacOS
	@cp $(BIN) $(EXE)
	@printf '<?xml version="1.0" encoding="UTF-8"?>\n\
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n\
<plist version="1.0"><dict>\n\
  <key>CFBundleIdentifier</key>    <string>com.clarevoyance.game</string>\n\
  <key>CFBundleName</key>          <string>Clarevoyance</string>\n\
  <key>CFBundleExecutable</key>    <string>clarevoyance</string>\n\
  <key>CFBundlePackageType</key>   <string>APPL</string>\n\
  <key>CFBundleVersion</key>       <string>1</string>\n\
  <key>NSHighResolutionCapable</key><true/>\n\
  <key>NSPrincipalClass</key>      <string>NSApplication</string>\n\
</dict></plist>\n' > $(APP)/Contents/Info.plist

run: bundle
	@cp -r art $(APP)/Contents/MacOS/
	open $(APP)

# ---------------------------------------------------------------------------
# WebAssembly
# ---------------------------------------------------------------------------

build-wasm: $(SRC) $(wildcard src/engine/*.h) $(wildcard src/game/*.h) web/pre.js
	@mkdir -p $(WASM_OUT)
	bash -c '$(EMSDK_ENV) && emcc $(WASM_FLAGS) $(SRC) -o $(WASM_OUT)/stress.html'

run-wasm: build-wasm
	bash -c '$(EMSDK_ENV) && emrun $(WASM_OUT)/stress.html'

deploy: build-wasm
	@mkdir -p docs-web
	cp $(WASM_OUT)/stress.html  docs-web/stress.html
	cp $(WASM_OUT)/stress.js    docs-web/stress.js
	cp $(WASM_OUT)/stress.wasm  docs-web/stress.wasm
	cp $(WASM_OUT)/stress.data  docs-web/stress.data
	cp web/landing.html         docs-web/index.html
	@echo "Built to docs-web/ — commit and push to deploy"

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

$(IMGDIFF): tools/imgdiff.c src/engine/third_party/stb_image.h
	clang -O2 -Isrc/engine -o $@ $< -lm

test: build $(IMGDIFF)
	@mkdir -p $(TEST_DIR)
	@echo "--- Desktop test ($(TEST_FRAMES) frames, t=$(TEST_TIME)) ---"
	CV_TEST_FRAMES=$(TEST_FRAMES) CV_FIXED_TIME=$(TEST_TIME) CV_SCREENSHOT=1 \
	    $(BIN) 2>&1 | tee $(TEST_DIR)/desktop.log
	@if grep -q "CV_GLERROR" $(TEST_DIR)/desktop.log; then \
	    echo "FAIL: GL errors detected"; exit 1; fi
	@if grep -q "CV_BLANK" $(TEST_DIR)/desktop.log; then \
	    echo "FAIL: blank framebuffer"; exit 1; fi
	@bash scripts/extract_shot.sh $(TEST_DIR)/desktop.log $(TEST_DIR)/desktop.png
	@echo "Desktop test PASSED"

test-wasm: build-wasm
	@mkdir -p $(TEST_DIR)
	@echo "--- Web test ($(TEST_FRAMES) frames, t=$(TEST_TIME)) ---"
	bash -c '$(EMSDK_ENV) && emrun --kill-exit --timeout 30 \
	    "$(WASM_OUT)/stress.html?CV_TEST_FRAMES=$(TEST_FRAMES)&CV_FIXED_TIME=$(TEST_TIME)&CV_SCREENSHOT=1" \
	    2>&1 | tee $(TEST_DIR)/web.log'
	@if grep -q "CV_GLERROR" $(TEST_DIR)/web.log; then \
	    echo "FAIL: GL errors detected (web)"; exit 1; fi
	@if grep -q "CV_BLANK" $(TEST_DIR)/web.log; then \
	    echo "FAIL: blank framebuffer (web)"; exit 1; fi
	@bash scripts/extract_shot.sh $(TEST_DIR)/web.log $(TEST_DIR)/web.png
	@echo "Web test PASSED"

test-parity: $(TEST_DIR)/desktop.png $(TEST_DIR)/web.png $(IMGDIFF)
	@echo "--- Parity diff: desktop vs web ---"
	$(IMGDIFF) $(TEST_DIR)/desktop.png $(TEST_DIR)/web.png

clean:
	rm -rf build tools/imgdiff
