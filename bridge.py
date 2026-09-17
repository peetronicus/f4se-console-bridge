"""Local-file client for F4SE Console Bridge. Standard library only."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import time
import uuid


def execute(session: Path, command: str, timeout: float = 35) -> dict:
    session = session.resolve(strict=True)
    status = json.loads((session / "session.json").read_text(encoding="utf-8"))
    if status.get("protocol") != 1 or status.get("status") != "ready":
        raise RuntimeError("Selected session is not ready")
    payload = command.encode("utf-8")
    if not payload or len(payload) > 1023 or not command.strip() or any(x < 32 or x == 127 for x in payload):
        raise ValueError("Expected one nonempty command, at most 1023 UTF-8 bytes, no control characters")
    request_id = uuid.uuid4().hex
    partial = session / (request_id + ".partial")
    request = session / (request_id + ".request")
    response = session / (request_id + ".response.json")
    with partial.open("xb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    partial.rename(request)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if response.exists():
            result = json.loads(response.read_text(encoding="utf-8"))
            if result.get("id") != request_id:
                raise RuntimeError("Response identity mismatch")
            response.unlink()
            return result
        time.sleep(0.05)
    # Do not resubmit or remove a possibly executing command.
    raise TimeoutError(f"Outcome unknown; do not automatically retry. Request id: {request_id}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path, help="Exact session directory from --list")
    parser.add_argument("--list", action="store_true", help="List session records (old processes may have exited)")
    parser.add_argument("--command", action="append", default=[], help="One Fallout console command; repeat for serial commands")
    parser.add_argument("--file", type=Path, help="UTF-8 file of commands, one per nonempty line, sent serially")
    parser.add_argument("--timeout", type=float, default=35)
    args = parser.parse_args()
    if args.list:
        root = Path(os.environ["LOCALAPPDATA"]) / "F4SEConsoleBridge"
        for path in sorted(root.glob("*/session.json")):
            print(json.dumps({"session": str(path.parent), **json.loads(path.read_text(encoding="utf-8"))}))
        return 0
    commands = args.command
    if args.file:
        commands += [line for line in args.file.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    if not args.session or not commands or args.timeout <= 0:
        parser.error("Specify --session and --command or --file, with a positive timeout")
    for command in commands:
        result = execute(args.session, command, args.timeout)
        print(json.dumps(result, ensure_ascii=True), flush=True)
        if result.get("status") != "dispatched":
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
