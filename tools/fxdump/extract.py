"""Extract the game's compiled effects (PE resources of type 10, IDI_*_FX) from speed2.exe into a folder.

    python extract.py [out_dir]      (default: tools/fxdump/build/fx)

Then: build/fxdump.exe dis build/fx/IDI_WORLDHEADLIGHT_FX.bin"""
import os
import sys

import pefile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from paths import EXE  # noqa: E402

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'fx')
os.makedirs(out, exist_ok=True)
pe = pefile.PE(EXE)
for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    for n in t.directory.entries:
        name = str(n.name) if n.name else str(n.id)
        if not name.endswith('_FX'):
            continue
        for lang in n.directory.entries:
            s = lang.data.struct
            with open(os.path.join(out, name + '.bin'), 'wb') as f:
                f.write(pe.get_data(s.OffsetToData, s.Size))
            print(f'{name}.bin {s.Size} bytes')
