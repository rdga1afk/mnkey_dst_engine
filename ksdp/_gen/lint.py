#!/usr/bin/env python3
"""KSDP public lint: every functions/*.yml entry has the public fields only, the required ones, a known status, a unique address per module, and none of the words the public copy must not carry.
usage: lint.py <ksdp dir>   exit 1 on any finding"""
import re, sys, pathlib, yaml
KEEP = {"address", "module", "name", "summary", "status", "game_version", "refs"}
REQ = ("address", "module", "name", "summary", "status", "game_version")
STATUS = {"read", "verified-emulator", "verified-data", "partial", "unknown", "unused"}
BAD = re.compile(r"ghidra|decompil|okran|meitou|docs/research|tools_private|\.exe\.c\b", re.I)
root = pathlib.Path(sys.argv[1]); bad = 0
for f in sorted((root / "functions").glob("*.yml")):
    seen = set()
    for i, e in enumerate(yaml.safe_load(f.read_text()) or []):
        w = "%s[%d]" % (f.name, i)
        for k in set(e) - KEEP: print(w, "private field", k); bad += 1
        for k in REQ:
            if k not in e: print(w, "missing", k); bad += 1
        if e.get("status") not in STATUS: print(w, "status", e.get("status")); bad += 1
        key = (e.get("module"), str(e.get("address")))
        if key in seen: print(w, "duplicate", key); bad += 1
        seen.add(key)
        for k, v in e.items():
            if isinstance(v, str) and BAD.search(v): print(w, k, "forbidden word:", BAD.search(v).group(0)); bad += 1
print("ksdp lint: %d finding(s)" % bad); sys.exit(1 if bad else 0)
