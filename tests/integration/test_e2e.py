#!/usr/bin/env python3
"""The real forti-hole binary through the real forti-engine (libcurl, verified TLS, plan/apply, blob-stored
push bodies, audit) against forti-cli's stateful mock FortiOS, with lists served from file:// URLs.

The mock is extended here with what forti-hole needs and forti-cli's own tests do not: the
system/external-resource, dnsfilter/profile and dnsfilter/domain-filter tables, the feed entry list
with FortiOS's paging and counters (accepted/valid/invalid, accepted_limit, overflow,
last_content_update_time), the load delay of a new feed ("Connect pending"), and memory figures.

Exit 77 (skipped) without openssl, the mock or the binary.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

MOCK_DIR = os.environ.get("FORTI_MOCK_DIR", "")
if not MOCK_DIR or not Path(MOCK_DIR, "harness.py").exists():
    print("SKIP: FORTI_MOCK_DIR does not point at forti-cli's mock FortiOS", file=sys.stderr)
    sys.exit(77)
sys.path.insert(0, MOCK_DIR)

import mock_fortios  # noqa: E402
from harness import KEY_VAR, API_KEY, Sandbox, binary, require_tools  # noqa: E402
from mock_fortios import TABLES, TableSpec  # noqa: E402

PROFILE = "lab"
FAILURES: list[str] = []


def check(condition: bool, what: str) -> None:
    if not condition:
        FAILURES.append(what)
        print(f"FAIL: {what}", file=sys.stderr)


# ---- the mock, extended -------------------------------------------------------------------------

for path, mkey, numeric in (("system/external-resource", "name", False), ("dnsfilter/profile", "name", False),
                            ("dnsfilter/domain-filter", "id", True), ("system/dns-server", "name", False)):
    TABLES.setdefault(path, TableSpec(path, mkey, numeric, f"mock {path}", {}, lambda: {}))

LIMIT = 1_000_000
STATE: dict[str, Any] = {"pending": {}, "updated": {}, "clock": 1_800_000_000, "status_reads": 0}
_original_monitor = mock_fortios._Handler._monitor


def _monitor(self: Any, method: str, path: str, params: dict[str, str], body: str = "") -> tuple[int, Any, str]:
    mock = self.owner
    api = "monitor/" + path
    feeds_table = mock.device.tables["system/external-resource"]
    if method == "GET" and path == "system/external-resource/entry-list":
        name = params.get("mkey", "")
        with mock.lock:
            if name not in feeds_table:
                return 404, mock.envelope(method, api, 404, error=-3), "application/json"
            # A new feed is "Connect pending" for its first two reads.
            if STATE["pending"].get(name, 0) > 0:
                STATE["pending"][name] -= 1
                return 200, mock.envelope(method, api, results={"status": "error", "error": "Connect pending", "error_code": 1}), "application/json"
            mock.device.pending_feeds.discard(name)
            entries = mock.device.feeds.get(name, [])
            start, count = int(params.get("start", 0)), int(params.get("count", len(entries) or 1))
            page = [{"entry": e, "valid": True, "comment": ""} for e in entries[start:start + count]] if start < len(entries) else []
            results = {"status": "success", "resource_file_status": "downloaded" if name in STATE["updated"] else "unavailable",
                       "accepted_count": len(entries), "valid_count": len(entries), "invalid_count": 0, "accepted_limit": LIMIT,
                       "overflow": False, "last_content_update_time": STATE["updated"].get(name, 0), "entries": page}
            STATE["status_reads"] += 1
        return 200, mock.envelope(method, api, results=results), "application/json"
    if method == "POST" and path == "system/external-resource/dynamic":
        status, reply, kind = _original_monitor(self, method, path, params, body)
        with mock.lock:
            for result in reply.get("results", []):
                if result.get("status") == "success":
                    STATE["clock"] += 1
                    STATE["updated"][result["name"]] = STATE["clock"]
        return status, reply, kind
    if method == "GET" and path == "system/performance/status":
        with mock.lock:
            lines = sum(len(v) for v in mock.device.feeds.values())
        return 200, mock.envelope(method, api, results={"mem": {"total": 2_000_000_000, "used": 1_000_000_000 + 150 * lines,
                                                                 "free": 900_000_000, "freeable": 0}}), "application/json"
    if method == "GET" and path == "system/resource/usage":
        return 200, mock.envelope(method, api, results={"mem": [{"current": 50, "historical": {"24-hour": {"max": 60}}}]}), "application/json"
    return _original_monitor(self, method, path, params, body)


mock_fortios._Handler._monitor = _monitor  # type: ignore[method-assign]
_original_table = mock_fortios._Handler._table


def _table(self: Any, method: str, spec: Any, key: Any, query: Any, body: str) -> tuple[int, Any, str]:
    result = _original_table(self, method, spec, key, query, body)
    if spec.path == "system/external-resource" and method == "POST" and result[0] < 300:
        name = json.loads(body).get("name")
        with self.owner.lock:
            STATE["pending"][name] = 2
            self.owner.device.pending_feeds.add(name)
    if spec.path == "system/external-resource" and method == "DELETE" and result[0] < 300:
        with self.owner.lock:
            self.owner.device.feeds.pop(key, None)
    return result


mock_fortios._Handler._table = _table  # type: ignore[method-assign]


# ---- the scenario -------------------------------------------------------------------------------

def private(path: Path, text: str) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text)


def hosts(names: range) -> str:
    return f"# Number of unique domains: {len(names)}\n" + "".join(f"0.0.0.0 h{i}.example\n" for i in names)


def adblock(names: range, stem: str) -> str:
    return f"! Number of entries: {len(names)}\n" + "".join(f"||{stem}{i}.example^\n" for i in names)


def main() -> int:
    require_tools()
    fh = binary("FH_BINARY")
    with Sandbox() as sb:
        env = dict(sb.env)
        config_dir = Path(env["FORTI_CONFIG_DIR"])
        profile = {"host": "localhost", "port": sb.mock.port, "mode": "autonomous", "ca_bundle": str(sb.mock.ca_path),
                   "api_key": {"source": "env", "env": KEY_VAR}}
        private(config_dir / "config.json", json.dumps({"version": 1, "default_profile": PROFILE, "profiles": {PROFILE: profile}}))
        lists = Path(tempfile.mkdtemp(prefix="fh-lists-", dir=sb.root))
        (lists / "hosts").write_text(hosts(range(0, 3000)))
        (lists / "ads.txt").write_text(adblock(range(0, 4000), "a"))
        (lists / "tif.mini.txt").write_text(adblock(range(0, 500), "t"))
        config = {"format": 1, "enabled": True, "profile": PROFILE, "prefix": "fh-", "tiers": ["base", "threat"],
                  "sources": [{"id": "hosts", "tier": "base", "format": "hosts", "url": f"file://{lists}/hosts"},
                              {"id": "ads", "tier": "base", "format": "adblock", "url": f"file://{lists}/ads.txt"},
                              {"id": "tif", "tier": "threat", "format": "adblock", "url": f"file://{lists}/tif.mini.txt"}],
                  "profiles": {"home": {"tiers": ["base", "threat"]}, "kids": {"tiers": ["base"]}},
                  # 20,000-line feeds: snapshot bodies well past the engine's 64 KiB inline limit.
                  "feeds": {"max_entries": 20000}, "sync": {"chunk_entries": 500, "load_timeout_seconds": 30}}
        hole = Path(env["FORTI_HOME"]) / "hole"
        private(hole / "config.json", json.dumps(config))

        def run(*args: str) -> tuple[int, Any, str]:
            import subprocess
            if os.environ.get("FH_DEBUG") and args[0] == "sync" and not outputs:
                debug = subprocess.run(["lldb", "--batch", "-o", "break set -n __cxa_throw", "-o", "run", "-o", "bt 25", "--", fh, *args],
                                       capture_output=True, text=True, env=env, timeout=180)
                print(debug.stdout[-6000:], file=sys.stderr)
            proc = subprocess.run([fh, *args, "--json"], capture_output=True, text=True, env=env, timeout=180)
            outputs.append(proc.stdout + proc.stderr)
            try:
                data = json.loads(proc.stdout)
            except json.JSONDecodeError:
                data = None
            return proc.returncode, data, proc.stderr

        outputs: list[str] = []

        # 1. The first sync creates, pushes, verifies.
        code, out, err = run("sync")
        check(code == 0, f"first sync exits 0 (got {code}: {err[-500:]} {json.dumps(out)[:1500] if out else ''})")
        compiled = out["data"]["compile"] if out else {}
        lines = compiled.get("totals", {}).get("emitted_lines", -1)
        with sb.mock.lock:
            device_feeds = {k: list(v) for k, v in sb.mock.device.feeds.items() if k.startswith("fh-")}
            profiles = dict(sb.mock.device.tables["dnsfilter/profile"])
        on_device = sum(len(v) for v in device_feeds.values())
        check(on_device == lines, f"the device holds every compiled line ({on_device} vs {lines})")
        check(all(len(v) == len(set(v)) for v in device_feeds.values()), "no duplicate entry in a device feed")
        everything = [e for v in device_feeds.values() for e in v]
        check(len(everything) == len(set(everything)), "no entry in two device feeds")
        check("fh-home" in profiles and "fh-kids" in profiles, "the managed DNS filter profiles exist")
        verified = [v for v in (out or {}).get("data", {}).get("outcome", {}).get("verification", [])]
        check(verified and all(v["verified"] for v in verified), "every pushed feed verified on the device")
        # h0.example and a0.example: an exact name and a subtree.
        check("h0.example" in everything and "a0.example" in everything and "*.a0.example" in everything, "strict encoding on the device")

        # 2. The engine stored the large bodies once and deleted them after the apply.
        blobs = Path(env["FORTI_STATE_DIR"]) / "blobs"
        leftovers = [p for p in blobs.glob("*.json")] if blobs.exists() else []
        check(not leftovers, f"no push body left in state/blobs ({len(leftovers)})")
        plans = list((Path(env["FORTI_STATE_DIR"]) / "plans").glob("*.json"))
        biggest = max((p.stat().st_size for p in plans), default=0)
        check(biggest < 200_000, f"plan files stay small (largest {biggest} bytes)")

        # 3. Unchanged: nothing written.
        writes = len(sb.mock.writes())
        code, out, err = run("sync")
        check(code == 0, f"second sync exits 0 (got {code}: {err[-300:]})")
        check(len(sb.mock.writes()) == writes, f"an unchanged sync writes nothing ({len(sb.mock.writes()) - writes} writes)")

        # 4. A list change goes out as a delta: add and remove commands, no snapshot.
        (lists / "hosts").write_text(hosts(range(25, 3025)))
        sb.mock.clear_requests()
        code, out, err = run("sync")
        check(code == 0, f"delta sync exits 0 (got {code}: {err[-300:]})")
        commands = []
        for r in sb.mock.writes():
            if r.api == "monitor/system/external-resource/dynamic":
                commands += [c["command"] for c in r.json()["commands"]]
        check(commands and "snapshot" not in commands, f"only add/remove commands ({sorted(set(commands))})")
        check(out and out["data"]["plan"]["adds"] == 25 and out["data"]["plan"]["removes"] == 25, "25 added, 25 removed")
        with sb.mock.lock:
            everything = [e for k, v in sb.mock.device.feeds.items() if k.startswith("fh-") for e in v]
        check("h3024.example" in everything and "h0.example" not in everything, "the device follows the list")

        # 5. A missing list: the last good copy, nothing written, the device unchanged.
        (lists / "ads.txt").unlink()
        writes = len(sb.mock.writes())
        code, out, err = run("sync")
        check(code == 0, f"sync with a missing list exits 0 (got {code}: {err[-300:]})")
        check(len(sb.mock.writes()) == writes, "a failed download writes nothing")
        check(out and "last good copy" in json.dumps(out["data"]["compile"]["warnings"]), "the run says it used the last good copy")

        # 6. status --verify --full reads every entry back.
        code, out, err = run("status", "--verify", "--full")
        check(code == 0, f"status --verify --full exits 0 (got {code}: {err[-300:]})")

        # 7. remove deletes forti-hole's objects only.
        with sb.mock.lock:
            sb.mock.device.tables["system/external-resource"]["operator-feed"] = {"name": "operator-feed", "type": "domain",
                                                                                  "update-method": "push", "category": 221, "comments": ""}
        code, out, err = run("remove", "--yes")
        check(code == 0, f"remove exits 0 (got {code}: {err[-300:]})")
        with sb.mock.lock:
            names = sorted(sb.mock.device.tables["system/external-resource"])
            profiles = sorted(sb.mock.device.tables["dnsfilter/profile"])
        check(names == ["operator-feed"], f"only the operator's feed is left ({names})")
        check(profiles == [], f"forti-hole's profiles are gone ({profiles})")

        # 8. The API key never appears anywhere.
        leaked = [o for o in outputs if API_KEY in o]
        check(not leaked, "the API key is in no output")
        check(API_KEY not in sb.all_files_text(), "the API key is in no state file")
        for path in hole.rglob("*"):
            if path.is_file():
                check(API_KEY not in path.read_text(errors="replace"), f"the API key is not in {path.name}")
    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed", file=sys.stderr)
        return 1
    print("e2e: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
