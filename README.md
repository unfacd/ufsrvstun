# ufsrvstun

`ufsrvstun` is a Ufsrv server project for STUN over UDP. It owns the STUN
protocol and host configuration. Its only direct Ufsrv dependency is
`ufsrvcorelib`, which owns the shared server infrastructure.

| Directory | Purpose |
| --- | --- |
| `src/ufsrvstun/` | STUN protocol implementation and UDP integration |
| `src/ufsrv_core/ufsrv_config/` | Server-specific configuration adapters |
| `src/include/` | Application headers |
| `etc/` | Runtime configuration |
| `tests/` | Protocol and server tests |
| `dev/technical_designs/` | Design documents |
| `docs/` | Operational documentation |
| `devops/` | Deployment tooling |

This mirror does not publish `dev/technical_designs/` or `devops/`; the deployed
tree carries the code, the sample configuration and the tests.

Build against an installed `ufsrvcorelib` that exports `UdpServerStart`
and its UDP header. Use `CMAKE_PREFIX_PATH` for a separate install prefix.

```sh
./build_install_verify.sh dev
./build_install_verify.sh release --test
```

The script composes the configuration, builds the executable, and verifies
its version and linkage. Pass `--bypass-local` to build with uncommitted
changes. Docker and remote deployment are currently disabled.

Running `./build/dev/ufsrvstun` listens on IPv4 loopback at UDP port 3478.
Use `--port PORT` to override it. The server uses `src/rfc_codec` for unauthenticated STUN Binding,
including optional FINGERPRINT validation and replies, and exits cleanly on SIGINT or SIGTERM. See the
[UDP wiring design](dev/technical_designs/UDP_PROTOCOL_WIRING.md) for the
callback and thread lifecycle.

## Configuration

The build stages `ufsrvstun-1.lua` in its `etc/` directory and the schemas in
`etc/lua/`. The server loads this default file before opening sockets.
Use `-C DIR` to select a runtime root and `-c FILE` to select a config file.
Relative filenames resolve under `DIR/etc/`. Absolute filenames are accepted.

```sh
./build/dev/ufsrvstun -C build/dev -c ufsrvstun-1.lua --config=verify-strict
./build/dev/ufsrvstun --port 3479 --dump-config-json=./effective-config.json
```

Startup and `--config=verify-strict` reject unknown keys in recognized scopes.
`--config=verify` allows unknown keys. CLI overrides apply after Lua is loaded.
The `ufsrv` scope supplies the numeric IPv4 or IPv6 bind address, UDP port, and
protocol ID (0). The `ufsrvstun` scope supplies `shard_count` (one socket
per shard) and `workers_per_shard` (workers assigned to each shard). Both
default to 2. Startup ignores the inherited listener and session worker
pool settings. Set `ufsrvstun.is_enabled = true` to start the listener.
The staged sample enables it.

After changing the schema or field catalogue, remove
`src/ufsrv_core/ufsrv_config/.composed` and run
`cmake --build build/dev --target ufsrv_config_compose_host`. Review and commit
the Lua schema, the STUN `.c.input` catalogue under
`dev/scripts/ufsrv_config_generator/ufsrvstun/`, and the composed files under
`src/ufsrv_core/ufsrv_config/` and
`src/include/ufsrvcorelib/ufsrv_core/ufsrv_config/`. Git ignores the other
generator staging files and build scratch directories.

Ordinary builds consume the checked-in config tree. With maintainer targets
enabled, the build checks the tree and its `.composed` stamp. Use
`-DUFSRV_CONFIG_COMPOSE=OFF` to omit the maintainer targets.

| STUN setting | Default | Behavior |
| --- | --- | --- |
| `fingerprint_policy` | `optional` | Checks fingerprints when present. `required` drops requests without one. |
| `max_request_bytes` | 1200 | Drops larger datagrams before worker dispatch. |
| `binding_rate_limit_per_source` | 50 per second | Refill rate for each source IP, shared across shards and source ports. |
| `binding_rate_limit_burst` | 100 | Maximum tokens per source. Each admitted datagram uses one token before decoding. |

| TURN setting | Default | Behavior |
| --- | --- | --- |
| `relay_bind_address` | Main listener address | Numeric local address for relay sockets. Set a concrete address when listening on a wildcard. |
| `relay_advertised_address` | Relay bind address | Numeric address returned in Allocate. The bind and advertised addresses use the same family, and a NAT must preserve the allocated port. |
| `relay_min_port`, `relay_max_port` | Both zero | Zero uses OS-selected ports. A nonzero inclusive range is used for UDP relay sockets. |
| `peer_allow_cidrs`, `peer_deny_cidrs` | Empty | Applies to permission creation and both relay directions. An empty allow list adds no restriction. Deny entries take precedence, and built-in address restrictions still apply. |
| `max_permissions_per_allocation` | 1024 | Limits the number of active peer IP permissions per allocation. |
| `max_channels_per_allocation` | 256 | Limits each allocation to 1–4096 active channel bindings. Existing bindings can renew at the limit. |

The source table is bounded. A new source whose bucket holds only active
entries is dropped until a slot becomes reusable.

### DTLS listener

Set `ufsrvstun.dtls.is_enabled = true` and provide
`certificate_chain_file` and `private_key_file` as PEM paths. Relative paths
resolve from the process working directory. The DTLS listener runs alongside
plain UDP on port 5349 by default. Its `bind_address` inherits the plain bind
address unless set explicitly and must be a concrete numeric address.

`shard_hash_table_startup_size` sets the bucket count when a flow-mode shard
first indexes a peer. The default is 64. The index can grow independently
of the retained-flow cap.

DTLS uses the same shard and worker counts as the plain listener, with a
separate worker pool. Defaults are 256 retained flows and 256 queued sends
per shard, with a 60-second flow idle timeout. The request size, fingerprint,
and per-source rate policies apply to decrypted STUN messages. The rate
limit is shared across both listeners. `--port` overrides the plain listener.


### TURN transport

`ufsrvstun.turn.is_enabled` defaults to false. Enabling it switches the
plain UDP listener to flow mode and requires a concrete bind address.
The plain UDP listener reads its limits from `ufsrvstun.turn`.

| Key | Default |
| --- | ---: |
| `flows_per_shard` | 256 |
| `flow_idle_timeout_seconds` | 60 |
| `sends_per_shard` | 256 |
| `allocation_lifetime_seconds` | 600 |
| `max_allocation_lifetime_seconds` | 3600 |
| `max_allocations_per_principal` | 8 |

The last three settings are validated and reserved for the upcoming TURN
allocation handlers. The default allocation lifetime must be at most the
configured maximum. Both listener modes serve STUN Binding and can authenticate TURN requests.
Relay handlers are pending.

### Standalone TURN credentials

TURN startup requires `turn.realm`, `turn.credential_file`, and
`turn.nonce_secret` when `turn.is_enabled` is true. The nonce lifetime
defaults to 600 seconds. The server reads its 64-hex wrapping key from
`$CREDENTIALS_DIRECTORY/ufsrv_secret_key` under systemd or
`/opt/ufsrv/etc/secrets/ufsrv_secret_key` otherwise.

Use corelib's `dev/encrypt_plaintext/encrypt_plaintext.py` to prepare both
envelopes. Put the value after `VALUE=` in Lua for the nonce secret. Store
the credential envelope by itself in the configured file. Relative
credential paths resolve under the config root's `etc/` directory.

```sh
key_path=/opt/ufsrv/etc/secrets/ufsrv_secret_key
openssl rand -hex 32 | python3 ../ufsrvcorelib/dev/encrypt_plaintext/encrypt_plaintext.py --credential-file "$key_path"
python3 ../ufsrvcorelib/dev/encrypt_plaintext/encrypt_plaintext.py --credential-file "$key_path" < records.tsv |
    sed -n 's/^VALUE=//p' > turn-credentials.enc
```

To mint one username and a random password, write the encrypted records and
the client credentials to separate private files.

```sh
dev/scripts/mint_turn_credentials.sh --realm relay.example --username turn-test \
  --key-file "$key_path" --output turn-credentials.enc --client-output turn-client.json
```

Each decrypted credential row has five tab-separated fields:

```text
username    principal_hex    md5_key_hex    sha256_key_hex    expiry_unix
```

Use one realm for all rows and `-` for an absent key algorithm. Derive each
key using the corresponding digest of `username:realm:password`. A principal
is 1–32 bytes encoded as hex. Expiry `0` means no expiry. Blank lines and
lines starting with `#` are ignored. The nonce envelope is redacted in
effective configuration dumps. Keep plaintext provisioning input restricted.

Until the TURN relay handlers are installed, authenticated requests receive
a signed 500 response. Unauthenticated requests still get the normal
challenge and expired nonces get a fresh challenge.

### Command console

Set `ufsrv.ssl_command_console.is_enabled = true` to start the TLS command
console with the STUN service. It binds `ufsrv.command_console_address`
and `ufsrv.command_console_port`, which default to `127.0.0.1:19700`.
Supply PEM certificate-chain and key paths. Relative paths resolve under
the config root's `etc/` directory.

The console accepts one newline-terminated request per connection.
`GET /internals/version/self` and `GET /internals/version/linked_libs`
return JSON. Set `is_client_certificate_required = true` and populate
`allow_list_god` to require a pinned client certificate. Pins are lowercase
SHA-256 digests of DER SubjectPublicKeyInfo. The deny list takes precedence.
