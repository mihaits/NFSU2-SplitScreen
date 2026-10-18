#!/bin/sh
# restart the game and start a single-player quick race in mode $1 (right presses in the mode list), no intro
cd "$(dirname "$0")"
sh restart.sh --noskip
python -I boot.py 60 || exit 1
python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;g=t.Game();g.write(0x42FDA0,bytes([0x32,0xC0,0xC2,0x04,0x00]),code=True)"
python -I rec.py play start_to_split2p --from 3 --to 3 >/dev/null || exit 1
python -I - "${1:-0}" <<'PY'
import sys, time
sys.path.insert(0, '.')
import trainer as t, fe
g = t.Game()
def wait_top(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        top = fe.top(g) or ''
        if pred(top): return top
        time.sleep(0.1)
    return fe.top(g) or ''
wait_top(lambda s: s == 'UI_Main.fng', 10); time.sleep(0.3)
fe.press(g, 1, 'a')
wait_top(lambda s: s == 'UI_QRModeSelect.fng', 5); time.sleep(0.3)
for _ in range(int(sys.argv[1])): fe.press(g, 1, 'right'); time.sleep(0.3)
for want in ('UI_QRModeSelect.fng', 'UI_QRTrackSelect.fng', 'UI_QRModeOptions.fng', 'UI_QRCarSelect.fng'):
    wait_top(lambda s: s == want, 5); time.sleep(0.4); fe.press(g, 1, 'a')
if 'dialog' in wait_top(lambda s: 'dialog' in s.lower(), 1.5).lower(): fe.press(g, 1, 'a')
print('top', fe.top(g))
PY
