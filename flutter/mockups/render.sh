#!/bin/sh
# render.sh NAME — NAME.html → NAME.png at iPhone 12 size (390x844 @3x)
cd "$(dirname "$0")"
for n in "$@"; do
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --disable-gpu --hide-scrollbars \
    --force-device-scale-factor=2 --window-size=390,844 --virtual-time-budget=3000 \
    --screenshot="$PWD/$n.png" "file://$PWD/$n.html" 2>/dev/null
done
