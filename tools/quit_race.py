"""Quit the running race to the main menu through the pause menu (P1 = virtual pad 1)."""
import os, sys, time; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, fe
g = t.Game()
fe.press(g, 1, 'start'); time.sleep(1.5)
for _ in range(3): fe.press(g, 1, 'right'); time.sleep(0.6)
fe.press(g, 1, 'a', 0.3); time.sleep(1.2); fe.press(g, 1, 'left', 0.2); time.sleep(0.6); fe.press(g, 1, 'a', 0.3)
time.sleep(25); print('after quit:', fe.top(g))
