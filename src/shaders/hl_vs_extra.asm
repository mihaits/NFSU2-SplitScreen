// Appended to the game's headlight vertex shaders (vs_1_1, HeadlightWVP in c4..c7): the other players' projector
// coordinates q_k = q * T_k, q = pos * HeadlightWVP (the view's own projector), T_k in c20..c31 (columns).
vs_1_1
dcl_position v0
dp4 r10.x, v0, c4
dp4 r10.y, v0, c5
dp4 r10.z, v0, c6
dp4 r10.w, v0, c7
dp4 oT4.x, r10, c20
dp4 oT4.y, r10, c21
dp4 oT4.z, r10, c22
dp4 oT4.w, r10, c23
dp4 oT5.x, r10, c24
dp4 oT5.y, r10, c25
dp4 oT5.z, r10, c26
dp4 oT5.w, r10, c27
dp4 oT6.x, r10, c28
dp4 oT6.y, r10, c29
dp4 oT6.z, r10, c30
dp4 oT6.w, r10, c31
