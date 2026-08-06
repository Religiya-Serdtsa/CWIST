#!/usr/bin/env python3
"""Run reproducible CWIST microbenchmarks and render tracked SVG trends."""
from __future__ import annotations
import json, os, platform, re, resource, subprocess, sys, time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HISTORY = ROOT / "benchmarks" / "db.json"
SVG = ROOT / "docs" / "benchmark-trends.svg"
README = ROOT / "README"
ROADMAP = ROOT / "ROADMAP.md"

def child_usage():
    u = resource.getrusage(resource.RUSAGE_CHILDREN)
    return (u.ru_utime, u.ru_stime, u.ru_maxrss, u.ru_nvcsw, u.ru_nivcsw)

def run_measurement() -> dict:
    subprocess.run(["make", "bench_security_pool"], cwd=ROOT, check=True, stdout=subprocess.PIPE, text=True)
    samples, output = [], ""
    for _ in range(3):
        before = child_usage(); started = time.monotonic()
        completed = subprocess.run(["./bench_security_pool"], cwd=ROOT, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        elapsed = time.monotonic() - started; after = child_usage(); output = completed.stdout
        max_rss = max(0, after[2] - before[2]) or after[2]
        if sys.platform == "darwin": max_rss //= 1024
        samples.append({"elapsed": elapsed, "cpu": (after[0]-before[0])+(after[1]-before[1]), "rss_kib": max_rss,
                        "context_switches": (after[3]-before[3])+(after[4]-before[4])})
    waf = re.search(r"waf: ([0-9.]+) M checks/s", output); pool = re.search(r"pool: ([0-9.]+) M acquire-release/s", output)
    median = sorted(samples, key=lambda x: x["elapsed"])[1]
    return {"timestamp": datetime.now(timezone.utc).isoformat(), "os": platform.system(), "arch": platform.machine(),
            "cpu_percent": round(100 * median["cpu"] / max(median["elapsed"], .001), 2), "rss_kib": median["rss_kib"],
            "context_switches": median["context_switches"], "memory_recovery_kib": samples[-1]["rss_kib"] - samples[0]["rss_kib"],
            "waf_mchecks_s": float(waf.group(1)) if waf else 0, "throughput_mleases_s": float(pool.group(1)) if pool else 0}

def svg(history: list[dict]) -> str:
    rows = history[-20:]; metrics = [("CPU utilization (%)", "cpu_percent", "#ef4444"), ("Throughput (M leases/s)", "throughput_mleases_s", "#22c55e"), ("RSS (KiB)", "rss_kib", "#3b82f6"), ("Memory recovery drift (KiB)", "memory_recovery_kib", "#a855f7"), ("Context switches", "context_switches", "#f59e0b")]
    blocks = []
    for n, (label, key, color) in enumerate(metrics):
        vals = [float(x.get(key, 0)) for x in rows]; low, high = min(vals, default=0), max(vals, default=1)
        if high == low: high = low + 1
        pts = " ".join(f"{40+i*25},{55+n*95-(v-low)/(high-low)*45:.1f}" for i,v in enumerate(vals))
        blocks.append(f'<text x="40" y="{20+n*95}" class="label">{label}</text><text x="640" y="{20+n*95}" text-anchor="end" class="value">latest: {vals[-1] if vals else 0:.2f}</text><line x1="40" y1="{55+n*95}" x2="640" y2="{55+n*95}" class="axis"/><polyline points="{pts}" stroke="{color}" class="series"/>')
    return '<svg xmlns="http://www.w3.org/2000/svg" width="680" height="500" viewBox="0 0 680 500"><style>.label{font:14px sans-serif;fill:#e5e7eb}.value{font:12px sans-serif;fill:#9ca3af}.axis{stroke:#374151}.series{fill:none;stroke-width:2}</style><rect width="100%" height="100%" fill="#111827"/>'+''.join(blocks)+'</svg>\n'

def replace(path: Path, begin: str, end: str, content: str) -> None:
    text = path.read_text(); start = text.index(begin) + len(begin); finish = text.index(end, start)
    path.write_text(text[:start] + "\n" + content.rstrip() + "\n" + text[finish:])

WEBSERVER_HISTORY = ROOT / "benchmarks" / "webserver.json"
WEBSERVER_SVG = ROOT / "docs" / "webserver-benchmark-trends.svg"
README_MD = ROOT / "README.md"

def render_webserver_svg(history: list[dict]) -> str:
    rows = history[-20:]
    metrics = [
        ("Throughput (Requests/sec - Higher is better)", [("CWIST", "cwist_rps", "#22c55e"), ("Axum", "axum_rps", "#3b82f6"), ("Spring", "spring_rps", "#ef4444")]),
        ("Avg Latency (ms - Lower is better)", [("CWIST", "cwist_lat_ms", "#22c55e"), ("Axum", "axum_lat_ms", "#3b82f6"), ("Spring", "spring_lat_ms", "#ef4444")]),
        ("Peak RSS (KiB - Lower is better)", [("CWIST", "cwist_rss_kib", "#22c55e"), ("Axum", "axum_rss_kib", "#3b82f6"), ("Spring", "spring_rss_kib", "#ef4444")]),
        ("Context Switches (Lower is better)", [("CWIST", "cwist_csw", "#22c55e"), ("Axum", "axum_csw", "#3b82f6"), ("Spring", "spring_csw", "#ef4444")])
    ]
    blocks = []
    blocks.append('<text x="40" y="30" class="title">Web Server Benchmark Comparison (CWIST vs Axum vs Spring Boot)</text>')
    
    for m_idx, (m_title, series_list) in enumerate(metrics):
        base_y = 60 + m_idx * 130
        blocks.append(f'<text x="40" y="{base_y}" class="subtitle">{m_title}</text>')
        blocks.append(f'<line x1="40" y1="{base_y+65}" x2="640" y2="{base_y+65}" class="axis"/>')
        
        # collect all values to normalize y axis across series for this metric
        all_vals = []
        for _, key, _ in series_list:
            all_vals.extend(float(x.get(key, 0)) for x in rows)
        low, high = min(all_vals, default=0), max(all_vals, default=1)
        if high == low: high = low + 1
        
        for s_idx, (label, key, color) in enumerate(series_list):
            vals = [float(x.get(key, 0)) for x in rows]
            pts = " ".join(f"{40+i*30},{base_y+65-(v-low)/(high-low)*45:.1f}" for i, v in enumerate(vals))
            latest_val = vals[-1] if vals else 0
            fmt_val = f"{latest_val:.1f}" if "ms" in m_title else (f"{latest_val:.0f}" if "Requests" in m_title or "Switches" in m_title else f"{latest_val:.0f}")
            blocks.append(f'<polyline points="{pts}" stroke="{color}" class="series"/>')
            blocks.append(f'<text x="{500 + s_idx*50}" y="{base_y}" class="legend" fill="{color}">{label}: {fmt_val}</text>')
            
    return '<svg xmlns="http://www.w3.org/2000/svg" width="680" height="580" viewBox="0 0 680 580"><style>.title{font:16px sans-serif;font-weight:bold;fill:#f3f4f6}.subtitle{font:13px sans-serif;font-weight:600;fill:#e5e7eb}.legend{font:11px sans-serif;font-weight:500}.axis{stroke:#374151}.series{fill:none;stroke-width:2}</style><rect width="100%" height="100%" fill="#111827"/>'+''.join(blocks)+'</svg>\n'

def render() -> None:
    history = json.loads(HISTORY.read_text()) if HISTORY.exists() else []
    SVG.parent.mkdir(parents=True, exist_ok=True); SVG.write_text(svg(history))
    latest = history[-1] if history else {}; summary = f"Latest automated benchmark: **{latest.get('os','n/a')}** — {latest.get('throughput_mleases_s',0)} M leases/s, {latest.get('cpu_percent',0)}% CPU, {latest.get('rss_kib',0)} KiB RSS.\n\n![CWIST benchmark trends](docs/benchmark-trends.svg)"
    replace(README, "<!-- BENCHMARKS:START -->", "<!-- BENCHMARKS:END -->", summary)
    replace(ROADMAP, "<!-- CI-BENCHMARKS:START -->", "<!-- CI-BENCHMARKS:END -->", f"Automated OS benchmark history is published in `docs/benchmark-trends.svg`. Latest platform: **{latest.get('os','n/a')}**.")

    ws_history = json.loads(WEBSERVER_HISTORY.read_text()) if WEBSERVER_HISTORY.exists() else []
    WEBSERVER_SVG.parent.mkdir(parents=True, exist_ok=True)
    WEBSERVER_SVG.write_text(render_webserver_svg(ws_history))
    ws_latest = ws_history[-1] if ws_history else {}
    ws_summary = (
        f"Latest Web Server Benchmark (wrk 12t 400c):\n"
        f"- **CWIST**: {ws_latest.get('cwist_rps',0):.0f} req/s | Latency {ws_latest.get('cwist_lat_ms',0):.2f}ms | RSS {ws_latest.get('cwist_rss_kib',0):.0f}KiB | Csw {ws_latest.get('cwist_csw',0):.0f}\n"
        f"- **Axum**: {ws_latest.get('axum_rps',0):.0f} req/s | Latency {ws_latest.get('axum_lat_ms',0):.2f}ms | RSS {ws_latest.get('axum_rss_kib',0):.0f}KiB | Csw {ws_latest.get('axum_csw',0):.0f}\n"
        f"- **Spring Boot**: {ws_latest.get('spring_rps',0):.0f} req/s | Latency {ws_latest.get('spring_lat_ms',0):.2f}ms | RSS {ws_latest.get('spring_rss_kib',0):.0f}KiB | Csw {ws_latest.get('spring_csw',0):.0f}\n\n"
        f"![Web Server Benchmark Trends](docs/webserver-benchmark-trends.svg)"
    )
    if README.exists(): replace(README, "<!-- WEBSERVER_BENCHMARKS:START -->", "<!-- WEBSERVER_BENCHMARKS:END -->", ws_summary)
    if README_MD.exists(): replace(README_MD, "<!-- WEBSERVER_BENCHMARKS:START -->", "<!-- WEBSERVER_BENCHMARKS:END -->", ws_summary)

if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "measure": print(json.dumps(run_measurement()))
    elif len(sys.argv) == 2 and sys.argv[1] == "render": render()
    else: raise SystemExit("usage: benchmark.py measure|render")
