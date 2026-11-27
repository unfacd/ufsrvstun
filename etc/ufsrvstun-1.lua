return {
    ufsrv = {
        ufsrv_geogroup = 0,
        sys_runtime = {},
        listeners = {
            { name = "stun", protocol = "stun", address = "127.0.0.1", port = 3478 },
            { name = "stun-dtls", protocol = "stun-dtls", address = "127.0.0.1", port = 5349 },
        },
        -- Shared TLS command console. The listener starts only when enabled.
        command_console_address = "127.0.0.1",
        command_console_port = 19700,
        ssl_command_console = {
            is_enabled = false,
            -- Absolute PEM paths or paths relative to the config root's etc directory.
            certificate_file = "ufsrv_console_certificate.pem",
            key_file = "ufsrv_console.key",
            -- Require a client certificate with an allow-listed SHA-256 SPKI fingerprint.
            is_client_certificate_required = false,
            allow_list_god = {},
            deny_list = {},
        },
    },
    ufsrvstun = {
        -- Start the UDP listener. When false, the server validates config and exits.
        is_enabled = true,

        -- Each shard owns one UDP socket. Workers are assigned to a single shard.
        shard_count = 2,
        workers_per_shard = 2,

        -- Buckets allocated when a shard first indexes a peer. The index can grow.
        -- A value of 1 allocates the minimum of 2 buckets.
        shard_hash_table_startup_size = 64,

        -- "optional" validates fingerprints when present. "required" drops requests without one.
        fingerprint_policy = "optional",

        -- Maximum UDP payload size in bytes. Larger datagrams are dropped before worker dispatch.
        max_request_bytes = 1200,

        -- Tokens refilled per second per source IP. The limit spans all shards and source ports.
        binding_rate_limit_per_source = 50,

        -- Maximum tokens saved per source. Each admitted datagram consumes one before decoding.
        binding_rate_limit_burst = 100,

        turn = {
            -- Enable peer flows on plain UDP for TURN. Binding remains available.
            is_enabled = false,
            -- Realm sent in TURN challenges and used to derive credential keys.
            realm = "",
            nonce_lifetime_seconds = 600,
            credential_provider = "standalone",
            realm_credential_secret = "",
            max_credential_lifetime_seconds = 3600,
            -- File containing one enc:v1 envelope for the credential records.
            credential_file = "",
            -- enc:v1 envelope containing a 32-byte nonce secret encoded as 64 hex characters.
            nonce_secret = "",
            relay_bind_address = "",
            relay_advertised_address = "",
            relay_min_port = 0,
            relay_max_port = 0,
            peer_allow_cidrs = {},
            peer_deny_cidrs = {},
            max_permissions_per_allocation = 1024,
            max_channels_per_allocation = 256,
            -- Duration granted when a request omits LIFETIME.
            allocation_lifetime_seconds = 600,
            -- Maximum lifetime granted to Allocate and Refresh.
            max_allocation_lifetime_seconds = 3600,
            -- Concurrent allocations allowed per authenticated principal.
            max_allocations_per_principal = 8,
            -- Maximum retained peer flows per plain UDP shard.
            flows_per_shard = 256,
            -- Idle timeout for plain UDP flows without a live allocation lease.
            flow_idle_timeout_seconds = 60,
            -- Maximum queued outbound sends per plain UDP shard.
            sends_per_shard = 256,
        },

        dtls = {
            -- Start a secure UDP listener alongside the plain listener.
            is_enabled = false,
            -- Empty inherits main_listener_address. DTLS requires a concrete address.
            bind_address = "",
            port = 5349,
            -- PEM files. Both paths are required when the secure listener is enabled.
            certificate_chain_file = "",
            private_key_file = "",
            -- Bound retained associations and queued outbound datagrams on each shard.
            flows_per_shard = 256,
            sends_per_shard = 256,
            -- Reclaim idle associations after this many seconds.
            flow_idle_timeout_seconds = 60,
        },
    },
}
