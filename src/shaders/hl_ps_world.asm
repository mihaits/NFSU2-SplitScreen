// worldheadlight / worldheadlightnodepth ps (game: rgb = sat(2 t0 (v0 + L)), a = sat(t0.a v0.a)), every light
// c0 the game's HeadlightColor (the view's own light), c1..c5 the other players' colours, c6 the cap on the light sum
ps_2_0
def c7, 0.005, 0, 0, 0
def c8, 0.5, 0, 0, 1
dcl t0
dcl t1
dcl t2
dcl t4
dcl t5
dcl t6
dcl t7
dcl v0
dcl_2d s0
dcl_2d s1
dcl_2d s2
texld r0, t0, s0
mul r1, t1.w, c7
add r1, r1, c8
mul r2, t2.w, c7
add r2, r2, c8
mul r3, t4.w, c7
add r3, r3, c8
mul r4, t5.w, c7
add r4, r4, c8
mul r5, t6.w, c7
add r5, r5, c8
mul r6, t7.w, c7
add r6, r6, c8
texld r1, r1, s2
texld r2, r2, s2
texld r3, r3, s2
texld r4, r4, s2
texld r5, r5, s2
texld r6, r6, s2
texldp r8, t1, s1
texldp r9, t2, s1
texldp r10, t4, s1
texldp r11, t5, s1
mul r8, r8, r1
mul r7, r8, c0
mul r9, r9, r2
mad r7, r9, c1, r7
mul r10, r10, r3
mad r7, r10, c2, r7
mul r11, r11, r4
mad r7, r11, c3, r7
texldp r8, t6, s1
mul r8, r8, r5
mad r7, r8, c4, r7
texldp r9, t7, s1
mul r9, r9, r6
mad r7, r9, c5, r7
min r7, r7, c6
add r7, r7, v0
mul r7, r7, r0
add_sat r7.rgb, r7, r7
mul_sat r7.a, r0.a, v0.a
mov oC0, r7
