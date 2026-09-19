#!/usr/bin/env python3
"""Compare 11-channel RIFE ONNX models against real odd-frame ground truth."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import time

import numpy as np
import onnxruntime as ort
from PIL import Image


def parse_model(value):
    try:
        name, padding, path = value.split("=", 2)
        padding = int(padding)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "model must be NAME=PADDING=PATH") from error
    if padding not in (32, 64, 128):
        raise argparse.ArgumentTypeError("padding must be 32, 64, or 128")
    path = Path(path).resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"model not found: {path}")
    return name, padding, path


def extract_frames(video, count, directory):
    pattern = str(directory / "%05d.png")
    subprocess.run(
        [
            "ffmpeg", "-v", "error", "-i", str(video),
            "-frames:v", str(count), "-vsync", "0", pattern,
        ],
        check=True,
    )
    frames = sorted(directory.glob("*.png"))
    if len(frames) != count:
        raise RuntimeError(f"expected {count} frames, extracted {len(frames)}")
    return frames


def resize_rgb(image, width, height):
    if image.shape[1] == width and image.shape[0] == height:
        return image
    pil = Image.fromarray(np.round(image * 255.0).astype(np.uint8), "RGB")
    pil = pil.resize((width, height), Image.Resampling.BILINEAR)
    return np.asarray(pil, dtype=np.float32) / 255.0


def build_input(frame0, frame1, scale, padding, timestep=0.5):
    original_h, original_w = frame0.shape[:2]
    process_w = max(32, round(original_w * scale))
    process_h = max(32, round(original_h * scale))
    frame0 = resize_rgb(frame0, process_w, process_h)
    frame1 = resize_rgb(frame1, process_w, process_h)
    padded_w = math.ceil(process_w / padding) * padding
    padded_h = math.ceil(process_h / padding) * padding
    pad = ((0, padded_h - process_h), (0, padded_w - process_w), (0, 0))
    frame0 = np.pad(frame0, pad, mode="edge")
    frame1 = np.pad(frame1, pad, mode="edge")

    xs = np.linspace(-1.0, 1.0, padded_w, dtype=np.float32)
    ys = np.linspace(-1.0, 1.0, padded_h, dtype=np.float32)
    channels = [
        *frame0.transpose(2, 0, 1),
        *frame1.transpose(2, 0, 1),
        np.full((padded_h, padded_w), timestep, dtype=np.float32),
        np.broadcast_to(xs[None, :], (padded_h, padded_w)),
        np.broadcast_to(ys[:, None], (padded_h, padded_w)),
        np.full((padded_h, padded_w), 2.0 / (padded_w - 1),
                dtype=np.float32),
        np.full((padded_h, padded_w), 2.0 / (padded_h - 1),
                dtype=np.float32),
    ]
    tensor = np.stack(channels, axis=0)[None, ...]
    return tensor, (process_w, process_h), (original_w, original_h)


def restore_output(output, process_size, original_size):
    process_w, process_h = process_size
    original_w, original_h = original_size
    image = np.clip(output[0, :, :process_h, :process_w]
                    .transpose(1, 2, 0), 0.0, 1.0)
    return resize_rgb(image, original_w, original_h)


def box_filter(image, radius):
    size = radius * 2 + 1
    padded = np.pad(
        image.astype(np.float64, copy=False),
        radius,
        mode="reflect",
    )
    integral = np.pad(padded, ((1, 0), (1, 0)), mode="constant")
    integral = integral.cumsum(axis=0).cumsum(axis=1)
    return (
        integral[size:, size:]
        - integral[:-size, size:]
        - integral[size:, :-size]
        + integral[:-size, :-size]
    ) / float(size * size)


def ssim_luma(reference, candidate):
    weights = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
    x = reference @ weights
    y = candidate @ weights
    mu_x = box_filter(x, 5)
    mu_y = box_filter(y, 5)
    sigma_x = np.maximum(box_filter(x * x, 5) - mu_x * mu_x, 0.0)
    sigma_y = np.maximum(box_filter(y * y, 5) - mu_y * mu_y, 0.0)
    sigma_xy = box_filter(x * y, 5) - mu_x * mu_y
    covariance_bound = np.sqrt(sigma_x * sigma_y)
    sigma_xy = np.clip(sigma_xy, -covariance_bound, covariance_bound)
    c1 = 0.01 ** 2
    c2 = 0.03 ** 2
    score = (
        (2 * mu_x * mu_y + c1) * (2 * sigma_xy + c2)
        / ((mu_x * mu_x + mu_y * mu_y + c1)
           * (sigma_x + sigma_y + c2))
    )
    return float(np.mean(np.clip(score, -1.0, 1.0)))


def psnr(reference, candidate):
    mse = float(np.mean((reference - candidate) ** 2))
    return math.inf if mse == 0.0 else 10.0 * math.log10(1.0 / mse)


def percentile(values, value):
    return float(np.percentile(np.asarray(values, dtype=np.float64), value))


def save_image(path, image):
    Image.fromarray(
        np.round(np.clip(image, 0.0, 1.0) * 255.0).astype(np.uint8),
        "RGB",
    ).save(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("video", type=Path)
    parser.add_argument(
        "--model", action="append", type=parse_model, required=True,
        help="NAME=PADDING=PATH; repeat for every candidate")
    parser.add_argument("--pairs", type=int, default=20)
    parser.add_argument("--scale", type=float, default=0.5)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not args.video.is_file():
        parser.error(f"video not found: {args.video}")
    if args.pairs < 1:
        parser.error("--pairs must be positive")
    if not 0.25 <= args.scale <= 1.0:
        parser.error("--scale must be between 0.25 and 1.0")
    if "DmlExecutionProvider" not in ort.get_available_providers():
        raise RuntimeError("onnxruntime-directml is required")

    args.output.mkdir(parents=True, exist_ok=True)
    report = {
        "schema": "richasy.rife-quality-benchmark.v1",
        "video": {
            "name": args.video.name,
            "pairs": args.pairs,
            "scale": args.scale,
        },
        "runtime": {
            "onnxruntime": ort.__version__,
            "provider": "DmlExecutionProvider",
            "device": args.device,
        },
        "models": [],
    }

    with tempfile.TemporaryDirectory(prefix="rife-quality-") as temporary:
        frame_paths = extract_frames(
            args.video, args.pairs * 2 + 1, Path(temporary))
        frames = [
            np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
            / 255.0
            for path in frame_paths
        ]

        for name, padding, model_path in args.model:
            options = ort.SessionOptions()
            options.graph_optimization_level = (
                ort.GraphOptimizationLevel.ORT_ENABLE_ALL)
            session = ort.InferenceSession(
                str(model_path),
                sess_options=options,
                providers=[
                    ("DmlExecutionProvider", {"device_id": args.device}),
                    "CPUExecutionProvider",
                ],
            )
            if session.get_providers()[0] != "DmlExecutionProvider":
                raise RuntimeError(f"{name}: DirectML provider was not selected")
            input_name = session.get_inputs()[0].name
            output_name = session.get_outputs()[0].name
            timings = []
            psnr_values = []
            ssim_values = []
            worst = None

            for index in range(args.pairs):
                frame0 = frames[index * 2]
                truth = frames[index * 2 + 1]
                frame1 = frames[index * 2 + 2]
                tensor, process_size, original_size = build_input(
                    frame0, frame1, args.scale, padding)
                started = time.perf_counter()
                output = session.run(
                    [output_name], {input_name: tensor})[0]
                timings.append((time.perf_counter() - started) * 1000.0)
                candidate = restore_output(
                    output, process_size, original_size)
                pair_psnr = psnr(truth, candidate)
                pair_ssim = ssim_luma(truth, candidate)
                psnr_values.append(pair_psnr)
                ssim_values.append(pair_ssim)
                if worst is None or pair_ssim < worst[0]:
                    worst = (pair_ssim, index, frame0, truth,
                             candidate, frame1)

            assert worst is not None
            worst_dir = args.output / f"{name}-worst"
            worst_dir.mkdir(exist_ok=True)
            save_image(worst_dir / "frame0.png", worst[2])
            save_image(worst_dir / "truth.png", worst[3])
            save_image(worst_dir / "candidate.png", worst[4])
            save_image(worst_dir / "frame1.png", worst[5])

            result = {
                "name": name,
                "path": str(model_path),
                "padding": padding,
                "inferenceMs": {
                    "mean": float(np.mean(timings)),
                    "p50": percentile(timings, 50),
                    "p95": percentile(timings, 95),
                },
                "psnr": {
                    "mean": float(np.mean(psnr_values)),
                    "minimum": float(np.min(psnr_values)),
                },
                "ssim": {
                    "mean": float(np.mean(ssim_values)),
                    "minimum": float(np.min(ssim_values)),
                },
                "worstPair": int(worst[1]),
            }
            report["models"].append(result)
            print(json.dumps(result, ensure_ascii=False))

    report_path = args.output / "report.json"
    report_path.write_text(
        json.dumps(report, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    print(f"report={report_path}")


if __name__ == "__main__":
    main()
