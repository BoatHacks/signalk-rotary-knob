#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Regenerate the knob's text fonts (components/marine_ui/fonts/knob_font_<size>.c) with LVGL's font converter.
#
#   tools/gen_fonts.sh                 # uses npx lv_font_conv@1.5.3
#   LV_FONT_CONV="node /path/to/lv_font_conv.js" tools/gen_fonts.sh
#
# Run from the repository root after one build, so managed_components/lvgl__lvgl holds the source fonts LVGL ships with
# its built-in fonts. Coverage (measured 2026-11-24, docs/plans/issue-10-auto-demo.md):
#   Montserrat Medium: ASCII, Latin-1, Latin Extended-A, Cyrillic (221 of 256), typographic punctuation, euro, bullet
#   DejaVu Sans:       Greek (Montserrat has none)
#   Font Awesome:      the same symbol set as LVGL's built-in fonts (LV_SYMBOL_*)
# 4 bits per pixel, LVGL's compressed format: needs CONFIG_LV_USE_FONT_COMPRESSED=y (sdkconfig.defaults), else LVGL crashes at
# the first letter. Flash cost: 14 px 43.5 KB, 20 px 61.2 KB, 32 px 107.4 KB. No RAM.
set -eu

conv="${LV_FONT_CONV:-npx --yes lv_font_conv@1.5.3}"
src=managed_components/lvgl__lvgl/scripts/built_in_font
out=components/marine_ui/fonts
[ -f "$src/Montserrat-Medium.ttf" ] || { echo "no $src: build once first so the component manager fetches LVGL" >&2; exit 1; }
mkdir -p "$out"

text='0x20-0x7F,0xA0-0x17F,0x400-0x4FF,0x2010-0x2027,0x20AC,0x2022'
greek='0x370-0x3FF'
# LVGL's built-in symbol list (lv_font_montserrat_14.c header, LVGL 9.3).
symbols='61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650'

for size in 14 20 32; do
    # shellcheck disable=SC2086 # $conv may be a command with arguments
    $conv --bpp 4 --size "$size" \
        --font "$src/Montserrat-Medium.ttf" -r "$text" \
        --font "$src/DejaVuSans.ttf" -r "$greek" \
        --font "$src/FontAwesome5-Solid+Brands+Regular.woff" -r "$symbols" \
        --format lvgl --lv-include lvgl.h --lv-font-name "knob_font_$size" --force-fast-kern-format \
        -o "$out/knob_font_$size.c"
    echo "wrote $out/knob_font_$size.c"
done

# The alarm screen's big icons: only the warning triangle and the position symbol.
# shellcheck disable=SC2086
$conv --bpp 4 --size 96 --font "$src/FontAwesome5-Solid+Brands+Regular.woff" -r 61553,61732 \
    --format lvgl --lv-include lvgl.h --lv-font-name knob_icon_96 -o "$out/knob_icon_96.c"
echo "wrote $out/knob_icon_96.c"

# The mode ring's icons: ship's wheel, anchor, light bulb, music, USB, gear, and the propeller traced from the splash
# picture (assets/icons/propeller.otf, one glyph at U+F863, its outline is in propeller.json).
# shellcheck disable=SC2086
$conv --bpp 4 --size 28 --font "$src/FontAwesome5-Solid+Brands+Regular.woff" -r 0xf655,0xf13d,0xf0eb,0xf001,0xf287,0xf013 \
    --font assets/icons/propeller.otf -r 0xf863 \
    --format lvgl --lv-include lvgl.h --lv-font-name knob_icon_28 -o "$out/knob_icon_28.c"
echo "wrote $out/knob_icon_28.c"
