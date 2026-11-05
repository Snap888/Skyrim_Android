import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_snapframe_logs import analyze, read_csv


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
            self.assertEqual(report["generated_present"]["failed"], 1)
            self.assertEqual(report["generated_present"]["failure_vk_results"]["1"], 1)
            self.assertEqual(report["performance"]["avg_cpu_present_ms"]["median"], 6.0)
            codes = {item["code"] for item in report["findings"]}
            self.assertIn("GENERATED_WSI_BACKPRESSURE", codes)
            self.assertIn("AI_TELEMETRY_REJECTED", codes)


if __name__ == "__main__":
    unittest.main()
