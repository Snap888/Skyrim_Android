#!/usr/bin/env python3
"""Summarize SnapFrame/DXVK logs into a compact local diagnostic report."""
import argparse, csv, json, math, re, statistics
from collections import Counter
from pathlib import Path

FILES=("snapframe_events.csv","snapframe_perf_on.csv","snapframe_flow.csv","snapframe_gpu.csv","snapframe.log")

def read_csv(path):
    if not path.is_file() or not path.stat().st_size: return []
    with path.open("r",encoding="utf-8-sig",errors="replace",newline="") as f:
        lines=[x for x in f if x.strip()]
    lines=[x for x in lines if not x.startswith("===")]
    if lines and lines[0].startswith("columns:"):
        lines[0]=lines[0][len("columns:"):].lstrip()
    return list(csv.DictReader(lines)) if lines else []

def num(x):
    try:
        n=float(x)
        return n if math.isfinite(n) else None
    except (TypeError,ValueError): return None

def stats(values):
    v=sorted(x for x in values if x is not None)
    if not v: return None
    def pct(p):
        pos=(len(v)-1)*p; lo=int(pos); hi=math.ceil(pos)
        return v[lo]+(v[hi]-v[lo])*(pos-lo)
    return {"n":len(v),"mean":statistics.fmean(v),"median":statistics.median(v),
            "p95":pct(.95),"p99":pct(.99),"max":v[-1]}

def analyze(root):
    events,perf,flow,gpu=[read_csv(root/x) for x in FILES[:4]]
    log=(root/FILES[4]).read_text(encoding="utf-8",errors="replace") if (root/FILES[4]).is_file() else ""
    counts=Counter(x.get("event","unknown") for x in events); results=Counter(); reasons=Counter()
    for row in events:
        if row.get("event")=="midpoint_present_failed":
            d=row.get("detail",""); m=re.search(r"result=(-?\d+)",d)
            results[m.group(1) if m else "unknown"]+=1
            reasons[d.split(" result=")[0] or "unspecified"]+=1
    keys=("avg_cpu_present_ms","max_cpu_present_ms","avg_source_acquire_ms","avg_generated_acquire_ms",
          "avg_source_handoff_wait_ms","avg_generated_handoff_wait_ms","avg_source_submission_wait_ms",
          "avg_generated_submission_wait_ms","avg_generate_record_ms","avg_generated_requested","avg_generated_queued")
    perfstats={k:s for k in keys if (s:=stats([num(x.get(k)) for x in perf]))}
    flowkeys=("forward_mean_confidence","backward_mean_confidence","consistency_mean","nonfinite_values")
    flowstats={k:s for k in flowkeys if (s:=stats([num(x.get(k)) for x in flow]))}
    modes=Counter()
    for m in re.finditer(r"(?:requested present mode=|present_mode=)(AUTO|FIFO|MAILBOX|IMMEDIATE|[0-3])",log,re.I):
        v=m.group(1).upper(); modes[{"0":"AUTO","1":"FIFO","2":"MAILBOX","3":"IMMEDIATE"}.get(v,v)]+=1
    failed=counts.get("midpoint_present_failed",0); nr=results.get("1",0); rejected=counts.get("gpu_metrics_rejected",0)
    findings=[]
    if nr: findings.append({"severity":"high","code":"GENERATED_WSI_BACKPRESSURE","message":f"{nr} generated-frame failures report VK_NOT_READY.","recommendation":"Keep game presentation non-blocking. Inspect per-mode acquire availability and presenter pre-acquire/ticket ownership before increasing timeout."})
    if failed>nr: findings.append({"severity":"high","code":"GENERATED_WSI_ERRORS","message":f"{failed-nr} generated failures have results other than VK_NOT_READY.","recommendation":"Inspect exact VkResult and surface/swapchain lifecycle."})
    if rejected: findings.append({"severity":"high","code":"AI_TELEMETRY_REJECTED","message":f"{rejected} GPU metric records were rejected; adaptive AI tuning must remain disabled.","recommendation":"Validate shader valid/expected cell counts against CPU grid dimensions and derive coverage from the same counters."})
    if not gpu: findings.append({"severity":"medium","code":"GPU_STAGE_CSV_EMPTY","message":"snapframe_gpu.csv has no stage samples.","recommendation":"Enable stage profiling briefly; keep diagnostic V2 workloads disabled during normal gameplay."})
    if not flow: findings.append({"severity":"medium","code":"FLOW_CSV_EMPTY","message":"snapframe_flow.csv has no flow samples.","recommendation":"Check flow diagnostics export gating; accepted dispatch events alone do not prove flow quality."})
    sizes={n:((root/n).stat().st_size if (root/n).is_file() else 0) for n in FILES}
    return {"schema":"snapframe-diagnostic-report-v1","source_directory":root.name,"files_bytes":sizes,
      "event_rows":len(events),"event_counts":dict(counts),
      "generated_present":{"accepted":counts.get("midpoint_present_accepted",0),"failed":failed,
        "failure_vk_results":dict(results),"failure_reasons":dict(reasons)},
      "present_modes_seen_in_log":dict(modes),"performance":perfstats,
      "flow":{"rows":len(flow),"metrics":flowstats},"gpu_stage_rows":len(gpu),"findings":findings}

def to_markdown(r):
    out=["# SnapFrame diagnostic report","",f"- Schema: {r['schema']}",f"- Source folder: {r['source_directory']}",
      f"- Event rows: {r['event_rows']}",f"- Flow rows: {r['flow']['rows']}",f"- GPU stage rows: {r['gpu_stage_rows']}",
      "","## Present / WSI","",f"- Generated accepted: {r['generated_present']['accepted']}",
      f"- Generated failed: {r['generated_present']['failed']}",
      f"- VkResult counts: {json.dumps(r['generated_present']['failure_vk_results'],sort_keys=True)}",
      f"- Modes observed: {json.dumps(r['present_modes_seen_in_log'],sort_keys=True)}","","## Event counts",""]
    out += [f"- {k}: {v}" for k,v in sorted(r["event_counts"].items())]
    out += ["","## Performance (ms unless noted)",""]
    for k,v in r["performance"].items():
        unit="ratio" if "requested" in k or "queued" in k else "ms"
        out.append(f"- {k} ({unit}): mean {v['mean']:.3f}, median {v['median']:.3f}, p95 {v['p95']:.3f}, p99 {v['p99']:.3f}, max {v['max']:.3f} (n={v['n']})")
    out += ["","## Findings",""]
    for x in r["findings"]: out += [f"### [{x['severity'].upper()}] {x['code']}",x["message"],"",f"Recommendation: {x['recommendation']}",""]
    if not r["findings"]: out.append("No automated findings.")
    out += ["## Files","","| File | Bytes |","|---|---:|"]
    out += [f"| {k} | {v} |" for k,v in r["files_bytes"].items()]
    out += ["","Full logs remain local. This report contains aggregates and selected reasons; it does not upload files."]
    return "\n".join(out)+"\n"

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument("--log-dir",type=Path,required=True)
    p.add_argument("--format",choices=("markdown","json"),default="markdown"); p.add_argument("--output",type=Path)
    a=p.parse_args(); r=analyze(a.log_dir)
    text=to_markdown(r) if a.format=="markdown" else json.dumps(r,indent=2,sort_keys=True)+"\n"
    if a.output: a.output.parent.mkdir(parents=True,exist_ok=True); a.output.write_text(text,encoding="utf-8")
    else: print(text,end="")
if __name__=="__main__": main()
