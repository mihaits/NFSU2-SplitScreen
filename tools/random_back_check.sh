#!/bin/sh
# Random series: Back after Next Race ends it. Restart with [DEBUG] RollMode = 3 (mode rolls pick drag; restored at the
# end), Y on the split mode select (mode and track roll), join every player on virtual pads, a drag race (drag_blow.py),
# Post Race -> Next Race: the series rolls again; its track roll is held off (the plugin's g_randomTrackPending kept
# clear, address from build/NFSU2SplitScreen.map) and B on the track select returns to the mode select. The series must
# end there (log "random: series ended", g_randomSeries 0). A line per stage; stops at the first failure. The game
# window must stay focused.
cd "$(dirname "$0")"
set -o pipefail   # a stage piped through tail still fails the run
INI=../../../scripts/NFSU2SplitScreen.ini
LOG=../../../scripts/NFSU2SplitScreen.log
ts() { date +%H:%M:%S; }
grep -q "^RollMode" "$INI" || sed -i "s/^CarsMode = \(.*\)/CarsMode = \1\nRollMode = 0/" "$INI"
OLD=$(sed -n 's/^RollMode = //p' "$INI" | tr -d '\r')
trap 'sed -i "s/^RollMode = .*/RollMode = $OLD/" "$INI"' EXIT
sed -i "s/^RollMode = .*/RollMode = 3/" "$INI"
mkdir -p ../build/nr
echo "$(ts) boot"
sh restart.sh --noskip >/dev/null 2>&1 || { echo "$(ts) FAIL: restart"; exit 1; }
python -I boot.py 20 || exit 1
python -I rec.py play start_to_split2p --from 3 --to 4 >/dev/null || exit 1
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
fe.press(g, 1, 'y', 0.1)
wait(lambda: 'UI_QRModeOptions.fng' in act(), 10, 'no options after the mode and track rolls')
if g.u32(0x83AAB4) != 2: sys.exit(f'FAIL: rolled type {g.u32(0x83AAB4)}, not drag (RollMode = 3)')
say(f'rolled drag, track {g.u32(0x83AAC4 + 2 * 0x14 + 8) & 0xffff}')
time.sleep(1); fe.press(g, 1, 'a', 0.1)
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
say('drag race loading')
PY
echo "$(ts) racing"
python -I -u drag_blow.py ../build/nr 60 2>&1 | tail -1 || exit 1
python -I -u - "$LOG" <<'PY' || exit 1
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe, plugin
g = t.Game(); log = sys.argv[1]
act = lambda: [p[0] for p in fe.packages(g) if p[1]]
say = lambda m: print(time.strftime('%H:%M:%S'), m, flush=True)
def wait(pred, tmo, what, each=None):
    end = time.time() + tmo
    while time.time() < end:
        if each: each()
        if pred(): return
        time.sleep(0.02)
    sys.exit(f'FAIL: {what} {act()}')
base = plugin.module_base(g.pid) - 0x10000000
series = plugin.symbol('?g_randomSeries@@') + base
pending = plugin.symbol('?g_randomTrackPending@@') + base
wait(lambda: 'UI_PostRaceResults.fng' in act(), 40, 'no results'); time.sleep(2.5)
for _ in range(4):                                         # results, standings -> Post Race
    if 'UI_postrace.fng' in act(): break
    fe.press(g, 1, 'a', 0.3); time.sleep(1.5)
wait(lambda: 'UI_postrace.fng' in act(), 6, 'no Post Race'); time.sleep(1.0)
if g.u8(series) != 1: sys.exit('FAIL: no random series before Next Race')
start = len(open(log, errors='replace').read())
fe.press(g, 1, 'a', 0.1)                                   # Next Race: the series rolls the mode again
hold = lambda: g.w8(pending, 0)                            # ... and its track roll is held off
wait(lambda: 'UI_QRTrackSelect.fng' in act(), 30, 'no track select after Next Race', hold)
end = time.time() + 1.0
while time.time() < end: hold(); time.sleep(0.02)
if 'random track' in open(log, errors='replace').read()[start:]: sys.exit('FAIL: the track roll was not held off')
say('track select after Next Race, no track roll: Back')
fe.press(g, 1, 'b', 0.1)
wait(lambda: 'UI_QRModeSelect.fng' in act(), 6, 'Back did not reach the mode select'); time.sleep(0.5)
text = open(log, errors='replace').read()[start:]
for l in text.splitlines():
    if 'random' in l or 'next race' in l: print('  ' + l)
if 'random: series ended' not in text or g.u8(series): sys.exit(f'FAIL: the series goes on (g_randomSeries {g.u8(series)})')
say('ok: Back after Next Race ended the series')
PY
