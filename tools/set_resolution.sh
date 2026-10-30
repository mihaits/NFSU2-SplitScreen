#!/bin/sh
# sh set_resolution.sh W H: sets the game's resolution for its next boot. WidescreenFix keeps it in
# HKCU\Software\EA Games\Need for Speed Underground 2, g_RacingResolution = (0x8000 | W) << 16 | H. It goes through
# explorer like restart.sh: a shell inside the Claude app has its own view of HKCU, the game never sees its writes.
# The game must be closed: it saves its current value when it boots and when it quits.
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -n "$2" ] || { echo "usage: set_resolution.sh W H"; exit 1; }
taskkill //F //IM speed2.exe >/dev/null 2>&1
for i in $(seq 50); do tasklist //FI "IMAGENAME eq speed2.exe" | grep -qi speed2 || break; sleep 0.1; done
VALUE=$(( ((0x8000 | $1) << 16) | $2 ))
BAT="$HERE/../build/set_resolution.bat"; OUT="$HERE/../build/set_resolution.txt"
rm -f "$OUT"
cat > "$BAT" <<BAT
@reg add "HKCU\Software\EA Games\Need for Speed Underground 2" /v g_RacingResolution /t REG_DWORD /d $VALUE /f >nul
@reg query "HKCU\Software\EA Games\Need for Speed Underground 2" /v g_RacingResolution > "$(cygpath -w "$OUT")"
BAT
explorer.exe "$(cygpath -w "$BAT")"
for i in $(seq 50); do [ -s "$OUT" ] && break; sleep 0.1; done
[ -s "$OUT" ] || { echo "FAIL: the setting wasn't written"; exit 1; }
grep -o '0x[0-9a-f]*' "$OUT" | grep -qi "$(printf '%x' "$VALUE")" || { echo "FAIL: wrote $(cat "$OUT")"; exit 1; }
echo "resolution $1x$2 set"
