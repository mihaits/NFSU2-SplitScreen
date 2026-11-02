"""Compare this plugin's patch sites with another mod's, from the mod's public source on GitHub (read through gh, in memory).
python -I mod_overlap.py ExOptsTeam/NFSU2ExOpts nlgxzef/NFSU2Unlimiter [--names]
Per repo: byte overlaps, their patches within 0x20 of ours, and their patches on addresses we only read or call.
Their patches: lines with injector::MakeJMP/MakeCALL/MakeNOP/MakeRangedNOP/WriteMemory, with symbols resolved.
Ours: Write/WriteJump/WriteCall/... calls; #define constants are evaluated, so (0x7C2E2C + 0xC) counts.
--names prints a Markdown table of their names for addresses we use. Function pointers only; check a name before using it.
A clean result covers literal patches only: read the mod's hooks for what a patch does, and test in game."""
import json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
ADDR = r"0x0*([4-9A-Fa-f][0-9A-Fa-f]{5})\b"
sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def our_source():
    out = {}
    for d, _, names in os.walk(SRC):
        if "simcade" in d: continue
        for n in names:
            if n.endswith((".cpp", ".h")):
                out[os.path.join(d, n)] = open(os.path.join(d, n), encoding="utf-8", errors="replace").read()
    return out


def constants(text):
    defs, consts = {}, {}
    for t in text.values():
        for m in re.finditer(r"^\s*#define\s+([A-Za-z_]\w*)[ \t]+([^/\r\n]+)", t, re.M):
            defs[m.group(1)] = m.group(2).strip()
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*=\s*" + ADDR, t):
            consts.setdefault(m.group(1), int(m.group(2), 16))
    for _ in range(4):
        for k, v in defs.items():
            e = re.sub(r"0x[0-9A-Fa-f]+", lambda h: str(int(h.group(0), 16)), v)
            e = re.sub(r"[A-Za-z_]\w*", lambda w: str(consts.get(w.group(0), w.group(0))), e)
            if re.fullmatch(r"[\d\s+\-*()]+", e):
                try: consts[k] = eval(e, {"__builtins__": {}})
                except (SyntaxError, ZeroDivisionError): pass
    return consts


def our_sites():
    text = our_source()
    consts = constants(text)
    sites, refs = {}, {}
    call = re.compile(r"\b(\w*(?:Write|Patch|Jump|Hook|Nop|Tramp)\w*)\s*(?:<[^>]*>)?\s*\(([^;]*)")
    for f, t in text.items():
        for ln, line in enumerate(t.splitlines(), 1):
            if line.lstrip().startswith("//"): continue
            for m in call.finditer(line):
                arg = m.group(2).split(",")
                head = arg[0].strip()
                h = re.search(ADDR, head)
                a = int(h.group(1), 16) if h else None
                if a is None:
                    w = re.search(r"[A-Za-z_]\w*", head.split("+")[0])
                    if w and w.group(0) in consts:
                        a = consts[w.group(0)]
                        off = re.search(r"\+\s*(0x[0-9A-Fa-f]+|\d+)", head)
                        if off: a += int(off.group(1), 0)
                if a is None or not 0x400000 <= a < 0x900000: continue
                k = re.match(r"\s*(\d+)\s*\)?", arg[2]) if len(arg) >= 3 else None
                sites.setdefault(a, (int(k.group(1)) if k else 5, f"{os.path.basename(f)}:{ln}"))
    for f, t in text.items():
        for ln, line in enumerate(t.splitlines(), 1):
            code = line.split("//")[0]
            for m in re.finditer(ADDR, code):
                a = int(m.group(1), 16)
                if 0x401000 <= a < 0x900000 and a not in sites:
                    refs.setdefault(a, f"{os.path.basename(f)}:{ln}")
    for k, v in consts.items():
        if 0x401000 <= v < 0x900000 and v not in sites: refs.setdefault(v, k)
    return sites, refs


def gh(path, raw=False):
    cmd = ["gh", "api", path] + (["-H", "Accept: application/vnd.github.raw"] if raw else [])
    return subprocess.run(cmd, capture_output=True).stdout


def their_source(repo):
    tree = json.loads(gh(f"repos/{repo}/git/trees/HEAD?recursive=1"))["tree"]
    paths = [e["path"] for e in tree if e["type"] == "blob" and e["path"].endswith((".h", ".cpp"))
             and not e["path"].startswith("includes/")]
    with ThreadPoolExecutor(8) as ex:
        return dict(zip(paths, ex.map(lambda p: gh(f"repos/{repo}/contents/{p}", raw=True).decode("utf-8", "replace"), paths)))


def their_sites(data):
    syms = {}
    for t in data.values():
        for m in re.finditer(r"^\s*#define\s+(\w+)\s+" + ADDR, t, re.M): syms[m.group(1)] = int(m.group(2), 16)
        for m in re.finditer(r"\b(\w+)\s*=\s*\(?[\w\s\*\(\)]*?" + ADDR, t): syms.setdefault(m.group(1), int(m.group(2), 16))
    pat = re.compile(r"injector::(MakeRangedNOP|MakeNOP|MakeJMP|MakeCALL|WriteMemory\w*)\s*(?:<[^>]*>)?\s*\(\s*(\w+)\s*,\s*(\w+)?")
    sites = []
    for p, t in data.items():
        for ln, line in enumerate(t.splitlines(), 1):
            m = pat.search(line)
            if not m or line.lstrip().startswith("//"): continue
            kind, a0, a1 = m.groups()
            a = int(a0, 16) if a0.lower().startswith("0x") else syms.get(a0)
            if a is None: continue
            n = 5
            if kind == "MakeRangedNOP" and a1 and a1.lower().startswith("0x"): n = int(a1, 16) - a
            elif kind == "MakeNOP" and a1: n = int(a1, 0)
            elif kind.startswith("Write"): n = 1 if "BYTE" in line.split("(")[0] else 4
            sites.append((a, n, f"{p}:{ln}", line.strip()[:90]))
    return sorted(sites)


def their_names(data):
    names = {}
    for p, t in data.items():
        if "InGameFunctions" not in p: continue
        for m in re.finditer(r"\(\s*\w*\s*\*\s*(\w+)\s*\)\s*\([^)]*\)\s*=\s*\([^=;]*?\)\s*" + ADDR, t):
            names.setdefault(int(m.group(2), 16), m.group(1))
    return names


repos = [a for a in sys.argv[1:] if not a.startswith("--")]
if not repos: sys.exit(__doc__)
mine, refs = our_sites()
print(f"our patch sites: {len(mine)}, other addresses: {len(refs)}", flush=True)
found = {}
for repo in repos:
    data = their_source(repo)
    s = their_sites(data)
    print(f"\n=== {repo}: {len(data)} files, {len(s)} patches", flush=True)
    for title, test in (("byte overlaps", lambda a, n, b, m: a < b + m and b < a + n),
                        ("within 0x20", lambda a, n, b, m: not (a < b + m and b < a + n) and abs(a - b) <= 0x20 and a < 0x7A0000)):
        print("--", title)
        for a, n, where, code in s:
            for b, (m, w) in sorted(mine.items()):
                if test(a, n, b, m): print(f"{a:#x}+{n} {where}  {code}  <> ours {b:#x}+{m} {w}")
    print("-- on addresses we only read or call")
    for a, n, where, code in s:
        for r, w in sorted(refs.items()):
            if a < r + 10 and r < a + n: print(f"{a:#x}+{n} {where}  {code}  || {r:#x} {w}")
    found.update(their_names(data))
if "--names" in sys.argv:
    print("\n| Address | Name |\n|---|---|")
    for a in sorted(found):
        if a in refs or a in mine: print(f"| {a:#X} | {found[a]} |".replace("0X", "0x"))
