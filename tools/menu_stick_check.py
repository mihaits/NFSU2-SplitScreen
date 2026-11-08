"""Menu up/down follows the steering stick, not the triggers (GitHub #4): a check on the Quick Race options list.
python -I menu_stick_check.py [pad n] [--keyboard]

From the main menu (after restart.sh), opens Quick Race > Circuit > first track > options with virtual pad n
(fe.py numbering, default 1 = P1), then finds the selected row by pressing the stick right and left (the row whose
setting byte in the Quick Race settings 0x83AAC4 changes). Each "move" (LT, RT, stick down/up, D-pad down/up) is then
followed by that probe: the triggers must leave the row alone, the stick and the D-pad must move it. A line per step;
exits 1 on the first failure. The game window must stay focused.
--keyboard: the same with the arrow keys (probe right/left, moves down/up), after the pad opened the screen.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, fe, subprocess

QR = 0x83AAC4                  # quick race settings, 0x14 bytes per mode; circuit = 0

def say(m): print(time.strftime('%H:%M:%S'), m, flush=True)

def main():
    args = sys.argv[1:]
    pad = int(args[args.index('pad') + 1]) if 'pad' in args else 1
    g = t.Game()

    def wait_top(want, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if fe.top(g) == want: return True
            time.sleep(0.05)
        sys.exit(f'FAIL: no {want} (top {fe.top(g)})')

    if fe.top(g) != 'UI_QRModeOptions.fng':
        subprocess.run([sys.executable, '-I', 'rec.py', 'play', 'start_to_split2p', '--from', '3', '--to', '3'],
                       cwd=os.path.dirname(os.path.abspath(__file__)), stdout=subprocess.DEVNULL, check=True)
        wait_top('UI_Main.fng', 5); time.sleep(0.3)
        for here in ('UI_Main.fng', 'UI_QRModeSelect.fng', 'UI_QRTrackSelect.fng'):
            wait_top(here, 5); time.sleep(0.4); fe.press(g, pad, 'a')
        wait_top('UI_QRModeOptions.fng', 5); time.sleep(0.6)
    say('on the Quick Race options')

    def settings(): return g.read(QR, 0x14)

    STICK = {'lx+': dict(lx=32767), 'lx-': dict(lx=-32768), 'ly+': dict(ly=32767), 'ly-': dict(ly=-32768)}
    keyboard = '--keyboard' in args
    KEYS = {'lx+': 'right', 'lx-': 'left', 'ly-': 'down', 'ly+': 'up'}
    def push(move, hold):   # fe.press takes '+' as its button separator, so stick moves go through vpad
        if keyboard: t.key(KEYS[move], hold)
        elif move in STICK: fe.vpad(g, pad, **STICK[move]); time.sleep(hold); fe.vpad(g, pad)
        else: fe.press(g, pad, move, hold)

    def row():
        before = settings()
        push('lx+', 0.1); time.sleep(0.35)
        after = settings()
        push('lx-', 0.1); time.sleep(0.35)
        changed = [i for i in range(0x14) if before[i] != after[i]]
        if settings() != before: sys.exit(f'FAIL: stick left did not undo stick right ({before.hex()} -> {settings().hex()})')
        return changed[0] if len(changed) == 1 else None

    if keyboard: g.focus(); time.sleep(0.3)
    start = row()
    if start is None: sys.exit('FAIL: stick right changed no single setting on the first row')
    say(f'selected row: setting byte {start} (stick right/left change it)')
    current = start
    if keyboard: g.focus(); time.sleep(0.3)
    moves = (('ly-', True), ('ly+', True)) if keyboard else         (('lt', False), ('rt', False), ('ly-', True), ('ly+', True), ('down', True), ('up', True))
    for move, should_move in moves:
        push(move, 0.15); time.sleep(0.35)
        now = row()
        moved = now != current
        ok = moved == should_move and now is not None
        say(f'{move:4s} -> setting byte {now} ({"moved" if moved else "stayed"}) {"ok" if ok else "FAIL"}')
        if not ok: sys.exit(1)
        if move in ('ly+', 'up') and now != start: sys.exit(f'FAIL: {move} did not come back to byte {start}')
        current = now
    say('menu stick check passed')

if __name__ == '__main__':
    main()
