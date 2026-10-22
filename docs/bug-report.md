# Bug reports

## Quick report
Copy this into an issue (or a chat). Short answers are fine; skip what you don't know.

```
What happened:
What you expected:
Players + AI:        (e.g. 3 + 2)
Race type / track:   (e.g. circuit, drift 4311, Free Run)
When:                (menu / intro / racing / results / restart / quit)
Steps:               1. ... 2. ... 3. ...
How often:           (every time / 1 in N / once)
First race after launching? Restarts or races before it:
Crash? Freeze?       (crash address from the Windows dialog or the log, if any)
Settings changed from default:  (ini, graphics options)
Other mods:          (WidescreenFix version, others)
Log:                 scripts\NFSU2SplitScreen.log (set Debug = 1 in the ini first)
Screenshot / video:
```

Things that help most:
- **What changes it.** "Works with 2 + 0 and 2 + 2, not with 3 + 0" narrows it down at once.
- **Whether it is new.** The last version it worked in, if you know it.
- **The game left as it is.** If the game is still open in the bad state, say so before closing it: its memory can be
  read while it is happening.

## Handoff (between work sessions)
When a session stops on a bug it couldn't solve, it writes this to `docs/bugs/<short-name>.md`, so a fresh session can
pick it up without the old conversation. When the bug is fixed, the findings go into the subsystem doc and the file is
deleted.

```
# <short name>
Status:       open / cause known / fixed in <commit>
Report:       the quick report above, as given
Repro:        exact steps and setup that reproduce it; how reliable
Area:         subsystem doc(s) involved
Known:        confirmed facts (addresses, values, what the log shows)
Ruled out:    each hypothesis tried and the evidence that killed it
Leads:        what to check next, best first
Tools:        commands / scripts used, and how to rerun them
```
