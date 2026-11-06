import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dxvk"))
import generate_native_262_android_manifest as manifest


class DxvkNativeManifestParserTests(unittest.TestCase):
    def test_extract_cpp_array(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "meson.build").write_text(
                "unit_src = [\n  'one.cpp',\n  'two.c',\n]\n",
                encoding="utf-8",
            )
            self.assertEqual(
                manifest.extract_array(root, "meson.build", "unit_src"),
                ["one.cpp", "two.c"],
            )

    def test_extract_cpp_files_wrapper(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "meson.build").write_text(
                "unit_src = files([\\n"
                "  'one.cpp',\\n"
                "  'two.c',\\n"
                "])\\n",
                encoding="utf-8",
            )
            self.assertEqual(
                manifest.extract_array(root, "meson.build", "unit_src"),
                ["one.cpp", "two.c"],
            )

    def test_extract_shader_array(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "meson.build").write_text(
                "shader_src = files([\n"
                "  'first.comp',\n"
                "  'second.frag',\n"
                "  'third.vert',\n"
                "  'fourth.geom',\n"
                "])\n",
                encoding="utf-8",
            )
            self.assertEqual(
                manifest.extract_shader_array(root, "meson.build", "shader_src"),
                ["first.comp", "second.frag", "third.vert", "fourth.geom"],
            )

    def test_missing_shader_array_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "meson.build").write_text("other = []\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                manifest.extract_shader_array(root, "meson.build", "shader_src")


if __name__ == "__main__":
    unittest.main()
