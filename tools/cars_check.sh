#!/bin/sh
# Cars row check: starts split races with a given Cars mode on virtual pads (go_race_nointro.sh), waits for the
# countdown and checks the log: each seat's car (the picks for "each", player 1's car for everyone for "same", distinct
# cars within the logged rating range for "random"), then grid_check and quit. Stops at the first failure; the ini's
# player count and Cars mode are restored. The game window must stay focused.
# sh cars_check.sh [config ...]   config = humans:cars[:mode], cars 0 each / 1 same / 2 random, mode as in
#                                 go_race_nointro.sh (default 2 = drag). Default: 2:1 4:1 2:2 4:2 6:2 3:0
cd "$(dirname "$0")"
set -o pipefail   # a stage piped through tail still fails the run
INI=../../../scripts/NFSU2SplitScreen.ini
LOG=../../../scripts/NFSU2SplitScreen.log
OLD_EXTRA=$(sed -n 's/^ExtraPlayers = //p' "$INI" | tr -d '\r')
OLD_CARS=$(sed -n 's/^CarsMode = //p' "$INI" | tr -d '\r')
grep -q '^CarsMode = ' "$INI" || { sed -i 's/^ExtraPlayers = .*/&\nCarsMode = 0/' "$INI"; OLD_CARS=0; }
restore() { sed -i "s/^ExtraPlayers = .*/ExtraPlayers = $OLD_EXTRA/; s/^CarsMode = .*/CarsMode = $OLD_CARS/" "$INI"; }
trap restore EXIT
[ $# -gt 0 ] || set -- 2:1 4:1 2:2 4:2 6:2 3:0
ts() { date +%H:%M:%S; }
for cfg in "$@"; do
    IFS=: read -r humans cars mode <<EOT
$cfg
EOT
    sed -i "s/^ExtraPlayers = .*/ExtraPlayers = $((humans - 2))/; s/^CarsMode = .*/CarsMode = $cars/" "$INI"
    echo "$(ts) == $cfg: boot and join"
    sh go_race_nointro.sh "${mode:-2}" 2>&1 | tail -1 || exit 1
    echo "$(ts) == $cfg: waiting for the countdown"
    python -I -u - "$humans" "$cars" "$LOG" <<'PY' || exit 1
import sys, time, re
sys.path.insert(0, '.')
import trainer as t
g = t.Game()
humans, cars, log = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
end = time.time() + 12
while time.time() < end:
    mgr = g.u32(0x890118)
    if g.u32(0x8654A4) == 6 and mgr: break
    time.sleep(0.1)
else:
    sys.exit('FAIL: no race after 12 s')
text = open(log, errors='replace').read()
text = text[text.rfind('player 1 joined'):]
picked = {int(k): v for k, v in re.findall(r'race: player (\d) car ([0-9A-F]{8})', text)}
given = {int(k): v for k, v in re.findall(r'cars: player (\d) gets ([0-9A-F]{8})', text)}
print('picked', picked, 'given', given, flush=True)
if sorted(picked) != list(range(1, humans + 1)): sys.exit(f'FAIL: race cars of {sorted(picked)}, expected 1..{humans}')
if cars == 0 and given: sys.exit('FAIL: cars given with "Pick Each"')
if cars == 1:
    if sorted(given) != list(range(2, humans + 1)): sys.exit(f'FAIL: given to {sorted(given)}')
    if len(set(picked.values())) != 1: sys.exit('FAIL: not the same car for everyone')
if cars == 2:
    if sorted(given) != list(range(1, humans + 1)): sys.exit(f'FAIL: given to {sorted(given)}')
    if any(picked[k] != given[k] for k in given): sys.exit('FAIL: a race car differs from the given one')
    rows = re.findall(r'random cars: player \d gets [0-9A-F]{8} \(rating ([\d.]+), anchor ([\d.]+) \+-(\d+)%, (\d+) of (\d+)', text)
    for r, a, tol, m, n in rows:
        if abs(float(r) - float(a)) > float(a) * int(tol) / 100 + 0.1: sys.exit(f'FAIL: rating {r} outside {a} +-{tol}%')
    if len(set(given.values())) < min(humans, int(rows[0][3])): sys.exit('FAIL: repeated cars while others were in range')
    print(f'ratings {[r[0] for r in rows]} anchor {rows[0][1]} +-{rows[0][2]}%, {rows[0][3]} of {rows[0][4]} cars', flush=True)
print('race loading with the expected cars', flush=True)
PY
    python -I -u grid_check.py 20 || exit 1
    python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;t.Game().screenshot(r'../build/cars_$humans$cars.png', max_w=1280)"
    echo "$(ts) == $cfg: screenshot build/cars_$humans$cars.png"
    python -I quit_race.py >/dev/null 2>&1
    echo "$(ts) == $cfg: passed"
done
echo "$(ts) all passed: $*"
