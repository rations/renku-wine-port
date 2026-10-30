# Wine tests for the RenkuOS/Haiku patches

Small Windows programs that check the behaviour copy protection depends on. Each one prints
PASS or FAIL, or what it found. Build them on any machine with a MinGW-w64 cross compiler (the
build line is at the top of each file), then run them with `wine` on RenkuOS or Haiku. For comparison,
run them on Linux or Windows too.

| Test | Checks | Needs |
|---|---|---|
| `traptest.c` | A single step (trap flag) and an `int3` reach a vectored exception handler. | A kernel that delivers user traps as SIGTRAP |
| `hwbptest.c` | Hardware breakpoints set with `SetThreadContext`: on the current thread, on another suspended thread, and a write watchpoint. | Patch 17 and a kernel with `THREAD_SET_DEBUG_REGISTERS` |
| `probetest.c` + `probe.S` | PACE's breakpoint probe, copied instruction for instruction: every call must trap, after sleeps, exceptions, threads, file I/O, a guard page, crypto and suspension by another thread. Also prints the DR6 value the program reads back (Windows: `0xffff0fff`). | Same as `hwbptest` |
| `loadvst.c` | Loads a VST3 the way a plug-in host does (`LoadLibraryW`, then `GetPluginFactory`), so licence checks in `DllMain` can be tested without a DAW. | A VST3 plug-in |
| `voltest.c` | What `GetDriveType` and `GetVolumeInformation` return for drive roots. | — |

`probetest` accepts part of an activity name to run only that one, for example
`wine probetest.exe suspended`. On Linux it is slow (several minutes), because Wine sets debug
registers there through ptrace.
