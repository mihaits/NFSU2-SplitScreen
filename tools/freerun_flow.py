"""From the split-screen mode select to a loading Free Run: Free Run (the last mode), its options screen, then each
Press START joined on a virtual pad (P1 = pad 1, P2..P6 = pads 2, 3, 0, 4, 5) until the loading screen. Every step is
a short polled wait and prints a timestamped line; exits 1 on the first screen that doesn't come.
python -I freerun_flow.py        VIRTUAL=n: join only players 1..n and stop at the next Press START (real pads)"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, fe

g = t.Game()


def log(s): print(time.strftime('%X'), s, flush=True)


def fail(s):
    log('FAIL: %s (screen %s)' % (s, fe.top(g)))
    sys.exit(1)


def wait_top(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        top = fe.top(g) or ''
        if pred(top): return top
        time.sleep(0.05)
    return fe.top(g) or ''


g.focus()
if 'ModeSelect' not in wait_top(lambda s: 'ModeSelect' in s, 3): fail('no mode select')
for _ in range(10): fe.press(g, 1, 'right'); time.sleep(0.12)   # the list stops at its last mode, Free Run
fe.press(g, 1, 'a')
if 'Options' not in wait_top(lambda s: 'Options' in s, 3): fail('no Free Run options screen')
log('options')
time.sleep(0.3); fe.press(g, 1, 'a')
virtual = int(os.environ.get('VIRTUAL', '6'))
for i, pad in enumerate((1, 2, 3, 0, 4, 5)):
    top = wait_top(lambda s: s == '2P_PressStart.fng' or 'Loading' in s, 3)
    if 'Loading' in top: break
    if top != '2P_PressStart.fng': fail('no Press START for P%d' % (i + 1))
    if i >= virtual:
        log('players 1..%d joined; P%d.. press START on real pads' % (virtual, virtual + 1))
        sys.exit(0)
    fe.press(g, pad, 'start'); wait_top(lambda s: s != '2P_PressStart.fng', 1.5); time.sleep(0.25)
    fe.press(g, pad, 'a')
    if 'dialog' in wait_top(lambda s: 'dialog' in s.lower() or s == '2P_PressStart.fng' or 'Loading' in s, 1.5).lower():
        fe.press(g, pad, 'a')
    log('P%d joined' % (i + 1))
if 'Loading' not in wait_top(lambda s: 'Loading' in s, 4): fail('no loading screen')
log('loading')
