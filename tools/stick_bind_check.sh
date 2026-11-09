#!/bin/sh
# Stick and trigger bindings end to end (stick_bind_check.py): boot, Controls captures / labels / rules / P3 page and a
# confirmed profile save, restart and check they came back, then a 3P race where P1's and P3's bound inputs fire.
# The players ini and the profile's save file are backed up first and restored at the end (also on failure).
# The game window must stay focused. Takes about 2 minutes.
cd "$(dirname "$0")"
set -o pipefail
OUT=$(mktemp)
INI=../../../scripts/NFSU2SplitScreen.ini
OLD_EXTRA=$(sed -n 's/^ExtraPlayers = //p' "$INI" | tr -d '\r')
finish() {
    taskkill //F //IM speed2.exe >/dev/null 2>&1; sleep 1
    sed -i "s/^ExtraPlayers = .*/ExtraPlayers = $OLD_EXTRA/" "$INI"
    python -I -u stick_bind_check.py restore
}
trap finish EXIT
stage() { echo "$(date +%H:%M:%S) == $1"; }
stage "boot"
sh restart.sh || exit 1
stage "menu"
python -I -u stick_bind_check.py menu | tee "$OUT" || exit 1
set -- $(sed -n 's/^RESULT //p' "$OUT")
[ $# -eq 2 ] || { echo "FAIL: no RESULT line"; exit 1; }
stage "restart"
sh restart.sh || exit 1
stage "persist"
python -I -u stick_bind_check.py persist "$1" "$2" || exit 1
stage "3P race"
sed -i "s/^ExtraPlayers = .*/ExtraPlayers = 1/" "$INI"
sh go_race_nointro.sh 4 2>&1 | tail -1 || exit 1
python -I -u stick_bind_check.py race "$1" "$2" || exit 1
stage "all passed"
