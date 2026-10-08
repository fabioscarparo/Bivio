#!/bin/bash
# Dev tool: packages Bivio.app in a drag-to-install disk image, on the background in assets/.
# Build and run: make dmg
#
# Same steps as create-dmg, with system tools only: hdiutil builds the image and Finder, driven by
# AppleScript, lays out the window and saves the layout in the image's .DS_Store. The first run asks
# to allow controlling Finder.
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: make_dmg.sh <Bivio.app> <background.png> <volume icon.icns> <output.dmg>" >&2
    exit 2
fi
app=$1 background=$2 volume_icon=$3 output=$4
name=$(basename "$app" .app)

# Layout of the background (made with dmgly): window content size, icon size and Finder label size
# in points, icon centers from the top left corner of the content.
window_width=642 window_height=406 icon_size=128 text_size=16
app_x=116 app_y=98 link_x=529 link_y=240
# Finder's window bounds include the title bar: 28 points up to macOS 15, 32 on macOS 26 and later,
# where the background loses its last 4 points rather than showing a strip of white below it. The path
# bar is a global Finder setting, not one of the window: where it is on, it covers the bottom of the
# background (no icons there).
title_bar=28

work=$(mktemp -d -t bivio-dmg)
device=
cleanup() {
    [[ -n $device ]] && hdiutil detach -quiet "$device" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

# Mounts the image at a random path, so another volume with the same name doesn't get in the way, and
# sets device and mount. (macOS 27 deprecates hdiutil attach for diskutil image attach, which older
# systems don't have.)
attach() {
    local output
    output=$(hdiutil attach -mountrandom /Volumes -readwrite -noverify -noautoopen "$@" "$work/rw.dmg" 2>&1 |
        grep -v deprecated)
    device=$(echo "$output" | grep -E '^/dev/' | sed 1q | awk '{print $1}')
    mount=$(echo "$output" | grep -E '/Volumes/' | sed 's/.*\(\/Volumes\/.*\)/\1/')
}

detach() {
    hdiutil detach -quiet "$device"
    device=
}

mkdir "$work/stage"
ditto "$app" "$work/stage/$name.app"
ln -s /Applications "$work/stage/Applications"
mkdir "$work/stage/.background"
cp "$background" "$work/stage/.background/background.png"
cp "$volume_icon" "$work/stage/.VolumeIcon.icns"
SetFile -c icnC "$work/stage/.VolumeIcon.icns"

hdiutil create -quiet -srcfolder "$work/stage" -volname "$name" -fs HFS+ -format UDRW -size 20m "$work/rw.dmg"

attach
SetFile -a C "$mount"  # the volume has a custom icon: .VolumeIcon.icns

# The window size goes last: applying the view options makes Finder resize the window. Finder keeps the
# layout in memory and writes it to the volume's .DS_Store when the volume is ejected.
osascript <<EOF
tell application "Finder"
    tell disk "$(basename "$mount")"
        open
        tell container window
            set current view to icon view
            set toolbar visible to false
            set statusbar visible to false
        end tell
        set options to icon view options of container window
        tell options
            set icon size to $icon_size
            set text size to $text_size
            set arrangement to not arranged
        end tell
        set background picture of options to file ".background:background.png"
        set extension hidden of item "$name.app" to true
        set position of item "$name.app" to {$app_x, $app_y}
        set position of item "Applications" to {$link_x, $link_y}
        set bounds of container window to {10, 60, 10 + $window_width, 60 + $window_height + $title_bar}
        close
        open
        delay 1
        set bounds of container window to {10, 60, 10 + $window_width - 10, 60 + $window_height + $title_bar - 10}
        delay 1
        set bounds of container window to {10, 60, 10 + $window_width, 60 + $window_height + $title_bar}
        delay 2
        close
    end tell
end tell
EOF

detach

# Finder has no AppleScript property for the tab bar, and saves it as shown when it is on in the Finder
# of the Mac building the image. Turn it off in the saved window settings: ShowTabView, in the binary
# plist of the "bwsp" record. Flipping the shared true object to false keeps the record's length.
# Mounted out of Finder's sight (-nobrowse), so that it doesn't write the file again.
attach -nobrowse
grep -q bwsp "$mount/.DS_Store" || { echo "Finder did not save the window layout." >&2; exit 1; }
python3 - "$mount/.DS_Store" <<'PYTHON'
import struct, sys
path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
record = b'\0\0\0\1' + '.'.encode('utf-16-be') + b'bwspblob'
at = data.find(record) + len(record)
length, = struct.unpack('>I', data[at:at + 4])
plist = at + 4  # start of the binary plist in data
offset_size, ref_size, _, top, table = struct.unpack('>6xBBQQQ', data[plist + length - 32:plist + length])

def number(start, size):
    return int.from_bytes(data[start:start + size], 'big')

def offset(index):  # position of an object in data
    return plist + number(plist + table + index * offset_size, offset_size)

def text(index):  # an ASCII string object
    start = offset(index)
    size = data[start] & 0x0F
    if size == 0x0F:  # longer strings: the length follows as an integer object
        width = 1 << (data[start + 1] & 0x0F)
        size, start = number(start + 2, width), start + 1 + width
    return data[start + 1:start + 1 + size].decode()

top_object = offset(top)
count = data[top_object] & 0x0F  # a dictionary with fewer than 15 entries
keys = [text(number(top_object + 1 + n * ref_size, ref_size)) for n in range(count)]
values = [number(top_object + 1 + (count + n) * ref_size, ref_size) for n in range(count)]
settings = dict(zip(keys, values))
if 'ShowTabView' in settings and data[offset(settings['ShowTabView'])] == 0x09:
    # true and false are single objects that all the settings share: flip it only if it is ShowTabView's alone
    assert all(key == 'ShowTabView' or data[offset(value)] != 0x09 for key, value in settings.items())
    data[offset(settings['ShowTabView'])] = 0x08
    open(path, 'wb').write(data)
PYTHON

chmod -Rf go-w "$mount" || true
rm -rf "$mount/.fseventsd"
detach

rm -f "$output"
hdiutil convert -quiet "$work/rw.dmg" -format ULMO -o "$output"  # LZMA, macOS 10.15 and later
