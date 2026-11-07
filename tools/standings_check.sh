#!/bin/sh
# Session standings check: N split circuit races (1 lap, every car on autopilot, released 1 s apart so the places
# differ) joined by Next Race. After each race's Event Results the tournament standings screen must come up with the
# session totals (log "session: race", "standings ... shown"), then Post Race. Screenshots: build/nr/standings_<r>.png.
# Stops at the first failure. The game window must stay focused.
# sh standings_check.sh [races=2]
cd "$(dirname "$0")"
LOG=../../../scripts/NFSU2SplitScreen.log
ts() { date +%H:%M:%S; }
mkdir -p ../build/nr
echo "$(ts) boot circuit 4011 (shortest), 1 lap"
TRACK=4011 POKES="83AACE=1" sh go_race_nointro.sh 0 2>&1 | tail -1
for r in $(seq 1 "${1:-2}"); do
    echo "$(ts) == race $r: racing"
    python -I -u autorace.py --stagger 1 2>&1 | tail -1
    python -I -u - "$r" "${1:-2}" "$LOG" <<'PY' || exit 1
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game(); r, races, log = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
act = lambda: [p[0] for p in fe.packages(g) if p[1]]
say = lambda m: print(time.strftime('%H:%M:%S'), m, flush=True)
def wait(pred, tmo, what):
    end = time.time() + tmo
    while time.time() < end:
        if pred(): return
        time.sleep(0.05)
    sys.exit(f'FAIL: {what} {act()}')
wait(lambda: 'UI_PostRaceResults.fng' in act(), 40, 'no results'); time.sleep(2.5)
fe.press(g, 1, 'a', 0.3)
wait(lambda: 'UI_PostRace_TournStandings.fng' in act(), 6, 'no standings after the results')
time.sleep(1.5); g.screenshot(rf'../build/nr/standings_{r}.png', max_w=1280); say(f'standings: build/nr/standings_{r}.png')
text = open(log, errors='replace').read()
lines = [l for l in text.splitlines() if 'session:' in l]
for l in lines[-5:]: print('  ' + l)
if f'session: race {r},' not in text: sys.exit(f'FAIL: race {r} not scored')
fe.press(g, 1, 'a', 0.3)
wait(lambda: 'UI_postrace.fng' in act(), 6, 'no Post Race after the standings'); time.sleep(1.2)
say('post race')
if r < races:
    fe.press(g, 1, 'a', 0.1)
    wait(lambda: 'UI_QRTrackSelect.fng' in act(), 30, 'no track select'); time.sleep(1.2)
    fe.press(g, 1, 'a', 0.1)
    wait(lambda: g.u32(0x8654A4) == 6 and g.u32(0x890118), 25, 'no race')
    say('next race loading')
PY
done
echo "$(ts) all races passed"
