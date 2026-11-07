#!/bin/sh
# Restart the game and start a split Outrun (PLAYERS=n, default 2; the ini ExtraPlayers must match) without the intro. Then: autorace.py --players 1 lets P1 pull away.
cd "$(dirname "$0")"
sh restart.sh --noskip
python -I boot.py 60 || exit 1
python -I -c "import sys;sys.path.insert(0,'.');import trainer as t;g=t.Game();g.write(0x42FDA0,bytes([0x32,0xC0,0xC2,0x04,0x00]),code=True)"
python -I rec.py play start_to_split2p --from 3 --to 5 >/dev/null || exit 1
RIGHTS=6 VIRTUAL=${PLAYERS:-2} python -I freerun_flow.py
