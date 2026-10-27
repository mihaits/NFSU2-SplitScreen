"""Live car telemetry of every local player (the values the DualSense lights and triggers use), printed every interval.
python -I telemetry.py [interval_s=0.25] [seconds=0 (forever)]
Per player: race stage, engine rpm (what the tach shows), shift-light rpm, gear, nitrous burning, grip used per wheel
(FL FR RL RR, 1.0 = at the limit), wheels in the air (grip used 0), position, finished; ground speed (body velocity),
wheel angular speed, wheel radius (front/rear) and surface speed / ground speed. Needs the plugin loaded (its local
player array); reads only."""
import ctypes, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, plugin

g = t.Game()
players = plugin.LocalPlayers(g)
f32 = lambda a: struct.unpack('<f', g.read(a, 4))[0]
class _XS(ctypes.Structure):
    _fields_ = [('packet', ctypes.c_uint32), ('buttons', ctypes.c_uint16), ('lt', ctypes.c_uint8), ('rt', ctypes.c_uint8),
                ('sticks', ctypes.c_int16 * 4)]
_xinput = ctypes.windll.xinput1_4
def pedals():                                                      # first connected XInput pad: brake / throttle 0..255
    for u in range(4):
        st = _XS()
        if _xinput.XInputGetState(u, ctypes.byref(st)) == 0: return st.lt, st.rt
    return 0, 0
STAGES = {0: 'none', 1: 'flyby', 2: 'prerace', 3: 'countdown', 4: 'racing'}

def player_line(k, p):
    car = g.u32(p + 4); spec = g.u32(car + 0x14); r968 = g.u32(car + 0x968)
    drag = g.u8(0x89E7D8) or g.u32(0x890108); drift = g.u8(0x89E7D9) or g.u32(0x890104)
    use968 = g.u32(spec + 4) == 1 and not g.u8(spec + 0x900) and r968 and not drag and not drift
    rpm = f32(r968 + 0x58) if use968 else f32(car + 0x440)
    gear = g.i32(car + 0x434)
    shift = None
    if g.u32(car + 0x28) == 1:                                     # 0x5ECD30 / 0x594F00 for the player's drivetrain
        drv = g.u32(g.u32(car + 0x34) + 0x4C)
        if 0 <= gear < 9:
            f = f32(0x7FBF60 + 4 * (gear + 9 * g.u32(drv + 0xF0))) * f32(drv + 0xE4)
            shift = f32(drv + 0x84 + 4 * gear) - f32(0x7FBF5C) * f
    burn = g.u32(car + 0x40 + 0x420); nos = burn not in (0, 0x7FFFFFFF)
    mover = g.u32(car + 0x34); wheels = [g.u32(mover + 0x38 + 4 * i) for i in range(4)]
    grip = '/'.join(f'{f32(w + 0x170):.2f}' for w in wheels)
    air = ''.join('A' if f32(w + 0x170) < 0.005 else '-' for w in wheels)     # no load: grip used reads 0
    body = g.u32(mover + 0x20); vel = struct.unpack('<3f', g.read(body + 0xA0, 12)) if body else (0, 0, 0)
    ground = (vel[0] ** 2 + vel[1] ** 2 + vel[2] ** 2) ** 0.5
    ws = [f32(w + 0x28) for w in wheels]                           # angular speed
    rad = [f32(g.u32(w + 0x14) + 0x10) for w in wheels]            # radius from the tyre data
    slip = ' '.join(f'{x:6.1f}' for x in ws)
    racer = g.u32(car + 0x1C)
    pos = g.u8(racer + 0xA) if racer else 0; done = g.u32(p + 0x50) != 0      # racer +0x14 counts laps
    return (f'P{k + 1} rpm {rpm:6.0f} shift {shift or 0:6.0f} gear {gear} nos {"ON " if nos else "off"} '
            f'grip {grip} air {air} pos {pos}{" FIN" if done else ""}\n'
            f'     pedals {pedals()[0]:3d}/{pedals()[1]:3d} ground {ground:5.1f} wheel w {slip} '
            f'radius {rad[0]:.4f}/{rad[2]:.4f} surface/ground '
            f'{"/".join(f"{x * r / ground:.2f}" if ground > 1 else "-" for x, r in zip(ws, rad))}')

interval = float(sys.argv[1]) if len(sys.argv) > 1 else 0.25
end = time.time() + float(sys.argv[2]) if len(sys.argv) > 2 and float(sys.argv[2]) > 0 else float('inf')
while time.time() < end:
    try:
        mgr = g.u32(0x890118); stage = g.u32(mgr) if mgr else -1
        lines = [player_line(k, p) for k, p in enumerate(players())] if g.u32(0x8654A4) == 6 else []
        print(f'{time.strftime("%H:%M:%S")} state {g.u32(0x8654A4)} stage {STAGES.get(stage, stage)} clock '
              f'{f32(0x7FB718):.2f}' + ''.join('\n  ' + s for s in lines), flush=True)
    except OSError as e:
        print('read failed:', e, flush=True)
    time.sleep(interval)
