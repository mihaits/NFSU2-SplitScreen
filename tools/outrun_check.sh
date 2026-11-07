#!/bin/sh
# Split Outrun check for n players (sh outrun_check.sh [n ...], default 2 3 6): restart, start an n-player Outrun, 2
# races. Each race P1 drives off on autopilot (the others drop out 300 m away, straight distance), then the post-race
# screens are stepped through with a screenshot each (Event Results, standings) to Post Race; after the first, Next
# Race must load the second with the same players and no input. Prints the logged grid ("random", then
# "last race reversed" with P1 last) and session points. Stops at the first failure. The ini's ExtraPlayers is
# restored. The game window must stay focused.
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
    for race in 1 2; do
        # the gap is the straight distance: an autopilot that turns back toward the others can take a while
        python -I -u autorace.py --players 1 --timeout 100 --stall 100 --wait-race 25 > "$SHOTS/race.txt" 2>&1 &
        tail -f "$SHOTS/race.txt" --pid=$! | grep -E --line-buffered 'autopilot on|finished|FAIL|Traceback'
        grep -q -E 'FAIL|Traceback' "$SHOTS/race.txt" && { say "FAIL: ${n}P race $race"; exit 1; }
        python -I -u - "$SHOTS" "${n}p_r$race" "$race" <<'PY' || exit 1
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game(); g.focus()
shots, tag, race = sys.argv[1], sys.argv[2], int(sys.argv[3])
def say(s): print(time.strftime('%X'), s, flush=True)
def fail(s): say('FAIL: ' + s); sys.exit(1)
act = lambda: [p[0] for p in fe.packages(g) if p[1] and p[0] not in fe.HELPERS and 'HUD' not in p[0].upper()]
def wait(pred, tmo):
    end = time.time() + tmo
    while time.time() < end:
        if pred(act()): return True
        time.sleep(0.05)
    return False
shot, last, pressed, end = 0, None, 0.0, time.time() + 30
while time.time() < end:
    a = act()
    if 'ui_postrace.fng' in [x.lower() for x in a]: break
    if a and (a != last or time.time() - pressed > 3):   # the same screen twice in a row: press again
        time.sleep(0.8); path = r'%s\%s_%d.png' % (shots, tag, shot); shot += 1
        g.screenshot(path, 960); say('%s: %s' % (' '.join(a), path))
        fe.press(g, 1, 'a'); pressed = time.time(); last = a
    time.sleep(0.1)
else: fail('no Post Race %s' % act())
time.sleep(1); path = r'%s\%s_post.png' % (shots, tag); g.screenshot(path, 960); say('Post Race: ' + path)
if race == 2: sys.exit()
m = g.u32(0x890118); before = g.i32(m + 0x14) if m else 1 << 30
seen = set()
fe.press(g, 1, 'a', 0.3)                                  # Next Race: straight into the next Outrun
end = time.time() + 20
while time.time() < end:                                  # a new race: the race time starts over
    seen |= set(act())
    m = g.u32(0x890118)
    if m and g.u32(0x89E7B0) == 7 and 0 <= g.i32(m + 0x14) < before: break
    time.sleep(0.05)
else: fail('no Outrun race after Next Race %s' % act())
say('Next Race: next Outrun loaded with no input')
PY
        grep -a -E "outrun: grid|session: (race|standings)|Outrun options accepted" "$LOG" | tail -n $((n + 3))
    done
    say "== ${n}P: done"
done
say "all done: $*"
