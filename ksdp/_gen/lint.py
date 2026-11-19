#!/usr/bin/env python3
"""KSDP public lint: every functions/*.yml entry has the public fields only, the required ones, a known status, a unique address per module, and none of the words the public copy must not carry.
usage: lint.py <ksdp dir>   exit 1 on any finding"""
import re, sys, pathlib, yaml, hashlib
KEEP = {"address", "module", "name", "summary", "status", "game_version", "refs", "rule"}
REQ = ("address", "module", "name", "summary", "status", "game_version")
STATUS = {"read", "verified-emulator", "verified-data", "partial", "unknown", "unused", "auto"}
BAD = re.compile(r"ghidra|decompil|docs/research|tools_private|\.exe\.c\b", re.I)
# names of third-party projects are kept as SHA-256 of the lowercased word, so the public file does not carry them; any substring of a field that hashes to one is a finding
NAME_HASH = {"2da4aa962b08caa787ddbad4bd2f4db809f10134323b850c788923d600a0c3bc", "563da863d584d041ab62e7305539471dade45d07369a68096f4f8c0c88498fbf", "9fb6200d7f129b6f1122d8ff4f6666b37de2e1fd4253ea22878cda6c7f63c5e2", "1c99231ee34df8c39a00153a6acff9caa3c3668b3e4ca15e13baf15b8e6a767e", "167fef642636718ae0fee594b50198eba7622ee2671b7eafbc39a3cfa4377e0e"}
NAME_LEN = (5, 6, 8, 9)
def third_party(t):
    t = t.lower()
    for n in NAME_LEN:
        for i in range(len(t) - n + 1):
            if hashlib.sha256(t[i:i + n].encode()).hexdigest() in NAME_HASH: return t[i:i + n]
    return None
root = pathlib.Path(sys.argv[1]); bad = 0
for f in sorted((root / "functions").glob("*.yml")):
    seen = set()
    for i, e in enumerate(yaml.safe_load(f.read_text()) or []):
        w = "%s[%d]" % (f.name, i)
        for k in set(e) - KEEP: print(w, "private field", k); bad += 1
        for k in REQ:
            if k not in e: print(w, "missing", k); bad += 1
        if e.get("status") == "auto" and not e.get("rule"): print(w, "auto without a rule"); bad += 1
        if e.get("status") not in STATUS: print(w, "status", e.get("status")); bad += 1
        key = (e.get("module"), str(e.get("address")))
        if key in seen: print(w, "duplicate", key); bad += 1
        seen.add(key)
        for k, v in e.items():
            if isinstance(v, str) and BAD.search(v): print(w, k, "forbidden word:", BAD.search(v).group(0)); bad += 1
            if isinstance(v, str) and third_party(v): print(w, k, "third-party project name"); bad += 1
print("ksdp lint: %d finding(s)" % bad); sys.exit(1 if bad else 0)
