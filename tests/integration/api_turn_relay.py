#!/usr/bin/env python3
import argparse
import base64
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import sys
import time
from types import SimpleNamespace
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit
from urllib.request import HTTPSHandler, HTTPRedirectHandler, Request, build_opener

from turn_relay import exercise


class RefuseRedirects(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def request_headers(args):
    prefs = getattr(args, "prefs", None)
    if prefs is None:
        return json.loads(args.auth_file.read_text())

    content = prefs.read_text()
    fields = {}
    for name in ("ufsrvuid", "password", "cookie"):
        match = re.search(rf'(?m)^\s*{name}\s*=\s*"([^"\n]+)"', content)
        if not match:
            raise ValueError(f"Test account prefs lack {name}")
        fields[name] = match.group(1)
    basic = base64.b64encode(f"{fields['ufsrvuid']}:{fields['password']}".encode()).decode()
    return {"Authorization": "Basic " + basic, "Cookie": fields["cookie"]}


def fetch_credentials(args):
    headers = request_headers(args)
    authorization = headers.get("Authorization")
    if not authorization or not authorization.startswith(("Bearer ", "Basic ")):
        raise ValueError("API auth file must contain a Bearer or Basic Authorization header")
    headers["Accept"] = "application/json"

    context = ssl.create_default_context(cafile=str(args.api_ca_file) if args.api_ca_file else None)
    opener = build_opener(RefuseRedirects(), HTTPSHandler(context=context))
    with opener.open(Request(args.api_url, headers=headers), timeout=args.timeout) as response:
        cache_control = response.headers.get("Cache-Control", "")
        if "no-store" not in {item.strip().lower() for item in cache_control.split(",")}:
            raise ValueError("API credential response must include Cache-Control: no-store")
        size_limit = 65536
        payload = response.read(size_limit + 1)
        if len(payload) > size_limit:
            raise ValueError("API credential response is too large")
        data = json.loads(payload)

    if not isinstance(data.get("expiresAt"), int) or data["expiresAt"] <= time.time():
        raise ValueError("API returned expired credentials")
    for field in ("username", "credential"):
        if not isinstance(data.get(field), str) or not data[field]:
            raise ValueError(f"API credential response lacks {field}")
    if not isinstance(data.get("urls"), list) or not all(isinstance(uri, str) for uri in data["urls"]):
        raise ValueError("API credential response lacks TURN URLs")
    return data


def endpoints(urls):
    result = {}
    for url in urls:
        scheme, separator, rest = url.partition(":")
        if not separator or scheme not in {"turn", "turns"}:
            continue
        parsed = urlsplit("//" + rest)
        transport = parse_qs(parsed.query).get("transport", ["udp" if scheme == "turn" else "tcp"])
        if parsed.hostname and transport == ["udp"]:
            name = "udp" if scheme == "turn" else "dtls"
            result.setdefault(name, (parsed.hostname, parsed.port or (3478 if name == "udp" else 5349)))
    return result


def main():
    parser = argparse.ArgumentParser(description="Fetch API-issued TURN credentials and test the advertised relays.")
    parser.add_argument("--api-url", required=True, help="Full URL of the authenticated /v1/turn route")
    authentication = parser.add_mutually_exclusive_group(required=True)
    authentication.add_argument("--auth-file", type=Path, help="Private JSON file with API request headers")
    authentication.add_argument("--prefs", type=Path, help="Existing ufsrvcli test account prefs")
    parser.add_argument("--api-ca-file", type=Path)
    parser.add_argument("--turn-ca-file", type=Path)
    parser.add_argument("--realm", required=True)
    parser.add_argument("--transport", choices=("udp", "dtls", "both"), default="both")
    parser.add_argument("--harness-e2e", action="store_true", help="Run the UDP two-peer and plain-peer media tests")
    parser.add_argument("--packets", type=int, default=200, help="Media packets per direction for --harness-e2e")
    parser.add_argument("--pps", type=float, default=20, help="Media packets per second per direction")
    parser.add_argument("--report", type=Path, help="Optional JSON media test report")
    parser.add_argument("--peer-bind")
    parser.add_argument("--timeout", type=float, default=4)
    args = parser.parse_args()

    if args.harness_e2e and args.transport == "dtls":
        parser.error("--harness-e2e supports UDP")
    selected = ("udp",) if args.harness_e2e else (
        ("udp", "dtls") if args.transport == "both" else (args.transport,)
    )
    if "dtls" in selected and not args.turn_ca_file:
        parser.error("DTLS requires --turn-ca-file")
    try:
        issued = fetch_credentials(args)
        targets = endpoints(issued["urls"])
        missing = set(selected) - targets.keys()
        if missing:
            raise ValueError("API did not advertise the required TURN transports: " + ", ".join(sorted(missing)))
    except HTTPError as error:
        parser.exit(1, f"API credential request failed with HTTP {error.code}\n")
    except (URLError, OSError, ValueError) as error:
        parser.exit(1, f"API credential test could not start: {error}\n")

    print("PASS authenticated API issuance", flush=True)
    if args.harness_e2e:
        host, port = targets["udp"]
        command = [sys.executable, str(Path(__file__).with_name("stun_turn_harness.py")),
                   "--host", host, "--port", str(port), "--tests", "e2e",
                   "--packets", str(args.packets), "--pps", str(args.pps),
                   "--timeout", str(args.timeout), "--max-loss", "0"]
        if args.report:
            command += ["--json", str(args.report)]
        if args.peer_bind:
            command += ["--peer-bind", args.peer_bind]
        env = dict(os.environ, TURN_USER=issued["username"], TURN_PASS=issued["credential"])
        env.pop("TURN_SECRET", None)
        raise SystemExit(subprocess.run(command, env=env).returncode)

    credentials = {"username": issued["username"], "realm": args.realm, "password": issued["credential"]}
    for transport in selected:
        host, port = targets[transport]
        print(f"Testing API-issued credentials over {transport.upper()}", flush=True)
        exercise(SimpleNamespace(host=host, port=port, transport=transport, server_name=host,
                                 ca_file=args.turn_ca_file, peer_bind=args.peer_bind,
                                 timeout=args.timeout, require_username=True), credentials)


if __name__ == "__main__":
    main()
