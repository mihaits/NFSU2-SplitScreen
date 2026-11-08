#!/bin/sh
# Random series with Outrun: restart with [DEBUG] RollMode = 7 (mode rolls pick Outrun; restored at the end), go to
# the split mode select, roll (Y): the Outrun options must come up; accept, join 2 players on
# virtual pads, P1 wins the race on autopilot, step the post-race screens to Post Race, Next Race: the series
# must roll Outrun again, accept its options by itself and load with the same players and cars, no input. A line per
# stage, screenshots in $TEMP/nfsu2_outrun/series_*.png. Stops at the first failure. Keep the game window focused.
cd "$(dirname "$0")"
INI=../../../scripts/NFSU2SplitScreen.ini
LOG=../../../scripts/NFSU2SplitScreen.log
grep -q "^RollMode" "$INI" || sed -i "s/^CarsMode = \(.*\)/CarsMode = \1\nRollMode = 0/" "$INI"
OLD=$(sed -n 's/^RollMode = //p' "$INI" | tr -d '\r'); OLD_EXTRA=$(sed -n 's/^ExtraPlayers = //p' "$INI" | tr -d '\r')
trap 'sed -i "s/^RollMode = .*/RollMode = $OLD/; s/^ExtraPlayers = .*/ExtraPlayers = $OLD_EXTRA/" "$INI"' EXIT
sed -i "s/^RollMode = .*/RollMode = 7/" "$INI"
sed -i "s/^ExtraPlayers = .*/ExtraPlayers = 0/" "$INI"
say() { echo "$(date +%T) $*"; }
say "boot"
sh restart.sh --noskip || { say "FAIL: restart"; exit 1; }
python -I boot.py 20 || exit 1
python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;g=t.Game();g.write(0x42FDA0,bytes([0x32,0xC0,0xC2,0x04,0x00]),code=True)"
python -I rec.py play start_to_split2p --from 3 --to 5 >/dev/null || { say "FAIL: no split mode select"; exit 1; }
python -I -u - "$TEMP/nfsu2_outrun" "$LOG" <<'PY' || exit 1
import sys, time, re
sys.path.insert(0, '.')
import trainer as t, fe, subprocess
g = t.Game(); g.focus()
shots, log = sys.argv[1], sys.argv[2]
def say(s): print(time.strftime('%X'), s, flush=True)
def fail(s): say('FAIL: ' + s); sys.exit(1)
act = lambda: [p[0] for p in fe.packages(g) if p[1] and p[0] not in fe.HELPERS and 'HUD' not in p[0].upper()]
def wait(pred, tmo):
    end = time.time() + tmo
    while time.time() < end:
        if pred(act()): return True
        time.sleep(0.05)
    return False
def shot(name):
    p = r'%s\series_%s.png' % (shots, name); g.screenshot(p, 960); return p
def logtail(start): return open(log, errors='replace').read()[start:]
if not wait(lambda a: 'UI_QRModeSelect.fng' in a, 5): fail('no mode select %s' % act())
time.sleep(0.5); fe.press(g, 1, 'y')
if not wait(lambda a: 'UI_QRModeOptions.fng' in a, 5): fail('no Outrun options after the roll %s' % act())
time.sleep(1)
if g.u32(0x83AAB4) != 8: fail('type %d' % g.u32(0x83AAB4))
say('rolled Outrun: ' + shot('options'))
fe.press(g, 1, 'a')
for i, pad in enumerate((1, 2)):
    if not wait(lambda a: '2P_PressStart.fng' in a, 4): fail('no Press START for P%d %s' % (i + 1, act()))
    fe.press(g, pad, 'start'); wait(lambda a: '2P_PressStart.fng' not in a, 1.5); time.sleep(0.25)
    fe.press(g, pad, 'a')
    if wait(lambda a: any('dialog' in x.lower() for x in a), 1.5): fe.press(g, pad, 'a')
    say('P%d joined' % (i + 1))
for rnd in (1, 2):
    r = subprocess.run([sys.executable, '-I', 'autorace.py', '--players', '1', '--timeout', '100', '--stall', '100',
                        '--wait-race', '30'], capture_output=True, text=True)
    if 'FAIL' in r.stdout or r.returncode: fail('round %d: %s' % (rnd, r.stdout.strip().splitlines()[-1:]))
    say('round %d: P1 won' % rnd)
    last, pressed, end = None, 0, time.time() + 30
    while time.time() < end:
        a = act()
        if 'ui_postrace.fng' in [x.lower() for x in a]: break
        if a and (a != last or time.time() - pressed > 3):
            time.sleep(0.8); say('%s: %s' % (' '.join(a), shot('r%d_%d' % (rnd, int(time.time()) % 1000))))
            fe.press(g, 1, 'a'); pressed = time.time(); last = a
        time.sleep(0.1)
    else: fail('no Post Race %s' % act())
    time.sleep(1); say('Post Race: ' + shot('post%d' % rnd))
    if rnd == 2: break
    start = len(open(log, errors='replace').read())
    fe.press(g, 1, 'a', 0.3)                          # Next Race (the first option)
    seen = set(); end = time.time() + 30
    while time.time() < end:
        seen |= set(act())
        m = g.u32(0x890118)
        if g.u32(0x8654A4) == 6 and m and g.u32(0x89E7B0) == 7: break
        time.sleep(0.05)
    else: fail('no Outrun round after Next Race %s' % act())
    text = logtail(start)
    print(''.join('  ' + l + '\n' for l in text.splitlines() if re.search(r'random|next race|outrun: (grid|new)', l)), end='')
    if 'Outrun options accepted' not in text: fail('the Outrun options were not skipped')
    if 'UI_QRCarSelect.fng' in seen and 'next race: same cars' not in text: fail('the car select wanted picks')
    say('series: Outrun again, same players, no input')
say('ok')
PY
grep -a -E "outrun: (round|tournament)|session: race" "$LOG" | tail -6
