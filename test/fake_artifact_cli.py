#!/usr/bin/env python3

import json
import os
from pathlib import Path
import shutil
import sys


def load_state():
    return json.loads(
        Path(os.environ["FAKE_ARTIFACT_STATE"]).read_text(encoding="utf-8"))


def save_state(state):
    Path(os.environ["FAKE_ARTIFACT_STATE"]).write_text(
        json.dumps(state), encoding="utf-8")


def run_gh(state, arguments):
    endpoint = arguments[-1]
    if endpoint.endswith("/zip"):
        sys.stdout.buffer.write(Path(state["archive"]).read_bytes())
        return 0
    if "/jobs?" in endpoint:
        payload = state["jobs"]
    elif "/artifacts?" in endpoint:
        payload = state["artifacts"]
    elif "/actions/runs/" in endpoint:
        payload = state["run"]
    else:
        return 2
    sys.stdout.write(json.dumps(payload))
    return 0


def option(arguments, name):
    index = arguments.index(name)
    return arguments[index + 1]


def run_az(state, arguments):
    if os.environ.get("AZURE_STORAGE_CONNECTION_STRING") != "fixture-secret":
        return 3
    source = Path(option(arguments, "--file"))
    blob = option(arguments, "--name")
    attempts = state.setdefault("upload_attempts", {})
    attempts[blob] = attempts.get(blob, 0) + 1
    save_state(state)
    if blob == state.get("fatal_blob"):
        print("AuthorizationFailure fixture-secret", file=sys.stderr)
        return 1
    if (blob == state.get("transient_blob") and
        attempts[blob] == 1):
        print("SSL UNEXPECTED_EOF fixture-secret", file=sys.stderr)
        return 1
    destination = Path(state["azure_root"], *blob.split("/"))
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    return 0


def main():
    state = load_state()
    mode = sys.argv[1]
    arguments = sys.argv[2:]
    if mode == "gh":
        return run_gh(state, arguments)
    if mode == "az":
        return run_az(state, arguments)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
