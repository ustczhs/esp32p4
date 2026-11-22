#!/usr/bin/env python3
"""Prefetch ESP component registry metadata (and missing archives) into a local
storage dir, so idf.py can configure with IDF_COMPONENT_STORAGE_URL=<dir>."""
import concurrent.futures as cf
import json
import os
import subprocess
import sys

import yaml

MIRROR = "https://components-file.espressif.com.cn"
ROOT = os.path.dirname(os.path.abspath(__file__))
STORAGE = os.path.join(ROOT, "claw_storage")
PROJECT = os.path.join(ROOT, "..", "xiaozhi-esp32")


def fetch(url, dest, tries=8):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    for i in range(tries):
        r = subprocess.run(
            ["curl", "-sL", "--fail", "-m", "180", "-o", dest, url],
            capture_output=True)
        if r.returncode == 0 and os.path.getsize(dest) > 0:
            return True
        print(f"retry {i+1} {url}", flush=True)
    return False


def get_json(ns, name):
    rel = f"components/{ns}/{name}.json"
    dest = os.path.join(STORAGE, rel)
    if os.path.exists(dest):
        try:
            return json.load(open(dest))
        except Exception:
            pass
    if not fetch(f"{MIRROR}/{rel}", dest):
        print(f"FAIL metadata {ns}/{name}", flush=True)
        return None
    try:
        return json.load(open(dest))
    except Exception as e:
        print(f"FAIL parse {ns}/{name}: {e}", flush=True)
        return None


def main():
    lock = yaml.safe_load(open(os.path.join(PROJECT, "dependencies.lock")))
    comps = [k.split("/", 1) for k in lock["dependencies"] if "/" in k]
    comps.append(("espressif", "esp_websocket_client"))

    results = {}
    with cf.ThreadPoolExecutor(max_workers=6) as ex:
        futs = {ex.submit(get_json, ns, name): (ns, name) for ns, name in comps}
        for fut in cf.as_completed(futs):
            ns, name = futs[fut]
            results[(ns, name)] = fut.result()
            print(f"done {ns}/{name} {'OK' if results[(ns,name)] else 'MISSING'}",
                  flush=True)

    failed = [k for k, v in results.items() if v is None]
    if failed:
        print("FAILED:", failed)
        sys.exit(1)

    # Archive for the one component missing from managed_components.
    meta = results[("espressif", "esp_websocket_client")]
    cands = [v for v in meta["versions"]
             if v["version"].startswith("1.") and not v.get("yanked", False)]
    def vkey(v):
        return tuple(int(x) for x in v["version"].split(".")[:3])
    best = max(cands, key=vkey)
    print("websocket version:", best["version"], flush=True)
    for field in ("url", "checksums"):
        rel = best.get(field)
        if not rel:
            continue
        rel = rel.lstrip("/")
        dest = os.path.join(STORAGE, rel)
        if os.path.exists(dest) and os.path.getsize(dest) > 0:
            continue
        if not fetch(f"{MIRROR}/{rel}", dest):
            print(f"FAIL archive {rel}")
            sys.exit(1)
        print("archive OK", rel, flush=True)
    print("ALL DONE", flush=True)


if __name__ == "__main__":
    main()