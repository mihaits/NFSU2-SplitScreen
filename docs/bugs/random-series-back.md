# random-series-back
Status:       fixed (2026-11-08)
Report:       code review, 2026-11-08: the docs say a random series ends when the mode select comes up any other way
              than Next Race (Quit, Back), but Back after Next Race kept it going.
Repro:        a random series (Y on the split mode select), Post Race → Next Race, Back on the track select before its
              roll is accepted (or on a mode whose track roll finds no track), a mode picked by hand, a race: the next
              Next Race rolled mode and track again instead of opening the picked mode's track select.
Area:         [frontend.md](../frontend.md) (Random).

## Cause
`RandomTick` ended the series when the mode select came up with `g_nextRace` clear. After Next Race `g_nextRace`
stays set until player 1's car select popup, so a Back from the track select reached the mode select with it still
set and the series went on.

## Fix
The test is `g_nextRaceToTrack` instead: it is set only from Next Race's choice until `NextRaceTick` goes on from the
mode select, so only that arrival keeps the series.
Tested 2026-11-08 with `tools/random_back_check.sh`: a drag series (3P), Next Race rolled the mode again by itself, and
Back on its track select (track roll held off) ended the series ("random: series ended", `g_randomSeries` 0).
