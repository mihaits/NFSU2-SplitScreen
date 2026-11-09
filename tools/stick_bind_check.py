"""Stick and trigger bindings on the Controls screen (docs/input.md, "Stick and trigger bindings").
Run through stick_bind_check.sh (boots, restarts and restores the files). Steps, each against the running game:
python -I stick_bind_check.py menu            # main menu: backup, P1 page captures, labels, rules, P3 page, save
python -I stick_bind_check.py persist A A3    # after a restart: P1's and P3's bindings came back
python -I stick_bind_check.py race A A3       # 3P race: P1's and P3's bound inputs fire, P2 has none
python -I stick_bind_check.py restore         # game stopped: the players ini and the profile as before the run

The first capture goes through the row (select it, A); the others are started the way the row does it (0x5C2100:
record +0x33C action, +0x340 column, byte 0x870768 = 1). Results come from the record (table 0, action bits +8) and
the plugin's log. A line per stage; exits 1 on the first failure. The game window must stay focused.
"""
import os, re, shutil, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths, trainer as t, fe

RECORDS, REC_SIZE = 0x874C40, 0x370
CONTROLLER = 'UI_Options_PC_Controller.fng'
P1_PAD, P3_PAD = 1, 3
PLAYERS_INI = os.path.join(os.path.dirname(paths.LOG), 'NFSU2SplitScreen_Players.ini')
BACKUP = os.path.join(os.path.dirname(paths.LOG), 'stick_bind_check_backup')
PROFILE = None               # the loaded profile's save file (set from the game)
def profile_file(g):
    name = g.read(0x83A9E0, 32).split(b'\0')[0].decode('latin1')
    return os.path.join(os.environ['LOCALAPPDATA'], 'NFS Underground 2', name, name)
def say(m): print(time.strftime('%H:%M:%S'), m, flush=True)
def fail(m): say('FAIL: ' + m); sys.exit(1)

def pad_record(g, slot):
    for d in range(6):
        r = RECORDS + d * REC_SIZE
        if g.i32(r + 0x230) == slot and g.u32(r + 0x234) != 0x13: return r
    fail(f'no pad record for slot {slot}')

def table0(g, rec): return list(struct.unpack('<32i', g.read(rec + 0x18, 128)))
def bits(g, rec): return struct.unpack('<Q', g.read(rec + 8, 8))[0]

def log_mark():
    return os.path.getsize(paths.LOG)
def log_since(mark):
    with open(paths.LOG, 'rb') as f:
        f.seek(mark); return f.read().decode('latin1')
def wait_log(mark, pattern, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        m = re.search(pattern, log_since(mark))
        if m: return m
        time.sleep(0.05)
    fail(f'{what}: no log line /{pattern}/ in {timeout} s')

def wait(cond, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        if cond(): return
        time.sleep(0.02)
    fail(what)

def poke8(g, addr, v):       # the mailbox writes dwords: keep the other 3 bytes
    d = g.u32(addr & ~3); sh = (addr & 3) * 8
    fe.command(g, 3, addr & ~3, (d & ~(0xFF << sh)) | (v << sh))

def capture(g, rec, action, column):
    fe.command(g, 3, rec + 0x340, column)
    fe.command(g, 3, rec + 0x33C, action)
    poke8(g, 0x870768, 1)

STICK = {'Left Stick X-': dict(lx=-32768), 'Left Stick X+': dict(lx=32767), 'Left Stick Y+': dict(ly=32767),
         'Left Stick Y-': dict(ly=-32768), 'Right Stick X-': dict(rx=-32768), 'Right Stick X+': dict(rx=32767),
         'Right Stick Y+': dict(ry=32767), 'Right Stick Y-': dict(ry=-32768), 'Left Trigger': dict(lt=255),
         'Right Trigger': dict(rt=255)}

def push(g, pad, name, hold=0.25):
    fe.vpad(g, pad, **STICK[name]); time.sleep(hold); fe.vpad(g, pad); time.sleep(0.1)

def bind_stick(g, rec, pad, action, column, name):
    mark = log_mark()
    fe.vpad(g, pad); time.sleep(0.1)
    capture(g, rec, action, column)
    push(g, pad, name)
    m = wait_log(mark, r'sticks: (.+?) bound to action (\d+) \(column (\d)\)', 1.0, f'capture of {name}')
    if (m[1], int(m[2]), int(m[3])) != (name, action, column): fail(f'captured {m[0]!r}, expected {name} {action} {column}')
    wait(lambda: g.u32(rec + 0x33C) == 0 and g.read(0x870768, 1) == b'\0', 0.5, 'capture not closed')
    say(f'bound {name} to action {action} (column {column})')

def fires(g, rec, pad, name, action, expect=True):
    """The action's bit (active low) while the input is held, and back when released."""
    fe.vpad(g, pad, **STICK[name])
    end, seen = time.time() + 0.6, False
    while time.time() < end and not seen:
        seen = not (bits(g, rec) >> action) & 1
        time.sleep(0.01)
    fe.vpad(g, pad); time.sleep(0.15)
    if seen != expect: fail(f'{name}: action {action} {"not set" if expect else "set"} while held')
    if expect and not (bits(g, rec) >> action) & 1: fail(f'{name}: action {action} still set after release')
    say(f'{name} {"sets" if expect else "does not set"} action {action}')

def open_page(g, page):
    """Through the menus (Back must find Options under it): main menu, 6 right to Options, 4 right to Controls."""
    if fe.top(g) != CONTROLLER:
        for _ in range(6): fe.press(g, P1_PAD, 'right', 0.08); time.sleep(0.35)
        fe.press(g, P1_PAD, 'a', 0.08)
        wait(lambda: fe.top(g) == 'UI_OptionsMain.fng', 3, 'Options not open')
        time.sleep(0.4)
        for _ in range(4): fe.press(g, P1_PAD, 'right', 0.08); time.sleep(0.35)
        fe.press(g, P1_PAD, 'a', 0.08)
        wait(lambda: fe.top(g) == CONTROLLER, 3, 'Controls screen not open')
        time.sleep(0.4)
    fe.command(g, 3, 0x8364EC, page); fe.command(g, 1, 0xAF0BBD92, s1=CONTROLLER)
    time.sleep(0.5)
    if g.i32(0x8364EC) != page: fail(f'page {g.i32(0x8364EC)}, expected {page}')

def refresh_page(g):         # a poked capture isn't the row's: rebuild the page so every label is asked again
    fe.command(g, 3, 0x8364EC, g.i32(0x8364EC)); fe.command(g, 1, 0xAF0BBD92, s1=CONTROLLER); time.sleep(0.4)

def ui_capture(g, rec, pad, name, row=4):
    """The row's own path on a freshly opened page (no row selected: the first Down selects row 0): select the row,
    A, push the input; the row's label updates. Row 4 is Shift Up, the first button action."""
    for _ in range(row + 1): fe.press(g, pad, 'down', 0.08); time.sleep(0.3)
    fe.press(g, pad, 'a', 0.08)
    wait(lambda: g.u32(rec + 0x33C) != 0, 1.0, 'row did not start a capture')
    action = g.u32(rec + 0x33C)
    if action <= 4: fail(f'row {row} captures axis role {action}, not a button action')
    mark = log_mark()
    push(g, pad, name)
    wait_log(mark, rf'sticks: {re.escape(name)} bound to action {action} \(column 0\)', 1.0, 'UI capture')
    wait_log(mark, rf'sticks: label action {action} column 0 = {re.escape(name)}', 1.0, 'UI label')
    say(f'UI: row {row} bound to {name} (action {action}), label shown')
    return action

def pick_action(g, rec):
    """The action of the first face button that has one (>4: not an axis role), and that button."""
    t0 = table0(g, rec)
    for b in range(4):
        if t0[b] > 4: return t0[b], b
    fail(f'no button action in table 0: {t0}')

def menu(g):
    backup(g)
    fe.vpad(g, P1_PAD); time.sleep(0.2)                  # P1's pad connected: the last active one, slot 0
    open_page(g, 1); say('Controls screen, P1 joypad page')
    rec = pad_record(g, 0)
    act, btn = pick_action(g, rec)
    say(f'action {act} (button {btn}), table 0 {table0(g, rec)[:16]}')
    if ui_capture(g, rec, P1_PAD, 'Left Stick Y-') != act: fail('the first button-action row is not button A\'s')
    g.screenshot(os.path.join(os.path.dirname(paths.LOG), '..', '_splitscreen_mod', 'shots', 'stick_bind_ui.png'), max_w=1280)
    mark = log_mark()
    bind_stick(g, rec, P1_PAD, act, 0, 'Right Stick Y+')
    if table0(g, rec)[btn] == act: fail('primary capture left the button bound')
    refresh_page(g)
    wait_log(mark, rf'sticks: label action {act} column 0 = Right Stick Y\+', 1.0, 'label')
    say('label: Right Stick Y+ in the primary column')
    fires(g, rec, P1_PAD, 'Right Stick Y+', act)
    fires(g, rec, P1_PAD, 'Right Stick Y-', act, expect=False)
    # secondary: a button next to the stick (the stick stays the primary, label column 1 = the button)
    fe.vpad(g, P1_PAD); capture(g, rec, act, 1)
    fe.press(g, P1_PAD, 'abxy'[btn], 0.15)
    wait(lambda: g.u32(rec + 0x33C) == 0, 1.0, 'button capture not closed')
    if table0(g, rec)[btn] != act: fail('secondary button capture did not bind the button')
    fires(g, rec, P1_PAD, 'Right Stick Y+', act)
    say('secondary button kept the stick binding')
    # primary on the button again: the stick loses it
    capture(g, rec, act, 0); fe.press(g, P1_PAD, 'abxy'[btn], 0.15)
    wait(lambda: g.u32(rec + 0x33C) == 0, 1.0, 'button capture not closed')
    fires(g, rec, P1_PAD, 'Right Stick Y+', act, expect=False)
    say('primary button capture cleared the stick binding')
    # a trigger as the secondary, then the stick as the primary of the same action: button cleared, trigger kept
    bind_stick(g, rec, P1_PAD, act, 1, 'Left Trigger')
    fires(g, rec, P1_PAD, 'Left Trigger', act)
    mark = log_mark()
    bind_stick(g, rec, P1_PAD, act, 0, 'Right Stick X-')
    if table0(g, rec)[btn] == act: fail('primary stick capture left the button bound')
    fires(g, rec, P1_PAD, 'Left Trigger', act, expect=False)
    fires(g, rec, P1_PAD, 'Right Stick X-', act)
    # back to the stick + trigger pair for the persistence step
    bind_stick(g, rec, P1_PAD, act, 1, 'Left Trigger')
    refresh_page(g)
    wait_log(mark, rf'sticks: label action {act} column 1 = Left Trigger', 1.0, 'secondary label')
    say('P1: Right Stick X- primary, Left Trigger secondary')
    # player 3's page: own table, P1's untouched
    m = re.findall(r'controls: pages (\d+)\.\.', log_since(0))
    if not m: fail('no player 3-6 pages in the log')
    mark = log_mark()
    open_page(g, int(m[-1]))
    wait_log(mark, r"player 3's bindings in", 1.0, 'P3 page swap')
    act3 = pick_action(g, rec)[0]
    bind_stick(g, rec, P1_PAD, act3, 0, 'Right Stick X+')
    fires(g, rec, P1_PAD, 'Right Stick X+', act3)
    fires(g, rec, P1_PAD, 'Right Stick X-', act, expect=False)   # player 1's binding, not on this page
    p3 = entries('Player3', 'StickBindings')
    if not p3 or p3[5] != act3: fail(f'Player3 StickBindings not saved: {p3}')
    say(f'P3 page: Right Stick X+ -> action {act3}, saved ({p3})')
    mark = log_mark()
    open_page(g, 1)
    wait_log(mark, r"player 1's bindings back", 1.0, 'P1 page swap back')
    fires(g, rec, P1_PAD, 'Right Stick X-', act)
    fires(g, rec, P1_PAD, 'Right Stick X+', act3, expect=False)
    say('P1 page: own bindings back')
    save_out(g)
    print(f'RESULT {act} {act3}', flush=True)

def save_out(g):
    """Back out of Controls and Options to the save prompt and confirm: the profile file is written, and the plugin
    writes players 1/2's entries with it (not before: the prompt's own check packs the profile too)."""
    before = os.path.getmtime(PROFILE) if os.path.exists(PROFILE) else 0
    mark = log_mark()
    fe.press(g, P1_PAD, 'b', 0.08)
    wait(lambda: fe.top(g) == 'UI_OptionsMain.fng', 3, 'Back did not reach Options')
    fe.press(g, P1_PAD, 'b', 0.08)
    wait(lambda: fe.top(g) == 'MC_Main.fng', 3, 'no save prompt')
    for prompt in ('save the profile?', 'overwrite?'):
        time.sleep(0.6)
        if 'sticks: saved' in log_since(mark): fail(f'players 1/2 entries written before "{prompt}" was answered')
        fe.press(g, P1_PAD, 'left', 0.08); time.sleep(0.3); fe.press(g, P1_PAD, 'a', 0.08)   # Yes
    wait_log(mark, r'sticks: saved for \[Profile ', 5, 'profile save')
    wait(lambda: os.path.getmtime(PROFILE) != before, 5, 'profile file not written')
    say(f'saved: profile file written, [{re.search(r"sticks: saved for .(.+?).$", log_since(mark), re.M)[1]}]')

def entries(section, key):
    m = re.search(rf'\[{re.escape(section)}\][^\[]*?{key}=([\d,]+)', open(PLAYERS_INI).read())
    return [int(x) for x in m[1].split(',')] if m else None

def persist(g, act, act3):
    """After a restart: players 1's and 3's entries came back (profile load, seat store) and still fire."""
    fe.vpad(g, P1_PAD); time.sleep(0.2)
    mark = log_mark()
    open_page(g, 1); refresh_page(g)
    rec = pad_record(g, 0)
    wait_log(mark, rf'sticks: label action {act} column 0 = Right Stick X-', 1.0, 'P1 primary label after restart')
    wait_log(mark, rf'sticks: label action {act} column 1 = Left Trigger', 1.0, 'P1 secondary label after restart')
    fires(g, rec, P1_PAD, 'Right Stick X-', act)
    fires(g, rec, P1_PAD, 'Left Trigger', act)
    say('P1: bindings and labels back after the restart')
    base = int(re.findall(r'controls: pages (\d+)\.\.', log_since(0))[-1])
    mark = log_mark()
    open_page(g, base); refresh_page(g)
    wait_log(mark, rf'sticks: label action {act3} column 0 = Right Stick X\+ \(player 3\)', 1.0, 'P3 label after restart')
    fires(g, rec, P1_PAD, 'Right Stick X+', act3)
    say('P3: binding and label back after the restart')

def race(g, act, act3):
    """In a 3P race: P1's and P3's bound inputs set their action, P2 has none."""
    if g.u32(0x8654A4) != 6: fail('no race')
    rec1, rec2 = pad_record(g, 0), pad_record(g, 1)
    fires(g, rec1, P1_PAD, 'Right Stick X-', act)
    fires(g, rec1, P1_PAD, 'Left Trigger', act)
    say('P1 (slot 0 record) in the race')
    fires(g, rec2, 2, 'Right Stick X-', act, expect=False)
    say('P2 has no stick bindings')
    # players 3..6's ports are polled (PollExtraPort) only once the race takes input: push until it registers
    end = time.time() + 6
    while True:
        mark = log_mark()
        fe.vpad(g, P3_PAD, **STICK['Right Stick X+']); time.sleep(0.3); fe.vpad(g, P3_PAD); time.sleep(0.2)
        if re.search(rf'sticks: player 3 Right Stick X\+ pressed \(action {act3}\)', log_since(mark)): break
        if time.time() > end: fail('P3 press in the race: not logged in 6 s')
    wait_log(mark, rf'sticks: player 3 Right Stick X\+ released', 0.5, 'P3 release in the race')
    if re.search(r'sticks: player [12] ', log_since(mark)): fail('P3\'s stick fired another player\'s binding')
    say('P3 (its own decoder run) in the race')

def backup(g):
    """Before anything is bound: the players ini and the profile's save file (the run saves the profile)."""
    global PROFILE
    PROFILE = profile_file(g)
    if os.path.exists(BACKUP): fail(f'{BACKUP} exists: an earlier run was not restored (run the restore step)')
    os.makedirs(BACKUP)
    shutil.copy2(PLAYERS_INI, os.path.join(BACKUP, 'players.ini'))
    shutil.copy2(PROFILE, os.path.join(BACKUP, 'profile'))
    open(os.path.join(BACKUP, 'profile.path'), 'w').write(PROFILE)
    say(f'backed up the players ini and the profile ({PROFILE})')

def restore():
    if not os.path.exists(BACKUP): return say('nothing to restore')
    shutil.copy2(os.path.join(BACKUP, 'players.ini'), PLAYERS_INI)
    shutil.copy2(os.path.join(BACKUP, 'profile'), open(os.path.join(BACKUP, 'profile.path')).read())
    shutil.rmtree(BACKUP)
    say('restored the players ini and the profile')

def main():
    global PROFILE
    step = sys.argv[1] if len(sys.argv) > 1 else 'menu'
    if step == 'restore': return restore()
    g = t.Game()
    PROFILE = profile_file(g)
    args = [int(x) for x in sys.argv[2:4]]
    if step == 'menu': menu(g)
    elif step == 'persist': persist(g, *args)
    elif step == 'race': race(g, *args)
    else: fail('unknown step ' + step)
    say(step + ' done')

if __name__ == '__main__':
    main()
