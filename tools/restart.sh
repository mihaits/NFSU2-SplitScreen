#!/bin/sh
# Kill the game, install build/NFSU2SplitScreen.asi into <game>/scripts, launch it through the shell (so the per-user
# VirtualStore graphics settings apply) and wait for the main menu. Expects this repo checked out somewhere inside the
# game folder (the game folder is the nearest parent holding speed2.exe).
# sh restart.sh [--noskip]
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="$HERE"; until [ -f "$GAME/speed2.exe" ]; do [ "$GAME" = / ] && { echo "speed2.exe not found"; exit 1; }; GAME="$(dirname "$GAME")"; done
taskkill //F //IM speed2.exe >/dev/null 2>&1; sleep 2
cp "$HERE/../build/NFSU2SplitScreen.asi" "$GAME/scripts/" || exit 1
rm -f "$GAME/scripts/NFSU2SplitScreen.log"
explorer.exe "$(cygpath -w "$HERE/launch_game.bat")"
[ "$1" = "--noskip" ] && exit 0
cd "$HERE" && python -I boot.py 60
