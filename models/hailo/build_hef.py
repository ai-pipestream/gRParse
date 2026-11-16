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
import os
import re
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


# Optimization level 2+ needs >= 1024 calibration samples (or a GPU) or the DFC
# silently drops to 0; the level is set explicitly so a drop is an error in
# the log, not a quiet accuracy loss. HAILO_OPT_LEVEL / HAILO_PRECISION=0
# reproduce the first (level 0, all 8-bit) build.
OPT_LEVEL = int(os.environ.get("HAILO_OPT_LEVEL", "2"))
SIXTEEN_BIT = os.environ.get("HAILO_PRECISION", "1") != "0"


def short(name: str) -> str:
    return name.split("/", 1)[1]


def tail_until(layers: dict, stop_type: str) -> list[str]:
    """Layers walked back from the output layer until (excluding) stop_type."""
    out = next(n for n, l in layers.items() if l["type"] == "output_layer")
    chain, cur = [], layers[out]["input"][0]
    while layers[cur]["type"] != stop_type:
        chain.append(cur)
        cur = layers[cur]["input"][0]
    return chain


def sixteen_bit_layers(variant: str, layers: dict) -> list[str]:
    """Layers kept at a16_w16, named as in the expanded (optimization-time) graph.

    rec: the convolutional backbone (input normalization .. last avgpool). det: the whole graph.
    """
    names = list(layers)
    skip = {"input_layer", "output_layer"}
    picked = []
    if variant.startswith("rec"):
        # The MobileNetV1-enhance backbone, input normalization through the
        # last avgpool. PP-OCRv3 rec does not survive 8-bit activations there:
        # the DFC's layer noise analysis puts the output at -17 dB and every
        # line decodes empty; a16_w16 on the backbone alone brings CER vs
        # float to ~2% at level 0. The SVTR neck and CTC head stay at the
        # DFC's defaults: forcing them wider leaves the DFC's own
        # precision_change layers at a16_w16_a8, which hailo10h rejects
        # ("Unsupported layers for the target OptimizationTarget.MERCURY").
        last_pool = max(i for i, n in enumerate(names) if layers[n]["type"] == "avgpool")
        picked = [n for n in names[:last_pool + 1] if layers[n]["type"] not in skip | {"layer_normalization"}
                  and not short(n).startswith("precision_change")]
    elif variant.startswith("det"):
        # Every layer. 16-bit on the SE blocks and head alone leaves the
        # binary map at IoU 0.53-0.85 vs float; the whole graph at a16_w16
        # matches every float box with mean IoU 0.93-0.96 (level 0, det_640).
        # det has no matmul or LayerNorm, so no precision_change boundary.
        picked = [n for n in names if layers[n]["type"] not in skip]
    return [short(n) for n in names if n in set(picked)]


def base_script(variant: str) -> str:
    # The rec graph already has normalization layers (normalization1..5, its
    # LayerNorms), so the input normalization takes a distinct name.
    lines = [f"input_normalization1 = {SCRIPTS[variant]}",
             f"model_optimization_flavor(optimization_level={OPT_LEVEL}, compression_level=0)"]
    for layer, k in AVGPOOL_REDUCTION.get(variant, {}).items():
        lines.append(f"pre_quantization_optimization(global_avgpool_reduction, layers={layer}, "
                     f"division_factors=[{k}, {k}])")
    return "\n".join(lines) + "\n"


def precision_lines(modes: dict) -> str:
    by_mode = {}
    for layer, mode in modes.items():
        by_mode.setdefault(mode, []).append(layer)
    return "".join(f"quantization_param([{', '.join(layers)}], precision_mode={mode})\n"
                   for mode, layers in by_mode.items())


def expanded_layers(har: Path, arch: str, script: str, calib) -> dict:
    """The graph as the quantizer sees it: base script applied, full precision."""
    runner = ClientRunner(har=str(har), hw_arch=arch)
    runner.load_model_script(script)
    runner.optimize_full_precision(calib)
    return runner.get_hn_dict()["layers"]


def main() -> None:
    variant, har_dir, calib_dir, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
    arch = sys.argv[5] if len(sys.argv) > 5 else "hailo10h"
    out.mkdir(parents=True, exist_ok=True)
    har = har_dir / f"{variant}.har"
    calib = np.load(calib_dir / f"{variant}_calib.npy")
    print(f"{variant}: calibration set {calib.shape} {calib.dtype}", flush=True)
    base = base_script(variant)
    modes = {}
    if SIXTEEN_BIT:
        modes = {n: "a16_w16" for n in sixteen_bit_layers(variant, expanded_layers(har, arch, base, calib))}

    t0 = time.time()
    while True:
        script = base + precision_lines(modes)
        print(f"{variant}: model script:\n{script}", flush=True)
        runner = ClientRunner(har=str(har), hw_arch=arch)
        runner.load_model_script(script)
        try:
            runner.optimize(calib)
            break
        except ValueError as err:
            # The DFC names one layer whose type does not take the asked mode;
            # 16-bit output (a8_w8_a16) is the next widest most types accept.
            hit = re.search(r"Unsupported value .* in layer \S+?/(\S+?)\.", str(err))
            if not hit or hit.group(1) not in modes:
                raise
            layer = hit.group(1)
            if modes[layer] == "a16_w16":
                modes[layer] = "a8_w8_a16"
            else:
                del modes[layer]
            print(f"{variant}: {layer} rejected that precision, now {modes.get(layer, 'default')}", flush=True)
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
