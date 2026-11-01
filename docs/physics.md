# Car physics

How `speed2.exe` v1.2 simulates cars. **Unlike the other docs, this one comes from static analysis only** (decompilation,
disassembly, GLOBALB.BUN): none of it has been checked in game yet. Formulas and offsets are read from code; names are
ours. Items marked *(inferred)* are interpretation. Check a value live before building on it.

Units look like metres, seconds, tonnes and kilonewtons *(inferred)*: stock mass is about 1.0-1.7, torque about
0.13-0.5, spring rate about 27.

## Objects

| object | where | notes |
|---|---|---|
| mover `P` | car +0x34 | mode at car +0x28, set by 0x5F46A0 |
| input object | P +0x04 = car +0x30 | +0x1E0 steer word (65536 = 360°), +0x20C throttle, +0x210 handbrake, +0x214 brake, +0x1E4 gear command, +0x2C0 gearbox mode |
| rigid body | P +0x20 | [below](#rigid-body) |
| steering | P +0x24 | TwoWheelAckerman, vtable 0x7A2084 |
| suspension[4] / wheel[4] | P +0x28 / +0x38 | 0 FL, 1 FR, 2 RL, 3 RR |
| engine / drivetrain | P +0x48 / +0x4C | |
| aero | P +0x50 | plain struct |
| spec `S` | P +0x54 = `*(car+0x20)` | final per-car parameters, [tuning](#parameters-and-tuning) |
| car state | P +0x58 = car +0x40 | state published to the rest of the game, so `state+X` = `car+0x40+X` |

Offsets that alias: car +0x424 (`state+0x3E4`) is the grounded wheel count; car +0x464 (`state+0x424`) is the Stability
Control value. Search for both spellings when looking for readers.

### Movers

0x5F4400 picks the mode:

| mode | class | ctor | used by | model |
|---|---|---|---|---|
| 0 | NullMover | | parked | none |
| 1 | PhysicsMover | 0x5B5CC0 | local humans; AI in drag and in flagged cases | full, all of this doc |
| 2 | TrafficMover | 0x408740 | traffic | kinematic, no forces |
| 3 | MellowMover | 0x5B6D00 | most AI racers | [simplified](#mellowmover) |
| 4 | BeltMover | 0x5A9030 | | no rigid body |
| 5 | NIS mover | 0x5B6FC0 | intros and outros | springs only, tyre forces zeroed (0x5AB540) |

0x404890 switches AI between 3 and 1. All three body movers keep wheels at +0x38, but Melo and NIS have the chassis at
+0x24 where PhysicsMover has steering. [0x8021AC] (file value -1) overrides a live human's mode.

## Frame and substeps

PhysicsMover::Update 0x5B72D0(dt), vtable 0x7A25E0 slot 1:

1. 0x5AB990: axle preload, mass refresh, wheel inertia.
2. 0x5A49A0: gravity, damping flags, yaw clamp, top speed scale, catch-up, heat enable.
3. 0x5ABBF0: pedals, gears, nitrous, steering.
4. 0x5A4D80: per-wheel brake, handbrake and grip inputs.
5. 0x5B6300: launch hop, post-shift grip.
6. `n = trunc(dt / [0x7FC070]) + 1` substeps, [0x7FC070] = 0.0044 (4 at 60 Hz), `h = dt / n`, stored at 0x7FC800. There is no
   accumulator, so h varies with frame time. Each substep: 0x5AA480(h), then 0x5BB0D0 re-orthonormalises the matrix.
7. 0x5A5540: publish to car state, slew visible steering, tach filter, wheel matrices 0x5921B0.

0x5AA480 is explicit Euler over the node list (body, then 4 × wheel + suspension, engine, drivetrain): derivatives
(node slot +0x18), `S += h·dS`, post-steps (slot +0x1C, 0x5B04D0), rest logic 0x5ABB00, then the force pass 0x5B0520. Forces
computed at the end of a substep are integrated at the start of the next.

Force pass 0x5B0520: clutch torque 0x594CB0, aero 0x59E620, tyre load 0x59D160, tyre solver 0x5ABDF0, apply tyre forces,
air stabiliser 0x5A5B60, drift 0x5AC260 (drift only), hydraulics 0x5A51B0, dyno pin 0x5A5C20.

## Rigid body

State is 0x20 floats from +0x20 (0x210 bytes, vtable 0x7A20C4):

| offset | meaning |
|---|---|
| +0x20 | position (Z up) |
| +0x30 | orientation 4×4 matrix; row 0 forward, row 2 (+0x50) up. Not a quaternion |
| +0x70 / +0x80 | linear / angular momentum |
| +0x90 | low-speed stiction state |
| +0xA0 / +0xC0 | velocity / angular velocity (derived) |
| +0xB0 | acceleration in g |
| +0x110 / +0x120 | force / torque accumulators |
| +0x130 / +0x140 | last step's force / torque |
| +0x150 / +0x154 / +0x158 | stiction on / angular drag on / alternate inertia |
| +0x160 / +0x164 / +0x168 | gravity / mass / 1/mass |
| +0x170 / +0x1B0 | inverse inertia / alternate with Izz scaled |
| +0x1F0 | CG offset |
| +0x200 | yaw torque clamp, 0 = off |
| +0x204 / +0x208 | top speed base (mph) / scale |

Mass and inertia are rewritten each frame from S by 0x59B940, so edit S, not the body.

- **Force input:** 0x59BB00(F, r) caps each call at |F| 1200 and |τ| 100; 0x59BC60(J, r) applies an impulse, capped at 1000 and 10.
- **Gravity:** −m·g, applied in post-step 0x59C5F0. g = 9.8 ([0x7FBEF0]), 11 ([0x7FC084]) when car +0x38 == 1, 0 when asleep.
- **Hard caps:** |v| ≤ 200 m/s ([0x7FC6EC]), |ω| ≤ 250.
- **Top speed limiter** (0x59C1E0, skipped if +0x204 == 0): `limit_mph = +0x208 · +0x204 + 1.9`, ×1.03 if +0x15C, ×0.9..1.1 by
  slope. Above it the forward force is replaced by a small retard. Base is S +0x290, 0 once certain ECU parts are fitted.
- **Damping:** only angular drag (table of |ω|) and stiction that fades above 4 m/s. Air drag is in aero.
- **Rest:** 0x5A5E20 decays momentum ×0.95 below 0.7 m/s and freezes the body below 0.25 with gravity off. A mod pushing a stopped car has to wake it.
- **car +0x38 == 1** *(inferred: wreck)*: gravity 11, no damping, no aero, throttle ≤ 0.4.

## Collision

GJK contacts, resolved in 0x5FFB80 (up to 20 iterations): rewind both bodies to contact time, apply impulse 0x5AF0E0, advance.

- Restitution 0.1, 0.05 on walls. Friction 0.3, 0.09 on walls, floors `0.3·(1 + 0.17·(4 − grounded))` capped at 1.
- Effective car mass is at least 1.85 (0x7A1884); angular share is 0.4 (0x7A1888), so hits spin cars less than they should.
- Each contact gives at least 0.3 m/s of Δv (0x7FBF24).
- Car-car crash kick 0x59FB70, |J| capped at 40: at |J| ≥ 30 (0x7A1868) it sets car +0x38 = 1. Near head-on hits (d < −0.97) add a sideways kick, and all get a vertical kick. Check signs in the disassembly before patching.

Humans and AI get the same impulse scale. Catch-up and rubber band don't touch collisions.

## Steering

Input path: raw stick → filter 0x41DFC0 → input +0x1E0 → 0x5ABD1F → steering slot +4 0x59DEF0, which makes one wheel's angle
`Lθ/(L + Tθ)` (Ackermann, L wheelbase, T track) and builds per-wheel matrices. The tyres use those matrices at once. The visible
wheels use car +0x3CE, slewed toward +0x1E0 at up to 120°/s in 0x5A5540, one angle for both. Nothing adds a steering term between the filter and the tyres.

Pad filter 0x416BB0 (wheels use 0x4061C0, see [input](input.md)):

```
in    = curve[idx](raw)                 idx 1 normal, 4 drift (0x7E9E4C + idx*0x14)
ratio = S+0x270                         × 0x7E9F50(|car+0x128|) in drift when in*car128 > 0
out   = 0x405DC0(in) * in * ratio       clamped to 45°; rate limited by 0x4060E0 (180, or 110 in drift)
```

0x405DC0 gives the angle limit: max angle by speed (0x7E9ED8: 40, 20, 10, 5, 3.5, 3.1, 2.9, 2.9, 2.9, 2.9, x = 0..160, unit not
proven) scaled by pedals. **Countersteer:** a slide flag sets above 15° of slide at ≥ 13.41 m/s and clears below 5°. When the
input opposes the slide, and the flag is set or the slide exceeds the limit, the limit becomes `|slide°| × |curve(input)|`,
capped at `45° × |curve(input)|`. With no input there is no countersteer.

0x406A40 is a dead debug path ([0x81CB8C] has no writer).

## Tyres

Per wheel, each substep. Wheel fields are listed at the end of this section.

**Inputs** (0x5A4D80, per frame):
- `+0x178 foot = 2·bias·brake`, bias = S+0x27C for rear, 1 − that for front. Burnout: throttle > 0.8, brake > 0.5, < 10 mph: rears use mph·0.05.
- `+0x17C hand = {0,0,1,1}[i]·handbrake`, +0.5 when > 0.2 ([conditions](#stability-control)).
- `+0x180 G = P+0x6C · 0.85 (stability case) · (1 − 0.025·wet)`, rears × stabiliser, all × {1, 1.4, 1.7} by nitrous stage. 0x5B6300 then multiplies launch and post-shift effects.

**Load:** `N = spring load · max(0.3, 4c − 3)`, `c = 1 − (axle·n)²` (0x59D160). Tilt costs grip and this is the only camber model.

**Slip** (0x5A8280): `sx = r·ω − vx`, `alpha` = wrapped atan2(vy, vx) folded to ±90°. Brake torque `4·S.foot·input` and `10·S.hand·input`
oppose spin. ABS: if `sx < −0.5` under braking without handbrake, torque × `0.5/|sx|`. This is on for every car unless `[car+0x3C]+0x18` is set.
Lock test 0x59D400 sets `+0x10C` when braking beats the road, and a locked wheel slides.

**Sliding vs rolling:** slides if `|sx| > 0.5` ([0x7FC704]), locked, or `|alpha| > tyre[0]`. The last compares radians with tyre[0] = 12 on
stock data, so it can't trigger: lateral slip alone never makes a tyre slide.

```
mu = surface[+0x28] * +0x184 * G * tyre[4]     (× 0x59D490 in the stability case 1)
Sliding:  Fx =  mu*N * sx/|s|     Fy = -mu*N * vy/|s|     s = (sx, vy)
Rolling:  Fx = T/r (solved)       Fy = -sign(alpha)*sign(vx) * 2.5 * tyre[2] * y(N, |alpha|)
```

`y` (0x59D510) interpolates tables 1..6 at `i = 6·alpha°/tyre[0]`, so it saturates at 12° of slip, at load axis `x = 0.8·N/tyre[1]`:

| k \ x | 0 | 2 | 4 | 6 | 8 | 10 |
|---|---|---|---|---|---|---|
| 1 | 0 | 1.2 | 2.3 | 3.0 | 3.0 | 2.8 |
| 2 | 0 | 1.7 | 3.2 | 4.3 | 5.1 | 5.2 |
| 3 | 0 | 1.8 | 3.5 | 4.9 | 5.8 | 6.1 |
| 4 | 0 | 1.83 | 3.6 | 5.0 | 5.96 | 6.4 |
| 5 | 0 | 1.86 | 3.7 | 5.1 | 6.13 | 6.7 |
| 6 | 0 | 1.9 | 3.8 | 5.2 | 6.3 | 7.1 |

It is load sensitive and has no peak-and-drop: past the slip limit force stays flat. Tables: pointers 0x7FC7D4, data 0x7FC744.

**Solver** 0x5ABDF0: five passes. For rolling wheels it picks Fx so wheel surface acceleration matches the ground at the contact, given the summed
body force and yaw acceleration, and couples the engine (0x594DA0) while the clutch is closed. Wheel inertia is 0.01 for all wheels ([0x7FC6F0]). The wheel
derivative 0x59D350 also snaps rolling wheels to ground speed each substep.

**Friction circle** 0x59D600:
```
limit = surface[+0x24] * tyre[3] * +0x188 * +0x184 * G * N * [0x7FC718]
Fx *= 1.5 if driven and not locked (undone if inside the limit, kept if clipped)
if |F| > limit: F *= limit/|F|
+0x170 = |F|/limit       forced to 1 sliding or locked, 0 airborne
if |vx| > 1: Fx -= 0.15*sin(alpha)*Fy
```
Driving force therefore eats lateral grip early.

**Rear stabiliser** 0x5990C0, rears only, off in drift: rear grip gains up to +35% with slip angle, tapered by 0x5A4D80's ramp (P +0x70: +1/s, −20/s, floor
0.5 while the handbrake is held). A handbrake pull under 20° of slip cancels the gain. Its speed ramp reads BSS at 0x86B314/18, assumed 0 *(not confirmed)*.

**Launch hop** 0x5B6300: below 15 mph at race stage 4, peak wheelspin over 0.5 starts a count that alternates grip reduction on diagonal wheels every 0.5 s.
The same state skews the L/R torque split.

**Surfaces** 0x800EE0 + 0x98·id: +0x24 limit, +0x28 slide mu, +0x2C rolling drag, +0x30 air drag factor. In the file, road, wet and drift are all 1.0. Dirt .97/.958, gravel
.99/.981, puddle .96/.951. Drag and Street X force road. Rain's only direct effect found is the `wet` term (at most 2.5%).

Wheel fields: +0x14 wheel spec, +0x18 tyre, +0x1C brake spec, +0x24 angle, +0x28 ω, +0x2C inertia, +0x70.. frame, +0xF0 contact velocity, +0x108 torque, +0x10C locked, +0x114 surface, +0x118 sx,
+0x11C contact, +0x124 Fx, +0x128 Fy, +0x130 world force, +0x160 sliding, +0x164 N, +0x168 bump, +0x170 grip used, +0x174 flat, +0x178 foot, +0x17C hand, +0x180 G, +0x184 catch-up, +0x188 drift rear limit,
+0x18C road offset, +0x190 / +0x194 Fx / Fy scale (1.0, no writer found).

## Suspension and road

Road contact (0x5A8790, 0x59D250) is a 2.5D query, not a ray: 0x5E72E0 returns road height, surface and normal at x,y, plus bump noise 0x5D3CA0. Compression `x` = distance along body up + radius, clamped to S's
travel limits. Below min there is no contact.

```
F = (max(x,0)*P + 1)*k*x + (v>0 ? bump : rebound)*v + preload + ARB*(x - x_partner)      floored at 0
preload (0x5AB990) = axle share of (mg + downforce rate*max(vfwd,0)*S+0x26C)
```

Preload holds the car up, so ride height doesn't depend on spring rate. Load transfer has no formula of its own: it comes from the body rolling on the springs. Compression hitting max
fires bump stop 0x59E1A0, impulse `0.225·m·closing speed`. Hydraulics 0x5A51B0 slews lift to `input·0.2` at 0.5 m/s, with a hop impulse.

Axle spec (S +0x170, rear +0x190): [0] progression, [1] k, [2] bump, [3] rebound, [4] ARB, [5] max, [6] min compression. *(Slots from code; +0x1C unknown.)*

## Aero

0x59E620, per substep:
```
drag  = -(1 + (1-a4)) * surfaceAir * (1 - car+0x4DC) * Cd * |v|/scale * v           quadratic; scale = catch-up
c     = max(0, vhat·forward);   k = drift ? c^4 : max(c^2, 0.4)
Df/Dr = k * S.DF{f,r} * up * |v|/scale                                              linear in speed
```
Downforce acts along body −z at x = ±1 m (0x7FC860/70), whatever the wheelbase. car +0x4DC is *(inferred)* slipstream. With no wheels down the split becomes 0.4/0.41 of the sum. There is no air control.
Air stabiliser 0x5A5B60 damps slow spin with fewer than 2 wheels down.

## Engine and drivetrain

Torque 0x594300:
```
th = limiterCut ? 0 : clutchGate(eng+0x80) * throttle
T  = Curve(rpm) * eng+0x90 * (eng+0x74 ? 1.05 : 1) * th
T += th * Nos(rpm) * S+0x2AC * {1, 1.4, 1.7}[stage-1]        if nitrous
T *= heat fade   (1 → 0.25 for heat 0.6..1)
T -= EngBrake(rpm) * (1 - throttle) * (cut ? 2 : 1)
```
Curve: 9 points idle..redline (S +0x200). Fuel cut at redline − 25, resume at redline × 0.9875 (0x5AA760). Over-rev heat runs in drag only: +0.45/s above S+0x1F4, blown at 1.2. Throttle has a floor of 0.5 below 3000 rpm once past 0.15. There is no traction control.

Clutch states (0x5A1400): 0 opening, 1 open (gear changes), 2 closing, 3 closed; rates `{−3, 1.5}·S+0x1CC`. Slip torque `S+0x1C4·clutch·clamp(Δω, ±300)`.

Delivery 0x594A30: `front = (1−split)·FD·ratio·T`, `rear = split·FD·ratio·T`. Each axle is a limited-slip diff *(inferred)*: shares by speed in the 0.35..0.65 band, else 82.5/17.5. Above 0.8 rear split, below 60 mph, with `*(car+0x3C)+0x120 > 0`, split is forced to 0.65 (0x5A49A0, meaning unknown).

Wheelspin (0x5AA830): with throttle > 0.2 and mean slip > 0.1, wheel speed is rewritten from engine speed (0x5A12D0), so the engine leads.

Shifting: auto 0x5AA960, needs all 4 wheels down and slip < 4 to go up. Tables in 0x5AC7D0 come from torque-crossing points; Up[1] is the launch target. Gearbox mode (input +0x2C0): 0 auto, 1 manual, 2 auto with held gear *(inferred)*, 3 pedal clutch. Tach car +0x440 is the engine rpm filtered 0.55/0.45 in 0x5A5540.

**Perfect shift and launch are drag only** (0x5A1700): grade {−1, 0, .5, .85, 1} from 0x594F20, default windows 1100, 750, 400, 325, 290, 230 rpm by gear. Grade x gives `torque × (1 + 2x(1 − t/0.5))` for 0.5 s, faster clutch `1 + x`, and grip `1 + min(1, x/div)`, div by class and perf class.

## Nitrous

State fields: +0x414 ticks per bar, +0x41C charge, +0x420 burn start; 4000 ticks = 1 s. `Fill = Rem/Total`, above 1 `1 + (Rem − Total)/Total`. Stage 3 above 1.7, 2 above 1.4, else 1 while > 0 (0x59FA30). A stage multiplies nitrous torque and **all wheel grip** by {1, 1.4, 1.7}. Nitrous bypasses catch-up. Refill 0x601785: humans from style points, AI once a second by speed, 0 in drag. Capacity is 0 in drift and Street X. No speed-breaker or time scaling exists in the physics.

## Stability Control

Stored in car +0x464 (player +0x21, 0 = on per [players](players.md)):

| | value 0 | value != 0 |
|---|---|---|
| wheel grip (not drift) | × 0.85 ([0x7FC060]) | × 1 |
| rear driven grip | | value 1: × 0.85 up to 30 mph (0x59D490) |
| yaw torque clamp body +0x200 | S+0x288 (17 on the data checked) | S+0x28C; if 0, unlimited, or 20 while countersteering |
| handbrake | always +0.5 | +0.5 only with slip > 0.3 rad under 80 mph |
| rear stabiliser | spec gains full | gain ramped by 0x5EC790 |

The main aid is the yaw clamp. It also has a stub 0x5A33E0 in MellowMover multiplying by 1.0. Other readers: 0x599301, 0x5A343B, 0x5AC338 (`>= 2`). What sets 2 wasn't worked out.

## Catch-up

0x599170 distributes `s = car+0x4E4` (if non-zero with car+0x95C == 0), else car +0x468, to engine +0x90, body +0x208, aero +8 and wheels +0x184. So one number raises base torque, top speed, drag speed and grip together. [0x7FC0A0] = 0 forces 1. With car+0x95C set and `[car+0x3C]+0x18` set, the engine gets 0.4·s.

Human writer 0x5F6740: curve (−600, 1.08), (−250, 1.03), (−120, 1.015), (−50, 1.0) of the gap, at [0x81CB60]+0xA6C, so it only helps when behind. AI writer 0x41773E: `max(x, 1)`, rubber band.

## Parameters and tuning

Chunk 0x34600 of GLOBAL\GLOBALB.BUN at [0x8A1CCC]: 46 records, stride 0x890, base block `B` = record + 0x110. `S` is a 0x2B0-byte copy built by 0x5B4DC0 before the race, not per frame: copy B, apply parts, unique bonuses, sliders 0x5B4780, rating and class, then the mode table. Setting `T+4 = -1` (T = `*(car+0x10)`+0x10) makes it rebuild.

| S offset | field (Peugeot) |
|---|---|
| +0x00 | CG x, y, z (0.3, 0, 0.3) |
| +0x10 + i·0x30 | wheel: xyz, +0x10 radius, +0x14 width |
| +0xD0 / +0xF0 | tyre f/r: [0] slip limit° 12, [1] load scale 1, [2] lateral gain 1, [3] limit 1.45, [4] slide 1.2 |
| +0x110 | mass 1.055, box L W H, +0x120 inertia |
| +0x160 | yaw inertia factor 2.22 |
| +0x170 / +0x190 | suspension |
| +0x1B0 | gearbox: launch fraction, +8 / +0xC finals, +0x10 rear split, +0x14 clutch stiffness, +0x18 gears, +0x1C clutch rate, +0x20 ratios (R, N, 1..6) |
| +0x1F0 | idle, +4 peak, +8 redline, +0xC engine inertia .00025 |
| +0x200 / +0x224 / +0x248 | torque[9] / boost part of it / engine braking[3] |
| +0x260.. | Cd .000375, DF front, DF rear, +0x26C preload factor |
| +0x270.. | steering ratio 1.15, brake f, brake r, bias .55 |
| +0x280 / +0x284 | stabiliser base, gain 2.5 |
| +0x288 / +0x28C / +0x290 | yaw clamps, limiter mph 125 |
| +0x2A0 / +0x2AC | NOS capacity, NOS strength |

Parts lerp S toward targets by summed part fraction (table in chunk 0x34A15). Slider value is `current + (hi−lo)·(byte−100)/100`, range from the highest installed part. ECU and turbo bars are zero-sum: they reshape the curve but add no torque. Slider 0x11 is NOS balance and 0x12 brake bias.

Mode tables (0x7FC2E4 drift, 0x7FC33C Street X, 0x7FC394 drag, + class·0x144; class 0 FWD, 1 AWD, 2 RWD; 22 floats) multiply downforce, mass, yaw inertia and grip and shift split, CG and bias, interpolated by part level. Drift: FWD +.15 rear split, RWD −.15. Street X adds up to 60% rear downforce and 6-35% grip by class and axle. Drag adds 20-40% downforce and −5% mass.

## Modes

| flag | mode |
|---|---|
| 0x89E7D8 | drag |
| 0x89E7D9 | drift |
| 0x89E7E3 | Street X |
| 0x89E7E2 | race type 11, HUD_Drift on track 4097; reachability unknown |
| 0x890100..0x890114 | debug overrides of the above, no retail writers |

Free roam and URL use circuit physics. `is_drift = !drag && (D9 || 0x890104 || 0x890114)`.

| | drag | drift | Street X | others |
|---|---|---|---|---|
| rear stabiliser | on | off | on | on |
| drift yaw and rear limit 0x5AC260 | | yes | | |
| downforce vs angle | c², ≥ .4 | c⁴ | c², ≥ .4 | c², ≥ .4 |
| steer curve / rate | 1 / 180 | 4 / 110 | 1 / 180 | 1 / 180 |
| surface | forced road | | forced road | |
| shift cap | redline | peak | peak | peak |
| perfect shift, over-rev | yes | | | |

Drift 0x5AC260: when slide and yaw share a sign with ≥ 2 wheels down, yaw torque `table(|slide|)·k·yaw`, k = FWD −10/−7/−4, AWD −7.5/−5/−4, RWD −15/−10/−6 by difficulty (0x89E7F4); rear wheel +0x188 falls from 1.10 to .76 with drift size.

Difficulty's physics reach in the reviewed code is drift yaw and drag shift windows. Human drag lane changing isn't in this model and wasn't traced (0x4299C0 family).

## MellowMover

Forces 0x5A38E0 from four corner points (springs without ARB), a plane fitted to road samples, no surface or bump. `F = −1.25·k·N·slip`, capped at `4·N·s` (2 in Street X, 3.2 on tracks with flag 0x2000). No slip angle or load curve. Integrated at 1/70 s ([0x7FC040]). Drive force wasn't read. 0x5AB190 derives rpm from wheel speed.

## Hook points

| where | change |
|---|---|
| 0x5B0520 (slot 0x7A25DC) | wrap and add 0x59BB00 forces per substep |
| 0x59C5F0 (slot 0x7A20E0) | read or alter body state after each integration |
| 0x5B72D0 (slot 0x7A25E4) | per-frame, per-car; dt available |
| 0x59D600 | replacement tyre: has Fx, Fy, N, limit |
| wheel +0x180 after 0x5B6300 | per-wheel grip |
| 0x5EBBE0 / car +0x4D8 | per-car grip |
| 0x599170 or car +0x468 | power, top speed and grip together |
| end of 0x5B4DC0 | edit S: any per-car parameter, in every mode |
| [0x8A1CCC] + i·0x890 + 0x110 | edit base data, then `T+4 = -1` |
| 0x7FC2E4 etc. | per-mode rebalance, data only |
| 0x7FC718, 0x7FC7D4, 0x7FC720 | global grip, curve shape (could add a peak-and-drop), drive-eats-grip |
| 0x5990C0 → 1.0 | remove anti-spin grip |
| 0x7FBEF0 / 0x7FC084 | gravity; preload reads it too |
| 0x7FC6EC, S+0x290 | speed cap, limiter |
| 0x7A1884, 0x7A1888, 0x7A1868 | crash mass floor, spin share, wreck threshold |
| 0x5A1700 | allow perfect shifts outside drag |
| 0x7FC070 | smaller = more substeps |
| 0x890114 = 1 | drift handling in any mode; global, so all players |
| 0x5A7750, 0x5A77C0 | NOS in drift and Street X |

S is per car and these 0x7Fxxxx values are global, so per-player changes need S, runtime objects or a per-car wrapper. Constants at 0x784xxx are shared with unrelated code: patch the reading instruction instead.

## Not worked out

- Force and unit checks live: body +0x164, tyre forces, cap sizes.
- Drag lane logic, race type 11, `*(car+0x3C)` (+0x18, +0x120, +0x140), car +0x4E4 writer, state +0x414 writer (nitrous seconds), drv +0xF0 writer.
- Stability value 2; 0x86B314/18; drift table 0x86B3C8; air stabiliser table 0x86B4A0.
- Aero slot [4] copied from input +0x1E0 (reads as a float).
- Traffic response to hits; where dt comes from; MellowMover drive force.
