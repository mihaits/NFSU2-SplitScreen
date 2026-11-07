#!/bin/sh
# Split Outrun check for n players (sh outrun_check.sh [n ...], default 2 3 6): restart, start an n-player Outrun,
# 2 rounds. Each round P1 drives off on autopilot (the others drop out 300 m away, straight distance), then the post-race screens are
# stepped through with a screenshot each (Event Results, standings) until the next round loads. Checks the log for the
# grid ("random", then "last round reversed" with P1 last) and the session points. Stops at the first failure.
# POINTS=i sets the options' Number of Points to Win (0x83ABBC: 0..5 = 10 / 20 / 30 / 40 / 50 / 100, 6 = endless,
# the default here) after the start: each round then reports whether the tournament went on or ended (the options
# screen). The ini's ExtraPlayers is restored. The game window must stay focused.
cd "$(dirname "$0")"
INI=../../../scripts/NFSU2SplitScreen.ini
LOG=../../../scripts/NFSU2SplitScreen.log
SHOTS=${SHOTS:-$TEMP/nfsu2_outrun}
mkdir -p "$SHOTS"
OLD_EXTRA=$(sed -n 's/^ExtraPlayers = //p' "$INI" | tr -d '\r')
trap 'sed -i "s/^ExtraPlayers = .*/ExtraPlayers = $OLD_EXTRA/" "$INI"' EXIT
say() { echo "$(date +%T) $*"; }
[ $# -gt 0 ] || set -- 2 3 6

for n in "$@"; do
    sed -i "s/^ExtraPlayers = .*/ExtraPlayers = $((n - 2))/" "$INI"
    say "== ${n}P: start"
    out=$(PLAYERS=$n sh go_outrun.sh 2>&1); echo "$out" | grep -E 'main menu|loading|FAIL'
    echo "$out" | grep -q 'loading' && ! echo "$out" | grep -q FAIL || { say "FAIL: ${n}P didn't start"; exit 1; }
    python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;t.Game().w32(0x83ABBC, ${POINTS:-6})"
    for round in 1 2; do
        # the gap is the straight distance: an autopilot that turns back toward the others can take a while
        python -I -u autorace.py --players 1 --timeout 100 --stall 100 --wait-race 25 > "$SHOTS/race.txt" 2>&1 &
        tail -f "$SHOTS/race.txt" --pid=$! | grep -E --line-buffered 'autopilot on|finished|FAIL|Traceback'
        grep -q -E 'FAIL|Traceback' "$SHOTS/race.txt" && { say "FAIL: ${n}P round $round"; exit 1; }
        python -I -u - "$SHOTS" "${n}p_r$round" <<'PY' || exit 1
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game(); g.focus()
shots, tag = sys.argv[1], sys.argv[2]
def say(s): print(time.strftime('%X'), s, flush=True)
def wait(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        top = fe.top(g) or ''
        if pred(top): return top
        time.sleep(0.05)
    return fe.top(g) or ''
def active(): return [n for n, o, on in fe.packages(g) if on and n not in fe.HELPERS]
def race_time():
    mgr = g.u32(0x890118)
    return g.i32(mgr + 0x14) if mgr else -1
# the round's HUD packages stay listed under the post-race screens: a new round shows as the race time starting over
last, shot, end = race_time(), 0, time.time() + 25
seen = None
while time.time() < end:
    now = race_time()
    if 0 <= now < last: say('next round: race time restarted'); sys.exit()
    if any('QRModeOptions' in n for n in active()): say('tournament over: options screen'); sys.exit()
    last = max(last, now)
    names = [n for n in active() if 'HUD' not in n.upper()]
    if names and names != seen:
        time.sleep(0.8)
        path = r'%s\%s_%d.png' % (shots, tag, shot); shot += 1
        g.screenshot(path, 960); say('%s: %s' % (' '.join(names), path)); seen = names
        fe.press(g, 1, 'a')
    time.sleep(0.1)
sys.exit('FAIL: no next round within 25 s (%s)' % active())
PY
        grep -a -E "outrun: grid|session: (race|standings)" "$LOG" | tail -n $((n + 2))
    done
    say "== ${n}P: done"
done
say "all done: $*"
