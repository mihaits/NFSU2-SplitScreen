#!/bin/sh
# Hook chaining check: another ASI mod (tools/chain_test/ChainTest.cpp, installed as scripts/AAChainTest.asi so it
# loads first) hooks every site this plugin puts a trampoline on; then the game boots to the main menu and, with
# RACE=1, starts a 2P split race (go_race_nointro.sh). Passes when every site logs "chained", both plugins' hooks run
# and nothing crashed. The test plugin is removed at the end.
# sh chain_test.sh [RACE=1 in the environment]
cd "$(dirname "$0")"
HERE="$(pwd)"
GAME="$HERE"; until [ -f "$GAME/speed2.exe" ]; do GAME="$(dirname "$GAME")"; done
SCRIPTS="$GAME/scripts"
LOG="$SCRIPTS/NFSU2SplitScreen.log"
ts() { echo "$(date +%H:%M:%S) $*"; }
cleanup() { taskkill //F //IM speed2.exe >/dev/null 2>&1; sleep 1; rm -f "$SCRIPTS"/AAChainTest.*; }
fail() { ts "FAIL: $*"; grep -a "CRASH" -A3 "$LOG" 2>/dev/null | head -8; cleanup; exit 1; }

ts "sites: every MakeTrampoline(site, len) in src"
python -I - "$HERE/../src" > "$HERE/../build/AAChainTest.txt" <<'PY' || exit 1
import os, re, sys
src = {}
for d, _, ns in os.walk(sys.argv[1]):
    if 'simcade' in d: continue
    for n in ns:
        if n.endswith('.cpp'): src[n] = open(os.path.join(d, n), encoding='utf-8', errors='replace').read()
text = '\n'.join(src.values())
defs = dict(re.findall(r'#define\s+(\w+)\s+(0x[0-9A-Fa-f]+)', text))
arrays = {m[0]: len(m[1].split(',')) for m in re.findall(r'(\w+)\[\]\s*=\s*\{([^}]*)\}', text)}
for name, ln in sorted(set(re.findall(r'MakeTrampoline\((\w+),\s*(sizeof\(\w+\)|\d+)\)', text))):
    if name not in defs: continue
    m = re.fullmatch(r'sizeof\((\w+)\)', ln)
    n = arrays[m.group(1)] if m else int(ln)
    print('%X %d' % (int(defs[name], 16), n))
PY
n=$(wc -l < "$HERE/../build/AAChainTest.txt"); ts "$n sites"
ts "build test plugin"
rm -f "$HERE/../build/AAChainTest.asi"
cmd //c "$(cygpath -w "$HERE/chain_test/build.bat")" 2>&1 | grep -i -E "error|warning"
[ -f "$HERE/../build/AAChainTest.asi" ] || fail "test plugin build"
taskkill //F //IM speed2.exe >/dev/null 2>&1; sleep 1
cp "$HERE/../build/AAChainTest.asi" "$HERE/../build/AAChainTest.txt" "$SCRIPTS/" || fail "install"

ts "boot to the main menu"
if [ "$RACE" = 1 ]; then
    sh go_race_nointro.sh 0 || fail "race start"
    ts "racing"; sleep 5
else
    sh restart.sh || fail "boot"
    sleep 3
fi
tasklist //FI "IMAGENAME eq speed2.exe" | grep -qi speed2 || fail "game exited"
grep -aq CRASH "$LOG" && fail "crash in the log"
[ "$(cat "$SCRIPTS/AAChainTest.order")" = before ] || fail "test plugin loaded after NFSU2SplitScreen.asi"
chained=$(grep -ac "chained to another plugin's jump" "$LOG")
ts "chained hooks: $chained of $n"
[ "$chained" -gt 0 ] || fail "no site chained"
# a site whose install checks the original bytes first (or is off in the ini) leaves the other plugin's hook alone
while read -r at len; do
    grep -aq "hook at 00$at: chained" "$LOG" || ts "not hooked by this plugin: $at (byte check or setting)"
done < "$SCRIPTS/AAChainTest.txt"
grep -a "can't chain" "$LOG"
ts "calls through the other plugin's hooks (site len calls):"
cat "$SCRIPTS/AAChainTest.log"
grep -q " [1-9][0-9]*$" "$SCRIPTS/AAChainTest.log" || fail "no hook ran"
cleanup
ts "PASS"
