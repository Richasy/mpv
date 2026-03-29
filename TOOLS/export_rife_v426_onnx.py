#!/usr/bin/env python3
"""
Export RIFE v4.26 IFNet from PyTorch .pkl to ONNX format.

Usage:
    python export_rife_v426_onnx.py --rife-repo /path/to/rife_v4.26 \
                                     --output /path/to/rife.onnx \
                                     --height 720 --width 1280

Produces a single ONNX file (rife.onnx) that takes:
  Input:  imgs     [1, 6, H, W]  — concatenated img0 and img1
          timestep [1, 1, 1, 1]  — interpolation timestep (e.g. 0.5)
  Output: output   [1, 3, H, W]  — interpolated frame

The model expects H,W to be padded to multiples of 64 (5 IFBlocks with
stride-2 downsampling + scale_list max 16 → effectively 64-pixel alignment).
"""

import argparse
import os
import sys

import torch
import torch.nn as nn
import torch.nn.functional as F


def parse_args():
    p = argparse.ArgumentParser(description="Export RIFE v4.26 to ONNX")
    p.add_argument("--rife-repo", required=True,
                    help="Path to rife_v4.26 repository")
    p.add_argument("--output", required=True,
                    help="Output path for ONNX file (e.g. rife.onnx)")
    p.add_argument("--height", type=int, default=720,
                    help="Reference height for export (default: 720)")
    p.add_argument("--width", type=int, default=1280,
                    help="Reference width for export (default: 1280)")
    p.add_argument("--opset", type=int, default=17,
                    help="ONNX opset version (default: 17)")
    return p.parse_args()


# --- Standalone warp function (no cached grid, ONNX-friendly) ---

def warp(tenInput, tenFlow):
    """ONNX-exportable warp: builds grid from scratch each call."""
    B, C, H, W = tenInput.shape
    tenHor = torch.linspace(-1.0, 1.0, W, dtype=tenFlow.dtype,
                            device=tenFlow.device).view(1, 1, 1, W).expand(B, 1, H, W)
    tenVer = torch.linspace(-1.0, 1.0, H, dtype=tenFlow.dtype,
                            device=tenFlow.device).view(1, 1, H, 1).expand(B, 1, H, W)
    flow_norm = torch.cat([
        tenFlow[:, 0:1] / ((W - 1.0) / 2.0),
        tenFlow[:, 1:2] / ((H - 1.0) / 2.0)
    ], 1)
    grid = (torch.cat([tenHor, tenVer], 1) + flow_norm).permute(0, 2, 3, 1)
    return F.grid_sample(tenInput, grid, mode='bilinear',
                         padding_mode='border', align_corners=True)


# --- Model architecture (copied from IFNet_HDv3.py with warp inlined) ---

def conv(in_planes, out_planes, kernel_size=3, stride=1, padding=1, dilation=1):
    return nn.Sequential(
        nn.Conv2d(in_planes, out_planes, kernel_size=kernel_size, stride=stride,
                  padding=padding, dilation=dilation, bias=True),
        nn.LeakyReLU(0.2, True)
    )


class Head(nn.Module):
    def __init__(self):
        super().__init__()
        self.cnn0 = nn.Conv2d(3, 16, 3, 2, 1)
        self.cnn1 = nn.Conv2d(16, 16, 3, 1, 1)
        self.cnn2 = nn.Conv2d(16, 16, 3, 1, 1)
        self.cnn3 = nn.ConvTranspose2d(16, 4, 4, 2, 1)
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        x0 = self.cnn0(x)
        x = self.relu(x0)
        x1 = self.cnn1(x)
        x = self.relu(x1)
        x2 = self.cnn2(x)
        x = self.relu(x2)
        x3 = self.cnn3(x)
        return x3


class ResConv(nn.Module):
    def __init__(self, c, dilation=1):
        super().__init__()
        self.conv = nn.Conv2d(c, c, 3, 1, dilation, dilation=dilation, groups=1)
        self.beta = nn.Parameter(torch.ones((1, c, 1, 1)), requires_grad=True)
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        return self.relu(self.conv(x) * self.beta + x)


class IFBlock(nn.Module):
    def __init__(self, in_planes, c=64):
        super().__init__()
        self.conv0 = nn.Sequential(
            conv(in_planes, c // 2, 3, 2, 1),
            conv(c // 2, c, 3, 2, 1),
        )
        self.convblock = nn.Sequential(
            ResConv(c), ResConv(c), ResConv(c), ResConv(c),
            ResConv(c), ResConv(c), ResConv(c), ResConv(c),
        )
        self.lastconv = nn.Sequential(
            nn.ConvTranspose2d(c, 4 * 13, 4, 2, 1),
            nn.PixelShuffle(2)
        )

    def forward(self, x, flow=None, scale=1):
        x = F.interpolate(x, scale_factor=1. / scale, mode="bilinear",
                          align_corners=False)
        if flow is not None:
            flow = F.interpolate(flow, scale_factor=1. / scale, mode="bilinear",
                                 align_corners=False) * 1. / scale
            x = torch.cat((x, flow), 1)
        feat = self.conv0(x)
        feat = self.convblock(feat)
        tmp = self.lastconv(feat)
        tmp = F.interpolate(tmp, scale_factor=scale, mode="bilinear",
                            align_corners=False)
        flow = tmp[:, :4] * scale
        mask = tmp[:, 4:5]
        feat = tmp[:, 5:]
        return flow, mask, feat


class IFNet(nn.Module):
    def __init__(self):
        super().__init__()
        self.block0 = IFBlock(7 + 8, c=192)
        self.block1 = IFBlock(8 + 4 + 8 + 8, c=128)
        self.block2 = IFBlock(8 + 4 + 8 + 8, c=96)
        self.block3 = IFBlock(8 + 4 + 8 + 8, c=64)
        self.block4 = IFBlock(8 + 4 + 8 + 8, c=32)
        self.encode = Head()


# --- ONNX export wrapper ---

class RIFEExport(nn.Module):
    """Wraps IFNet for clean ONNX export with inlined warp."""
    def __init__(self, ifnet):
        super().__init__()
        self.block0 = ifnet.block0
        self.block1 = ifnet.block1
        self.block2 = ifnet.block2
        self.block3 = ifnet.block3
        self.block4 = ifnet.block4
        self.encode = ifnet.encode

    def forward(self, imgs, timestep):
        """
        imgs:     [B, 6, H, W]  — concatenated img0, img1
        timestep: [B, 1, 1, 1]  — scalar timestep
        Returns:  [B, 3, H, W]  — interpolated frame
        """
        img0 = imgs[:, :3]
        img1 = imgs[:, 3:]
        H, W = img0.shape[2], img0.shape[3]

        # Timestep map expanded to spatial dimensions
        timestep_map = timestep.expand(-1, 1, H, W)

        # Head encoder features (4 channels each)
        f0 = self.encode(img0)
        f1 = self.encode(img1)

        # 5-scale iterative flow refinement
        scale_list = [16, 8, 4, 2, 1]
        blocks = [self.block0, self.block1, self.block2, self.block3, self.block4]

        flow = None
        mask = None
        feat = None
        warped_img0 = img0
        warped_img1 = img1

        for i in range(5):
            if flow is None:
                flow, mask, feat = blocks[i](
                    torch.cat((img0[:, :3], img1[:, :3], f0, f1, timestep_map), 1),
                    None, scale=scale_list[i]
                )
            else:
                wf0 = warp(f0, flow[:, :2])
                wf1 = warp(f1, flow[:, 2:4])
                fd, m0, feat = blocks[i](
                    torch.cat((warped_img0[:, :3], warped_img1[:, :3],
                               wf0, wf1, timestep_map, mask, feat), 1),
                    flow, scale=scale_list[i]
                )
                mask = m0
                flow = flow + fd

            warped_img0 = warp(img0, flow[:, :2])
            warped_img1 = warp(img1, flow[:, 2:4])

        mask = torch.sigmoid(mask)
        merged = warped_img0 * mask + warped_img1 * (1 - mask)
        return merged


def main():
    args = parse_args()

    device = torch.device("cpu")
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    # Load weights
    print("Loading RIFE v4.26 weights...")
    ifnet = IFNet()
    weights_path = os.path.join(args.rife_repo, "flownet.pkl")
    state_dict = torch.load(weights_path, map_location=device)

    # Strip "module." prefix if present (from DDP training)
    clean_sd = {}
    for k, v in state_dict.items():
        clean_sd[k.replace("module.", "")] = v

    ifnet.load_state_dict(clean_sd, strict=False)
    ifnet.eval()
    print(f"  Loaded {len(clean_sd)} parameters from {weights_path}")

    # Create export wrapper
    model = RIFEExport(ifnet).eval()

    H, W = args.height, args.width
    # Pad to 64-pixel alignment (required by 5 IFBlocks with scale=16)
    pad_h = (64 - H % 64) % 64
    pad_w = (64 - W % 64) % 64
    pH = H + pad_h
    pW = W + pad_w
    print(f"  Trace size: {H}x{W} -> padded {pH}x{pW}")

    # Dummy inputs
    imgs = torch.randn(1, 6, pH, pW)
    timestep = torch.tensor([[[[0.5]]]])

    # Export
    print(f"  Exporting to {args.output}...")
    torch.onnx.export(
        model, (imgs, timestep),
        args.output,
        input_names=["imgs", "timestep"],
        output_names=["output"],
        opset_version=args.opset,
        do_constant_folding=True,
        dynamic_axes={
            "imgs": {0: "batch", 2: "height", 3: "width"},
            "output": {0: "batch", 2: "height", 3: "width"},
        },
    )

    size_mb = os.path.getsize(args.output) / (1024 * 1024)
    print(f"  Done: {args.output} ({size_mb:.1f} MB)")
    print(f"\nUsage in mpv: --gmfss-model=/path/to/directory/containing/rife.onnx")


if __name__ == "__main__":
    main()
