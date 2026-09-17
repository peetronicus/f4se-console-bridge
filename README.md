# F4SE Console Bridge

A standalone, local-file command bridge for Fallout 4. Send ordinary console
commands from Python or any program that can write files. No console key,
foreground window, network listener, ESP, Papyrus script, or command recompilation
is required.

Experimental: initially supports the Steam Fallout 4 **1.11.240.0** executable
identified in [runtime support](docs/runtime-support.md), with **F4SE 0.7.9**.
Other executable identities are refused, including other builds with the same
version label. Live verification is not yet complete.

## Safety and semantics

- This is arbitrary **Fallout console execution**, not a shell or remote desktop.
  There is no command allowlist. Commands can modify saves, change the world, or
  quit the game. Back up saves and send only trusted commands.
- The mailbox is under the current user's Local AppData, in a new unpredictable
  directory per game process. It trusts that user's local processes. Do not share
  the directory over a network or grant untrusted users write access.
- Exactly one request is dispatched at a time through F4SE's game task queue.
  Each request is one line, at most 1023 UTF-8 bytes. `--file` sends lines serially.
- `status: dispatched` means the native console dispatcher returned. **It does not
  mean the game accepted or finished the command.** `commandSuccess` is always
  null: inspect game readback/output to establish the result.
- Replies include console output observed during dispatch and the following
  1000 milliseconds, capped at 64 KiB with an explicit truncation flag. Fallout
  may execute work asynchronously. Other scripts/mods can print during that same
  window, and later output is not included. Output is a byte-preserving Latin-1
  JSON representation; it is not falsely attributed as exclusively this request's.
- A 30-second dispatch timeout disables further requests for that session. A
  client timeout is an unknown outcome, never a reason to retry automatically.
- An in-memory observer forwards console output unchanged. No game executable
  is modified on disk. No settings or commands run automatically on startup.
- This is diagnostic instrumentation, not a performance-neutral game baseline.

## Install and use

1. Install the matching [official F4SE](https://f4se.silverlock.org/) yourself.
2. Build this project and copy only `F4SEConsoleBridge.dll` into
   `Fallout 4/Data/F4SE/Plugins/`.
3. Launch using `f4se_loader.exe`, then list sessions:

```powershell
python bridge.py --list
python bridge.py --session "<exact session directory>" --command "GetF4SEVersion"
python bridge.py --session "<exact session directory>" --command "player.getpos x"
python bridge.py --session "<exact session directory>" --file commands.txt
```

The client prints JSON replies and removes consumed response files. It leaves
unanswered requests intact for diagnosis. Session records may outlive the game;
choose the process you started, not simply the newest directory. To uninstall,
exit Fallout and remove this plugin DLL. The plugin creates no save records.
Old mailbox directories may be deleted **after their game process exits**.

## Build

Requires Windows x64, CMake 3.24+, Visual Studio C++ build tools, and an unmodified
[F4SE source checkout](https://github.com/ianpatt/f4se) at commit
`6f6a7caa9aeaebc957a70d934b6ab6b80eb768b2` (0.7.9).
No F4SE source or binary is redistributed here. Its headers supply the plugin
and task interfaces; the plugin does not link the full F4SE implementation.

```powershell
$env:F4SE_SOURCE_ROOT = "<official F4SE source checkout>"
.\build.cmd Release
.\build\Release\bridge_protocol_test.exe --red-control
# Expected failure: multi-command request rejection.
.\build\Release\bridge_protocol_test.exe
# Expected PASS.
```

Output: `build/Release/F4SEConsoleBridge.dll`. Tests are synthetic and need no
game installation. The red control must fail before accepting the green result.

## License

This project's own source is MIT licensed; see [LICENSE](LICENSE).
F4SE remains under its authors' terms, including their public-plugin-source
requirement. F4SE and the game must be obtained separately from their authors.
No game code, data, binaries, or disassembly are included.
