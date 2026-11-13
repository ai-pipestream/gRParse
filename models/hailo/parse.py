"""Step 1: parse a shape-pinned ONNX into a Hailo HAR (hailo parser onnx).

Usage: python parse.py ONNX HAR [HW_ARCH]
"""
import sys

from hailo_sdk_client import ClientRunner


def main() -> None:
    onnx_path, har_path = sys.argv[1], sys.argv[2]
    arch = sys.argv[3] if len(sys.argv) > 3 else "hailo10h"
    runner = ClientRunner(hw_arch=arch)
    name = har_path.rsplit("/", 1)[-1].removesuffix(".har")
    hn, _ = runner.translate_onnx_model(onnx_path, name)
    runner.save_har(har_path)
    layers = hn.get("layers", {}) if isinstance(hn, dict) else {}
    print(f"parsed {onnx_path} -> {har_path} ({len(layers)} layers, arch {arch})")


if __name__ == "__main__":
    main()
