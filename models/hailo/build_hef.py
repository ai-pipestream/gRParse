"""Steps 2 and 3: quantize a parsed HAR with a calibration set, then compile a HEF.

Usage: python build_hef.py VARIANT HAR_DIR CALIB_DIR OUT_DIR [HW_ARCH]

VARIANT is one of det_960, det_640, rec_48x320, rec_48x640, cls_48x192.
The calibration set is CALIB_DIR/<VARIANT>_calib.npy (uint8 NHWC BGR, from
prep_calib.py). Normalization is folded onto the chip through the model
script, matching RapidOcrOnnx as gRParse builds it. Optimization and
compression stay at the DFC defaults; whatever level the DFC actually
applies is printed in its own log.

Writes OUT_DIR/<VARIANT>.q.har (quantized) and OUT_DIR/<VARIANT>.hef.
"""
import hashlib
import sys
import time
from pathlib import Path

import numpy as np
from hailo_sdk_client import ClientRunner

DET_NORM = "normalization([123.675, 116.28, 103.53], [58.395, 57.12, 57.375])"
LINE_NORM = "normalization([127.5, 127.5, 127.5], [127.5, 127.5, 127.5])"
SCRIPTS = {
    "det_960": DET_NORM,
    "det_640": DET_NORM,
    "rec_48x320": LINE_NORM,
    "rec_48x640": LINE_NORM,
    "cls_48x192": LINE_NORM,
}
# The det neck's SE blocks end in global average pools over the full feature
# map; at 240x240 or 120x120 the DFC rejects them ("Shift delta in
# det_960/avgpool1/avgpool_op is larger than 2 (2.64), cannot quantize").
# global_avgpool_reduction splits each into a pool of kernel spatial/k and
# a k x k global pool, so no stage averages more than 40x40 at once.
AVGPOOL_REDUCTION = {
    "det_960": {"avgpool1": 8, "avgpool8": 8, "avgpool2": 4, "avgpool7": 4, "avgpool3": 2, "avgpool6": 2},
    "det_640": {"avgpool1": 4, "avgpool8": 4, "avgpool2": 2, "avgpool7": 2},
}


def model_script(variant: str) -> str:
    # The rec graph already has normalization layers (normalization1..5, its
    # LayerNorms), so the input normalization takes a distinct name.
    lines = [f"input_normalization1 = {SCRIPTS[variant]}"]
    for layer, k in AVGPOOL_REDUCTION.get(variant, {}).items():
        lines.append(f"pre_quantization_optimization(global_avgpool_reduction, layers={layer}, "
                     f"division_factors=[{k}, {k}])")
    return "\n".join(lines) + "\n"


def main() -> None:
    variant, har_dir, calib_dir, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
    arch = sys.argv[5] if len(sys.argv) > 5 else "hailo10h"
    out.mkdir(parents=True, exist_ok=True)
    runner = ClientRunner(har=str(har_dir / f"{variant}.har"), hw_arch=arch)
    script = model_script(variant)
    print(f"{variant}: model script:\n{script}", flush=True)
    runner.load_model_script(script)
    calib = np.load(calib_dir / f"{variant}_calib.npy")
    print(f"{variant}: calibration set {calib.shape} {calib.dtype}", flush=True)

    t0 = time.time()
    runner.optimize(calib)
    q_har = out / f"{variant}.q.har"
    runner.save_har(str(q_har))
    print(f"{variant}: optimize done in {time.time() - t0:.0f} s -> {q_har}", flush=True)

    t0 = time.time()
    hef = runner.compile()
    hef_path = out / f"{variant}.hef"
    hef_path.write_bytes(hef)
    digest = hashlib.sha256(hef).hexdigest()
    print(f"{variant}: compile done in {time.time() - t0:.0f} s -> {hef_path} "
          f"{len(hef)} bytes sha256={digest}", flush=True)


if __name__ == "__main__":
    main()
