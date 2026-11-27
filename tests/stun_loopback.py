"""Run an IPv4 STUN Binding exchange against a local ufsrvstun binary."""

import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import zlib


def main(binary):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
        reserve.bind(("127.0.0.1", 0))
        address = reserve.getsockname()

    with subprocess.Popen(
        [binary, "--port", str(address[1])], stderr=subprocess.PIPE, bufsize=0,
    ) as server:
        try:
            while True:
                if not select.select([server.stderr], [], [], 5)[0]:
                    raise RuntimeError("timed out waiting for server startup")
                line = server.stderr.readline()
                if not line:
                    raise RuntimeError("server exited during startup")
                if b"Listening on" in line:
                    break

            names = [
                (thread / "comm").read_text().strip()
                for thread in Path(f"/proc/{server.pid}/task").iterdir()
            ]
            assert names.count("ufUdpShard") == 2, names
            assert names.count("ufUdpWorker") == 4, names

            cookie = 0x2112A442
            for _ in range(16):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                    client.connect(address)
                    client.settimeout(1)
                    transaction = os.urandom(12)
                    client.send(struct.pack("!HHI12s", 1, 0, cookie, transaction))
                    reply = client.recv(128)
                    expected = struct.pack(
                        "!HHI12sHHBBHI", 0x0101, 12, cookie, transaction,
                        0x0020, 8, 0, 1, client.getsockname()[1] ^ (cookie >> 16),
                        int.from_bytes(socket.inet_aton("127.0.0.1"), "big") ^ cookie,
                    )
                    assert reply == expected, reply.hex()

            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.connect(address)
                client.settimeout(1)
                # A padded optional SOFTWARE attribute followed by FINGERPRINT.
                software = struct.pack("!HH4s", 0x8022, 3, b"stn")
                body = struct.pack("!HHI12s", 1, len(software) + 8, cookie, transaction) + software
                fingerprint = zlib.crc32(body) ^ 0x5354554E
                request = body + struct.pack("!HHI", 0x8028, 4, fingerprint)
                client.send(request)
                reply = client.recv(128)
                expected = struct.pack(
                    "!HHI12sHHBBHI", 0x0101, 20, cookie, transaction,
                    0x0020, 8, 0, 1, client.getsockname()[1] ^ (cookie >> 16),
                    int.from_bytes(socket.inet_aton("127.0.0.1"), "big") ^ cookie,
                )
                expected += struct.pack("!HHI", 0x8028, 4, zlib.crc32(expected) ^ 0x5354554E)
                assert reply == expected, reply.hex()

                client.settimeout(0.2)
                invalid_requests = (
                    b"invalid",
                    request[:-1] + bytes([request[-1] ^ 1]),
                    struct.pack("!HHI12sHH", 1, 4, cookie, transaction, 0x8022, 20),
                    struct.pack("!HHI12sHH", 1, 4, cookie, transaction, 0x0006, 0),
                )
                for invalid in invalid_requests:
                    client.send(invalid)
                    try:
                        client.recv(128)
                    except TimeoutError:
                        pass
                    else:
                        raise AssertionError(f"unexpected reply to {invalid.hex()}")
        finally:
            if server.poll() is None:
                server.terminate()
            try:
                status = server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                raise
            print(server.stderr.read().decode(), end="")
            assert status == 0, status

    print("PASS: 2 shards, 4 workers, 17 Binding replies including FINGERPRINT, 4 invalid requests dropped, clean shutdown")


if __name__ == "__main__":
    main(sys.argv[1])
