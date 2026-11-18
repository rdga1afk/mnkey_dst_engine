#!/usr/bin/env python3
"""KSDP public site generator: reads <ksdp dir>/functions/*.yml, writes <out>/index.html (one searchable table, no scripts from outside) and <out>/functions.json.
usage: build.py <ksdp dir> <out dir>"""
import html, json, sys, pathlib, yaml
src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
rows = [e for f in sorted((src / "functions").glob("*.yml")) for e in (yaml.safe_load(f.read_text()) or [])]
(out / "functions.json").write_text(json.dumps(rows, indent=1, ensure_ascii=False))
tr = "".join("<tr><td><code>%s</code></td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" % tuple(html.escape(str(e.get(k, ""))) for k in ("address", "module", "name", "status", "summary")) for e in rows)
(out / "index.html").write_text("""<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>KSDP function index</title>
<style>:root{--bg:#fff;--fg:#1b1b1b;--line:#ccc}@media(prefers-color-scheme:dark){:root{--bg:#161616;--fg:#e6e6e6;--line:#444}}body{background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif;margin:0 auto;max-width:1200px;padding:0 16px}
td,th{border-bottom:1px solid var(--line);padding:4px 8px;vertical-align:top;text-align:left}table{border-collapse:collapse;width:100%%}input{width:100%%;padding:6px;margin:8px 0}.t{overflow-x:auto}</style></head><body>
<h1>KSDP: Kenshi structures, function index</h1>
<p>%d entries from our own reading of the game program, version 1.0.68 Steam. Status: <em>verified-emulator</em> = matched against the original function run in an emulator; <em>read</em> = described from reading only.
<a href="functions.json">functions.json</a></p>
<p><small>This is an independent research project, not affiliated with Lo-Fi Games. It contains descriptions in our own words and no game files.</small></p>
<input id="q" placeholder="filter by name, address, module, text"><div class="t"><table id="t"><tr><th>address</th><th>module</th><th>name</th><th>status</th><th>description</th></tr>%s</table></div>
<script>q.oninput=()=>{const s=q.value.toLowerCase();for(const r of t.rows)if(r.rowIndex)r.hidden=!r.textContent.toLowerCase().includes(s)}</script></body></html>""" % (len(rows), tr))
print("ksdp build: %d entries -> %s" % (len(rows), out))
