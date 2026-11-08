#!/bin/sh
# Random series check: boots to the split mode select, presses Y (mode roll, then track roll, then the options),
# joins every player on virtual pads and races; then per round Post Race -> Next Race must roll a new mode and track
# by itself and load the race with no input (log "random mode", "random track", "last round again"). Races are
# driven to the results by drag_blow.py (drag) or autorace.py (the rest), 1 lap where a mode has laps.
# Screenshots: build/nr/rnd_*.png. Stops at the first failure. The game window must stay focused.
# sh random_check.sh [rounds=2]
cd "$(dirname "$0")"
set -o pipefail   # a stage piped through tail still fails the run
LOG=../../../scripts/NFSU2SplitScreen.log
ts() { date +%H:%M:%S; }
mkdir -p ../build/nr
# 1 lap: circuit, drift, URL, Street X (quick race settings +0xA)
export POKES="83AACE=1 83AB0A=1 83AB1E=1 83AB96=1"
echo "$(ts) boot"
sh restart.sh --noskip >/dev/null 2>&1 || { echo "$(ts) FAIL: restart"; exit 1; }
python -I boot.py 20 || exit 1
python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;g=t.Game();[g.w32(int(a,16),int(v)) for a,v in (x.split('=') for x in sys.argv[1:])]" $POKES || exit 1
python -I rec.py play start_to_split2p --from 3 --to 4 >/dev/null || exit 1
race() {   # drive the running race to the results
    if [ "$(python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;print(t.Game().u32(0x83AAB4))")" = 2 ]; then
        python -I -u drag_blow.py ../build/nr 60 2>&1 | tail -1
    else
        python -I -u autorace.py 2>&1 | tail -1
    fi
}
python -I -u - <<'PY' || exit 1
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game()
act = lambda: [p[0] for p in fe.packages(g) if p[1]]
say = lambda m: print(time.strftime('%H:%M:%S'), m, flush=True)
def wait(pred, tmo, what):
    end = time.time() + tmo
    while time.time() < end:
        if pred(): return
        time.sleep(0.05)
    sys.exit(f'FAIL: {what} {act()}')
fe.press(g, 1, 'a', 0.1); wait(lambda: 'UI_QRModeSelect.fng' in act(), 6, 'no mode select'); time.sleep(1.2)
g.screenshot(r'../build/nr/rnd_modes.png', max_w=1280); say('mode select: build/nr/rnd_modes.png')
fe.press(g, 1, 'y', 0.1)
wait(lambda: 'UI_QRModeOptions.fng' in act(), 10, 'no options after the mode and track rolls')
ty = g.u32(0x83AAB4); say(f'rolled type {ty}, track {g.u32(0x83AAC4 + ty * 0x14 + 8) & 0xffff}')
time.sleep(1)
fe.press(g, 1, 'a', 0.1)   # options -> car select (every player joins below, player 1 on pad 1)
def wait_top(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        top = fe.top(g) or ''
        if pred(top): return top
        time.sleep(0.1)
    return fe.top(g) or ''
for pad in (1, 2, 3, 0, 4, 5):
    if wait_top(lambda s: s == '2P_PressStart.fng', 3.0) != '2P_PressStart.fng': break
    fe.press(g, pad, 'start'); wait_top(lambda s: s != '2P_PressStart.fng', 1.5); time.sleep(0.3)
    fe.press(g, pad, 'a')
    if 'dialog' in wait_top(lambda s: 'dialog' in s.lower() or s == '2P_PressStart.fng', 1.0).lower():
        fe.press(g, pad, 'a')
wait(lambda: g.u32(0x8654A4) == 6 and g.u32(0x890118), 25, 'no race')
say('first race loading')
PY
for r in $(seq 1 "${1:-2}"); do
    echo "$(ts) == round $r: racing"
    race || exit 1
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
        time.sleep(0.05)
    sys.exit(f'FAIL: {what} {act()}')
start = len(open(log, errors='replace').read())
last = g.u32(0x83AAB4); lastTrack = g.u32(0x83AAC4 + last * 0x14 + 8) & 0xffff
wait(lambda: 'UI_PostRaceResults.fng' in act(), 40, 'no results'); time.sleep(2.5)
fe.press(g, 1, 'a', 0.3); wait(lambda: 'UI_postrace.fng' in act(), 6, 'no Post Race'); time.sleep(1.2)
fe.press(g, 1, 'a', 0.1); t0 = time.time(); seen = set()
while time.time() - t0 < 40:
    seen |= set(act())
    if 'UI_QRTrackSelect.fng' in seen and time.time() - t0 > 1 and 'shot' not in seen:
        g.screenshot(rf'../build/nr/rnd_track_{r}.png', max_w=1280); seen.add('shot')
    if g.u32(0x8654A4) == 6 and g.u32(0x890118) and 'UI_QRTrackSelect.fng' in seen: break
    time.sleep(0.05)
else:
    sys.exit(f'FAIL: no race after Next Race {act()}')
if 'UI_QRModeOptions.fng' in seen: sys.exit('FAIL: the options screen came up')
ty = g.u32(0x83AAB4); tr = g.u32(0x83AAC4 + ty * 0x14 + 8) & 0xffff
text = open(log, errors='replace').read()[start:]
for l in text.splitlines():
    if 'random' in l or 'last round again: race' in l: print('  ' + l)
if ty == last: sys.exit(f'FAIL: the same mode {ty} again')
if 'last round again' not in text: sys.exit('FAIL: the last round was not started again')
say(f'round {r}: type {last} track {lastTrack} -> type {ty} track {tr}, race loading {time.time() - t0:.1f} s after Next Race')
PY
done
echo "$(ts) all rounds passed"
