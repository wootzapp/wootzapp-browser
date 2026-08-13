#!/usr/bin/env python3
"""Print Chromium runtime status from the public CDP endpoint."""

from __future__ import annotations

import argparse
import http.client
import json
import os
import sys
from typing import Any


def env_int(name: str, default: int) -> int:
    value = os.environ.get(name)
    return int(value) if value else default


def request_version(timeout: float) -> dict[str, Any]:
    host = os.environ.get("CDP_PROBE_HOST", "127.0.0.1")
    port = env_int("CDP_PORT", 9222)
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request("GET", "/json/version")
        response = conn.getresponse()
        body = response.read().decode("utf-8", errors="replace")
    finally:
        conn.close()
    if response.status < 200 or response.status >= 300:
        raise RuntimeError(f"HTTP {response.status}: {body[:300]}")
    payload = json.loads(body)
    if not isinstance(payload, dict):
        raise RuntimeError("/json/version did not return an object")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--timeout", type=float, default=2.0)
    args = parser.parse_args()

    status: dict[str, Any] = {
        "schema": "chromium-image.status.v1",
        "cdp_url": f"http://{os.environ.get('CDP_PROBE_HOST', '127.0.0.1')}:{env_int('CDP_PORT', 9222)}",
        "headless": os.environ.get("CHROMIUM_HEADLESS", "1"),
        "novnc_url": (
            f"http://127.0.0.1:{env_int('NOVNC_PORT', 6080)}/vnc.html?host=127.0.0.1&port={env_int('NOVNC_PORT', 6080)}&autoconnect=1&resize=scale"
            if os.environ.get("CHROMIUM_HEADLESS", "1") != "1"
            else None
        ),
    }
    try:
        version = request_version(args.timeout)
        status.update(
            {
                "cdp_ready": True,
                "browser": version.get("Browser"),
                "protocol_version": version.get("Protocol-Version"),
                "web_socket_debugger_url": version.get("webSocketDebuggerUrl"),
            }
        )
    except Exception as exc:
        status.update({"cdp_ready": False, "error": str(exc)})

    if not args.quiet:
        print(json.dumps(status, indent=2, sort_keys=True))
    return 0 if status["cdp_ready"] else 1


if __name__ == "__main__":
    sys.exit(main())
