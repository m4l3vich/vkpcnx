#!/usr/bin/env bash
# Regenerates every app icon from resources/icon/vkpcnx_logo.svg.
# Needs rsvg-convert (librsvg) and ImageMagick 7 (`magick`); on macOS the
# .icns is built with iconutil, elsewhere with ImageMagick.
#
#   ./scripts/gen-icons.sh
#
# Outputs:
#   resources/icon/icon.jpg   Switch .nro icon (256x256 JPEG, no alpha)
#   resources/icon/icon.png   desktop window icon (glfwSetWindowIcon)
#   vkpcnx/vkpcnx.ico         Windows executable icon
#   vkpcnx/vkpcnx.icns        macOS bundle icon
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=resources/icon/vkpcnx_logo.svg
# Switch home menu tiles are square and opaque; matches the app's dark theme.
SWITCH_BG="${SWITCH_BG:-#1f2226}"

for tool in rsvg-convert magick; do
    command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# render SIZE PADDING_PERCENT OUT: logo centred on a transparent SIZExSIZE
# canvas with PADDING_PERCENT of empty margin on each side.
render() {
    local size=$1 pad=$2 out=$3
    local inner=$((size - 2 * size * pad / 100))
    rsvg-convert -w "$inner" -h "$inner" --keep-aspect-ratio "$SRC" -o "$TMP/inner.png"
    magick "$TMP/inner.png" -background none -gravity center -extent "${size}x${size}" \
        -define png:color-type=6 "$out"
}

# Switch: opaque JPEG, logo inset so it doesn't touch the tile's rounded corners
render 256 12 "$TMP/switch.png"
magick "$TMP/switch.png" -background "$SWITCH_BG" -flatten -quality 95 resources/icon/icon.jpg

# Desktop window icon
render 256 0 resources/icon/icon.png

# Windows: multi-resolution .ico (PNG-compressed 256 entry)
ico_parts=()
for s in 16 24 32 48 64 128 256; do
    render "$s" 0 "$TMP/ico_$s.png"
    ico_parts+=("$TMP/ico_$s.png")
done
magick "${ico_parts[@]}" vkpcnx/vkpcnx.ico

# macOS: Big Sur grid keeps artwork within ~824/1024 of the canvas (~10% margin)
ICONSET="$TMP/vkpcnx.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    render "$s" 10 "$ICONSET/icon_${s}x${s}.png"
    render $((s * 2)) 10 "$ICONSET/icon_${s}x${s}@2x.png"
done
if command -v iconutil >/dev/null; then
    iconutil -c icns "$ICONSET" -o vkpcnx/vkpcnx.icns
else
    magick "$ICONSET/icon_512x512@2x.png" vkpcnx/vkpcnx.icns
fi

echo "icons written:"
ls -l resources/icon/icon.jpg resources/icon/icon.png vkpcnx/vkpcnx.ico vkpcnx/vkpcnx.icns
