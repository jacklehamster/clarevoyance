CXX      := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -DGL_SILENCE_DEPRECATION
SDL2     := $(shell sdl2-config --cflags --libs)
GL_FLAGS := -framework OpenGL

SRC  := src/engine/main.cpp
BIN  := build/clarevoyance
APP  := build/Clarevoyance.app
EXE  := $(APP)/Contents/MacOS/clarevoyance

.PHONY: all build bundle run clean

all: bundle

build: $(BIN)

$(BIN): $(SRC)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SDL2) $(GL_FLAGS) $< -o $@

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
	open $(APP)

clean:
	rm -rf build
