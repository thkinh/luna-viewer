#!/usr/bin/env python3
"""Create a private LunaViewer registration profile for this Linux machine."""

import argparse
import base64
import getpass
import ipaddress
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys


def default_route():
    result = subprocess.run(
        ["ip", "-j", "-4", "route", "get", "1.1.1.1"],
        check=True,
        capture_output=True,
        text=True,
    )
    routes = json.loads(result.stdout)
    if not routes:
        raise ValueError("No IPv4 route found")
    return routes[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", help="Network interface to register")
    parser.add_argument("--local-ip", help="IPv4 address on that interface")
    parser.add_argument("--output", default="identity-profile.json", help="Output file")
    args = parser.parse_args()

    try:
        if args.interface and args.local_ip:
            interface, local_ip = args.interface, args.local_ip
        else:
            route = default_route()
            interface = args.interface or route.get("dev", "")
            local_ip = args.local_ip or route.get("prefsrc") or route.get("src", "")

        if not interface or "/" in interface or interface in (".", ".."):
            raise ValueError("Could not determine a network interface; pass --interface")
        local_ip = str(ipaddress.IPv4Address(local_ip))
        mac_path = Path("/sys/class/net") / interface / "address"
        mac = mac_path.read_text(encoding="ascii").strip().replace(":", "").upper()
        if len(mac) != 12 or any(c not in "0123456789ABCDEF" for c in mac):
            raise ValueError("Could not read a valid MAC address for " + interface)

        template = Path(__file__).resolve().parents[1] / "identity-profile.example.json"
        profile = json.loads(template.read_text(encoding="utf-8"))
        hostname = socket.gethostname().encode("ascii", "ignore").decode("ascii")
        hostname = "".join(c for c in hostname if c.isalnum() or c == "-") or "linux"
        profile["display_name"] = ("LunaViewer-" + hostname)[:20]
        fields = profile["registration_fields"]
        fields[1] = secrets.token_hex(16).upper()
        fields[2] = f"{secrets.randbelow(100_000_000):08d}"
        fields[4] = local_ip
        fields[6] = base64.b64encode(socket.gethostname().encode("utf-8")).decode("ascii")
        username = getpass.getuser()
        fields[16] = (
            username
            if username.isascii() and ":" not in username
            else base64.b64encode(username.encode("utf-8")).decode("ascii")
        )
        fields[8] = mac

        output = Path(args.output)
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(profile, stream, indent=2)
            stream.write("\n")
        print(f"Created private profile: {output} (mode 0600). Keep it for future runs.")
    except FileExistsError:
        print(f"Refusing to overwrite existing file: {args.output}", file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, json.JSONDecodeError, subprocess.SubprocessError) as error:
        print(f"Could not create profile: {error}", file=sys.stderr)
        print("If route detection failed, pass --interface NAME --local-ip ADDRESS.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
