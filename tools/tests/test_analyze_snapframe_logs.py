import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_snapframe_logs import analyze, read_csv, to_markdown


class AnalyzerTests(unittest.TestCase):
    def test_reads_dxvk_columns_preamble(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "perf.csv"
            path.write_text(
                "=== DXVK_PERF_START run=test ===\n"
                "columns: frame,avg_cpu_present_ms,avg_generated_queued\n"
                "120,4.0,0.5\n240,8.0,1.0\n", encoding="utf-8")
            rows = read_csv(path)
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["frame"], "120")
            self.assertEqual(rows[1]["avg_generated_queued"], "1.0")

    def test_reports_wsi_and_telemetry_findings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "snapframe_events.csv").write_text(
                "frame,event,width,height,detail\n"
                '1,midpoint_present_failed,2448,1080,"generation_or_wsi_present_not_accepted result=1"\n'
                '2,gpu_metrics_rejected,2448,1080,"metrics_grid_coverage_mismatch"\n'
                '3,midpoint_present_accepted,2448,1080,"wsi_present_completion_vk_success"\n',
                encoding="utf-8")
            (root / "snapframe_perf_on.csv").write_text(
                "=== DXVK_PERF_START run=test ===\n"
                "columns: frame,avg_cpu_present_ms,avg_generated_queued\n"
                "120,4.0,0.5\n240,8.0,1.0\n", encoding="utf-8")
            report = analyze(root)
            self.assertEqual(report["generated_present"]["accepted"], 1)
            self.assertEqual(report["generated_present"]["failed"], 0)
            self.assertEqual(report["generated_present"]["skipped_backpressure"], 1)
            self.assertEqual(report["generated_present"]["backpressure_vk_results"]["1"], 1)
            self.assertEqual(report["performance"]["avg_cpu_present_ms"]["median"], 6.0)
            codes = {item["code"] for item in report["findings"]}
            self.assertIn("GENERATED_WSI_BACKPRESSURE", codes)
            self.assertIn("AI_TELEMETRY_REJECTED", codes)

    def test_separates_real_generated_present_errors_from_backpressure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "snapframe_events.csv").write_text(
                "frame,event,width,height,detail\n"
                '1,midpoint_present_skipped_backpressure,2448,1080,"generated_wsi_image_unavailable result=1"\n'
                '2,midpoint_present_failed,2448,1080,"generation_failed result=-1000000000"\n',
                encoding="utf-8")
            report = analyze(root)
            self.assertEqual(report["generated_present"]["skipped_backpressure"], 1)
            self.assertEqual(report["generated_present"]["failed"], 1)
            codes = {item["code"] for item in report["findings"]}
            self.assertIn("GENERATED_WSI_ERRORS", codes)


    def test_flags_surface_clamping_of_wsi_image_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "log.txt").write_text(
                "SnapFrame: actual present mode=FIFO image_count=2 extent=2448x1080\n",
                encoding="utf-8")
            report = analyze(root)
            self.assertEqual(report["actual_wsi_configurations"][0]["image_count"], 2)
            codes = {item["code"] for item in report["findings"]}
            self.assertIn("WSI_IMAGE_COUNT_BELOW_TARGET", codes)


    def test_reports_dxvk_queue_and_vulkan_wsi_stalls(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "log.txt").write_text(
                "DXVK-WSI: queue_present_us=9000 pre_acquire_us=1000 frame_limiter_us=0 total_present_us=10000 acquire_result=1 result=0 present_mode=1 apply_frame_limit=1\n"
                "DXVK-PRESENT: queue_residence_us=12000 queue_depth=3 queue_lock_wait_us=100 present_call_us=15000 total_entry_us=27100 result=0 diagnostic_count=1\n",
                encoding="utf-8")
            report = analyze(root)
            self.assertEqual(report["dxvk_wsi"]["wsi_rows"], 1)
            self.assertEqual(report["dxvk_wsi"]["queue_rows"], 1)
            self.assertEqual(report["dxvk_wsi"]["wsi_ms"]["vk_queue_present_ms"]["p95"], 9.0)
            self.assertEqual(report["dxvk_wsi"]["queue_ms"]["submission_queue_residence_ms"]["p95"], 12.0)
            codes = {item["code"] for item in report["findings"]}
            self.assertIn("VULKAN_QUEUE_PRESENT_STALL", codes)
            self.assertIn("DXVK_SUBMISSION_QUEUE_BACKLOG", codes)
            self.assertIn("DXVK_PRESENT_CALL_STALL", codes)

    def test_distinguishes_disabled_flow_readback_from_missing_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "snapframe.conf").write_text("enabled=1\npresent_mode=2\nprofiling_enabled=0\n", encoding="utf-8")
            report = analyze(root)
            self.assertFalse(report["diagnostic_config"]["profiling_enabled"])
            self.assertTrue(report["diagnostic_config"]["motion_analysis"])
            codes = {item["code"] for item in report["findings"]}
            self.assertIn("FLOW_READBACK_DISABLED_BY_CONFIG", codes)
            self.assertNotIn("FLOW_CSV_EMPTY", codes)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "snapframe.conf").write_text("motion_analysis=1\nprofiling_enabled=1\n", encoding="utf-8")
            report = analyze(root)
            codes = {item["code"] for item in report["findings"]}
            self.assertIn("FLOW_READBACK_MISSING", codes)
            self.assertNotIn("FLOW_READBACK_DISABLED_BY_CONFIG", codes)

    def test_reports_ai_wsi_throttle_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "snapframe_events.csv").write_text(
                "frame,event,width,height,detail\n"
                '1,ai_wsi_backpressure,2448,1080,"reason=generated_wait;source_wait_ms=2.0;generated_wait_ms=9.0"\n'
                '2,ai_wsi_backpressure,2448,1080,"reason=severe_source_wait;source_wait_ms=17.0;generated_wait_ms=0.4"\n'
                '3,ai_wsi_backpressure,2448,1080,"source_wait_ms=10.0;generated_wait_ms=9.0"\n',
                encoding="utf-8")
            report = analyze(root)
            self.assertEqual(report["wsi_throttle_reasons"], {
                "generated_wait": 1,
                "severe_source_wait": 1,
                "legacy_or_unspecified": 1,
            })
            rendered = to_markdown(report)
            self.assertIn("## WSI throttle reasons", rendered)
            self.assertIn("- generated_wait: 1", rendered)
            self.assertIn("- severe_source_wait: 1", rendered)
            self.assertIn("- legacy_or_unspecified: 1", rendered)

if __name__ == "__main__":
    unittest.main()
