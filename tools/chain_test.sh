#!/bin/sh
# Hook chaining check: another ASI mod (tools/chain_test/ChainTest.cpp, installed as scripts/AAChainTest.asi so it
# loads first) hooks every site this plugin puts a trampoline on (a jmp at the function start) and redirects every
# call this plugin replaces (WriteCall), the way other mods do. Stages:
#   1. boot without the test plugin to the main menu: the plugin's "call sites:" log lines give the call list
#   2. boot with the test plugin to the main menu and, with RACE=1, start a 2P split race (go_race_nointro.sh)
# Passes when nothing crashed, every trampoline site and every redirected call logs "chained", and the other mod's
# hooks run. Lists the trampoline sites this plugin leaves alone (an install that checks the original bytes first).
# The test plugin is removed at the end.  sh chain_test.sh   (RACE=1 in the environment for the race)
cd "$(dirname "$0")"
HERE="$(pwd)"
GAME="$HERE"; until [ -f "$GAME/speed2.exe" ]; do GAME="$(dirname "$GAME")"; done
SCRIPTS="$GAME/scripts"
LOG="$SCRIPTS/NFSU2SplitScreen.log"
BUILD="$HERE/../build"
ts() { echo "$(date +%H:%M:%S) $*"; }
cleanup() { taskkill //F //IM speed2.exe >/dev/null 2>&1; sleep 1; rm -f "$SCRIPTS"/AAChainTest*; }
fail() { ts "FAIL: $*"; grep -a "CRASH" -A3 "$LOG" 2>/dev/null | head -8; cleanup; exit 1; }
alive() { tasklist //FI "IMAGENAME eq speed2.exe" | grep -qi speed2; }

ts "trampoline sites: every MakeTrampoline(site, len) in src"
python -I - "$HERE/../src" > "$BUILD/AAChainTest.txt" <<'PY' || exit 1
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
ts "$(wc -l < "$BUILD/AAChainTest.txt") trampoline sites"
ts "build test plugin"
rm -f "$BUILD/AAChainTest.asi"
cmd //c "$(cygpath -w "$HERE/chain_test/build.bat")" 2>&1 | grep -i -E "error|warning"
[ -f "$BUILD/AAChainTest.asi" ] || fail "test plugin build"

ts "stage 1: boot without the test plugin"
rm -f "$SCRIPTS"/AAChainTest*
sh restart.sh || fail "boot without the test plugin"
alive || fail "game exited"
grep -aq CRASH "$LOG" && fail "crash in the log"
grep -a "call sites:" "$LOG" | sed 's/.*call sites://' | tr ' ' '\n' | grep . > "$BUILD/AAChainTest_calls.txt"
ncalls=$(wc -l < "$BUILD/AAChainTest_calls.txt")
[ "$ncalls" -gt 0 ] || fail "no call sites in the log (Debug = 1 in the installed ini?)"
ts "$ncalls call sites; chained to the mods already installed (WidescreenFix's camera, ...):"
grep -a "chained to another plugin" "$LOG" | sed 's/^ *[0-9]* /    /'

ts "stage 2: boot with the test plugin"
taskkill //F //IM speed2.exe >/dev/null 2>&1; sleep 1
cp "$BUILD/AAChainTest.asi" "$BUILD/AAChainTest.txt" "$BUILD/AAChainTest_calls.txt" "$SCRIPTS/" || fail "install"
if [ "$RACE" = 1 ]; then
    sh go_race_nointro.sh 0 || fail "race start"
    ts "racing"; sleep 5
else
    sh restart.sh || fail "boot"
    sleep 3
fi
alive || fail "game exited"
grep -aq CRASH "$LOG" && fail "crash in the log"
[ "$(cat "$SCRIPTS/AAChainTest.order")" = before ] || fail "test plugin loaded after NFSU2SplitScreen.asi"
sleep 1
REPORT="$SCRIPTS/AAChainTest.log"
jmps=$(grep -ac "hook at .*chained to another plugin's jump" "$LOG")
ts "trampoline sites chained: $jmps of $(grep -c '^jmp' "$REPORT")"
[ "$jmps" -gt 0 ] || fail "no trampoline site chained"
grep '^jmp' "$REPORT" | while read -r _ at _ _; do
    grep -aq "hook at $at: chained" "$LOG" || ts "  left alone: $at (its install checks the original bytes, or a setting leaves it off)"
done
grep -a "can't chain" "$LOG"
redirected=$(grep -c '^call [0-9A-F]* 5 ' "$REPORT")
calls=$(grep '^call [0-9A-F]* 5 ' "$REPORT" | while read -r _ at _ _; do grep -aq "call at $at: chained" "$LOG" && echo "$at"; done | wc -l)
ts "calls chained: $calls of $redirected redirected"
missing=$(grep '^call [0-9A-F]* 5 ' "$REPORT" | while read -r _ at _ _; do grep -aq "call at $at: chained" "$LOG" || echo "$at"; done)
[ -z "$missing" ] || fail "redirected calls not chained: $(echo $missing)"
ran=$(grep -c ' [1-9][0-9]*$' "$REPORT")
ts "sites whose other-mod hook ran: $ran (site len calls below)"
cat "$REPORT"
[ "$ran" -gt 0 ] || fail "no hook ran"
cleanup
ts "PASS"
