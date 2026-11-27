#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export PYTHONDONTWRITEBYTECODE=1
exec python3 "$script_dir/api_turn_relay.py" \
    --api-url http://172.19.0.31:8080/v1/turn \
    --realm ufsrvstun.flat.docker \
    --harness-e2e "$@"
