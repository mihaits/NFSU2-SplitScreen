#!/bin/sh
# Next Race check: starts a split drag race on virtual pads (go_race_nointro.sh 2), drives it to the results
# (drag_blow.py), then N times: Post Race -> Next Race (first icon) -> track select -> A, and checks that the next race
# loads without the options or car select and with the same players (log: "next race", "last round again").
# Screenshots of Post Race and the track select go to build/nr/. Stops at the first failure. Keep the window focused.
# sh next_race_check.sh [rounds=2]
cd "$(dirname "$0")"
LOG=../../../scripts/NFSU2SplitScreen.log
ts() { date +%H:%M:%S; }
mkdir -p ../build/nr
echo "$(ts) boot and join"
sh go_race_nointro.sh 2 2>&1 | tail -1
for r in $(seq 1 "${1:-2}"); do
    echo "$(ts) == round $r: racing"
    python -I -u drag_blow.py ../build/nr 45 2>&1 | tail -1
    python -I -u - "$r" "$LOG" <<'PY' || exit 1
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game(); r, log = sys.argv[1], sys.argv[2]
act = lambda: [p[0] for p in fe.packages(g) if p[1]]
say = lambda m: print(time.strftime('%H:%M:%S'), m, flush=True)
def wait(pred, tmo, what):
    end = time.time() + tmo
    while time.time() < end:
        if pred(): return
        time.sleep(0.1)
    sys.exit(f'FAIL: {what} {act()}')
start = len(open(log, errors='replace').read())
fe.press(g, 1, 'a', 0.3)
wait(lambda: 'UI_postrace.fng' in act(), 6, 'no Post Race')
time.sleep(1.2); g.screenshot(rf'../build/nr/post_{r}.png', max_w=960); say(f'post race: build/nr/post_{r}.png')
fe.press(g, 1, 'a', 0.3)
wait(lambda: 'UI_QRTrackSelect.fng' in act(), 30, 'no track select')
time.sleep(1.5); g.screenshot(rf'../build/nr/track_{r}.png', max_w=960); say(f'track select: build/nr/track_{r}.png')
fe.press(g, 1, 'a', 0.3)
t0 = time.time(); seen = set()
while time.time() - t0 < 25:
    seen |= set(act())
    if g.u32(0x8654A4) == 6 and g.u32(0x890118): break
    time.sleep(0.05)
else:
    sys.exit(f'FAIL: no race after 25 s {act()}')
if 'UI_QRModeOptions.fng' in seen: sys.exit('FAIL: the options screen came up')
text = open(log, errors='replace').read()[start:]
print(''.join(l + '\n' for l in text.splitlines() if 'next race' in l or 'last round again' in l or 'race: player' in l), end='')
if 'last round again' not in text: sys.exit('FAIL: the last round was not started again')
say(f'race loading {time.time() - t0:.1f} s after the track pick')
PY
done
echo "$(ts) all rounds passed"
