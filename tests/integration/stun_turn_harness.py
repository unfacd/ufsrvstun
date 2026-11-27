#!/usr/bin/env python3
"""
stun_turn_harness.py - end-to-end STUN/TURN server test harness (stdlib only, Python 3.8+)

Covers: RFC 5389/8489 (STUN), RFC 5766/8656 (TURN), RFC 6062 (TCP allocations),
        RFC 6156 (IPv6), RFC 5780 (NAT behaviour discovery), ICE attributes (RFC 8445),
        long-term / REST-ephemeral credentials, UDP / TCP / TLS control transports.

The headline test is  e2e.two_peer_relay : two independent clients each get a relay
allocation, authorise each other, and exchange RTP-shaped media (Send/Data indications
AND ChannelData, both directions, concurrently).  Every packet carries a deterministic
payload so corruption/loss/reordering/latency/jitter are measured, and the source
address reported by the server (XOR-PEER-ADDRESS / channel) is checked to prove the
packets really crossed the relay.

Examples
  python stun_turn_harness.py --host turn.example.com -u alice -p secret
  python stun_turn_harness.py --host 10.0.0.5 --port 3478 --secret 'static-auth-secret'   # coturn use-auth-secret
  python stun_turn_harness.py --host turn.example.com --transport tls --port 5349 -u a -p b --insecure
  python stun_turn_harness.py --host 127.0.0.1 -u a -p b --tests e2e --packets 1000 --pps 100 --payload-size 800
  python stun_turn_harness.py --host x -u a -p b --rfc6062 --json report.json -v
  python stun_turn_harness.py --list
"""
import argparse, base64, collections, fnmatch, hashlib, hmac, ipaddress, json, math, os
import random, socket, ssl, struct, sys, threading, time, traceback, zlib

# --------------------------------------------------------------------------- constants
MAGIC = 0x2112A442
REQ, IND, SUCCESS, ERROR = 0x000, 0x010, 0x100, 0x110
CLS_NAME = {REQ: "request", IND: "indication", SUCCESS: "success", ERROR: "error"}
M_BINDING, M_ALLOCATE, M_REFRESH, M_SEND, M_DATA = 0x001, 0x003, 0x004, 0x006, 0x007
M_CREATE_PERM, M_CHANNEL_BIND, M_CONNECT, M_CONN_BIND, M_CONN_ATTEMPT = 0x008, 0x009, 0x00A, 0x00B, 0x00C
M_NAME = {M_BINDING: "Binding", M_ALLOCATE: "Allocate", M_REFRESH: "Refresh", M_SEND: "Send", M_DATA: "Data",
          M_CREATE_PERM: "CreatePermission", M_CHANNEL_BIND: "ChannelBind", M_CONNECT: "Connect",
          M_CONN_BIND: "ConnectionBind", M_CONN_ATTEMPT: "ConnectionAttempt"}

_ATTRS = dict(
    MAPPED_ADDRESS=0x0001, CHANGE_REQUEST=0x0003, USERNAME=0x0006, MESSAGE_INTEGRITY=0x0008, ERROR_CODE=0x0009,
    UNKNOWN_ATTRIBUTES=0x000A, CHANNEL_NUMBER=0x000C, LIFETIME=0x000D, XOR_PEER_ADDRESS=0x0012, DATA=0x0013,
    REALM=0x0014, NONCE=0x0015, XOR_RELAYED_ADDRESS=0x0016, REQUESTED_ADDRESS_FAMILY=0x0017, EVEN_PORT=0x0018,
    REQUESTED_TRANSPORT=0x0019, DONT_FRAGMENT=0x001A, MESSAGE_INTEGRITY_SHA256=0x001C, PASSWORD_ALGORITHM=0x001D,
    USERHASH=0x001E, XOR_MAPPED_ADDRESS=0x0020, RESERVATION_TOKEN=0x0022, PRIORITY=0x0024, USE_CANDIDATE=0x0025,
    PADDING=0x0026, RESPONSE_PORT=0x0027, CONNECTION_ID=0x002A, ADDITIONAL_ADDRESS_FAMILY=0x8000,
    ADDRESS_ERROR_CODE=0x8001, PASSWORD_ALGORITHMS=0x8002, ALTERNATE_DOMAIN=0x8003, ICMP=0x8004,
    SOFTWARE=0x8022, ALTERNATE_SERVER=0x8023, CACHE_TIMEOUT=0x8027, FINGERPRINT=0x8028, ICE_CONTROLLED=0x8029,
    ICE_CONTROLLING=0x802A, RESPONSE_ORIGIN=0x802B, OTHER_ADDRESS=0x802C)
for _k, _v in _ATTRS.items():
    globals()["A_" + _k] = _v
A_NAME = {v: k for k, v in _ATTRS.items()}
ADDR_ATTRS = {A_MAPPED_ADDRESS, A_XOR_MAPPED_ADDRESS, A_XOR_PEER_ADDRESS, A_XOR_RELAYED_ADDRESS,
              A_RESPONSE_ORIGIN, A_OTHER_ADDRESS, A_ALTERNATE_SERVER}
XOR_ATTRS = {A_XOR_MAPPED_ADDRESS, A_XOR_PEER_ADDRESS, A_XOR_RELAYED_ADDRESS}
TRANSPORT_UDP, TRANSPORT_TCP = 17, 6
CH_MIN, CH_MAX = 0x4000, 0x7FFF

A = None          # parsed args (global)
LOG_V = False


def pad4(n):
    return (4 - n % 4) % 4


# --------------------------------------------------------------------------- codec
def enc_method(m):
    return (m & 0xF) | ((m & 0x70) << 1) | ((m & 0xF80) << 2)


def dec_method(t):
    return (t & 0xF) | ((t >> 1) & 0x70) | ((t >> 2) & 0xF80)


def enc_addr(ip, port, tid=None, xor=False):
    ipa = ipaddress.ip_address(ip)
    raw = ipa.packed
    if xor:
        port ^= MAGIC >> 16
        mask = struct.pack("!I", MAGIC) + (tid or b"\0" * 12)
        raw = bytes(a ^ b for a, b in zip(raw, mask))
    return struct.pack("!BBH", 0, 1 if ipa.version == 4 else 2, port) + raw


def dec_addr(v, tid=None, xor=False):
    _, fam, port = struct.unpack("!BBH", v[:4])
    raw = v[4:4 + (4 if fam == 1 else 16)]
    if xor:
        port ^= MAGIC >> 16
        mask = struct.pack("!I", MAGIC) + (tid or b"\0" * 12)
        raw = bytes(a ^ b for a, b in zip(raw, mask))
    return str(ipaddress.ip_address(raw)), port


def xaddr(ip, port):
    """attribute value factory (needs the message's transaction id)"""
    return lambda m: enc_addr(ip, port, m.tid, True)


class Msg:
    def __init__(self, method, cls=REQ, tid=None):
        self.method, self.cls = method, cls
        self.tid = tid or os.urandom(12)
        self.attrs, self.offsets, self.raw, self.src = [], {}, b"", None

    # building
    def add(self, t, v=b""):
        self.attrs.append((t, v(self) if callable(v) else v))
        return self

    def _hdr(self, length):
        return struct.pack("!HHI12s", enc_method(self.method) | self.cls, length, MAGIC, self.tid)

    def encode(self, key=None, alg="sha1", fp=False, corrupt=False):
        body = b"".join(struct.pack("!HH", t, len(v)) + v + b"\0" * pad4(len(v)) for t, v in self.attrs)
        if key is not None:
            code, size, h = (A_MESSAGE_INTEGRITY, 20, hashlib.sha1) if alg == "sha1" else \
                (A_MESSAGE_INTEGRITY_SHA256, 32, hashlib.sha256)
            mac = hmac.new(key, self._hdr(len(body) + 4 + size) + body, h).digest()
            if corrupt:
                mac = bytes([mac[0] ^ 0xFF]) + mac[1:]
            body += struct.pack("!HH", code, size) + mac
        if fp:
            crc = (zlib.crc32(self._hdr(len(body) + 8) + body) ^ 0x5354554E) & 0xFFFFFFFF
            body += struct.pack("!HHI", A_FINGERPRINT, 4, crc)
        return self._hdr(len(body)) + body

    # parsing
    @staticmethod
    def decode(data):
        if len(data) < 20:
            raise ValueError("short")
        t, ln, magic, tid = struct.unpack("!HHI12s", data[:20])
        if t & 0xC000:
            raise ValueError("not STUN")
        if len(data) < 20 + ln:
            raise ValueError("truncated")
        m = Msg(dec_method(t), t & 0x0110, tid)
        m.raw, m.magic = data[:20 + ln], magic
        pos = 20
        while pos + 4 <= 20 + ln:
            at, al = struct.unpack("!HH", data[pos:pos + 4])
            v = data[pos + 4:pos + 4 + al]
            if len(v) < al:
                raise ValueError("attr truncated")
            m.attrs.append((at, v))
            m.offsets.setdefault(at, pos)
            pos += 4 + al + pad4(al)
        return m

    def get(self, t):
        for a, v in self.attrs:
            if a == t:
                return v
        return None

    def getall(self, t):
        return [v for a, v in self.attrs if a == t]

    def get_addr(self, t):
        v = self.get(t)
        return dec_addr(v, self.tid, t in XOR_ATTRS) if v else None

    def getall_addr(self, t):
        return [dec_addr(v, self.tid, t in XOR_ATTRS) for v in self.getall(t)]

    def check_integrity(self, key, alg="sha1"):
        code, size, h = (A_MESSAGE_INTEGRITY, 20, hashlib.sha1) if alg == "sha1" else \
            (A_MESSAGE_INTEGRITY_SHA256, 32, hashlib.sha256)
        off = self.offsets.get(code)
        if off is None:
            return None
        given = self.get(code)
        hdr = bytearray(self.raw[:20])
        struct.pack_into("!H", hdr, 2, off - 20 + 4 + len(given))
        mac = hmac.new(key, bytes(hdr) + self.raw[20:off], h).digest()
        return hmac.compare_digest(mac[:len(given)], given)

    def check_fingerprint(self):
        off = self.offsets.get(A_FINGERPRINT)
        if off is None:
            return None
        hdr = bytearray(self.raw[:20])
        struct.pack_into("!H", hdr, 2, off - 20 + 8)
        crc = (zlib.crc32(bytes(hdr) + self.raw[20:off]) ^ 0x5354554E) & 0xFFFFFFFF
        return struct.unpack("!I", self.get(A_FINGERPRINT))[0] == crc

    def error_code(self):
        v = self.get(A_ERROR_CODE)
        return (v[2] & 7) * 100 + v[3] if v and len(v) >= 4 else None

    def error_text(self):
        v = self.get(A_ERROR_CODE)
        return f"{self.error_code()} {v[4:].decode(errors='replace')}".strip() if v and len(v) >= 4 else "?"

    def name(self):
        return f"{M_NAME.get(self.method, hex(self.method))} {CLS_NAME.get(self.cls, '?')}"

    def dump(self):
        out = [f"  {self.name()} tid={self.tid.hex()}"]
        for t, v in self.attrs:
            n = A_NAME.get(t, f"0x{t:04x}")
            try:
                if t in ADDR_ATTRS and len(v) >= 8:
                    s = "%s:%d" % dec_addr(v, self.tid, t in XOR_ATTRS)
                elif t == A_ERROR_CODE:
                    s = f"{(v[2] & 7) * 100 + v[3]} {v[4:].decode(errors='replace')}"
                elif t in (A_USERNAME, A_REALM, A_NONCE, A_SOFTWARE):
                    s = v.decode(errors="replace")
                elif t == A_LIFETIME:
                    s = str(struct.unpack('!I', v)[0])
                else:
                    s = v[:24].hex() + ("..." if len(v) > 24 else "")
            except Exception:
                s = v[:24].hex()
            out.append(f"    {n} ({len(v)}B) {s}")
        return "\n".join(out)


def interpret(d):
    """raw datagram/frame -> ('chan', num, payload) | ('data', peer, payload) | ('stun', Msg) | None"""
    if not d:
        return None
    if (d[0] & 0xC0) == 0x40 and len(d) >= 4:
        num, ln = struct.unpack("!HH", d[:4])
        return ("chan", num, d[4:4 + ln])
    try:
        m = Msg.decode(d)
    except ValueError:
        return None
    if m.method == M_DATA and m.cls == IND:
        return ("data", m.get_addr(A_XOR_PEER_ADDRESS), m.get(A_DATA) or b"")
    return ("stun", m)


# --------------------------------------------------------------------------- transport
class Conn:
    def __init__(self, host, port, transport="udp", timeout=5.0, insecure=False, family=0, strict_src=True):
        self.transport, self.timeout, self.strict = transport, timeout, strict_src
        self.buf, self.last_src, self.wlock = b"", None, threading.Lock()
        udp = transport == "udp"
        af, st, _, _, sa = socket.getaddrinfo(host, port, family, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM)[0]
        self.server, self.af = sa, af
        self.sock = socket.socket(af, st)
        if udp:
            self.sock.bind(("::" if af == socket.AF_INET6 else "0.0.0.0", 0))
        else:
            self.sock.settimeout(timeout)
            self.sock.connect(sa)
            if transport == "tls":
                ctx = ssl.create_default_context()
                if insecure:
                    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
                self.sock = ctx.wrap_socket(self.sock, server_hostname=host)

    def local(self):
        return self.sock.getsockname()[:2]

    def send(self, data):
        with self.wlock:
            if self.transport == "udp":
                self.sock.sendto(data, self.server)
            else:
                self.sock.sendall(data)

    def send_channel(self, num, payload):
        d = struct.pack("!HH", num, len(payload)) + payload
        if self.transport != "udp":
            d += b"\0" * pad4(len(d))
        self.send(d)

    def recv(self, timeout):
        end = time.time() + timeout
        if self.transport == "udp":
            while True:
                rem = end - time.time()
                if rem <= 0:
                    return None
                self.sock.settimeout(rem)
                try:
                    data, addr = self.sock.recvfrom(65535)
                except socket.timeout:
                    return None
                except OSError:
                    continue
                self.last_src = addr[:2]
                if self.strict and addr[:2] != self.server[:2]:
                    continue
                return data
        while True:
            n = self._frame_len()
            if n is not None and len(self.buf) >= n:
                d, self.buf = self.buf[:n], self.buf[n:]
                return d
            rem = end - time.time()
            if rem <= 0:
                return None
            self.sock.settimeout(rem)
            try:
                chunk = self.sock.recv(65535)
            except (socket.timeout, ssl.SSLError):
                return None
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk

    def _frame_len(self):
        if len(self.buf) < 4:
            return None
        ln = struct.unpack("!H", self.buf[2:4])[0]
        if (self.buf[0] & 0xC0) == 0:
            return 20 + ln
        n = 4 + ln
        return n + pad4(n)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def vlog(tag, m):
    if LOG_V:
        print(f"    [{tag}]\n" + m.dump())


def transact(conn, msg, timeout, key=None, alg="sha1", fp=False, corrupt=False, events=None):
    """Send request, retransmit on UDP (RFC 8489 RTO doubling), return matching response or None."""
    raw = msg.encode(key, alg, fp, corrupt)
    vlog("tx", msg)
    udp = conn.transport == "udp"
    tries = 3 if udp else 1
    unit = timeout / 7.0 if udp else timeout
    for i in range(tries):
        conn.send(raw)
        end = time.time() + (unit * (2 ** i) if udp else timeout)
        while True:
            rem = end - time.time()
            if rem <= 0:
                break
            d = conn.recv(rem)
            if d is None:
                break
            if (d[0] & 0xC0) != 0:
                if events is not None:
                    events.append(d)
                continue
            try:
                r = Msg.decode(d)
            except ValueError:
                continue
            if r.tid == msg.tid and r.cls in (SUCCESS, ERROR):
                r.src = conn.last_src
                vlog("rx", r)
                return r
            if events is not None:
                events.append(d)
    return None


# --------------------------------------------------------------------------- TURN client
def make_creds():
    if A.secret:
        user = f"{int(time.time()) + 3600}:{A.rest_user}"
        pw = base64.b64encode(hmac.new(A.secret.encode(), user.encode(), hashlib.sha1).digest()).decode()
        return user, pw
    return A.username, A.password


class TurnClient:
    def __init__(self, conn, user, pw):
        self.conn, self.user, self.pw = conn, user, pw
        self.realm = self.nonce = self.key = None
        self.events = collections.deque()
        self.alloc_ok, self.relayed, self.mapped, self.lifetime = False, None, None, None
        self.last_challenge = None
        self.tmo = A.timeout

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def close(self):
        try:
            if self.alloc_ok and self.key:
                self.tmo = 1.0
                self.refresh(0)
        except Exception:
            pass
        self.conn.close()

    # auth ---------------------------------------------------------------
    def _adopt(self, r):
        if r.get(A_REALM) is not None:
            self.realm = r.get(A_REALM)
        if r.get(A_NONCE) is not None:
            self.nonce = r.get(A_NONCE)
        if self.realm is not None and self.user is not None:
            self.key = hashlib.md5(self.user.encode() + b":" + self.realm + b":" + self.pw.encode()).digest()
        self.last_challenge = r

    def challenge(self):
        m = Msg(M_ALLOCATE).add(A_REQUESTED_TRANSPORT, struct.pack("!B3x", TRANSPORT_UDP))
        r = transact(self.conn, m, self.tmo, events=self.events)
        if r and r.cls == ERROR and r.error_code() == 401:
            self._adopt(r)
        return r

    def request(self, method, attrs=(), auth=True, retry=True, corrupt=False, fp=False):
        r = None
        for _ in range(3):
            m = Msg(method)
            for t, v in attrs:
                m.add(t, v)
            key = None
            if auth and self.key is not None:
                m.add(A_USERNAME, self.user.encode()).add(A_REALM, self.realm).add(A_NONCE, self.nonce)
                key = self.key
            r = transact(self.conn, m, self.tmo, key=key, corrupt=corrupt, fp=fp, events=self.events)
            if r is None or r.cls != ERROR or not auth or not retry:
                return r
            c = r.error_code()
            if (c == 401 and self.key is None) or c == 438:
                self._adopt(r)
                continue
            return r
        return r

    # TURN operations ----------------------------------------------------
    def allocate(self, transport=TRANSPORT_UDP, family=None, even=None, token=None, dontfrag=False,
                 lifetime=None, add_family=None, no_transport=False, **kw):
        at = []
        if not no_transport:
            at.append((A_REQUESTED_TRANSPORT, struct.pack("!B3x", transport)))
        if family:
            at.append((A_REQUESTED_ADDRESS_FAMILY, struct.pack("!B3x", family)))
        if add_family:
            at.append((A_ADDITIONAL_ADDRESS_FAMILY, struct.pack("!B3x", add_family)))
        if even is not None:
            at.append((A_EVEN_PORT, bytes([0x80 if even else 0])))
        if token:
            at.append((A_RESERVATION_TOKEN, token))
        if dontfrag:
            at.append((A_DONT_FRAGMENT, b""))
        if lifetime is not None:
            at.append((A_LIFETIME, struct.pack("!I", lifetime)))
        r = self.request(M_ALLOCATE, at, **kw)
        if r is not None and r.cls == SUCCESS:
            self.alloc_ok = True
            self.relayed = r.get_addr(A_XOR_RELAYED_ADDRESS)
            self.mapped = r.get_addr(A_XOR_MAPPED_ADDRESS)
            lt = r.get(A_LIFETIME)
            self.lifetime = struct.unpack("!I", lt)[0] if lt else None
        return r

    def refresh(self, lifetime=None):
        at = [] if lifetime is None else [(A_LIFETIME, struct.pack("!I", lifetime))]
        r = self.request(M_REFRESH, at)
        if r is not None and r.cls == SUCCESS and lifetime == 0:
            self.alloc_ok = False
        return r

    def create_permission(self, peers):
        return self.request(M_CREATE_PERM, [(A_XOR_PEER_ADDRESS, xaddr(ip, port)) for ip, port in peers])

    def channel_bind(self, num, peer, **kw):
        at = [(A_CHANNEL_NUMBER, struct.pack("!HH", num, 0))]
        if peer:
            at.append((A_XOR_PEER_ADDRESS, xaddr(*peer)))
        return self.request(M_CHANNEL_BIND, at, **kw)

    def send_ind(self, peer, data, dontfrag=False):
        m = Msg(M_SEND, IND).add(A_XOR_PEER_ADDRESS, xaddr(*peer))
        if dontfrag:
            m.add(A_DONT_FRAGMENT, b"")
        m.add(A_DATA, data)
        self.conn.send(m.encode())

    def send_channel(self, num, data):
        self.conn.send_channel(num, data)

    def next_event(self, timeout):
        if self.events:
            return interpret(self.events.popleft())
        d = self.conn.recv(timeout)
        return interpret(d) if d is not None else None

    def wait_event(self, timeout, kinds=("data", "chan")):
        end = time.time() + timeout
        while True:
            rem = end - time.time()
            if rem <= 0:
                return None
            ev = self.next_event(rem)
            if ev and ev[0] in kinds:
                return ev


# --------------------------------------------------------------------------- test framework
class Fail(Exception):
    pass


class Skip(Exception):
    pass


class Warn(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


TESTS = []


def test(name, group, desc=""):
    def deco(fn):
        TESTS.append((name, group, fn, desc or (fn.__doc__ or "").strip()))
        return fn
    return deco


def new_conn(transport=None, port=None, strict=True):
    return Conn(A.host, port or A.port, transport or A.transport, A.timeout, A.insecure, A.family, strict)


def Cli(user=None, pw=None):
    u, p = make_creds()
    return TurnClient(new_conn(), user if user is not None else u, pw if pw is not None else p)


def need_alloc(r, who="client"):
    if r is None:
        raise Fail(f"{who}: no response to Allocate (server down / blocked / wrong port?)")
    if r.cls != SUCCESS:
        hint = ""
        if r.error_code() == 401:
            hint = "  (check --username/--password/--secret)"
        raise Fail(f"{who}: Allocate failed: {r.error_text()}{hint}")
    return r


def need_ok(r, what):
    if r is None:
        raise Fail(f"{what}: no response")
    if r.cls != SUCCESS:
        hint = "  (server denies this peer address; see --relay-host / coturn allow-loopback-peers / allowed-peer-ip)" \
            if r.error_code() == 403 else ""
        raise Fail(f"{what}: {r.error_text()}{hint}")
    return r


def relay_target(rel):
    ip = A.relay_host or rel[0]
    if ip in ("0.0.0.0", "::"):
        ip = socket.getaddrinfo(A.host, None)[0][4][0]
    return ip, rel[1]


def outbound_ip(af=socket.AF_INET):
    s = socket.socket(af, socket.SOCK_DGRAM)
    try:
        s.connect(socket.getaddrinfo(A.host, A.port, af, socket.SOCK_DGRAM)[0][4])
        return s.getsockname()[0]
    finally:
        s.close()


# --------------------------------------------------------------------------- codec self-test
@test("codec.roundtrip", "codec", "offline: encode/decode, integrity, fingerprint, XOR addresses")
def t_codec():
    check(enc_method(M_ALLOCATE) | ERROR == 0x0113, "Allocate error type code")
    check(enc_method(M_BINDING) | SUCCESS == 0x0101, "Binding success type code")
    check(enc_method(M_SEND) | IND == 0x0016 and enc_method(M_DATA) | IND == 0x0017, "Send/Data indication codes")
    for ip in ("192.0.2.33", "2001:db8:1234:5678:11:2233:4455:6677"):
        tid = os.urandom(12)
        check(dec_addr(enc_addr(ip, 32853, tid, True), tid, True) == (ip, 32853), f"xor addr {ip}")
    key = os.urandom(16)
    for alg in ("sha1", "sha256"):
        m = Msg(M_ALLOCATE).add(A_USERNAME, b"user").add(A_SOFTWARE, b"odd")   # odd length -> padding
        raw = m.encode(key=key, alg=alg, fp=True)
        d = Msg.decode(raw)
        check(d.check_integrity(key, alg) is True, f"{alg} integrity verify")
        check(d.check_integrity(os.urandom(16), alg) is False, f"{alg} integrity wrong key")
        check(d.check_fingerprint() is True, "fingerprint verify")
        bad = bytearray(raw)
        bad[24] ^= 1
        check(Msg.decode(bytes(bad)).check_fingerprint() is False, "fingerprint detects tamper")
    return "all codec checks ok"


# --------------------------------------------------------------------------- STUN tests
def stun_binding(c, *attrs, fp=False, strict_ok=True):
    m = Msg(M_BINDING)
    for t, v in attrs:
        m.add(t, v)
    return m, transact(c, m, A.timeout, fp=fp)


@test("stun.binding", "stun", "Binding request -> XOR-MAPPED-ADDRESS (+ MAPPED-ADDRESS, FINGERPRINT, SOFTWARE checks)")
def t_binding():
    c = new_conn()
    m, r = stun_binding(c)
    check(r is not None, "no response to Binding request")
    check(r.cls == SUCCESS, f"Binding failed: {r.error_text()}")
    check(r.tid == m.tid and getattr(r, "magic", MAGIC) == MAGIC, "transaction id / magic cookie")
    xa = r.get_addr(A_XOR_MAPPED_ADDRESS)
    check(xa, "XOR-MAPPED-ADDRESS missing")
    ma = r.get_addr(A_MAPPED_ADDRESS)
    check(ma is None or ma == xa, f"MAPPED-ADDRESS {ma} != XOR-MAPPED-ADDRESS {xa}")
    check(r.check_fingerprint() is not False, "FINGERPRINT present but invalid")
    loc = c.local()
    nat = "no NAT" if xa[1] == loc[1] else "NAT/port-mapped"
    sw = r.get(A_SOFTWARE)
    c.close()
    return f"reflexive={xa[0]}:{xa[1]} local port={loc[1]} ({nat}); software={sw.decode(errors='replace') if sw else '-'}"


@test("stun.fingerprint", "stun", "Binding with FINGERPRINT; response fingerprint must verify if present")
def t_fp():
    c = new_conn()
    _, r = stun_binding(c, fp=True)
    check(r is not None and r.cls == SUCCESS, "no success response")
    v = r.check_fingerprint()
    check(v is not False, "response FINGERPRINT invalid")
    return "response fingerprint " + ("valid" if v else "absent (SHOULD be echoed)")


@test("stun.indication", "stun", "Binding indication must not be answered; server stays alive")
def t_ind():
    c = new_conn()
    c.send(Msg(M_BINDING, IND).encode(fp=True))
    d = c.recv(1.0)
    check(d is None, "server answered an indication")
    _, r = stun_binding(c)
    check(r is not None and r.cls == SUCCESS, "server unresponsive after indication")
    return "ignored correctly"


@test("stun.unknown_required", "stun", "comprehension-required unknown attribute -> 420 + UNKNOWN-ATTRIBUTES")
def t_unk_req():
    c = new_conn()
    _, r = stun_binding(c, (0x7FF0, b"test"))
    check(r is not None, "no response")
    if r.cls == SUCCESS:
        raise Warn("server ignored unknown comprehension-required attribute (RFC 8489 s7.3.1 says reply 420)")
    check(r.error_code() == 420, f"expected 420 got {r.error_text()}")
    ua = r.get(A_UNKNOWN_ATTRIBUTES) or b""
    ids = [struct.unpack("!H", ua[i:i + 2])[0] for i in range(0, len(ua) - 1, 2)]
    check(0x7FF0 in ids, f"UNKNOWN-ATTRIBUTES does not list 0x7FF0: {ids}")
    return "420 + UNKNOWN-ATTRIBUTES listed 0x7FF0"


@test("stun.unknown_optional", "stun", "comprehension-optional unknown attribute must be ignored")
def t_unk_opt():
    _, r = stun_binding(new_conn(), (0x8FF0, b"xyz"))
    check(r is not None and r.cls == SUCCESS, "optional unknown attribute was not ignored")
    return "ignored"


@test("stun.attr_padding", "stun", "attributes with lengths 1,2,3,5,255 (padding handling)")
def t_pad():
    c = new_conn()
    for n in (1, 2, 3, 5, 255):
        _, r = stun_binding(c, (A_SOFTWARE, b"s" * n), (0x8FF1, b"p" * n))
        check(r is not None and r.cls == SUCCESS, f"failed with attribute length {n}")
    return "ok"


@test("stun.ice_attrs", "stun", "ICE connectivity-check attributes (PRIORITY, USE-CANDIDATE, ICE-CONTROLLING/ED)")
def t_ice():
    for a in ((A_ICE_CONTROLLING, os.urandom(8)), (A_ICE_CONTROLLED, os.urandom(8))):
        _, r = stun_binding(new_conn(), (A_PRIORITY, struct.pack("!I", 1845494015)), (A_USE_CANDIDATE, b""), a)
        if r is None or r.cls != SUCCESS:
            raise Warn(f"{A_NAME[a[0]]}: " + ("no response" if r is None else r.error_text()))
    return "ok"


@test("stun.retransmit_idempotent", "stun", "same transaction id sent 3x -> consistent answers")
def t_retx():
    c = new_conn()
    m = Msg(M_BINDING)
    raw = m.encode()
    for _ in range(3):
        c.send(raw)
    got = []
    end = time.time() + 2
    while time.time() < end and len(got) < 3:
        d = c.recv(max(0.05, end - time.time()))
        if d:
            r = Msg.decode(d)
            if r.tid == m.tid:
                got.append(r.get_addr(A_XOR_MAPPED_ADDRESS))
    check(len(got) >= 1, "no response")
    check(len(set(got)) == 1, f"inconsistent mapped addresses {got}")
    return f"{len(got)} identical responses"


@test("stun.burst", "stun", "100 pipelined Binding requests; expect >=95% answered")
def t_burst():
    c = new_conn()
    tids = set()
    for _ in range(100):
        m = Msg(M_BINDING)
        tids.add(m.tid)
        c.send(m.encode())
    ok, end = 0, time.time() + A.timeout
    while time.time() < end and ok < 100:
        d = c.recv(0.2)
        if d:
            try:
                if Msg.decode(d).tid in tids:
                    ok += 1
            except ValueError:
                pass
    if ok < 95:
        raise Warn(f"only {ok}/100 answered (rate limiting?)")
    return f"{ok}/100 answered"


@test("stun.malformed", "stun", "garbage / truncated / bad-magic packets must not break the server (UDP only)")
def t_malformed():
    if A.transport != "udp":
        raise Skip("UDP only")
    c = new_conn()
    tid = os.urandom(12)
    cases = {
        "random 7 bytes": os.urandom(7),
        "header only, length lies": struct.pack("!HHI12s", 1, 64, MAGIC, tid),
        "attr overruns message": struct.pack("!HHI12s", 1, 8, MAGIC, tid) + struct.pack("!HH", A_SOFTWARE, 200) + b"abcd",
        "bad magic cookie": struct.pack("!HHI12s", 1, 0, 0xDEADBEEF, tid),
        "length not multiple of 4": struct.pack("!HHI12s", 1, 3, MAGIC, tid) + b"abc",
        "20 zero bytes": b"\0" * 20,
        "oversize 4000B": os.urandom(4000),
    }
    for name, pkt in cases.items():
        c.send(pkt)
        c.recv(0.3)
    _, r = stun_binding(c)
    check(r is not None and r.cls == SUCCESS, "server stopped answering after malformed input")
    return f"survived {len(cases)} malformed packets"


@test("stun.rfc5780", "stun", "NAT behaviour discovery: OTHER-ADDRESS / CHANGE-REQUEST / RESPONSE-ORIGIN")
def t_5780():
    if A.transport != "udp":
        raise Skip("UDP only")
    c = new_conn(strict=False)
    _, r = stun_binding(c)
    check(r is not None and r.cls == SUCCESS, "binding failed")
    other, origin = r.get_addr(A_OTHER_ADDRESS), r.get_addr(A_RESPONSE_ORIGIN)
    if not other:
        raise Skip("server does not advertise OTHER-ADDRESS (RFC 5780 unsupported)")
    out = [f"origin={origin} other={other}"]
    for label, flags in (("change port", 0x02), ("change IP", 0x04), ("change IP+port", 0x06)):
        _, r = stun_binding(c, (A_CHANGE_REQUEST, struct.pack("!I", flags)))
        out.append(f"{label}: " + ("no response" if r is None else f"from {r.src}" if r.cls == SUCCESS else r.error_text()))
    return "\n".join(out)


@test("stun.transports", "stun", "Binding over UDP / TCP / TLS on the configured host")
def t_transports():
    out, ok = [], 0
    for tr, port in (("udp", A.port if A.transport == "udp" else A.udp_port),
                     ("tcp", A.port if A.transport == "tcp" else A.tcp_port),
                     ("tls", A.port if A.transport == "tls" else A.tls_port)):
        try:
            c = new_conn(tr, port)
            _, r = stun_binding(c)
            good = r is not None and r.cls == SUCCESS
            out.append(f"{tr}/{port}: " + (("ok " + "%s:%d" % r.get_addr(A_XOR_MAPPED_ADDRESS)) if good else "no/err response"))
            ok += good
            c.close()
        except Exception as e:
            out.append(f"{tr}/{port}: unavailable ({type(e).__name__})")
    check(ok >= 1, "no transport worked")
    return "\n".join(out)


# --------------------------------------------------------------------------- TURN: auth & allocation
@test("turn.auth_challenge", "turn", "unauthenticated Allocate -> 401 with REALM + NONCE")
def t_challenge():
    c = Cli()
    r = c.challenge()
    check(r is not None, "no response")
    check(r.cls == ERROR and r.error_code() == 401, f"expected 401, got {r.error_text() if r.cls == ERROR else 'success (auth disabled?)'}")
    check(r.get(A_REALM) and r.get(A_NONCE), "401 lacks REALM and/or NONCE")
    algs = r.get(A_PASSWORD_ALGORITHMS)
    c.close()
    return f"realm={r.get(A_REALM).decode()!r} nonce={len(r.get(A_NONCE))}B password-algorithms={'yes' if algs else 'no'}"


@test("turn.auth_bad_password", "turn", "wrong password must be rejected")
def t_badpw():
    with Cli(pw="definitely-wrong-" + os.urandom(3).hex()) as c:
        r = c.allocate()
        check(r is not None, "no response")
        check(r.cls == ERROR and r.error_code() in (401, 403), f"expected 401/403 got {'success' if r.cls == SUCCESS else r.error_text()}")
        return r.error_text()


@test("turn.auth_bad_user", "turn", "unknown username must be rejected")
def t_baduser():
    with Cli(user="nosuchuser-" + os.urandom(3).hex()) as c:
        r = c.allocate()
        check(r is not None, "no response")
        check(r.cls == ERROR and r.error_code() in (401, 403), f"expected 401/403 got {'success' if r.cls == SUCCESS else r.error_text()}")
        return r.error_text()


@test("turn.auth_tampered_integrity", "turn", "corrupt MESSAGE-INTEGRITY must be rejected")
def t_tamper():
    with Cli() as c:
        check(c.challenge() is not None, "no challenge")
        r = c.request(M_ALLOCATE, [(A_REQUESTED_TRANSPORT, struct.pack("!B3x", TRANSPORT_UDP))], retry=False, corrupt=True)
        check(r is not None, "no response")
        check(r.cls == ERROR, "server accepted a request with corrupted MESSAGE-INTEGRITY")
        c.alloc_ok = False
        return r.error_text()


@test("turn.auth_stale_nonce", "turn", "bogus nonce -> 438 Stale Nonce + fresh NONCE; retry succeeds")
def t_stale():
    with Cli() as c:
        check(c.challenge() is not None, "no challenge")
        c.nonce = b"bogus" + os.urandom(4).hex().encode()
        r = c.request(M_ALLOCATE, [(A_REQUESTED_TRANSPORT, struct.pack("!B3x", TRANSPORT_UDP))], retry=False)
        check(r is not None and r.cls == ERROR, "bogus nonce accepted" if r else "no response")
        msg = r.error_text()
        if r.error_code() != 438:
            raise Warn(f"expected 438, got {msg}")
        check(r.get(A_NONCE), "438 without fresh NONCE")
        need_alloc(c.allocate(), "after nonce refresh")
        return "438 then success with fresh nonce"


@test("turn.auth_sha256", "turn", "RFC 8489 PASSWORD-ALGORITHM(SHA-256) + MESSAGE-INTEGRITY-SHA256")
def t_sha256():
    with Cli() as c:
        r = c.challenge()
        check(r is not None and r.cls == ERROR, "no challenge")
        v, algs, i = r.get(A_PASSWORD_ALGORITHMS) or b"", [], 0
        while i + 4 <= len(v):
            alg, pl = struct.unpack("!HH", v[i:i + 4])
            algs.append(alg)
            i += 4 + pl + pad4(pl)
        if 2 not in algs:
            raise Skip(f"server does not offer SHA-256 password algorithm (offered: {algs})")
        key = hashlib.sha256(c.user.encode() + b":" + c.realm + b":" + c.pw.encode()).digest()
        m = Msg(M_ALLOCATE).add(A_REQUESTED_TRANSPORT, struct.pack("!B3x", TRANSPORT_UDP))
        m.add(A_USERNAME, c.user.encode()).add(A_REALM, c.realm).add(A_NONCE, c.nonce)
        m.add(A_PASSWORD_ALGORITHM, struct.pack("!HH", 2, 0))
        r = transact(c.conn, m, A.timeout, key=key, alg="sha256")
        check(r is not None, "no response")
        check(r.cls == SUCCESS, f"SHA-256 auth rejected: {r.error_text()}")
        ok = r.check_integrity(key, "sha256")
        rf = Msg(M_REFRESH).add(A_LIFETIME, struct.pack("!I", 0)).add(A_USERNAME, c.user.encode()) \
            .add(A_REALM, c.realm).add(A_NONCE, c.nonce).add(A_PASSWORD_ALGORITHM, struct.pack("!HH", 2, 0))
        transact(c.conn, rf, 2.0, key=key, alg="sha256")
        return f"accepted; response MESSAGE-INTEGRITY-SHA256 {'valid' if ok else 'absent/invalid'}"


@test("turn.allocate_ok", "turn", "valid Allocate: relayed + mapped address, LIFETIME, response integrity")
def t_alloc():
    with Cli() as c:
        r = need_alloc(c.allocate())
        check(c.relayed and c.relayed[1] > 0, "XOR-RELAYED-ADDRESS missing/zero port")
        check(c.mapped, "XOR-MAPPED-ADDRESS missing")
        check(c.lifetime and c.lifetime > 0, "LIFETIME missing/zero")
        integ = r.check_integrity(c.key)
        check(integ is not False, "response MESSAGE-INTEGRITY invalid")
        return (f"relayed={c.relayed[0]}:{c.relayed[1]} mapped={c.mapped[0]}:{c.mapped[1]} lifetime={c.lifetime}s "
                f"integrity={'valid' if integ else 'absent'}")


@test("turn.allocate_missing_transport", "turn", "Allocate without REQUESTED-TRANSPORT -> 400")
def t_notransport():
    with Cli() as c:
        r = c.allocate(no_transport=True)
        check(r is not None, "no response")
        check(r.cls == ERROR and r.error_code() == 400, f"expected 400 got {'success' if r.cls == SUCCESS else r.error_text()}")
        return "400"


@test("turn.allocate_bad_transport", "turn", "Allocate with unsupported transport (99) -> 442")
def t_badtransport():
    with Cli() as c:
        r = c.allocate(transport=99)
        check(r is not None, "no response")
        check(r.cls == ERROR, "server accepted bogus transport")
        if r.error_code() != 442:
            raise Warn(f"expected 442 got {r.error_text()}")
        return "442"


@test("turn.allocate_mismatch", "turn", "second Allocate on same 5-tuple -> 437")
def t_mismatch():
    with Cli() as c:
        need_alloc(c.allocate())
        r = c.allocate()
        check(r is not None and r.cls == ERROR, "second Allocate unexpectedly succeeded" if r else "no response")
        check(r.error_code() == 437, f"expected 437 got {r.error_text()}")
        return "437"


@test("turn.allocate_dont_fragment", "turn", "DONT-FRAGMENT attribute on Allocate and Send")
def t_df():
    with Cli() as c:
        r = c.allocate(dontfrag=True)
        check(r is not None, "no response")
        if r.cls == ERROR:
            raise Warn(f"DONT-FRAGMENT not supported: {r.error_text()}")
        return "accepted"


@test("turn.allocate_even_port_reservation", "turn", "EVEN-PORT + RESERVATION-TOKEN -> consecutive ports")
def t_even():
    with Cli() as a, Cli() as b:
        r = a.allocate(even=True)
        tok = r.get(A_RESERVATION_TOKEN) if r and r.cls == SUCCESS else None
        # token only returned when R bit set; our helper sets 0x80 so expect it
        if tok is None:
            raise Warn("EVEN-PORT accepted but no RESERVATION-TOKEN returned (feature ignored/unsupported)")
        check(len(tok) == 8, "RESERVATION-TOKEN must be 8 bytes")
        rb = b.allocate(token=tok)
        check(rb is not None and rb.cls == SUCCESS, f"allocation with token failed: {rb.error_text() if rb else 'no response'}")
        check(b.relayed[1] == a.relayed[1] + 1, f"expected port {a.relayed[1] + 1} got {b.relayed[1]}")
        return f"ports {a.relayed[1]} / {b.relayed[1]}"


@test("turn.allocate_ipv6", "turn", "REQUESTED-ADDRESS-FAMILY=IPv6 (RFC 6156) and IPv4 explicit")
def t_v6():
    out = []
    with Cli() as c:
        r = c.allocate(family=1)
        check(r is not None and r.cls == SUCCESS, "explicit IPv4 family failed")
        out.append(f"IPv4 ok {c.relayed[0]}")
    with Cli() as c:
        r = c.allocate(family=2)
        check(r is not None, "no response for IPv6")
        if r.cls == SUCCESS:
            if ":" not in c.relayed[0]:
                raise Warn(f"asked for IPv6 relay, server returned IPv4 {c.relayed[0]}")
            out.append(f"IPv6 ok {c.relayed[0]}")
        elif r.error_code() == 440:
            out.append("IPv6 -> 440 Address Family not Supported")
        else:
            raise Warn("IPv6 allocation: " + r.error_text())
    return "\n".join(out)


@test("turn.allocate_dual_stack", "turn", "ADDITIONAL-ADDRESS-FAMILY (RFC 8656) dual allocation")
def t_dual():
    with Cli() as c:
        r = c.allocate(add_family=2)
        check(r is not None, "no response")
        if r.cls == ERROR:
            raise Warn(f"not supported: {r.error_text()}")
        rel = r.getall_addr(A_XOR_RELAYED_ADDRESS)
        return f"relayed addresses: {rel}"


@test("turn.refresh", "turn", "Refresh (default, custom lifetime, lifetime 0) and Refresh without allocation")
def t_refresh():
    with Cli() as c:
        need_alloc(c.allocate())
        r = need_ok(c.refresh(), "Refresh default")
        l1 = struct.unpack("!I", r.get(A_LIFETIME))[0]
        r = need_ok(c.refresh(300), "Refresh 300")
        l2 = struct.unpack("!I", r.get(A_LIFETIME))[0]
        check(0 < l2 <= 600, f"unexpected lifetime for 300s request: {l2}")
        need_ok(c.refresh(0), "Refresh 0 (delete)")
        r = c.refresh(600)
        check(r is not None and r.cls == ERROR and r.error_code() == 437,
              f"Refresh after delete expected 437, got {r.error_text() if r else 'no response'}")
        return f"default={l1}s custom(300)={l2}s, delete ok, post-delete 437"


# --------------------------------------------------------------------------- TURN: permissions / channels
DUMMY = ("192.0.2.1", 9)


@test("turn.permission", "turn", "CreatePermission: single, multiple XOR-PEER-ADDRESS, missing attribute")
def t_perm():
    with Cli() as c:
        need_alloc(c.allocate())
        need_ok(c.create_permission([DUMMY]), "single permission")
        need_ok(c.create_permission([("192.0.2.2", 0), ("192.0.2.3", 1000), ("198.51.100.7", 5)]), "multi permission")
        need_ok(c.create_permission([DUMMY]), "permission refresh")
        r = c.request(M_CREATE_PERM, [])
        check(r is not None and r.cls == ERROR and r.error_code() == 400, f"missing peer expected 400, got {r.error_text() if r else 'no response'}")
        r = c.create_permission([("2001:db8::1", 0)])
        out = "IPv6 peer on IPv4 allocation: " + ("accepted" if r and r.cls == SUCCESS else r.error_text() if r else "no response")
        return "ok; " + out


@test("turn.peer_address_policy", "turn", "which special peer ranges does the server allow? (security audit)")
def t_policy():
    risky, out = [], []
    probes = {"loopback 127.0.0.1": "127.0.0.1", "unspecified 0.0.0.0": "0.0.0.0", "multicast 224.0.0.1": "224.0.0.1",
              "link-local 169.254.169.254": "169.254.169.254", "private 10.0.0.1": "10.0.0.1",
              "private 192.168.0.1": "192.168.0.1", "broadcast 255.255.255.255": "255.255.255.255"}
    with Cli() as c:
        need_alloc(c.allocate())
        for label, ip in probes.items():
            r = c.create_permission([(ip, 0)])
            allowed = r is not None and r.cls == SUCCESS
            out.append(f"{label}: {'ALLOWED' if allowed else (r.error_text() if r else 'no response')}")
            if allowed and ip in ("127.0.0.1", "0.0.0.0", "224.0.0.1", "169.254.169.254", "255.255.255.255"):
                risky.append(label)
    if risky:
        out.append("NOTE: relaying to " + ", ".join(risky) + " lets clients reach internal services through the TURN server")
        if A.strict:
            raise Fail("\n".join(out))
        raise Warn("\n".join(out))
    return "\n".join(out)


@test("turn.channel_bind", "turn", "ChannelBind validity rules")
def t_chbind():
    p1, p2 = ("192.0.2.10", 4000), ("192.0.2.11", 4001)
    with Cli() as c:
        need_alloc(c.allocate())
        need_ok(c.channel_bind(CH_MIN, p1), "bind 0x4000")
        need_ok(c.channel_bind(CH_MIN, p1), "rebind/refresh same channel+peer")
        res = {}
        for label, num, peer in (("number below range (0x3FFF)", 0x3FFF, p2), ("number above range (0x8000)", 0x8000, p2),
                                 ("same channel, different peer", CH_MIN, p2), ("same peer, different channel", CH_MIN + 1, p1),
                                 ("missing XOR-PEER-ADDRESS", CH_MIN + 2, None)):
            r = c.channel_bind(num, peer)
            res[label] = r.error_code() if r is not None and r.cls == ERROR else ("ACCEPTED" if r else "no-response")
        bad = [k for k, v in res.items() if v != 400]
        check(not bad, "expected 400 for: " + "; ".join(f"{k} -> {res[k]}" for k in bad))
        need_ok(c.channel_bind(CH_MIN + 3, p2), "second valid binding")
        return "valid binds ok; 5 invalid binds -> 400"


class Peer:
    """plain UDP socket acting as a non-TURN remote peer"""
    def __init__(self, af):
        self.af = af
        self.s = socket.socket(af, socket.SOCK_DGRAM)
        self.s.bind((A.peer_bind or ("::" if af == socket.AF_INET6 else "0.0.0.0"), 0))
        self.port = self.s.getsockname()[1]
        self.addr = None

    def discover(self):
        """address the server sees for this socket (reflexive), with fallbacks / overrides"""
        ip, port = None, self.port
        try:
            sa = socket.getaddrinfo(A.host, A.udp_port if A.transport != "udp" else A.port, self.af, socket.SOCK_DGRAM)[0][4]
            m = Msg(M_BINDING)
            self.s.sendto(m.encode(), sa)
            self.s.settimeout(2.0)
            d, _ = self.s.recvfrom(2048)
            ip, port = Msg.decode(d).get_addr(A_XOR_MAPPED_ADDRESS)
        except Exception:
            ip = outbound_ip(self.af)
        if A.peer_host:
            ip = A.peer_host
        self.addr = (ip, port)
        return self.addr

    def recv(self, t):
        self.s.settimeout(t)
        try:
            return self.s.recvfrom(65535)
        except socket.timeout:
            return None, None

    def close(self):
        self.s.close()


class PeerSession:
    def __enter__(self):
        self.c = Cli()
        need_alloc(self.c.allocate())
        af = socket.AF_INET6 if ":" in self.c.relayed[0] else socket.AF_INET
        self.peer = Peer(af)
        self.addr = self.peer.discover()
        self.tgt = relay_target(self.c.relayed)
        return self

    def permit(self, ip=None):
        need_ok(self.c.create_permission([(ip or self.addr[0], 0)]), "CreatePermission for local peer")

    def __exit__(self, *a):
        self.peer.close()
        self.c.close()


@test("turn.send_data_indication", "turn", "client->peer via Send; peer->client via Data; relayed source address check")
def t_sd():
    with PeerSession() as s:
        s.permit()
        s.c.send_ind(s.addr, b"hello-from-client")
        d, src = s.peer.recv(3)
        check(d == b"hello-from-client", "peer did not receive Send payload" if d is None else f"payload mismatch {d!r}")
        s.peer.s.sendto(b"hello-from-peer", s.tgt)
        ev = s.c.wait_event(3, ("data",))
        check(ev, "client did not receive Data indication from peer")
        check(ev[2] == b"hello-from-peer", f"payload mismatch {ev[2]!r}")
        pe = ev[1]
        check(pe[1] == s.addr[1] or pe[0] == s.addr[0], f"XOR-PEER-ADDRESS {pe} unexpected (peer is {s.addr})")
        return f"peer saw source {src[0]}:{src[1]} (relay {s.c.relayed[0]}:{s.c.relayed[1]}); Data XOR-PEER-ADDRESS={pe[0]}:{pe[1]}"


@test("turn.channeldata", "turn", "ChannelBind + ChannelData both directions with a plain UDP peer")
def t_cd():
    with PeerSession() as s:
        s.permit()
        need_ok(s.c.channel_bind(CH_MIN + 7, s.addr), "ChannelBind")
        s.c.send_channel(CH_MIN + 7, b"chan-out")
        d, _ = s.peer.recv(3)
        check(d == b"chan-out", "peer did not receive ChannelData payload")
        s.peer.s.sendto(b"chan-in", s.tgt)
        ev = s.c.wait_event(3)
        check(ev, "no packet from peer")
        check(ev[0] == "chan" and ev[1] == CH_MIN + 7, f"expected ChannelData on 0x{CH_MIN + 7:x}, got {ev[0]} {ev[1] if ev[0] == 'chan' else ''}")
        check(ev[2] == b"chan-in", "payload mismatch")
        return "ChannelData in+out ok"


@test("turn.payload_sizes", "turn", "echo 0..1400-byte payloads through Send and ChannelData")
def t_sizes():
    sizes = [0, 1, 3, 4, 5, 63, 100, 548, 1000, 1200, 1400]
    with PeerSession() as s:
        s.permit()
        need_ok(s.c.channel_bind(CH_MIN + 9, s.addr), "ChannelBind")
        for mode in ("indication", "channel"):
            for n in sizes:
                pl = os.urandom(n)
                (s.c.send_ind(s.addr, pl) if mode == "indication" else s.c.send_channel(CH_MIN + 9, pl))
                d, _ = s.peer.recv(2)
                check(d == pl, f"{mode} size {n}: not delivered intact")
                s.peer.s.sendto(d, s.tgt)
                ev = s.c.wait_event(2)
                check(ev and ev[2] == pl, f"{mode} size {n}: echo back failed")
        return f"{len(sizes) * 2} round-trips intact"


@test("turn.permission_enforced", "turn", "peer without permission is dropped; with permission is delivered")
def t_perm_enf():
    with PeerSession() as s:
        need_ok(s.c.create_permission([("192.0.2.77", 0)]), "permission for unrelated IP")
        s.peer.s.sendto(b"should-be-dropped", s.tgt)
        ev = s.c.wait_event(1.5)
        check(ev is None, "server relayed data from a peer without permission (open relay!)")
        s.permit()
        s.peer.s.sendto(b"now-allowed", s.tgt)
        ev = s.c.wait_event(3)
        check(ev and ev[2] == b"now-allowed", "data not delivered after CreatePermission")
        return "dropped before permission, delivered after"


@test("turn.send_without_permission", "turn", "Send indication to peer lacking permission must be dropped")
def t_send_noperm():
    with PeerSession() as s:
        s.c.send_ind(s.addr, b"no-permission")
        d, _ = s.peer.recv(1.5)
        check(d is None, "server forwarded Send to a peer without permission")
        return "dropped"


@test("turn.channeldata_unbound", "turn", "ChannelData on unbound channel is discarded; session stays healthy")
def t_cd_unbound():
    with PeerSession() as s:
        s.permit()
        s.c.send_channel(0x4321, b"orphan")
        d, _ = s.peer.recv(1.0)
        check(d is None, "unbound ChannelData was forwarded")
        need_ok(s.c.refresh(), "Refresh after unbound ChannelData")
        return "ok"


# --------------------------------------------------------------------------- E2E media relay
def make_payload(ssrc, idx, size):
    head = struct.pack("!QI", time.monotonic_ns(), idx)
    seed = hashlib.sha256(struct.pack("!II", ssrc, idx)).digest()
    return head + (seed * (size // 32 + 1))[:max(0, size - len(head))]


def rtp(ssrc, idx, size, pt=111):
    return struct.pack("!BBHII", 0x80, pt, idx & 0xFFFF, (idx * 960) & 0xFFFFFFFF, ssrc) + make_payload(ssrc, idx, size - 12)


class Stats:
    def __init__(self, name, ssrc, expect_src, expect_chan):
        self.name, self.ssrc, self.expect_src, self.expect_chan = name, ssrc, expect_src, expect_chan
        self.sent = self.bytes_sent = self.recv = self.bytes_recv = self.corrupt = self.dup = self.reorder = 0
        self.seen, self.lat, self.via = set(), [], collections.Counter()
        self.bad_src = set()
        self.last_idx = -1
        self.jitter, self.prev = 0.0, None
        self.t_first = self.t_last = None

    def on_event(self, ev):
        now = time.monotonic_ns()
        kind = ev[0]
        pl = ev[2]
        if len(pl) < 12 + 12 or pl[0] >> 6 != 2:
            self.corrupt += 1
            return
        seq, ts, ssrc = struct.unpack("!HII", pl[2:12])
        body = pl[12:]
        sent_ns, idx = struct.unpack("!QI", body[:12])
        if ssrc != self.ssrc:
            return
        seed = hashlib.sha256(struct.pack("!II", ssrc, idx)).digest()
        if body[12:] != (seed * (len(body) // 32 + 1))[:len(body) - 12] or seq != (idx & 0xFFFF):
            self.corrupt += 1
            return
        if idx in self.seen:
            self.dup += 1
            return
        self.seen.add(idx)
        if idx < self.last_idx:
            self.reorder += 1
        self.last_idx = max(self.last_idx, idx)
        self.recv += 1
        self.bytes_recv += len(pl)
        if kind == "data":
            self.via["Data-indication"] += 1
            if ev[1] is None or ev[1][0] != self.expect_src[0] or (ev[1][1] != self.expect_src[1]):
                self.bad_src.add("%s:%s" % ev[1])
        else:
            self.via[f"ChannelData(0x{ev[1]:04x})"] += 1
            if ev[1] != self.expect_chan:
                self.bad_src.add(f"channel 0x{ev[1]:04x}")
        lat = (now - sent_ns) / 1e6
        self.lat.append(lat)
        if self.prev is not None:
            d = (now - self.prev[0]) / 1e6 - (sent_ns - self.prev[1]) / 1e6
            self.jitter += (abs(d) - self.jitter) / 16.0
        self.prev = (now, sent_ns)
        self.t_first = self.t_first or now
        self.t_last = now

    def pct(self, p):
        if not self.lat:
            return 0.0
        s = sorted(self.lat)
        return s[min(len(s) - 1, int(math.ceil(p / 100 * len(s))) - 1)]

    def loss(self):
        return 100.0 * (1 - self.recv / self.sent) if self.sent else 100.0

    def summary(self):
        dur = ((self.t_last - self.t_first) / 1e9) if self.t_first and self.t_last and self.t_last > self.t_first else 0
        kbps = (self.bytes_recv * 8 / 1000 / dur) if dur else 0
        return dict(name=self.name, sent=self.sent, received=self.recv, loss_pct=round(self.loss(), 2),
                    corrupt=self.corrupt, duplicates=self.dup, reordered=self.reorder,
                    lat_p50_ms=round(self.pct(50), 2), lat_p95_ms=round(self.pct(95), 2), lat_max_ms=round(max(self.lat or [0]), 2),
                    jitter_ms=round(self.jitter, 3), kbps=round(kbps, 1), via=dict(self.via), bad_sources=sorted(self.bad_src))

    def round_trip_summary(self):
        return dict(samples=len(self.lat),
                    min_ms=round(min(self.lat), 3) if self.lat else None,
                    mean_ms=round(sum(self.lat) / len(self.lat), 3) if self.lat else None,
                    p50_ms=round(self.pct(50), 3) if self.lat else None,
                    p95_ms=round(self.pct(95), 3) if self.lat else None,
                    p99_ms=round(self.pct(99), 3) if self.lat else None,
                    max_ms=round(max(self.lat), 3) if self.lat else None)

    def line(self, rtt=None):
        s = self.summary()
        if rtt is None:
            timing = f"lat p50/p95/max={s['lat_p50_ms']}/{s['lat_p95_ms']}/{s['lat_max_ms']}ms"
        else:
            timing = (f"RTT samples={rtt['samples']} min/mean/p50/p95/p99/max="
                      f"{rtt['min_ms']}/{rtt['mean_ms']}/{rtt['p50_ms']}/{rtt['p95_ms']}/"
                      f"{rtt['p99_ms']}/{rtt['max_ms']}ms")
        return (f"{s['name']}: sent={s['sent']} recv={s['received']} loss={s['loss_pct']}% corrupt={s['corrupt']} dup={s['duplicates']} "
                f"reorder={s['reordered']} {timing} jitter={s['jitter_ms']}ms rate={s['kbps']}kbps via={s['via']}")


E2E_REPORT = {}


@test("e2e.two_peer_relay", "e2e", "two TURN clients relay RTP-shaped media to each other (Send/Data + ChannelData)")
def t_e2e():
    size = max(40, A.payload_size)
    with Cli() as a, Cli() as b:
        need_alloc(a.allocate(), "peer A")
        need_alloc(b.allocate(), "peer B")
        relA, relB = a.relayed, b.relayed
        tA, tB = relay_target(relA), relay_target(relB)
        lines = [f"A: mapped={a.mapped[0]}:{a.mapped[1]}  relayed={relA[0]}:{relA[1]}",
                 f"B: mapped={b.mapped[0]}:{b.mapped[1]}  relayed={relB[0]}:{relB[1]}"]
        check(relA != relB, "both peers got the same relayed address")
        need_ok(a.create_permission([tB]), "A CreatePermission(B relay)")
        need_ok(b.create_permission([tA]), "B CreatePermission(A relay)")

        # ---- phase 1: single packet via Send/Data indications (no channel bound yet)
        for (src, dst, rel_src, tgt, nm) in ((a, b, relA, tB, "A->B"), (b, a, relB, tA, "B->A")):
            pl = b"probe-" + nm.encode() + os.urandom(16)
            src.send_ind(tgt, pl)
            ev = dst.wait_event(3)
            check(ev, f"{nm}: probe not delivered through relay")
            check(ev[0] == "data" and ev[2] == pl, f"{nm}: probe mismatch / wrong framing ({ev[0]})")
            check(ev[1] == rel_src or (A.relay_host and ev[1][1] == rel_src[1]),
                  f"{nm}: XOR-PEER-ADDRESS {ev[1]} != sender's relayed address {rel_src}")
        lines.append("phase1 Send/Data indications: delivered intact, XOR-PEER-ADDRESS == sender's relayed address (proves relay path)")

        # ---- phase 2: channel bindings (distinct numbers per side)
        chA, chB = CH_MIN, CH_MIN + 1
        need_ok(a.channel_bind(chA, tB), "A ChannelBind")
        need_ok(b.channel_bind(chB, tA), "B ChannelBind")
        pl = b"chan-probe" + os.urandom(8)
        a.send_channel(chA, pl)
        ev = b.wait_event(3)
        check(ev and ev[2] == pl, "ChannelData A->B not delivered")
        check(ev[0] == "chan" and ev[1] == chB, f"B should receive ChannelData on 0x{chB:x}, got {ev[0]}")
        pl = b"chan-probe2" + os.urandom(8)
        b.send_channel(chB, pl)
        ev = a.wait_event(3)
        check(ev and ev[0] == "chan" and ev[1] == chA and ev[2] == pl, "ChannelData B->A failed")
        lines.append("phase2 ChannelData both ways: delivered intact on the correct channel numbers")

        # ---- phase 3: concurrent bidirectional media stream
        ssrcA, ssrcB = random.getrandbits(32), random.getrandbits(32)
        st_ab = Stats("A->B", ssrcA, relA, chB)       # received by B
        st_ba = Stats("B->A", ssrcB, relB, chA)       # received by A
        stop = threading.Event()

        def rx(cl, st):
            while not stop.is_set():
                try:
                    ev = cl.next_event(0.1)
                except ConnectionError:
                    return
                if ev and ev[0] in ("data", "chan"):
                    st.on_event(ev)

        def tx(cl, tgt, ch, ssrc, st, use_chan):
            interval = 1.0 / A.pps
            t0 = time.monotonic()
            for i in range(A.packets):
                pkt = rtp(ssrc, i, size)
                (cl.send_channel(ch, pkt) if use_chan else cl.send_ind(tgt, pkt))
                st.sent += 1
                st.bytes_sent += len(pkt)
                dly = t0 + (i + 1) * interval - time.monotonic()
                if dly > 0:
                    time.sleep(dly)

        mode = A.stream_mode
        a_chan = mode in ("mixed", "channel")
        b_chan = mode == "channel"
        th = [threading.Thread(target=rx, args=(b, st_ab), daemon=True), threading.Thread(target=rx, args=(a, st_ba), daemon=True)]
        tt = [threading.Thread(target=tx, args=(a, tB, chA, ssrcA, st_ab, a_chan)),
              threading.Thread(target=tx, args=(b, tA, chB, ssrcB, st_ba, b_chan))]
        for t in th + tt:
            t.start()
        for t in tt:
            t.join()
        idle_end = time.time() + 2.0
        while time.time() < idle_end and (st_ab.recv < st_ab.sent or st_ba.recv < st_ba.sent):
            time.sleep(0.05)
        stop.set()
        for t in th:
            t.join(1)
        lines.append(f"phase3 media: {A.packets} pkts x {size}B @ {A.pps} pps each way, mode={mode}")
        lines += ["  " + st_ab.line(), "  " + st_ba.line()]
        E2E_REPORT.update(a_relayed=f"{relA[0]}:{relA[1]}", b_relayed=f"{relB[0]}:{relB[1]}",
                          a_to_b=st_ab.summary(), b_to_a=st_ba.summary())
        for st in (st_ab, st_ba):
            check(st.sent == A.packets, f"{st.name}: sender did not finish")
            check(st.recv > 0, f"{st.name}: no media arrived\n" + "\n".join(lines))
            check(st.corrupt == 0, f"{st.name}: {st.corrupt} corrupted packets\n" + "\n".join(lines))
            check(not st.bad_src, f"{st.name}: wrong source/channel seen {sorted(st.bad_src)}\n" + "\n".join(lines))
            check(st.loss() <= A.max_loss, f"{st.name}: loss {st.loss():.2f}% > {A.max_loss}%\n" + "\n".join(lines))
        lines.append("PROOF: every received packet verified bit-exact (deterministic payload), arrived with the other peer's relayed "
                     "address as source, and was never sent to the receiver's own transport address directly.")
        return "\n".join(lines)


@test("e2e.relay_to_plain_peer", "e2e", "RTP stream client -> relay -> plain UDP peer, echoed back through relay")
def t_e2e_plain():
    size, n = max(40, A.payload_size), min(A.packets, 200)
    with PeerSession() as s:
        s.permit()
        need_ok(s.c.channel_bind(CH_MIN + 20, s.addr), "ChannelBind")
        ssrc = random.getrandbits(32)
        st = Stats("client->peer->client", ssrc, s.c.relayed, CH_MIN + 20)
        got_at_peer = set()
        start = time.monotonic()
        for i in range(n):
            if A.pps > 0:
                delay = start + i / A.pps - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
            s.c.send_channel(CH_MIN + 20, rtp(ssrc, i, size))
            st.sent += 1
            d, src = s.peer.recv(1.0)
            if d is None:
                continue
            got_at_peer.add(i)
            s.peer.s.sendto(d, s.tgt)                       # echo back to the relay
            ev = s.c.wait_event(1.0)
            if ev:
                st.on_event(ev)
        rtt = st.round_trip_summary()
        E2E_REPORT["plain_peer"] = {
            "sent": st.sent,
            "received_at_peer": len(got_at_peer),
            "received": st.recv,
            "loss_pct": round(st.loss(), 2),
            "jitter_ms": round(st.jitter, 3),
            "rtt": rtt,
        }
        check(len(got_at_peer) >= n * (100 - A.max_loss) / 100, f"peer received only {len(got_at_peer)}/{n}")
        check(st.corrupt == 0 and not st.bad_src, f"corrupt={st.corrupt} bad_src={st.bad_src}")
        check(st.loss() <= A.max_loss, f"round-trip loss {st.loss():.2f}%")
        return st.line(rtt=rtt)


# --------------------------------------------------------------------------- RFC 6062 TCP allocations
@test("rfc6062.tcp_allocation", "rfc6062", "TCP allocation: Allocate(TCP) + CreatePermission + Connect + ConnectionBind + data both ways")
def t_6062():
    if A.transport == "udp":
        raise Skip("needs --transport tcp or tls (control connection must be TCP)")
    with Cli() as c:
        r = c.allocate(transport=TRANSPORT_TCP)
        check(r is not None, "no response")
        if r.cls == ERROR:
            raise Warn(f"TCP allocations unsupported: {r.error_text()}")
        lst = socket.socket()
        lst.bind((A.peer_bind or "0.0.0.0", 0))
        lst.listen(1)
        ip = A.peer_host or outbound_ip()
        peer = (ip, lst.getsockname()[1])
        try:
            need_ok(c.create_permission([peer]), "CreatePermission")
            r = need_ok(c.request(M_CONNECT, [(A_XOR_PEER_ADDRESS, xaddr(*peer))]), "Connect")
            cid = r.get(A_CONNECTION_ID)
            check(cid, "no CONNECTION-ID in Connect response")
            lst.settimeout(5)
            peer_sock, _ = lst.accept()
            dc = TurnClient(new_conn(), c.user, c.pw)
            dc.realm, dc.nonce, dc.key = c.realm, c.nonce, c.key
            r = need_ok(dc.request(M_CONN_BIND, [(A_CONNECTION_ID, cid)]), "ConnectionBind")
            dc.conn.sock.settimeout(5)
            dc.conn.sock.sendall(b"tcp-from-client")
            peer_sock.settimeout(5)
            got = peer_sock.recv(100)
            check(got == b"tcp-from-client", f"peer got {got!r}")
            peer_sock.sendall(b"tcp-from-peer")
            got = dc.conn.sock.recv(100)
            check(got == b"tcp-from-peer", f"client got {got!r}")
            peer_sock.close()
            dc.conn.close()
            return "TCP relay both directions ok"
        finally:
            lst.close()


# --------------------------------------------------------------------------- runner
def select_tests():
    pats = [p.strip() for p in A.tests.split(",")] if A.tests else None
    skips = [p.strip() for p in A.skip.split(",")] if A.skip else []
    out = []
    for t in TESTS:
        name, group = t[0], t[1]
        if group == "rfc6062" and not A.rfc6062 and not (pats and any(fnmatch.fnmatch(name, p) or p == group for p in pats)):
            continue
        if pats and not any(p == group or fnmatch.fnmatch(name, p) for p in pats):
            continue
        if any(p == group or fnmatch.fnmatch(name, p) for p in skips):
            continue
        out.append(t)
    return out


def main():
    global A, LOG_V
    p = argparse.ArgumentParser(description="STUN/TURN end-to-end test harness", formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="Examples" + __doc__.split("Examples", 1)[1])
    p.add_argument("--host", help="STUN/TURN server host or IP")
    p.add_argument("--port", type=int, default=3478, help="server port for the chosen --transport (default 3478)")
    p.add_argument("-u", "--username", default=os.environ.get("TURN_USER"), help="long-term username (env TURN_USER)")
    p.add_argument("-p", "--password", default=os.environ.get("TURN_PASS"), help="long-term password (env TURN_PASS)")
    p.add_argument("--secret", default=os.environ.get("TURN_SECRET"),
                   help="shared secret for REST/ephemeral credentials (coturn use-auth-secret); overrides -u/-p")
    p.add_argument("--rest-user", default="harness", help="user part of ephemeral username (default 'harness')")
    p.add_argument("--transport", choices=("udp", "tcp", "tls"), default="udp", help="client->server transport (default udp)")
    p.add_argument("--insecure", action="store_true", help="do not verify the TLS certificate")
    p.add_argument("-4", dest="family", action="store_const", const=socket.AF_INET, default=0, help="force IPv4 to server")
    p.add_argument("-6", dest="family", action="store_const", const=socket.AF_INET6, help="force IPv6 to server")
    p.add_argument("--udp-port", type=int, default=3478, help="UDP port for stun.transports probe")
    p.add_argument("--tcp-port", type=int, default=3478, help="TCP port for stun.transports probe")
    p.add_argument("--tls-port", type=int, default=5349, help="TLS port for stun.transports probe")
    p.add_argument("--timeout", type=float, default=5.0, help="per-request timeout seconds (default 5)")
    p.add_argument("--relay-host", help="send to this IP instead of the XOR-RELAYED-ADDRESS IP (server behind NAT)")
    p.add_argument("--peer-host", help="IP the server should use to reach this machine's plain-peer sockets")
    p.add_argument("--peer-bind", help="local address for plain-peer sockets")
    p.add_argument("--packets", type=int, default=200, help="media packets per direction (default 200)")
    p.add_argument("--pps", type=float, default=50.0, help="packets per second per direction (default 50)")
    p.add_argument("--payload-size", type=int, default=172, help="RTP packet size in bytes incl. header (default 172)")
    p.add_argument("--stream-mode", choices=("mixed", "channel", "indication"), default="mixed",
                   help="mixed: A uses ChannelData, B uses Send indications (default)")
    p.add_argument("--max-loss", type=float, default=1.0, help="max acceptable media loss %% (default 1.0)")
    p.add_argument("--tests", help="comma list of group names or glob patterns (stun, turn, e2e, codec, rfc6062, 'turn.channel*')")
    p.add_argument("--skip", help="comma list of groups/globs to skip")
    p.add_argument("--rfc6062", action="store_true", help="include RFC 6062 TCP-allocation test (needs --transport tcp|tls)")
    p.add_argument("--strict", action="store_true", help="treat conformance warnings as failures")
    p.add_argument("--json", metavar="FILE", help="write machine-readable report")
    p.add_argument("--list", action="store_true", help="list tests and exit")
    p.add_argument("-v", "--verbose", action="store_true", help="dump every STUN message sent/received")
    A = p.parse_args()
    LOG_V = A.verbose

    if A.list:
        for n, g, _, d in TESTS:
            print(f"{n:38s} [{g}] {d}")
        return 0
    chosen = select_tests()
    needs_net = any(t[1] != "codec" for t in chosen)
    if needs_net and not A.host:
        p.error("--host is required")
    needs_auth = any(t[1] in ("turn", "e2e", "rfc6062") for t in chosen)
    if needs_auth and not A.secret and not (A.username and A.password):
        p.error("TURN tests need -u/-p or --secret (or use --tests stun)")

    tty = sys.stdout.isatty()
    col = {"PASS": "32", "FAIL": "31", "WARN": "33", "SKIP": "36"}
    results = []
    print(f"target {A.host}:{A.port}/{A.transport}   {len(chosen)} tests\n")
    for name, group, fn, desc in chosen:
        t0 = time.time()
        try:
            detail, status = fn() or "", "PASS"
        except Fail as e:
            detail, status = str(e), "FAIL"
        except Warn as e:
            detail, status = str(e), "FAIL" if A.strict else "WARN"
        except Skip as e:
            detail, status = str(e), "SKIP"
        except Exception as e:
            detail, status = f"{type(e).__name__}: {e}" + ("\n" + traceback.format_exc() if A.verbose else ""), "FAIL"
        dt = time.time() - t0
        results.append(dict(name=name, group=group, status=status, seconds=round(dt, 2), detail=detail))
        tag = f"\033[{col[status]}m{status}\033[0m" if tty else status
        first, _, rest = detail.partition("\n")
        print(f"[{tag}] {name:36s} {dt:5.1f}s  {first}")
        for ln in rest.splitlines():
            print(" " * 8 + ln)
        sys.stdout.flush()

    cnt = collections.Counter(r["status"] for r in results)
    print(f"\nSUMMARY: {cnt['PASS']} passed, {cnt['FAIL']} failed, {cnt['WARN']} warnings, {cnt['SKIP']} skipped")
    if A.json:
        with open(A.json, "w") as f:
            json.dump(dict(target=f"{A.host}:{A.port}", transport=A.transport, time=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                           results=results, e2e=E2E_REPORT), f, indent=2)
        print(f"report written to {A.json}")
    return 1 if cnt["FAIL"] else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)