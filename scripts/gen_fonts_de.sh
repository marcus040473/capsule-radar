#!/usr/bin/env bash
# Erzeugt die Schriften in src/fonts/ neu: LVGLs Montserrat (gleiche Groessen, gleiche
# Symbole wie die eingebauten Schriften), aber mit Latin-1 (0xA0-0xFF) fuer Umlaute.
# Braucht Node.js und einen LVGL-8.4-Checkout (fuer die Quell-Schriftdateien):
#   git clone --depth 1 --branch v8.4.0 https://github.com/lvgl/lvgl /tmp/lvgl
#   ./scripts/gen_fonts_de.sh /tmp/lvgl
set -euo pipefail
cd "$(dirname "$0")/.."
LVGL="${1:?Pfad zum LVGL-8.4-Checkout}"
OUT="$PWD/src/fonts"
mkdir -p "$OUT"
SYMS=$(grep -oE "FontAwesome5-Solid\+Brands\+Regular.woff -r [0-9,]+" "$LVGL/src/font/lv_font_montserrat_14.c" | sed 's/.* -r //')
cd "$LVGL/scripts/built_in_font"
for s in 12 14 16 18 20 28; do
  f="$OUT/lv_font_montserrat_${s}.c"
  npx -y lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size "$s" \
    --font Montserrat-Medium.ttf -r 0x20-0x7F,0xA0-0xFF,0x2022 \
    --font FontAwesome5-Solid+Brands+Regular.woff -r "$SYMS" \
    --format lvgl -o "lv_font_montserrat_${s}.c" --force-fast-kern-format   # relative: keeps paths out of the header
  mv "lv_font_montserrat_${s}.c" "$f"
  # immer kompilieren (LV_FONT_MONTSERRAT_xx ist in lv_conf.h aus) und lvgl.h direkt einbinden
  python3 - "$f" "$s" <<'PY'
import sys
f, s = sys.argv[1], sys.argv[2]
t = open(f, encoding='utf-8').read()
t = t.replace('#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif\n',
              '// Capsule Radar (Deutsch): Montserrat %spx mit Latin-1 (ä ö ü ß ° ...) statt nur ASCII.\n'
              '// Ersetzt LVGLs eingebaute Schrift gleichen Namens (siehe include/lv_conf.h, scripts/gen_fonts_de.sh).\n'
              '#include "lvgl.h"\n' % s, 1)
t = t.replace('#ifndef LV_FONT_MONTSERRAT_%s\n#define LV_FONT_MONTSERRAT_%s 1\n#endif\n\n#if LV_FONT_MONTSERRAT_%s\n' % (s, s, s), '', 1)
t = t.replace('#endif /*#if LV_FONT_MONTSERRAT_%s*/' % s, '', 1)
# Zeilenhoehe/Grundlinie wie LVGLs Originalschrift: die Akzent-Grossbuchstaben wuerden die
# Zeilen sonst hoeher machen und das ganze Layout verschieben.
import re
lh, bl = {'12': (15, 3), '14': (16, 3), '16': (18, 3), '18': (21, 4), '20': (22, 4), '28': (30, 5)}[s]
t = re.sub(r'\.line_height = \d+,', '.line_height = %d,' % lh, t, count=1)
t = re.sub(r'\.base_line = \d+,', '.base_line = %d,' % bl, t, count=1)
open(f, 'w', encoding='utf-8').write(t)
PY
  echo "==> $f"
done
