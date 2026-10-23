"""Game folder lookup for the tools: the nearest parent of this folder that holds speed2.exe (so the repo can be checked
out anywhere inside the game folder)."""
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def _find_game():
    d = HERE
    while True:
        if os.path.isfile(os.path.join(d, 'speed2.exe')):
            return d
        up = os.path.dirname(d)
        if up == d:
            raise RuntimeError('speed2.exe not found in any parent of ' + HERE)
        d = up


GAME = _find_game()
EXE = os.path.join(GAME, 'speed2.exe')
LOG = os.path.join(GAME, 'scripts', 'NFSU2SplitScreen.log')
