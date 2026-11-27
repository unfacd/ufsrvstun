#!/usr/bin/env bash
set -euo pipefail
umask 077

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 - "$script_dir/../../../ufsrvcorelib/dev/encrypt_plaintext/encrypt_plaintext.py" "$@" <<'PY'
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys

parser = argparse.ArgumentParser(description="Mint one standalone TURN user and an encrypted records file.")
parser.add_argument("--username", required=True)
parser.add_argument("--realm", required=True)
parser.add_argument("--key-file", type=Path, required=True, help="Server wrapping key file")
parser.add_argument("--output", type=Path, required=True, help="Encrypted records file to create")
parser.add_argument("--client-output", type=Path, required=True, help="Private JSON file for the client")
parser.add_argument("--expires-at", type=int, default=0, help="Unix timestamp, or 0 for no expiry")
parser.add_argument("--encrypt-tool", type=Path, default=Path(sys.argv[1]))
args = parser.parse_args(sys.argv[2:])

if not re.fullmatch(r"[A-Za-z0-9_.-]{1,508}", args.username) or args.username.startswith("#"):
    parser.error("--username must be 1–508 ASCII letters, digits, dots, underscores or hyphens")
if not re.fullmatch(r"[!-~]{1,763}", args.realm) or ":" in args.realm:
    parser.error("--realm must be 1–763 printable ASCII characters without spaces or colons")
if not 0 <= args.expires_at <= (1 << 63) - 1:
    parser.error("--expires-at is out of range")
if args.output.resolve() == args.client_output.resolve():
    parser.error("--output and --client-output must be different files")

password = secrets.token_urlsafe(32)
principal = secrets.token_hex(16)
material = f"{args.username}:{args.realm}:{password}".encode()
record = "\t".join((args.username, principal, hashlib.md5(material).hexdigest(),
                    hashlib.sha256(material).hexdigest(), str(args.expires_at)))
encrypted = subprocess.run(
    [sys.executable, str(args.encrypt_tool), "--credential-file", str(args.key_file)],
    input=record.encode(), stdout=subprocess.PIPE, check=True,
).stdout.decode()
envelopes = [line.removeprefix("VALUE=") for line in encrypted.splitlines() if line.startswith("VALUE=")]
if len(envelopes) != 1 or not re.fullmatch(r"enc:v1:[0-9a-f]{24}:[0-9a-f]+:[0-9a-f]{32}", envelopes[0]):
    parser.exit(1, "Encryption tool did not return an enc:v1 envelope\n")

created = []
try:
    for path, content in (
        (args.client_output, json.dumps({"username": args.username, "realm": args.realm,
                                        "password": password, "expires_at": args.expires_at}) + "\n"),
        (args.output, envelopes[0] + "\n"),
    ):
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        created.append(path)
        with os.fdopen(fd, "w") as stream:
            stream.write(content)
except OSError as error:
    for path in created:
        path.unlink()
    parser.exit(1, f"{error}\n")
PY
