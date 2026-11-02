#!/bin/sh
# Runs race starts through start, autopilot drive-off, pause menu Restart and drive-off again, for changes to shared
# race-start code. Stops at the first failure. Prints a line per stage plus every jump teleport_watch sees: the moves
# when a scene lets go are expected, one after the go signal is a bug. The game window must stay focused.
# sh start_matrix.sh [config ...]      default: all. config = humans:ai:mode[:track], e.g. 3:0:4 or 2:4:3:4311
# modes as in go_race_nointro.sh: 0 circuit, 3 drift, 4 Street X. The ini's player and opponent counts are restored.
cd "$(dirname "$0")"
INI=../../../scripts/NFSU2SplitScreen.ini
OLD_EXTRA=$(sed -n 's/^ExtraPlayers = //p' "$INI" | tr -d '\r')
OLD_AI=$(sed -n 's/^Opponents = //p' "$INI" | tr -d '\r')
restore() { sed -i "s/^ExtraPlayers = .*/ExtraPlayers = $OLD_EXTRA/; s/^Opponents = .*/Opponents = $OLD_AI/" "$INI"; }
trap restore EXIT
[ $# -gt 0 ] || set -- 2:0:4 3:0:4 4:0:4 6:0:4 4:0:3:4311 6:0:3:4311 2:4:3:4311 6:0:0 2:3:0

watch() { python -I -u teleport_watch.py "$1" --min 1.0 2>&1 | grep -E --line-buffered 'JUMP|done:|Traceback'; }
for cfg in "$@"; do
    IFS=: read -r humans ai mode track <<EOT
$cfg
EOT
    sed -i "s/^ExtraPlayers = .*/ExtraPlayers = $((humans - 2))/; s/^Opponents = .*/Opponents = $ai/" "$INI"
    case $mode in 0) lap=83AACE=1 ;; 3) lap=83AB0A=1 ;; 4) lap=83AB96=2 ;; *) lap= ;; esac
    echo "== $cfg: start"
    INTRO=1 TRACK=$track POKES=$lap sh go_race_nointro.sh "$mode" 2>&1 | tail -1
    watch 22 &
    python -I -u grid_check.py 20 || exit 1
    python -I -u autorace.py --timeout 7 --stall 15 2>&1 | grep -E --line-buffered 'autopilot on|Traceback'
    wait
    echo "== $cfg: restart"
    python -I -u - <<'PY' || exit 1
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game()
fe.press(g, 1, 'start'); time.sleep(1.5); fe.press(g, 1, 'right'); time.sleep(0.6); fe.press(g, 1, 'a', 0.3); time.sleep(1.2)
fe.press(g, 1, 'left', 0.2); time.sleep(0.6); fe.press(g, 1, 'a', 0.3)
end = time.time() + 6
while time.time() < end:
    mgr = g.u32(0x890118)
    if g.u32(0x8654A4) == 6 and mgr and g.u32(mgr) == 3: sys.exit()
    time.sleep(0.03)
sys.exit('FAIL: no countdown after restart')
PY
    watch 12 &
    python -I -u grid_check.py 6 || exit 1
    python -I -u autorace.py --timeout 5 --stall 15 2>&1 | grep -E --line-buffered 'autopilot on|Traceback'
    wait
    echo "== $cfg: passed"
done
echo "all passed: $*"
