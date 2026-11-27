#!/usr/bin/env python3
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
from types import SimpleNamespace
from urllib.error import HTTPError

from api_turn_relay import endpoints, fetch_credentials
from turn_relay import COOKIE, attr, parse, transport

PORT_OCCUPIED = """
import errno, socket, sys
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
    try:
        probe.bind((sys.argv[1], int(sys.argv[2])))
    except OSError as error:
        if error.errno == errno.EADDRINUSE:
            sys.exit(1)
        raise
"""


def bound(args, port):
    result = subprocess.run(["docker", "exec", "-u", "ufsrv", args.container, "python3",
                             "-c", PORT_OCCUPIED, args.relay_bind, str(port)],
                            check=False, capture_output=True)
    if result.returncode == 1 and not result.stderr:
        return True
    result.check_returncode()
    return False


def run_expiry(args, protocol, endpoint, credentials):
    host, port = endpoint
    options = SimpleNamespace(host=host, port=port, transport=protocol, ca_file=args.turn_ca_file,
                              server_name=host)
    identity = credentials["username"].encode()
    realm = args.realm.encode()
    key = hashlib.md5(b":".join((identity, realm, credentials["credential"].encode()))).digest()
    requested = attr(0x19, bytes((17, 0, 0, 0)))

    def allocate(body, signed=False):
        header = struct.pack("!HHI12s", 3, len(body) + 24 * signed, COOKIE, os.urandom(12))
        return header + body + (attr(8, hmac.digest(key, header + body, "sha1")) if signed else b"")

    with transport(options) as (_, send, receive):
        request = allocate(requested)
        send(request)
        reply = receive(args.timeout)
        kind, fields = parse(reply)
        assert kind == 0x113 and reply[8:20] == request[8:20]
        assert fields[9][1][2:4] == bytes((4, 1)) and fields[0x14][1] == realm
        assert fields[0x15][1].startswith(b"obMatJos2gAAA")
        request = allocate(requested + attr(6, identity) + attr(0x14, realm) + attr(0x15, fields[0x15][1]), True)
        sent_at = time.monotonic()
        send(request)
        reply = receive(args.timeout)
        received_at = time.monotonic()
        kind, fields = parse(reply)
        assert kind == 0x103 and reply[8:20] == request[8:20], (hex(kind), fields.get(9))
        offset, mac = fields[8]
        covered = reply[:2] + struct.pack("!H", offset + 4) + reply[4:offset]
        assert hmac.compare_digest(mac, hmac.digest(key, covered, "sha1"))
        _, relay_port, _ = struct.unpack("!HHI", fields[0x16][1])
        relay_port ^= COOKIE >> 16
        lifetime = int.from_bytes(fields[0x0D][1], "big")
        assert lifetime > 0 and bound(args, relay_port)
        print(f"{protocol.upper()}: relay port {relay_port} allocated for {lifetime}s", flush=True)

        time.sleep(max(0, sent_at + lifetime - 1 - time.monotonic()))
        assert bound(args, relay_port), f"{protocol.upper()}: relay disappeared before its lifetime"
        deadline = received_at + lifetime + args.timeout
        while bound(args, relay_port):
            assert time.monotonic() < deadline, f"{protocol.upper()}: relay socket still bound after expiry"
            time.sleep(0.2)
        print(f"PASS {protocol.upper()}: relay port released after natural expiry", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Verify natural allocation expiry on the running Docker service.")
    parser.add_argument("--api-url", required=True)
    parser.add_argument("--auth-file", required=True, type=Path)
    parser.add_argument("--api-ca-file", type=Path)
    parser.add_argument("--realm", required=True)
    parser.add_argument("--turn-ca-file", required=True, type=Path)
    parser.add_argument("--container", default="ufsrvstun")
    parser.add_argument("--relay-bind", required=True, help="Relay socket's actual IPv4 bind address inside Docker")
    parser.add_argument("--timeout", type=float, default=4)
    args = parser.parse_args()
    try:
        credentials = fetch_credentials(args)
        targets = endpoints(credentials["urls"])
        if not {"udp", "dtls"} <= targets.keys():
            parser.error("API must advertise UDP and DTLS")
    except HTTPError as error:
        parser.exit(1, f"API credential request failed with HTTP {error.code}\n")
    with ThreadPoolExecutor(max_workers=2) as pool:
        tasks = [pool.submit(run_expiry, args, protocol, targets[protocol], credentials)
                 for protocol in ("udp", "dtls")]
        for task in tasks:
            task.result()


if __name__ == "__main__":
    main()
