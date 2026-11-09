#!/usr/bin/env bash
# Downloads the life-number fonts (Google Fonts repo, OFL / Apache licensed) and
# regenerates mtg_life_counter/fonts/*.h with tools/make_bigfont.py.
# The order here must match FONTS[] in the sketch (index = saved setting).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/mtg-life-counter-fonts"
OUT="$ROOT/mtg_life_counter/fonts"
GF=https://raw.githubusercontent.com/google/fonts/main
mkdir -p "$CACHE" "$OUT"

#     id        source (repo path or local file)                         variable-font style
FONTS=(
  "noto       /usr/share/fonts/truetype/noto/NotoSans-Bold.ttf          -"
  "cinzel     ofl/cinzel/Cinzel[wght].ttf                               Bold"
  "bebas      ofl/bebasneue/BebasNeue-Regular.ttf                       -"
  "orbitron   ofl/orbitron/Orbitron[wght].ttf                           Bold"
  "bungee     ofl/bungee/Bungee-Regular.ttf                             -"
  "blackops   ofl/blackopsone/BlackOpsOne-Regular.ttf                   -"
  "pixel      ofl/pressstart2p/PressStart2P-Regular.ttf                 -"
  "marker     apache/permanentmarker/PermanentMarker-Regular.ttf        -"
  "righteous  ofl/righteous/Righteous-Regular.ttf                       -"
  "bangers    ofl/bangers/Bangers-Regular.ttf                           -"
  "creepster  ofl/creepster/Creepster-Regular.ttf                       -"
  "griffy     ofl/griffy/Griffy-Regular.ttf                             -"
)

for line in "${FONTS[@]}"; do
  read -r id src var <<<"$line"
  if [[ "$src" == /* ]]; then
    ttf="$src"
  else
    ttf="$CACHE/$(basename "$src")"
    [ -s "$ttf" ] || curl -sfL -o "$ttf" "$GF/$(sed 's/\[/%5B/;s/\]/%5D/' <<<"$src")"
  fi
  V=(); [ "$var" != "-" ] && V=(--var "$var")
  up=$(tr a-z A-Z <<<"$id")
  python3 -I "$ROOT/tools/make_bigfont.py" "$ttf" "F_${up}_XL" "$OUT/${id}_xl.h" --xl "${V[@]}"
  python3 -I "$ROOT/tools/make_bigfont.py" "$ttf" "F_${up}_L"  "$OUT/${id}_l.h"  --l  "${V[@]}"
done
