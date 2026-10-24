// worldheadlightreflect ps (game: rgb = 2 t0 (v0 + L) + t3 t0.a v0.a, a = 1), every light
ps_2_0
dcl t3
dcl_2d s3
def c5, 0.005, 0, 0, 0
def c6, 0.5, 0, 0, 1
dcl t0
dcl t1
dcl t2
dcl t4
dcl t5
dcl t6
dcl v0
dcl_2d s0
dcl_2d s1
dcl_2d s2
texld r0, t0, s0
texldp r1, t1, s1
texld r2, t2, s2
mul r4, t4.w, c5
add r4, r4, c6
mul r5, t5.w, c5
add r5, r5, c6
mul r6, t6.w, c5
add r6, r6, c6
texldp r8, t4, s1
texld r4, r4, s2
texldp r9, t5, s1
texld r5, r5, s2
texldp r10, t6, s1
texld r6, r6, s2
mul r1, r1, r2
mul r7, r1, c0
mul r8, r8, r4
mad r7, r8, c1, r7
mul r9, r9, r5
mad r7, r9, c2, r7
mul r10, r10, r6
mad r7, r10, c3, r7
min r7, r7, c4
add r7, r7, v0
texldp r3, t3, s3
mul r7, r7, r0
add r7, r7, r7
mul r2.a, r0.a, v0.a
mad r7.rgb, r3, r2.a, r7
mov r7.a, c6.a
mov oC0, r7
