# menu-scroll-triggers
Status:       fixed (2026-11-08), [GitHub #4](https://github.com/mihaits/NFSU2-SplitScreen/issues/4)
Report:       a user with XtendedInput: the controller's triggers act like the left stick in menus.
Repro:        any list menu (Quick Race options, pause options) with a pad in the default binding: LT moves the
              selection down, RT up, the left stick's Y does nothing; stick X and the D-pad work. Every time. The
              game's own behaviour on PC, any pad, not caused by the plugin.
Area:         [input.md](../input.md) "Analog bytes".

## Cause
The menus read up/down (FENG_PAD_UP/DOWN_ALTERNATE, events 0x2E/0x2F; also hydraulics 0xA8/0xAD) from analog byte
+0x13, the console's menu stick Y. On PC the decoder 0x5C1840 fills +0x13 from the shared pedal axis (Z, both
triggers), and nothing from the stick's Y. The pedal events 0x93/0x96 read the same byte, so it could not simply be
given to the stick.

## Measured (2026-11-08)
- Virtual pad on the Quick Race options: LT down, RT up, stick Y nothing (before the fix).
- The game's float-to-int 0x75BED4 truncates; stick right gives +0x12 0xFE, up 0x01.
- The separate pedal channels were already at least the shared channel's value at every trigger position, so the
  shared channel only mattered at the very top: a raw 255 decodes to 253 (XOR 1, curve, truncation), brake 0.992,
  and the shared channel's 255 hid that through the larger-of rule.
- No reader of +0x13 besides the event records, the port merge, the previous-state copy and the record reset
  (`opscan.py` on 0x86EAFB and `+ 0x13]`).

## Ruled out
- A menu-only gate (stick Y into +0x13 only while a menu reads it): two meanings for one byte by state; rejected for
  one path in every state.
- An XtendedInput-style input rewrite: far more than the bug needs.
- First build wrote the raw trigger into the separate bytes: full LT gave brake 0.992 (above); now written XOR 1.

## Fix
`AnalogBytes` (parts/input.cpp) at both ends of the decoder's axis part (0x5C2044, 0x5C20D0) writes +0x13 from the
steering stick's Y in both pedal modes; `RetargetPedalEvents` points 0x93/0x96 at +0x10/+0x11 (mode 0, curve of
0x92/0x95); `SharedPedals` fills those from the triggers (value XOR 1) or splits another bound axis to the values the
game's shared decoding gives. Checked with `analog_check.py`, `menu_stick_check.py` (pad and keyboard),
`pedal_check.py` (P1-P3 pedals and pause), `remap_check.py` and `start_matrix.sh`; feel approved on an Xbox pad and a
DualSense.
