"""Get from a fresh launch to the main menu (UI_Main) as fast as possible: press A on the title/splash, the profile
list and the load confirmation as soon as each is up. python -I boot.py [timeout]"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, fe
deadline = time.time() + (float(sys.argv[1]) if len(sys.argv) > 1 else 60)
g = None
while time.time() < deadline:
    try: g = t.Game(); break
    except Exception: time.sleep(0.2)
if not g: sys.exit('game not running')
focused = False
last = 0
while time.time() < deadline:
    try:
        if not focused: g.focus(); focused = True
        s = {n.lower() for n, o, a in fe.packages(g) if a}
    except Exception:
        time.sleep(0.2); continue
    if 'ui_main.fng' in s:
        print('main menu after %.1f s' % (time.time() - deadline + (float(sys.argv[1]) if len(sys.argv) > 1 else 60)))
        sys.exit(0)
    if s and time.time() - last > 0.5:   # title, splash, profile list / confirmation: all advance with A
        try: fe.press(g, 1, 'a')
        except TimeoutError: focused = False   # no frames: still loading, or the window lost focus (focus again)
        last = time.time()
    time.sleep(0.1)
sys.exit('timeout; screens: %s' % sorted(s))
