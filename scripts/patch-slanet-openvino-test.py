#!/usr/bin/env python3
"""CPU-only provisioning regression: run with the pinned slanet_plus.onnx path."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import onnx

SCRIPT = Path(__file__).with_name("patch_slanet_openvino.py")
spec = importlib.util.spec_from_file_location("patch_slanet_openvino", SCRIPT)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
SOURCE = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else None


class ProvisioningTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if SOURCE is None or not SOURCE.is_file():
            raise RuntimeError("supply the pinned slanet_plus.onnx path; this test must not skip")
        cls.original = SOURCE.read_bytes()
        cls.model, cls.tokens = patch.derive(SOURCE)

    def test_deterministic_model_and_exact_vocabulary(self):
        second, tokens = patch.derive(SOURCE)
        self.assertEqual(second, self.model)
        self.assertEqual(tokens, self.tokens)
        self.assertEqual(hashlib.sha256(self.model).hexdigest(), patch.MODEL_SHA256)
        self.assertEqual(hashlib.sha256(self.tokens).hexdigest(),
                         "6df9c4ae78ad9f390833b5bf7f7980fb05692ba3dbeb2e1da16df26a45fe42a6")
        original = onnx.load_model_from_string(self.original)
        expected = next(p.value for p in original.metadata_props if p.key == "character")
        self.assertEqual(self.tokens, expected.encode("utf-8"))
        result = onnx.load_model_from_string(self.model)
        onnx.checker.check_model(result, full_check=True)
        onnx.shape_inference.infer_shapes(result, strict_mode=True, data_prop=True)
        for before, after in zip(original.graph.initializer, result.graph.initializer):
            self.assertEqual(before.SerializeToString(), after.SerializeToString())
        self.assertEqual([d.dim_value for d in result.graph.input[0].type.tensor_type.shape.dim],
                         [1, 3, 488, 488])
        self.assertEqual(SOURCE.read_bytes(), self.original)

    def test_unknown_source_is_rejected_without_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "changed.onnx"
            source.write_bytes(self.original + b"changed")
            target = Path(directory) / "output.onnx"
            for extra in ([], ["--tokens-only"]):
                result = subprocess.run([sys.executable, str(SCRIPT), str(source), str(target), *extra],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("source SHA256 differs", result.stderr)
                self.assertFalse(target.exists())

    def test_source_cannot_be_overwritten(self):
        result = subprocess.run([sys.executable, str(SCRIPT), str(SOURCE), str(SOURCE)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("output must differ", result.stderr)
        self.assertEqual(SOURCE.read_bytes(), self.original)


if __name__ == "__main__":
    unittest.main()
