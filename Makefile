# Builds both apps from a Mac.
#
#   make mac      build/Bivio.app  (Xcode command line tools: swiftc, codesign)
#   make win      build/Bivio.exe  (MinGW-w64 cross compiler: brew install mingw-w64)
#   make all      both
#   make dmg      build/Bivio.dmg, the drag-to-install disk image of the Mac app
#   make preview  renders the macOS popover pages to build/popover-*.png, to check the layout
#   make icons    regenerates mac/AppIcon.icns and win/bivio.ico from assets/ (needs Icon Composer)
#   make clean    removes build/

APP     := build/Bivio.app
EXE     := build/Bivio.exe
DMG     := build/Bivio.dmg
WIN_CC  := x86_64-w64-mingw32-gcc
WINDRES := x86_64-w64-mingw32-windres
ICTOOL  := "/Applications/Icon Composer.app/Contents/Executables/ictool"

.PHONY: all mac win dmg preview icons clean

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

dmg: $(DMG)

# Background and layout made with dmgly; see tools/make_dmg.sh. The first run asks to allow controlling Finder.
$(DMG): $(APP) assets/dmg-background.png mac/AppIcon.icns tools/make_dmg.sh
	tools/make_dmg.sh $(APP) assets/dmg-background.png mac/AppIcon.icns $@

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

# mac/AppIcon.icns comes from assets/Bivio.icon, the Icon Composer document, rendered by its ictool
# (Default appearance: a plain .icns has no dark or tinted variants); win/bivio.ico from assets/icon.svg
# (the bare icon). Both outputs are committed.
icons:
	mkdir -p build
	$(ICTOOL) assets/Bivio.icon --export-image --output-file build/Bivio-macOS.png \
		--platform macOS --rendition Default --width 1024 --height 1024 --scale 1
	swiftc -O tools/make_icons/main.swift -o build/make_icons
	rm -rf build/AppIcon.iconset
	build/make_icons build/Bivio-macOS.png build/AppIcon.iconset assets/icon.svg win/bivio.ico
	iconutil -c icns build/AppIcon.iconset -o mac/AppIcon.icns

clean:
	rm -rf build
