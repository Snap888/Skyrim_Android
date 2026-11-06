#!/usr/bin/env python3
"""Summarize SnapFrame/DXVK logs into a compact local diagnostic report."""
import argparse, csv, json, math, re, statistics
from collections import Counter
from pathlib import Path

FILES=("snapframe_events.csv","snapframe_perf_on.csv","snapframe_flow.csv","snapframe_gpu.csv","snapframe.log","log.txt","snapframe.conf")

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

def key_values(line):
    return {k: num(v) for k,v in re.findall(r"([a-z_]+)=(-?\d+(?:\.\d+)?)", line)}

def timing_group(rows, fields):
    return {name: value for field, name in fields.items()
            if (value := stats([(row.get(field) / 1000.0)
                                if row.get(field) is not None else None
                                for row in rows]))}

def analyze(root):
    events,perf,flow,gpu=[read_csv(root/x) for x in FILES[:4]]
    log="\n".join((root/name).read_text(encoding="utf-8",errors="replace") for name in FILES[4:6] if (root/name).is_file())
    config=(root/FILES[6]).read_text(encoding="utf-8",errors="replace") if (root/FILES[6]).is_file() else ""
    config_values={k:int(v) for k,v in re.findall(r"^\s*(motion_analysis|profiling_enabled)\s*=\s*([01])\s*(?:#.*)?$",config,re.M)}
    motion_analysis=config_values.get("motion_analysis",0)
    profiling_enabled=config_values.get("profiling_enabled",0)
    counts=Counter(x.get("event","unknown") for x in events); results=Counter(); reasons=Counter()
    backpressure=Counter()
    for row in events:
        event=row.get("event")
        d=row.get("detail","")
        m=re.search(r"result=(-?\d+)",d)
        result=m.group(1) if m else "unknown"
        # Older logs used midpoint_present_failed for VK_NOT_READY/TIMEOUT.
        if event=="midpoint_present_skipped_backpressure" or (
            event=="midpoint_present_failed" and result in ("1","2")):
            backpressure[result]+=1
        elif event=="midpoint_present_failed":
            results[result]+=1
            reasons[d.split(" result=")[0] or "unspecified"]+=1
    keys=("avg_cpu_present_ms","max_cpu_present_ms","avg_source_acquire_ms","avg_generated_acquire_ms",
          "avg_source_handoff_wait_ms","avg_generated_handoff_wait_ms","avg_source_submission_wait_ms",
          "avg_generated_submission_wait_ms","avg_generate_record_ms","avg_generated_requested","avg_generated_queued")
    perfstats={k:s for k in keys if (s:=stats([num(x.get(k)) for x in perf]))}
    flowkeys=("forward_mean_confidence","backward_mean_confidence","consistency_mean","nonfinite_values")
    flowstats={k:s for k in flowkeys if (s:=stats([num(x.get(k)) for x in flow]))}
    modes=Counter()
    for m in re.finditer(r"(?:requested present mode=|actual present mode=|present_mode=)(AUTO|FIFO|MAILBOX|IMMEDIATE|[0-3])",log+"\n"+config,re.I):
        v=m.group(1).upper(); modes[{"0":"AUTO","1":"FIFO","2":"MAILBOX","3":"IMMEDIATE"}.get(v,v)]+=1
    actual_wsi=[]
    for m in re.finditer(r"SnapFrame: actual present mode=(FIFO|MAILBOX|IMMEDIATE|FIFO_RELAXED|OTHER) image_count=(\d+) extent=(\d+)x(\d+)",log,re.I):
        actual_wsi.append({"mode":m.group(1).upper(),"image_count":int(m.group(2)),"width":int(m.group(3)),"height":int(m.group(4))})
    wsi_rows=[key_values(line) for line in log.splitlines() if "DXVK-WSI:" in line]
    queue_rows=[key_values(line) for line in log.splitlines() if "DXVK-PRESENT:" in line]
    wsi_ms=timing_group(wsi_rows, {
        "queue_present_us":"vk_queue_present_ms",
        "pre_acquire_us":"wsi_pre_acquire_ms",
        "frame_limiter_us":"frame_limiter_ms",
        "total_present_us":"presenter_total_ms"})
    queue_ms=timing_group(queue_rows, {
        "queue_residence_us":"submission_queue_residence_ms",
        "queue_lock_wait_us":"queue_lock_wait_ms",
        "present_call_us":"present_call_ms",
        "total_entry_us":"submission_entry_total_ms"})
    failed=counts.get("midpoint_present_failed",0)
    backpressure_count=sum(backpressure.values())
    rejected=counts.get("gpu_metrics_rejected",0)
    findings=[]
    if backpressure_count: findings.append({"severity":"medium","code":"GENERATED_WSI_BACKPRESSURE","message":f"{backpressure_count} generated-frame attempts were skipped because no WSI image was immediately available (VK_NOT_READY/VK_TIMEOUT).","recommendation":"Keep generated acquire non-blocking. Compare backpressure by present mode and improve image scheduling; do not treat these skips as interpolation failures."})
    if failed and results: findings.append({"severity":"high","code":"GENERATED_WSI_ERRORS","message":f"{sum(results.values())} generated-frame failures have non-backpressure VkResults.","recommendation":"Inspect exact VkResult and surface/swapchain lifecycle."})
    if rejected: findings.append({"severity":"high","code":"AI_TELEMETRY_REJECTED","message":f"{rejected} GPU metric records were rejected; adaptive AI tuning must remain disabled.","recommendation":"Validate shader valid/expected cell counts against CPU grid dimensions and derive coverage from the same counters."})
    for key, code, label in (
        ("submission_queue_residence_ms","DXVK_SUBMISSION_QUEUE_BACKLOG","DXVK submission queue residence"),
        ("queue_lock_wait_ms","DXVK_QUEUE_LOCK_CONTENTION","DXVK queue mutex wait"),
        ("present_call_ms","DXVK_PRESENT_CALL_STALL","DXVK presenter call"),
        ("vk_queue_present_ms","VULKAN_QUEUE_PRESENT_STALL","vkQueuePresentKHR"),
        ("wsi_pre_acquire_ms","WSI_PRE_ACQUIRE_STALL","WSI pre-acquire"),
        ("frame_limiter_ms","DXVK_FRAME_LIMITER_STALL","DXVK frame limiter")):
        sample = queue_ms.get(key) or wsi_ms.get(key)
        if sample and sample["p95"] >= 8.0:
            findings.append({"severity":"high","code":code,
                "message":f"{label} has P95 {sample['p95']:.2f} ms across {sample['n']} diagnostic samples.",
                "recommendation":"Compare this stage with source_submission_wait_ms. Fix the measured blocking stage; do not remove presenter synchronization or semaphore waits without per-acquire ticket ownership."})
    if actual_wsi and min(x["image_count"] for x in actual_wsi)<3:
        findings.append({"severity":"medium","code":"WSI_IMAGE_COUNT_BELOW_TARGET","message":"The actual surface swapchain image count fell below three.","recommendation":"Compare generated acquire VK_NOT_READY rates by actual present mode. The surface may cap image count; do not force blocking acquire to compensate."})
    if not gpu: findings.append({"severity":"medium","code":"GPU_STAGE_CSV_EMPTY","message":"snapframe_gpu.csv has no stage samples.","recommendation":"Enable stage profiling briefly; keep diagnostic V2 workloads disabled during normal gameplay."})
    if not flow:
        if not profiling_enabled or not motion_analysis:
            disabled=[]
            if not profiling_enabled: disabled.append("profiling_enabled=1")
            if not motion_analysis: disabled.append("motion_analysis=1")
            findings.append({"severity":"info","code":"FLOW_READBACK_DISABLED_BY_CONFIG",
                "message":"snapframe_flow.csv has no samples because diagnostic readback is disabled by configuration ("+", ".join(disabled)+"). Flow dispatch success events do not mean vectors were read back.",
                "recommendation":"For a short diagnostic capture only, set profiling_enabled=1 and motion_analysis=1 in port/snapframe.conf. Restore profiling_enabled=0 for normal gameplay; readback adds GPU-to-CPU work."})
        else:
            findings.append({"severity":"medium","code":"FLOW_READBACK_MISSING",
                "message":"Flow readback is enabled, but snapframe_flow.csv has no samples.",
                "recommendation":"Inspect flow_readback_recorded/completion/allocation/map events and the output path. Accepted flow dispatch events alone do not prove readback completed."})
    sizes={n:((root/n).stat().st_size if (root/n).is_file() else 0) for n in FILES}
    return {"schema":"snapframe-diagnostic-report-v1","source_directory":root.name,"files_bytes":sizes,
      "event_rows":len(events),"event_counts":dict(counts),
      "generated_present":{"accepted":counts.get("midpoint_present_accepted",0),
        "skipped_backpressure":backpressure_count,"failed":sum(results.values()),
        "backpressure_vk_results":dict(backpressure),
        "failure_vk_results":dict(results),"failure_reasons":dict(reasons)},
      "present_modes_seen_in_log":dict(modes),"actual_wsi_configurations":actual_wsi,"performance":perfstats,
      "diagnostic_config":{"motion_analysis":bool(motion_analysis),"profiling_enabled":bool(profiling_enabled)},
      "dxvk_wsi":{"wsi_rows":len(wsi_rows),"queue_rows":len(queue_rows),
        "wsi_ms":wsi_ms,"queue_ms":queue_ms},
      "flow":{"rows":len(flow),"metrics":flowstats},"gpu_stage_rows":len(gpu),"findings":findings}

def to_markdown(r):
    out=["# SnapFrame diagnostic report","",f"- Schema: {r['schema']}",f"- Source folder: {r['source_directory']}",
      f"- Event rows: {r['event_rows']}",f"- Flow rows: {r['flow']['rows']}",f"- GPU stage rows: {r['gpu_stage_rows']}",
      f"- Diagnostic readback config: motion_analysis={int(r['diagnostic_config']['motion_analysis'])}, profiling_enabled={int(r['diagnostic_config']['profiling_enabled'])}",
      "","## Present / WSI","",f"- Generated accepted: {r['generated_present']['accepted']}",
      f"- Generated skipped (WSI backpressure): {r['generated_present']['skipped_backpressure']}",
      f"- Generated failed (non-backpressure): {r['generated_present']['failed']}",
      f"- Backpressure VkResult counts: {json.dumps(r['generated_present']['backpressure_vk_results'],sort_keys=True)}",
      f"- Failure VkResult counts: {json.dumps(r['generated_present']['failure_vk_results'],sort_keys=True)}",
      f"- Modes observed: {json.dumps(r['present_modes_seen_in_log'],sort_keys=True)}",
      "","## DXVK WSI / queue timings (ms)","",
      f"- DXVK-WSI samples: {r['dxvk_wsi']['wsi_rows']}",
      f"- DXVK-PRESENT samples: {r['dxvk_wsi']['queue_rows']}"]
    for group in ("wsi_ms","queue_ms"):
        for key, value in r["dxvk_wsi"][group].items():
            out.append(f"- {key}: mean {value['mean']:.3f}, median {value['median']:.3f}, p95 {value['p95']:.3f}, max {value['max']:.3f} (n={value['n']})")
    out += ["","## Event counts",""]
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
