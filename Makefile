# Builds both apps from a Mac.
#
#   make mac      build/Bivio.app  (Xcode command line tools: swiftc, codesign)
#   make win      build/Bivio.exe  (MinGW-w64 cross compiler: brew install mingw-w64)
#   make all      both
#   make preview  renders the macOS popover pages to build/popover-*.png, to check the layout
#   make icons    regenerates mac/AppIcon.icns and win/bivio.ico from the SVGs in assets/
#   make clean    removes build/

APP     := build/Bivio.app
EXE     := build/Bivio.exe
WIN_CC  := x86_64-w64-mingw32-gcc
WINDRES := x86_64-w64-mingw32-windres

.PHONY: all mac win preview icons clean

all: mac win

mac: $(APP)

# Apple Silicon only (IOAVService, see mac/Bridge.h); macOS 14+ for @Observable and SMAppService.
# Ad-hoc signed, which is enough to run it locally.
$(APP): mac/*.swift mac/Bridge.h mac/Info.plist mac/AppIcon.icns
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cp mac/Info.plist $(APP)/Contents/Info.plist
	cp mac/AppIcon.icns $(APP)/Contents/Resources/
	swiftc -O -target arm64-apple-macos14.0 -import-objc-header mac/Bridge.h \
		mac/*.swift -o $(APP)/Contents/MacOS/Bivio
	codesign --force --sign - $(APP)

win: $(EXE)

# GUI subsystem (-mwindows), wide-character entry point (-municode), size-optimised and stripped.
# Only system DLLs are imported: the executable runs on any Windows 10/11 without installing anything.
$(EXE): win/*.c win/app.h win/bivio.rc win/bivio.manifest win/bivio.ico
	mkdir -p build
	$(WINDRES) --include-dir win win/bivio.rc -O coff -o build/bivio.res.o
	$(WIN_CC) -Os -s -Wall -Wextra -municode -mwindows -static-libgcc -o $@ \
		win/*.c build/bivio.res.o \
		-ldxva2 -ldwmapi -luxtheme -lcomctl32 -lshlwapi -lshell32 -ladvapi32 -lgdi32 -luser32

# Builds the popover sources without main.swift, plus a small tool that renders them off screen.
preview:
	mkdir -p build
	swiftc -O -target arm64-apple-macos14.0 -import-objc-header mac/Bridge.h \
		$(filter-out mac/main.swift,$(wildcard mac/*.swift)) tools/render_settings/main.swift -o build/render_settings
	build/render_settings build

# mac/AppIcon.icns comes from assets/icon-macos.svg (icon on a light tile, macOS grid);
# win/bivio.ico from assets/icon.svg (the bare icon). Both outputs are committed.
icons:
	mkdir -p build
	swiftc -O tools/make_icons/main.swift -o build/make_icons
	rm -rf build/AppIcon.iconset
	build/make_icons assets/icon-macos.svg build/AppIcon.iconset assets/icon.svg win/bivio.ico
	iconutil -c icns build/AppIcon.iconset -o mac/AppIcon.icns

clean:
	rm -rf build
