#!/bin/sh
# Restart the game and start a split-screen Free Run from start $1 (the Start row's index, [FREERUN] Start: 0 Airport,
# 1 City Core, ... 10 Vacant Lot; default 0), players joined on virtual pads (the Players row's count: the [DEBUG]
# ExtraPlayers default). VIRTUAL=n joins only players 1..n and stops at the next Press START, for real pads.
# Prints a timestamped line per stage and stops at the first one that fails (timeouts of a few seconds).
cd "$(dirname "$0")"
GAME="$(pwd)"; until [ -f "$GAME/speed2.exe" ]; do [ "$GAME" = / ] && { echo "speed2.exe not found"; exit 1; }; GAME="$(dirname "$GAME")"; done
INI="$GAME/scripts/NFSU2SplitScreen.ini"
LOG="$GAME/scripts/NFSU2SplitScreen.log"
stamp() { echo "$(date +%T) $*"; }
# the plugin reads [FREERUN] Start when it loads: set it for this launch, restore it once the game runs
old=$(sed -n '/^\[FREERUN\]/,/^\[/s/^Start *= *//p' "$INI" | head -1)
if [ -n "$old" ]; then sed -i "/^\[FREERUN\]/,/^\[/s/^Start *= *.*/Start = ${1:-0}/" "$INI"
else printf '\n[FREERUN]\nStart = %s\n' "${1:-0}" >> "$INI"; fi
stamp "start ${1:-0}: restart"
sh restart.sh --noskip < /dev/null > /dev/null 2>&1
boot=$(python -I boot.py 20 < /dev/null 2>&1 | tail -1)
[ -n "$old" ] && sed -i "/^\[FREERUN\]/,/^\[/s/^Start *= *.*/Start = $old/" "$INI"
stamp "$boot"
case "$boot" in "main menu"*) ;; *) stamp "FAIL: no main menu in 20 s (a hung boot: see docs/tooling.md)"; exit 1;; esac
python -I rec.py play start_to_split2p --from 3 --to 5 < /dev/null > /dev/null || { stamp "FAIL: split-screen menus"; exit 1; }
python -I freerun_flow.py < /dev/null || exit 1
[ "${VIRTUAL:-6}" -lt 6 ] && exit 0                       # the rest join on real pads
end=$(( $(date +%s) + 30 ))
while [ $(date +%s) -lt $end ]; do grep -aq "start spot 5" "$LOG" 2>/dev/null && break; sleep 0.3; done
grep -aq "start spot 5" "$LOG" 2>/dev/null || { stamp "FAIL: no start spots logged in 30 s (Debug = 1 in the ini?)"; exit 1; }
stamp "cars placed: $(grep -a 'start spot 0' "$LOG" | tail -1 | cut -d' ' -f4-)"
