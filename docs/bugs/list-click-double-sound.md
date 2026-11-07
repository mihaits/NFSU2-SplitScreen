# list-click-double-sound
Status:       fixed (2026-11-07)
Report:       user, 2026-11-07: a mouse click on Continue on the split options screen plays the menu sound twice; A and
              Enter play it once.
Repro:        any race options screen (split or single player Quick Race), left click on the help bar's "(A) Continue".
              Every time. Vanilla behaviour, not caused by the plugin.
Area:         [frontend.md](../frontend.md) "List screen sounds".

## Cause
Two messages for one click, each with the accept sound. Logging every FE sound (0x458520) with the caller's stack
showed two sound 4 plays from 0x55647B, one frame (15 ms) apart, the first for message 0x406415E3 (p3 = 0), the second
for the accept 0x0C407210 (p3 = the clicked object). 0x55647B is the end of the list screens' shared handler 0x556020,
which maps both messages to input 8 (accept sound). The FE mouse code (0x54E0E9-0x54E23E) sends the package click
0x406415E3 and the clicked object's accept; a click on Continue gets both, a click on a row only the package click.

## Measured (2026-11-07)
- Single-player Quick Race options, mouse Continue: the same two sounds, so it is the game's own behaviour.
- Row click on the options screen: one sound (0x406415E3), the screen stays.
- Mouse Continue on the track select and the mode select: one sound each (their own handlers, sounds from 0x50D190
  and 0x543FB8).
- Right click on the options screen: one Back sound (5, for 0x911AB364).

## Ruled out
- The car select's generic Back / Start drop (call 0x51DD81, the lead in the report): both sounds play on the options
  screen, before the car select opens.
- The plugin sending an accept: neither message comes from it.

## Fix
`FeSound` (`ListClickEcho`, seats.cpp) drops a sound from 0x55647B for the accept when the same site played the same
sound for 0x406415E3 within the last 200 ms. Checked: mouse Continue one sound (the accept's logged as dropped), pad A
one, row click one, right click one.
