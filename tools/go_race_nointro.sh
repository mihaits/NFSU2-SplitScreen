#!/bin/sh
# Restart the game and start a split race in quick race mode $1 (number of right presses in the mode list: 0 circuit,
# 2 drag, 3 drift, 4 Street X ...), skipping the race intro (2P intro filter patched to reject every scene).
# Players: the [DEBUG] ExtraPlayers default of the Players row (0 -> 2 players). P2..P4 join on virtual pads 2..3.
# POKES="addr=value ..." (hex address, decimal value) are written right after boot, e.g. POKES="870CFC=1" (overbright).
cd "$(dirname "$0")"
sh restart.sh --noskip
python -I boot.py 60 || exit 1
python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;g=t.Game();g.write(0x42FDA0,bytes([0x32,0xC0,0xC2,0x04,0x00]),code=True)"
python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;g=t.Game();[g.w32(int(a,16),int(v)) for a,v in (x.split('=') for x in sys.argv[1:])]" $POKES
python -I rec.py play start_to_split2p --from 3 --to 5 >/dev/null || exit 1
sleep 1
python -I - "${1:-0}" <<'PY'
import sys, time, os, subprocess
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game()
for _ in range(int(sys.argv[1])): fe.press(g, 1, 'right'); time.sleep(0.35)
# P1 picks the car (events 5..9); the gearbox dialog (event 10) only comes up in some modes (not drag): polled below
subprocess.run([sys.executable, '-I', 'rec.py', 'play', 'start_to_split2p', '--from', '5', '--to', '10'], stdout=subprocess.DEVNULL)
def wait_top(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        top = fe.top(g) or ''
        if pred(top): return top
        time.sleep(0.1)
    return fe.top(g) or ''
if 'dialog' in wait_top(lambda s: 'dialog' in s.lower() or s == '2P_PressStart.fng', 1.5).lower():
    fe.press(g, 1, 'a')
for pad in (2, 3):
    if wait_top(lambda s: s == '2P_PressStart.fng', 1.5) != '2P_PressStart.fng': break
    fe.press(g, pad, 'start')
    wait_top(lambda s: s != '2P_PressStart.fng', 1.5)
    time.sleep(0.3)
    for _ in range(int(os.environ.get('CAR_RIGHT', '0'))): fe.press(g, pad, 'right'); time.sleep(0.5)
    fe.press(g, pad, 'a')
    if 'dialog' in wait_top(lambda s: 'dialog' in s.lower() or s == '2P_PressStart.fng', 1.0).lower():
        fe.press(g, pad, 'a')
PY
