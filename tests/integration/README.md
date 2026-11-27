# TURN integration tests

## Docker TURN media harness

From the repository root, run the wrapper with a Docker test account that
can authenticate to `GET /v1/turn`.

```sh
tests/integration/run_stun_turn_harness.sh \
  --prefs ~/ufsrvcli/test_agents/3/ufsrvcli.prefs \
  --packets 200 --pps 20 --report /tmp/stun-turn-harness.json
```

Use `--auth-file /path/to/private-api-headers.json` if you have API headers
instead of a CLI profile. The wrapper defaults to the Docker API address
and realm, both overridable with `--api-url` and `--realm`. The API must
advertise a reachable UDP TURN endpoint backed by the Unfacd provider.
The test passes the issued credentials to the harness via its process environment.

The wrapper runs two tests with a zero-loss threshold.

| Test | What it checks |
| --- | --- |
| `e2e.two_peer_relay` | Two independent allocations exchange deterministic media in both directions. Checks the relay source, payload, loss, duplicates, and ordering. |
| `e2e.relay_to_plain_peer` | One TURN client exchanges media with a plain UDP peer. The peer echoes each datagram, and the harness verifies the returned ChannelData. |

The defaults are 200 packets and 20 packets per second per direction.
The plain-peer test caps its packet count at 200. Its RTT measures client
send through the TURN relay, peer echo, and return to the same client. It
reports sample count and min, mean, p50, p95, p99, and max in milliseconds.
The two-peer test reports one-way latency per direction. Both tests report
jitter, throughput, and loss. With `--report`, RTT is under `e2e.plain_peer.rtt`.
The Docker run at these defaults delivered 200/200 packets in each
two-peer direction and 200/200 plain-peer round trips. Use the separate
DTLS probe below for DTLS relay coverage.

### Docker UDP RTT baseline

Recorded on 2026-11-20 using API-issued Unfacd credentials. The wrapper ran
with `--packets 20 --pps 20` and 172-byte RTP-shaped payloads. The client
and plain UDP peer ran on the Docker host. The relay ran in the container.

All 20 round trips completed with zero loss or corruption. Reported jitter
was 0.078 ms.

| RTT statistic | Milliseconds |
| --- | ---: |
| Min | 0.369 |
| Mean | 0.485 |
| p50 | 0.458 |
| p95 | 0.615 |
| p99 | 0.710 |
| Max | 0.710 |

## Relay proof test

The stdlib-only `stun_turn_harness.py` includes a general STUN/TURN suite
and a two-peer relay proof. The Docker command above selects the media tests
supported by this deployment.

**Quick start**
```bash
python3 tests/integration/stun_turn_harness.py --host turn.example.com -u alice -p secret
python3 tests/integration/stun_turn_harness.py --host turn.example.com --secret 'static-auth-secret'      # coturn use-auth-secret (ephemeral creds)
python3 tests/integration/stun_turn_harness.py --host turn.example.com --transport tls --port 5349 -u a -p b --insecure
python3 tests/integration/stun_turn_harness.py --host 10.0.0.5 -u a -p b --tests e2e --packets 1000 --pps 100 --json report.json
python3 tests/integration/stun_turn_harness.py --list
```

**Switches**
- **Server and credentials:** `--host`, `--port`, `-u`/`-p` (or the `TURN_USER`/`TURN_PASS` env vars), `--secret`, `--rest-user`.
- **Transport:** `--transport udp|tcp|tls`, `--insecure`, `-4`/`-6`.
- **Run control:** `--tests` and `--skip` take group names or globs, plus `--strict`, `--json`, `-v` (dumps every STUN message) and `--timeout`.
- **Media stream:** `--packets`, `--pps`, `--payload-size`, `--stream-mode`, `--max-loss`.
- **NAT and firewall setups:** `--relay-host` and `--peer-host`.

**What it covers**
- **STUN:** Binding, FINGERPRINT, indications, unknown attributes (420), odd attribute padding, ICE attributes, malformed packets, RFC 5780 CHANGE-REQUEST and OTHER-ADDRESS, and a UDP/TCP/TLS probe.
- **TURN auth:** 401 challenge, wrong user or password, tampered MESSAGE-INTEGRITY, stale nonce (438), and SHA-256 password algorithm (RFC 8489).
- **TURN allocation:** valid Allocate, missing or bad transport (400/442), double Allocate (437), DONT-FRAGMENT, EVEN-PORT with RESERVATION-TOKEN, IPv6 and dual-stack, and Refresh including lifetime 0.
- **TURN data path:** CreatePermission, ChannelBind validity rules, Send/Data indications, ChannelData, payload sizes from 0 to 1400 bytes, and permission enforcement.
- **Security audit:** `turn.peer_address_policy` checks whether the server lets clients relay to loopback, link-local, multicast and other internal addresses (a common TURN misconfiguration).
- **RFC 6062 TCP allocations:** Allocate, Connect and ConnectionBind. This one is off by default; enable it with `--rfc6062` and `--transport tcp`.

**The relay proof (`e2e.two_peer_relay`)**
Two independent clients each allocate a relay and grant each other permission. They then exchange RTP-shaped media in both directions at the same time, one side over ChannelData and the other over Send indications. The test proves relaying in these ways:
- Every payload is deterministic, so corruption, loss, duplicates and reordering are detected.
- The source address the server reports (XOR-PEER-ADDRESS, or the channel number) must equal the other peer's relayed address.
- Packets are only ever addressed to the relay, never to the other peer directly.
- It reports latency p50/p95/max, jitter and throughput per direction, and fails if loss exceeds `--max-loss`.

`e2e.relay_to_plain_peer` also runs a stream through the relay to a plain UDP socket and echoes it back.

**Things to watch for**
- Servers like coturn deny relaying to loopback or private addresses by default. You'll get a `403` with a hint; use public IPs or set `allow-loopback-peers` / `allowed-peer-ip`. If the relay address is internal because of NAT, use `--relay-host`.
- A WARN means the server departed from a MUST or SHOULD in the RFC, or doesn't support an optional feature. `--strict` turns WARNs into failures.
- The exit code is 1 if any test fails, so you can use it in CI.
- DTLS control transport isn't included. TLS and TCP are.

Run from the STUN repository root. Supply a private credential JSON file
with `username`, `realm`, and `password`. The standalone minting script
writes this format.

## Docker UDP

```sh
python3 tests/integration/turn_relay.py \
  --host 172.19.0.33 --port 3478 --credentials etc/turn-client.json
```

The probe checks signed Allocate and retransmission, permission gating,
Send/Data and ChannelData in both directions, Refresh, and deallocation.
The client and peer must both be able to reach the advertised relay address.
Use `--peer-bind` when the peer should use a different local address.

## Docker DTLS

```sh
python3 tests/integration/docker_dtls.py \
  --host 172.19.0.33 --credentials etc/turn-client.json
```

The fixture runs the deployed binary in a temporary Docker process on spare
ports. It uses the deployed standalone records, wrapping key, and nonce
envelope, and creates a temporary certificate for hostname verification.
The installed service config stays unchanged.

For a running DTLS listener, use `turn_relay.py --transport dtls` with
`--server-name` and `--ca-file`.

## API-issued Unfacd credentials

Place the API request headers in a private JSON file. Use
`{"Authorization":"Bearer …"}` or Basic authorization with the account's
`Cookie` header. The runner holds the issued TURN password in memory.

```sh
python3 tests/integration/api_turn_relay.py \
  --api-url https://ufsrvapi.flat.docker/v1/turn --api-ca-file /path/to/api-ca.pem \
  --auth-file /path/to/private-api-headers.json --realm ufsrvstun.flat.docker \
  --turn-ca-file /path/to/stun-ca.pem
```

The API must advertise `turn:…?transport=udp` for UDP and
`turns:…?transport=udp` for DTLS. STUN must use the Unfacd provider with
the same underlying realm secret as the API. The API credential lifetime
must fit STUN's maximum credential lifetime.

## Natural expiry on Docker

Use the same API credentials and certificates, and specify the address
where STUN binds the relay sockets inside the container.

```sh
python3 tests/integration/docker_turn_expiry.py \
  --api-url http://172.19.0.31:8080/v1/turn \
  --auth-file /path/to/private-api-headers.json --realm ufsrvstun.flat.docker \
  --turn-ca-file /path/to/stun-ca.pem --relay-bind 172.19.0.33
```

The probe opens UDP and DTLS allocations in parallel and waits for the
server-granted lifetime. It checks that each relay socket remains bound
until shortly before its deadline and is released after expiry.
