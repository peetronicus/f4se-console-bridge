# Runtime support and provenance

The first adapter supports only Fallout 4 Steam 1.11.240.0, SHA-256
`fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8`.
Both F4SE's runtime identifier and an on-disk executable hash must match before
the plugin installs an observer or dispatches commands. This deliberately does
not claim compatibility with unseen updates or modified executables.

| Fact | Source |
|---|---|
| Plugin interface and game-task ownership | Official F4SE 0.7.9 `PluginAPI.h`, `GameThreads.h`, `Hooks_Threads.cpp` |
| Console dispatcher takes `const char*` | [CommonLibF4 Console API](https://github.com/libxse/commonlibf4/blob/main/include/RE/C/Console.h) |
| Dispatcher RVA `0x01036250` | Independently traced in the supported executable: the console's `executeCommand` registration leads to its string handler, which tail-calls this function |
| Dispatcher semantics | It queues compiled script work; its return is not execution success |
| Console output RVA `0x0103C250` | Official F4SE 0.7.9 `GameAPI.h` (`ConsoleManager::Print`) |
| Observer's 14-byte relocation span | Decoded instruction boundaries in the supported executable; three stack-only instructions, no RIP-relative operand or branch |

The public source records addresses, identities, and derivations only, not game
instruction bytes. The observer copies instructions from the running process
into a private trampoline and forwards every call to the original body.

Only our bridge source is published. The development build of F4SE was compiled
unmodified from the pinned official source with its `common` dependency at
`64e233c096735551f6ac9a773726a8a3960e46cd`; those components are not part of this
repository or a plugin release.

## Verification

The focused protocol check was demonstrated red with the intentional
multi-command-rejection fault, then green without it. The plugin built with
MSVC 19.51 and loaded successfully under F4SE 0.7.9 in one native session.
Without keyboard input, `GetF4SEVersion` returned version 0.7.9; after freezing
time and setting `GameHour` to 12.25, `GetGlobalValue GameHour` returned 12.25.
This verifies dispatch and observable state change for that session, not every
possible console command. The game was closed and temporary installation/profile
changes restored after the check. No game output or artifacts are redistributed.

Command changes never require recompilation. Supporting a new **game runtime**
does require independent address and instruction-boundary verification and a new
adapter build. Do not change the version/hash gate merely to make a DLL load.
