#!/usr/bin/env python3
import argparse
from contextlib import contextmanager
import hashlib
import hmac
import ipaddress
import json
import os
from pathlib import Path
import select
import socket
import struct
import sys
import subprocess

COOKIE = 0x2112A442


def attr(kind, value):
    return struct.pack("!HH", kind, len(value)) + value + bytes(-len(value) % 4)


def xor_address(address):
    ip, port = address
    return struct.pack("!HHI", 1, port ^ (COOKIE >> 16),
                       int.from_bytes(socket.inet_aton(ip), "big") ^ COOKIE)


def parse(reply):
    kind, size, magic = struct.unpack_from("!HHI", reply)
    assert magic == COOKIE and size == len(reply) - 20
    fields = {}
    offset = 20
    while offset < len(reply):
        kind_attr, size_attr = struct.unpack_from("!HH", reply, offset)
        end = offset + 4 + size_attr
        assert end <= len(reply)
        fields[kind_attr] = (offset, reply[offset + 4:end])
        offset = (end + 3) & ~3
    assert offset == len(reply)
    return kind, fields


@contextmanager
def transport(args):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.connect((args.host, args.port))
        local_ip = client.getsockname()[0]
        if args.transport == "udp":
            def receive(seconds):
                client.settimeout(seconds)
                return client.recv(65535)

            yield local_ip, client.send, receive
            return

    try:
        ipaddress.ip_address(args.server_name)
    except ValueError:
        identity = ["-verify_hostname", args.server_name, "-servername", args.server_name]
    else:
        identity = ["-verify_ip", args.server_name]
    command = ["openssl", "s_client", "-dtls1_2", "-quiet", "-verify_return_error",
               *identity, "-CAfile", str(args.ca_file), "-connect", f"{args.host}:{args.port}"]
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as dtls:
        def send(payload):
            dtls.stdin.write(payload)
            dtls.stdin.flush()

        def receive(seconds):
            if not select.select([dtls.stdout], [], [], seconds)[0]:
                raise TimeoutError("DTLS response timed out")
            reply = os.read(dtls.stdout.fileno(), 65535)
            if not reply:
                raise ConnectionError("OpenSSL closed the DTLS connection")
            return reply

        try:
            yield local_ip, send, receive
        finally:
            if dtls.poll() is None:
                dtls.terminate()
            dtls.wait(timeout=5)


def exercise(args, credentials):
    username = credentials["username"].encode()
    realm = credentials["realm"].encode()
    password = credentials["password"].encode()
    key = hashlib.md5(b":".join((username, realm, password))).digest()

    def packet(method, body, signed=False):
        header = struct.pack("!HHI12s", method, len(body) + 24 * signed, COOKIE, os.urandom(12))
        return header + body + (attr(8, hmac.digest(key, header + body, "sha1")) if signed else b"")

    with transport(args) as (local_ip, send, receive), socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind((args.peer_bind or local_ip, 0))
        peer.settimeout(args.timeout)

        def exchange(request, expected, signed=False):
            send(request)
            reply = receive(args.timeout)
            kind, fields = parse(reply)
            assert kind == expected and reply[8:20] == request[8:20], (hex(kind), fields.get(9))
            if signed:
                offset, mac = fields[8]
                covered = reply[:2] + struct.pack("!H", offset + 24 - 20) + reply[4:offset]
                assert hmac.compare_digest(mac, hmac.digest(key, covered, "sha1"))
            return reply, fields

        requested_transport = attr(0x19, bytes((17, 0, 0, 0)))
        _, challenge = exchange(packet(3, requested_transport), 0x113)
        assert challenge[9][1][2:4] == bytes((4, 1)) and challenge[0x14][1] == realm
        nonce = challenge[0x15][1]
        assert nonce.startswith((b"obMatJos2gAAA", b"obMatJos2wAAA"))
        if getattr(args, "require_username", False):
            assert nonce.startswith(b"obMatJos2gAAA"), "Expected the Unfacd USERNAME challenge"
        identity = attr(0x1E, hashlib.sha256(username + b":" + realm).digest()) if nonce[9:10] == b"w" else attr(6, username)
        auth = identity + attr(0x14, realm) + attr(0x15, nonce)
        request = packet(3, requested_transport + auth, True)
        allocated, fields = exchange(request, 0x103, True)
        try:
            assert exchange(request, 0x103, True)[0] == allocated
            print("PASS signed Allocate and byte-identical retransmission", flush=True)
            _, port, ip = struct.unpack("!HHI", fields[0x16][1])
            relay = (socket.inet_ntoa((ip ^ COOKIE).to_bytes(4, "big")), port ^ (COOKIE >> 16))
            target = attr(0x12, xor_address(peer.getsockname()))
            indication = packet(0x16, target + attr(0x13, b"from-client"))
            send(indication)
            peer.settimeout(0.25)
            try:
                peer.recv(4096)
            except TimeoutError:
                pass
            else:
                raise AssertionError("Traffic relayed without permission")
            finally:
                peer.settimeout(args.timeout)

            exchange(packet(8, target + auth, True), 0x108, True)
            send(indication)
            assert peer.recvfrom(4096) == (b"from-client", relay)
            peer.sendto(b"from-peer", relay)
            kind, fields = parse(receive(args.timeout))
            assert kind == 0x17 and fields[0x13][1] == b"from-peer"
            assert fields[0x12][1] == xor_address(peer.getsockname())
            print("PASS permission gating and Send/Data in both directions", flush=True)

            exchange(packet(9, attr(0x0C, struct.pack("!HH", 0x4001, 0)) + target + auth, True),
                     0x109, True)
            send(struct.pack("!HH", 0x4001, 4) + b"chan")
            assert peer.recvfrom(4096) == (b"chan", relay)
            peer.sendto(b"back", relay)
            assert receive(args.timeout) == struct.pack("!HH", 0x4001, 4) + b"back"
            print("PASS ChannelData in both directions", flush=True)

            _, fields = exchange(packet(4, auth, True), 0x104, True)
            assert int.from_bytes(fields[0x0D][1], "big") > 0
            print("PASS signed Refresh", flush=True)
        finally:
            if not sys.exc_info()[0]:
                _, fields = exchange(packet(4, attr(0x0D, bytes(4)) + auth, True), 0x104, True)
                assert fields[0x0D][1] == bytes(4)
                print("PASS signed deallocation", flush=True)
            else:
                try:
                    exchange(packet(4, attr(0x0D, bytes(4)) + auth, True), 0x104, True)
                except Exception as error:
                    print(f"Cleanup after probe failure also failed: {error}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Exercise TURN relay over UDP or verified DTLS.")
    parser.add_argument("--host", required=True, help="IPv4 address of the TURN server")
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--credentials", required=True, type=Path)
    parser.add_argument("--transport", choices=("udp", "dtls"), default="udp")
    parser.add_argument("--server-name")
    parser.add_argument("--ca-file", type=Path)
    parser.add_argument("--peer-bind", help="IPv4 address for the peer socket")
    parser.add_argument("--timeout", type=float, default=3)
    args = parser.parse_args()
    if args.transport == "dtls" and not (args.server_name and args.ca_file):
        parser.error("DTLS requires --server-name and --ca-file")
    exercise(args, json.loads(args.credentials.read_text()))


if __name__ == "__main__":
    main()
