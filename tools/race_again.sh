#!/bin/sh
# From the quick race mode select (e.g. after quit_race.py): start the same split race again.
cd "$(dirname "$0")"
python -I rec.py play start_to_split2p --from 5 --to 10 >/dev/null || exit 1
python -I - <<'PY'
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
if 'dialog' in wait_top(lambda s: 'dialog' in s.lower() or s == '2P_PressStart.fng', 1.5).lower():
    fe.press(g, 1, 'a')
for pad in (2, 3):
    if wait_top(lambda s: s == '2P_PressStart.fng', 1.5) != '2P_PressStart.fng': break
    fe.press(g, pad, 'start')
    wait_top(lambda s: s != '2P_PressStart.fng', 1.5)
    time.sleep(0.3)
    fe.press(g, pad, 'a')
    if 'dialog' in wait_top(lambda s: 'dialog' in s.lower() or s == '2P_PressStart.fng', 1.0).lower():
        fe.press(g, pad, 'a')
PY
