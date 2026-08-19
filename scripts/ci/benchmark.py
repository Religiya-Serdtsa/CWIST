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
    ws_latest = history[-1] if history else {}
    metrics = [
        ("Throughput (req/s)", [("CWIST", "cwist_rps", "#22c55e"), ("Axum", "axum_rps", "#3b82f6"), ("Gin", "gin_rps", "#06b6d4"), ("Spring", "spring_rps", "#ef4444")]),
        ("Avg Latency (ms)", [("CWIST", "cwist_lat_ms", "#22c55e"), ("Axum", "axum_lat_ms", "#3b82f6"), ("Gin", "gin_lat_ms", "#06b6d4"), ("Spring", "spring_lat_ms", "#ef4444")]),
        ("Peak RSS (KiB)", [("CWIST", "cwist_rss_kib", "#22c55e"), ("Axum", "axum_rss_kib", "#3b82f6"), ("Gin", "gin_rss_kib", "#06b6d4"), ("Spring", "spring_rss_kib", "#ef4444")]),
        ("Context Switches", [("CWIST", "cwist_csw", "#22c55e"), ("Axum", "axum_csw", "#3b82f6"), ("Gin", "gin_csw", "#06b6d4"), ("Spring", "spring_csw", "#ef4444")])
    ]
    
    width = 960
    height = 540
    blocks = []
    
    # Title & Legend
    blocks.append('<text x="30" y="35" class="title">Web Server Performance Comparison (wrk 12t 400c)</text>')
    blocks.append('<rect x="580" y="20" width="12" height="12" fill="#22c55e" rx="2"/><text x="598" y="31" class="legend">CWIST</text>')
    blocks.append('<rect x="665" y="20" width="12" height="12" fill="#3b82f6" rx="2"/><text x="683" y="31" class="legend">Axum</text>')
    blocks.append('<rect x="740" y="20" width="12" height="12" fill="#06b6d4" rx="2"/><text x="758" y="31" class="legend">Gin</text>')
    blocks.append('<rect x="805" y="20" width="12" height="12" fill="#ef4444" rx="2"/><text x="823" y="31" class="legend">Spring Boot</text>')
    
    # Render 4 grid subpanels (2x2 layout)
    panel_w = 420
    panel_h = 200
    offsets = [(30, 60), (490, 60), (30, 290), (490, 290)]
    
    for idx, (m_title, series_list) in enumerate(metrics):
        px, py = offsets[idx]
        blocks.append(f'<rect x="{px}" y="{py}" width="{panel_w}" height="{panel_h}" fill="#1f2937" rx="6" stroke="#374151"/>')
        blocks.append(f'<text x="{px+15}" y="{py+28}" class="panel-title">{m_title}</text>')
        
        vals = [float(ws_latest.get(key, 0)) for _, key, _ in series_list]
        max_val = max(vals, default=1.0)
        if max_val <= 0: max_val = 1.0
        
        bar_y_base = py + 55
        for s_idx, (label, key, color) in enumerate(series_list):
            val = float(ws_latest.get(key, 0))
            ratio = min(1.0, max(0.0, val / max_val))
            bar_len = int(ratio * 240)
            by = bar_y_base + s_idx * 34
            
            # Format value label
            if "ms" in m_title:
                val_str = f"{val:.2f} ms"
            elif "KiB" in m_title:
                val_str = f"{val:,.0f} KiB"
            elif "req/s" in m_title:
                val_str = f"{val:,.0f} req/s"
            else:
                val_str = f"{val:,.0f}"
                
            blocks.append(f'<text x="{px+15}" y="{by+16}" class="bar-label">{label}</text>')
            blocks.append(f'<rect x="{px+80}" y="{by}" width="240" height="22" fill="#374151" rx="3"/>')
            if bar_len > 0:
                blocks.append(f'<rect x="{px+80}" y="{by}" width="{bar_len}" height="22" fill="{color}" rx="3"/>')
            blocks.append(f'<text x="{px+330}" y="{by+16}" class="bar-val">{val_str}</text>')

    # Footer: recorded Spring/JVM & Go runtime environment & benchmark profile
    env = ws_latest.get("spring_env", {}) or {}
    go_env = ws_latest.get("go_env", {}) or {}
    if env or go_env:
        stack = env.get('stack')
        stack_part = f" ({stack})" if stack else ""
        vt = env.get('virtual_threads')
        vt_part = f" | virtual threads: {'on' if vt else 'off'}" if vt else ""
        footer1 = (f"Spring Boot {env.get('spring_boot_version','n/a')}{stack_part} | {env.get('java_version','n/a')} | "
                   f"JVM: {env.get('jvm_opts','n/a')}{vt_part}")
        footer2 = f"Profile: {ws_latest.get('wrk_profile', 'wrk 12t 400c')}"
        runner = ws_latest.get('runner_hw')
        if runner:
            footer2 += f" | Runner: {runner}"
        if go_env:
            footer2 += f" | {go_env.get('go_version', 'Go')} + {go_env.get('framework', 'Gin')}"
        blocks.append(f'<text x="30" y="512" class="footer">{footer1}</text>')
        blocks.append(f'<text x="30" y="530" class="footer">{footer2}</text>')

    svg_style = (
        '<style>'
        '.title{font:18px sans-serif;font-weight:bold;fill:#f9fafb}'
        '.panel-title{font:14px sans-serif;font-weight:600;fill:#9ca3af}'
        '.legend{font:13px sans-serif;fill:#d1d5db}'
        '.bar-label{font:13px sans-serif;font-weight:500;fill:#e5e7eb}'
        '.bar-val{font:12px sans-serif;font-weight:bold;fill:#f3f4f6}'
        '.footer{font:11px sans-serif;fill:#6b7280}'
        '</style>'
    )
    return f'<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{width}" height="{height}" viewBox="0 0 {width} {height}">{svg_style}<rect width="100%" height="100%" fill="#111827"/>' + ''.join(blocks) + '</svg>\n'

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

    def get_lat_part(prefix):
        p90 = ws_latest.get(f"{prefix}_p90_ms")
        p99 = ws_latest.get(f"{prefix}_p99_ms")
        if p90 is not None and p99 is not None:
            return f" (P90 {p90:.2f}ms, P99 {p99:.2f}ms)"
        return ""

    cwist_lat_part = get_lat_part("cwist")
    axum_lat_part = get_lat_part("axum")
    gin_lat_part = get_lat_part("gin")
    spring_lat_part = get_lat_part("spring")

    ws_summary = (
        f"Latest Web Server Benchmark ({ws_latest.get('wrk_profile','wrk 12t 400c')}):\n"
        f"- **CWIST**: {ws_latest.get('cwist_rps',0):.0f} req/s | Latency {ws_latest.get('cwist_lat_ms',0):.2f}ms{cwist_lat_part} | RSS {ws_latest.get('cwist_rss_kib',0):.0f}KiB | Csw {ws_latest.get('cwist_csw',0):.0f}\n"
        f"- **Axum**: {ws_latest.get('axum_rps',0):.0f} req/s | Latency {ws_latest.get('axum_lat_ms',0):.2f}ms{axum_lat_part} | RSS {ws_latest.get('axum_rss_kib',0):.0f}KiB | Csw {ws_latest.get('axum_csw',0):.0f}\n"
        f"- **Gin (Go)**: {ws_latest.get('gin_rps',0):.0f} req/s | Latency {ws_latest.get('gin_lat_ms',0):.2f}ms{gin_lat_part} | RSS {ws_latest.get('gin_rss_kib',0):.0f}KiB | Csw {ws_latest.get('gin_csw',0):.0f}\n"
        f"- **Spring Boot**: {ws_latest.get('spring_rps',0):.0f} req/s | Latency {ws_latest.get('spring_lat_ms',0):.2f}ms{spring_lat_part} | RSS {ws_latest.get('spring_rss_kib',0):.0f}KiB | Csw {ws_latest.get('spring_csw',0):.0f}\n"
    )
    ws_env = ws_latest.get("spring_env", {}) or {}
    if ws_env:
        ws_stack = ws_env.get('stack')
        ws_stack_part = f" ({ws_stack})" if ws_stack else ""
        ws_vt = ws_env.get('virtual_threads')
        ws_vt_part = f", virtual threads **{'on' if ws_vt else 'off'}**" if ws_vt else ""
        ws_summary += (
            f"\nSpring runtime env: **{ws_env.get('java_version','n/a')}**, Spring Boot **{ws_env.get('spring_boot_version','n/a')}**{ws_stack_part}"
            f"{ws_vt_part}, "
            f"JVM opts `{ws_env.get('jvm_opts','n/a')}`, warmup/profile: {ws_latest.get('wrk_profile','n/a')}\n"
        )
    ws_summary += f"\n![Web Server Benchmark Trends](docs/webserver-benchmark-trends.svg)"
    if README.exists(): replace(README, "<!-- WEBSERVER_BENCHMARKS:START -->", "<!-- WEBSERVER_BENCHMARKS:END -->", ws_summary)
    if README_MD.exists(): replace(README_MD, "<!-- WEBSERVER_BENCHMARKS:START -->", "<!-- WEBSERVER_BENCHMARKS:END -->", ws_summary)

    tuned_rps = ws_latest.get("cwist_tuned_rps")
    if tuned_rps and README_MD.exists() and "<!-- TUNED_BENCHMARK:START -->" in README_MD.read_text():
        tuned_profile = ws_latest.get("tuned_profile") or "CWIST_WORKERS sized to a pinned server core set, wrk pinned to the remaining cores"
        tuned_line = (
            f"**Tuned low-latency run ({tuned_profile}): "
            f"{tuned_rps:,.0f} req/s at {ws_latest.get('cwist_tuned_lat_ms',0):.2f}ms average latency "
            f"(P50 {ws_latest.get('cwist_tuned_p50_ms',0):.2f}ms, P90 {ws_latest.get('cwist_tuned_p90_ms',0):.2f}ms, "
            f"P99 {ws_latest.get('cwist_tuned_p99_ms',0):.2f}ms).** "
            f"Leaving headroom between server workers and load-generator threads keeps the latency tail flat — "
            f"oversubscribing the same cores shows a multi-ms average from scheduling jitter alone at similar throughput."
        )
        replace(README_MD, "<!-- TUNED_BENCHMARK:START -->", "<!-- TUNED_BENCHMARK:END -->", tuned_line)

if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "measure": print(json.dumps(run_measurement()))
    elif len(sys.argv) == 2 and sys.argv[1] == "render": render()
    else: raise SystemExit("usage: benchmark.py measure|render")
