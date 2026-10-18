"""List the FE package cache (name, screen object, active package) and the game state. python -I packages.py"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fe, trainer as t
g = t.Game()
for name, obj, active in fe.packages(g): print(f'{name:40s} {obj:08x} {active:08x}')
print('game state', g.u32(0x8654A4), '(6 = racing)')
