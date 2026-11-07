# random-cars-stock-rating
Status:       fixed (2026-11-07)
Report:       user, 2026-11-07: a Random cars drift race dealt two stock cars and a maxed-out career Miata.
Repro:        Cars = Random with upgraded My Cars or career cars in the garage; any mode, any player count. Whenever
              the anchor car is stock, upgraded cars of the same model fall within its tolerance.
Area:         [frontend.md](../frontend.md) "Cars row".

## Cause
`PickRandomCars` matches cars within 4% of a random anchor's rating, read from the garage entry's +0xC. That value is
right for stock cars (their vtable slot 3, 0x503A80, rates its build and stores it), but a My Cars / career entry's
setup (0x516B90) rates a fresh stock build of the car type (0x610270(type, 1, 0, 0)) and nothing rates the entry again.
So every upgraded car carried its stock rating: the career Miata 179.8, the stock Miata's exactly. The race in the
report: 180.6, 175.1 and 179.8, a "match" within 4%.

## Fix
`CarRating` rates each candidate from its own build: the entry's slot 3 into a scratch RideInfo, then 0x5B6990, as
the setup does with its stock build. Checked on the reporting garage: every stock car's computed rating equals its
stored one; the upgraded ones now rate 508-613 (career Miata 612.6, was 179.8), and a Random drift race dealt three
of them to each other (590-612). The first try passed the ride as slot 3's first argument and crashed in 0x610270
(write to 0): slot 3 is (mode, ride, slot).
