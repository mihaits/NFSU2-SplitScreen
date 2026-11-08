#!/bin/sh
# Next Race when the last round can't start again: a 2P circuit race (1 lap, autopilot), Post Race -> Next Race, Back
# from the track select to the mode select, Sprint, P2's virtual pad disconnected, a track. Player 1's popup must let
# the round be joined as usual and Sprint must still take the circuit's options it has (traffic, catch-up, difficulty:
# set differently in Sprint's settings before, then compared; log "next race: ... from type 0 to 1").
# A line per stage; stops at the first failure. The game window must stay focused.
cd "$(dirname "$0")"
set -o pipefail   # a stage piped through tail still fails the run
LOG=../../../scripts/NFSU2SplitScreen.log
ts() { date +%H:%M:%S; }
echo "$(ts) boot circuit 4013 (shortest), 1 lap"
TRACK=4013 POKES="83AACE=1" sh go_race_nointro.sh 0 2>&1 | tail -1 || exit 1
echo "$(ts) racing"
python -I -u autorace.py --stagger 1 2>&1 | tail -1 || exit 1
python -I -u - "$LOG" <<'PY' || exit 1
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game(); log = sys.argv[1]
act = lambda: [p[0] for p in fe.packages(g) if p[1]]
say = lambda m: print(time.strftime('%H:%M:%S'), m, flush=True)
def wait(pred, tmo, what):
    end = time.time() + tmo
    while time.time() < end:
        if pred(): return
        time.sleep(0.05)
    sys.exit(f'FAIL: {what} {act()}')
QR = 0x83AAC4
FIELDS = {'difficulty': 2, 'traffic': 4, 'catch-up': 5}   # quick race settings bytes both circuit and sprint offer
opts = lambda ty: {n: g.u8(QR + ty * 0x14 + o) for n, o in FIELDS.items()}
wait(lambda: 'UI_PostRaceResults.fng' in act(), 40, 'no results'); time.sleep(2.5)
fe.press(g, 1, 'a', 0.3)
wait(lambda: 'UI_PostRace_TournStandings.fng' in act(), 6, 'no standings'); time.sleep(1.2)
fe.press(g, 1, 'a', 0.3)
wait(lambda: 'UI_postrace.fng' in act(), 6, 'no Post Race'); time.sleep(1.2)
circuit = opts(0)
other = {'difficulty': (circuit['difficulty'] + 1) % 3, 'traffic': circuit['traffic'] ^ 1, 'catch-up': circuit['catch-up'] ^ 1}
for n, o in FIELDS.items(): g.write(QR + 1 * 0x14 + o, bytes([other[n]]))
say(f'circuit options {circuit}; sprint set to {opts(1)}')
start = len(open(log, errors='replace').read())
fe.press(g, 1, 'a', 0.1)                                  # Next Race
wait(lambda: 'UI_QRTrackSelect.fng' in act(), 30, 'no track select after Next Race'); time.sleep(1.0)
fe.press(g, 1, 'b', 0.1)
wait(lambda: 'UI_QRModeSelect.fng' in act(), 6, 'Back did not reach the mode select'); time.sleep(1.0)
say('back on the mode select: Sprint')
fe.press(g, 1, 'right', 0.1); time.sleep(0.5)
fe.press(g, 1, 'a', 0.1)
wait(lambda: 'UI_QRTrackSelect.fng' in act(), 8, 'no track select for Sprint'); time.sleep(1.0)
if g.u32(0x83AAB4) != 1: sys.exit(f'FAIL: race type {g.u32(0x83AAB4)}, not Sprint')
fe.vpad(g, 2, connected=False)
say("P2's pad disconnected; a track")
try:
    fe.press(g, 1, 'a', 0.1)
    wait(lambda: '2P_PressStart.fng' in act(), 8, "no player 1 popup")
    time.sleep(0.5)
    text = open(log, errors='replace').read()[start:]
    for l in text.splitlines():
        if 'next race' in l: print('  ' + l)
    if "can't start again" not in text: sys.exit('FAIL: the last round was taken although P2 had no pad')
    sprint = opts(1)
    say(f'sprint options now {sprint}')
    if sprint != circuit: sys.exit(f'FAIL: sprint {sprint} != circuit {circuit}')
    say('ok: joined as usual, the circuit options carried to Sprint')
finally:
    fe.vpad(g, 2)
PY
