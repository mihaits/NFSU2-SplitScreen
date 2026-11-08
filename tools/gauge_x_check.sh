#!/bin/sh
# 1P gauge copies' x restore (gauges.cpp SaveLayoutX / RestoreLayoutX), with WidescreenFix FixHUD on and Debug = 1:
# starts a split drag race (players: the [DEBUG] ExtraPlayers default) and takes a heat bar object from the log line
# "1P gauges: object data P x -254.0 moved to -363.0 during the update, restored". Checks that WidescreenFix's move is
# undone every frame (x stays -254), then sets x to -200 from outside the update: it must stay -200 (only the update's
# own move is undone), and puts -254 back. A line per stage; stops at the first failure. The game window must stay
# focused.
cd "$(dirname "$0")"
set -o pipefail   # a stage piped through tail still fails the run
LOG=../../../scripts/NFSU2SplitScreen.log
echo "$(date +%T) boot, drag race"
sh go_race_nointro.sh 2 2>&1 | tail -1 || exit 1
python -I -u - "$LOG" <<'PY'
import re, struct, sys, time
sys.path.insert(0, '.')
import trainer as t
g = t.Game(); log = sys.argv[1]
say = lambda m: print(time.strftime('%H:%M:%S'), m, flush=True)
def fail(m): say('FAIL: ' + m); sys.exit(1)
x = lambda a: struct.unpack('<f', g.read(a + 0x1C, 4))[0]
end = time.time() + 30
while time.time() < end:
    m = re.findall(r'object data ([0-9A-Fa-f]+) x (-?[\d.]+) moved to (-?[\d.]+)', open(log, errors='replace').read())
    if m: break
    time.sleep(0.2)
else: fail('no "1P gauges: object data ... moved" line in 30 s (WidescreenFix FixHUD off? Debug = 0?)')
data, before, after = int(m[0][0], 16), float(m[0][1]), float(m[0][2])
say(f'object data {data:08X}: x {before} moved to {after} during the update')
xs = []
for _ in range(20): xs.append(x(data)); time.sleep(0.05)
if any(v != before for v in xs): fail(f'x not kept at {before} between frames: {sorted(set(xs))}')
say(f'x stays {before} over 1 s (the update\'s move undone)')
g.write(data + 0x1C, struct.pack('<f', -200.0))
xs = []
for _ in range(20): xs.append(x(data)); time.sleep(0.05)
g.write(data + 0x1C, struct.pack('<f', before))
if any(v != -200.0 for v in xs): fail(f'x set to -200 outside the update was changed: {sorted(set(xs))}')
say(f'x set to -200 outside the update stays -200 over 1 s; {before} put back')
say('ok')
PY
