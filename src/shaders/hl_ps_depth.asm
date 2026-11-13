// depth_worldheadlight ps (game: rgb = sat(2 t0 (v0 + L)), a = sat(2 t0.a v0.a)), every light
// c0 the game's HeadlightColor (the view's own light), c1..c5 the other players' colours, c6 the cap on the light sum
// Each light texcoord is read one way only (ramp from .w, beam divided by w by hand, no texldp): NVIDIA's D3D9
// compiler up to 391.35 counts a projected read as a second input and overflows its input table when it adds fog
// (docs/bugs/nvidia-388-shader-compiler-crash.md).
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
rcp r2.w, t1.w
mul r2, t1, r2.w
mul r3, t2.w, c7
add r3, r3, c8
rcp r4.w, t2.w
mul r4, t2, r4.w
mul r5, t4.w, c7
add r5, r5, c8
rcp r6.w, t4.w
mul r6, t4, r6.w
texld r1, r1, s2
texld r2, r2, s1
texld r3, r3, s2
texld r4, r4, s1
texld r5, r5, s2
texld r6, r6, s1
mul r2, r2, r1
mul r7, r2, c0
mul r4, r4, r3
mad r7, r4, c1, r7
mul r6, r6, r5
mad r7, r6, c2, r7
mul r8, t5.w, c7
add r8, r8, c8
rcp r9.w, t5.w
mul r9, t5, r9.w
mul r10, t6.w, c7
add r10, r10, c8
rcp r11.w, t6.w
mul r11, t6, r11.w
mul r1, t7.w, c7
add r1, r1, c8
rcp r3.w, t7.w
mul r3, t7, r3.w
texld r8, r8, s2
texld r9, r9, s1
texld r10, r10, s2
texld r11, r11, s1
texld r1, r1, s2
texld r3, r3, s1
mul r9, r9, r8
mad r7, r9, c3, r7
mul r11, r11, r10
mad r7, r11, c4, r7
mul r3, r3, r1
mad r7, r3, c5, r7
min r7, r7, c6
add r7, r7, v0
mul r7, r7, r0
add_sat r7.rgb, r7, r7
mul r2.a, r0.a, v0.a
add_sat r7.a, r2.a, r2.a
mov oC0, r7
