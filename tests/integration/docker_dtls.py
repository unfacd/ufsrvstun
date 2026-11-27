#!/usr/bin/env python3
import argparse
import ipaddress
import json
from pathlib import Path
import select
import subprocess
import sys
import tempfile

FIXTURE = r'''
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

root = Path("/opt/ufsrv/ufsrvstun")
host = sys.argv[1]
config = (root / "etc/ufsrvstun-1.lua").read_text()
nonce = re.search(r'(?m)^\s*nonce_secret\s*=\s*"(enc:v1:[0-9a-f:]+)"', config)
if not nonce:
    raise SystemExit("The deployed config must contain an encrypted nonce secret")

with tempfile.TemporaryDirectory(prefix="stun-dtls-test-") as directory:
    fixture = Path(directory)
    cert, key = fixture / "server.pem", fixture / "server.key"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                    "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as plain,          socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as secure:
        for sock in (plain, secure):
            sock.bind((host, 0))
        plain_port, secure_port = plain.getsockname()[1], secure.getsockname()[1]
    fixture_config = fixture / "stun.lua"
    realm = json.dumps(sys.argv[2])
    fixture_config.write_text(f"""return {{
      ufsrv = {{ufsrv_geogroup = 0, sys_runtime = {{}}, main_listener_port = {plain_port},
                main_listener_address = "{host}"}},
      ufsrvstun = {{
        is_enabled = true, shard_count = 1, workers_per_shard = 1,
        turn = {{is_enabled = true, realm = {realm},
                 credential_file = "turn-credentials.enc", nonce_secret = "{nonce[1]}"}},
        dtls = {{is_enabled = true, port = {secure_port},
                 certificate_chain_file = "{cert}", private_key_file = "{key}"}}
      }}
    }}""")
    log = fixture / "stun.log"
    env = dict(os.environ, CREDENTIALS_DIRECTORY=str(root / "etc/secrets"))
    with log.open("w") as errors, subprocess.Popen(
        [str(root / "bin/ufsrvstun"), "-C", str(root), "-c", str(fixture_config)],
        env=env, stdout=errors, stderr=errors,
    ) as server:
        try:
            deadline = time.monotonic() + 8
            while "Listening on" not in log.read_text():
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise SystemExit(log.read_text())
                time.sleep(.05)
            print(json.dumps({"port": secure_port, "certificate": cert.read_text()}), flush=True)
            sys.stdin.buffer.read(1)
        finally:
            if server.poll() is None:
                server.terminate()
            server.wait(timeout=5)
'''


def main():
    parser = argparse.ArgumentParser(description="Run a temporary DTLS relay in the STUN Docker container.")
    parser.add_argument("--container", default="ufsrvstun")
    parser.add_argument("--host", required=True, help="Reachable IPv4 address of the container")
    parser.add_argument("--credentials", required=True, type=Path)
    args = parser.parse_args()
    try:
        args.host = str(ipaddress.IPv4Address(args.host))
    except ValueError as error:
        parser.error(str(error))
    credentials = json.loads(args.credentials.read_text())
    if not credentials["realm"].isascii() or not credentials["realm"].isprintable():
        parser.error("Docker fixture requires a printable ASCII realm")
    with tempfile.TemporaryDirectory(prefix="stun-dtls-ca-") as directory, subprocess.Popen(
        ["docker", "exec", "-i", "-u", "ufsrv", args.container, "python3", "-u", "-c",
         FIXTURE, args.host, credentials["realm"]],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    ) as fixture:
        try:
            if not select.select([fixture.stdout], [], [], 15)[0]:
                raise TimeoutError("Docker DTLS fixture did not become ready")
            line = fixture.stdout.readline()
            if not line:
                raise RuntimeError("Docker DTLS fixture exited before reporting readiness")
            info = json.loads(line)
            ca_file = Path(directory) / "ca.pem"
            ca_file.write_text(info["certificate"])
            subprocess.run([sys.executable, str(Path(__file__).with_name("turn_relay.py")),
                            "--host", args.host, "--port", str(info["port"]), "--transport", "dtls",
                            "--server-name", "localhost", "--ca-file", str(ca_file),
                            "--credentials", str(args.credentials)], check=True)
        finally:
            fixture.stdin.close()
            fixture.wait(timeout=10)


if __name__ == "__main__":
    main()
