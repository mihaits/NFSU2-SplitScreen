#!/bin/sh
# restart and start a split race: mode $1 (right presses in the mode list), then $2 right presses in the track list
cd "$(dirname "$0")"
sh restart.sh --noskip
python -I boot.py 60 || exit 1
python -I rec.py play start_to_split2p --from 3 --to 5 >/dev/null || exit 1
python -I - "${1:-0}" "${2:-0}" <<'PY'
import sys, time, subprocess
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game()
for _ in range(int(sys.argv[1])): fe.press(g, 1, 'right'); time.sleep(0.35)
time.sleep(0.4); fe.press(g, 1, 'a'); time.sleep(1.5)
for _ in range(int(sys.argv[2])): fe.press(g, 1, 'right'); time.sleep(0.4)
subprocess.run([sys.executable, '-I', 'rec.py', 'play', 'start_to_split2p', '--from', '6', '--to', '10'], stdout=subprocess.DEVNULL)
def wait_top(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        top = fe.top(g) or ''
        if pred(top): return top
        time.sleep(0.1)
    return fe.top(g) or ''
if 'dialog' in wait_top(lambda s: 'dialog' in s.lower() or s == '2P_PressStart.fng', 1.5).lower(): fe.press(g, 1, 'a')
for pad in (2, 3, 0):
    if wait_top(lambda s: s == '2P_PressStart.fng', 3.0) != '2P_PressStart.fng': break
    fe.press(g, pad, 'start'); wait_top(lambda s: s != '2P_PressStart.fng', 1.5); time.sleep(0.3)
    fe.press(g, pad, 'a')
    if 'dialog' in wait_top(lambda s: 'dialog' in s.lower() or s == '2P_PressStart.fng', 1.0).lower(): fe.press(g, pad, 'a')
print('track', g.u32(0x83AAC4 + g.u32(0x83AAB4) * 0x14 + 8) & 0xffff)
PY
