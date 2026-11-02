"""Checks the start grid of a split race: waits for the countdown, prints every racer's slot and position, fails when two
cars stand closer than 3 m. python -I grid_check.py [timeout_s=20]   Read-only."""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t

g = t.Game()
end = time.time() + (float(sys.argv[1]) if len(sys.argv) > 1 else 20)
while True:
    mgr = g.u32(0x890118)
    if mgr and g.u32(0x8654A4) == 6 and g.u32(mgr) >= 3: break
    if time.time() > end: sys.exit('FAIL: no countdown')
    time.sleep(0.05)
time.sleep(0.3)                        # the frame the scene lets go places the cars
world = g.u32(0x890080); pos = []
for i in range(g.u32(g.u32(0x890118) + 0x24)):
    car = g.u32(world + 0x1C + 4 * i); spec = g.u32(car + 0x14)
    x, y = struct.unpack('<2f', g.read(g.u32(g.u32(car + 0x34) + 0x20) + 0x20, 8))
    pos.append((x, y))
    print(f'  c{i} {"human" if g.u32(spec + 4) == 1 else "ai   "} slot {g.u8(spec + 3)} at {x:.2f} {y:.2f} '
          f'mode {g.u32(car + 0x28)}', flush=True)
gap = min(((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5 for i, a in enumerate(pos) for b in pos[:i])
print(f'  closest pair {gap:.2f} m', flush=True)
if gap < 3: sys.exit('FAIL: cars overlap')
