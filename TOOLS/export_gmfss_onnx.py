#!/usr/bin/env python3
"""
Export GMFSS_Fortuna sub-networks from PyTorch .pkl to ONNX format.

Usage:
    python export_gmfss_onnx.py --gmfss-repo /path/to/GMFSS_Fortuna \
                                --model-dir /path/to/fortuna_union_ft_animerun \
                                --output-dir /path/to/output \
                                --height 720 --width 1280

Produces 5 ONNX files:
    feat.onnx      - FeatureNet (3-scale feature extractor)
    flownet.onnx   - GMFlow (optical flow estimator)
    metric.onnx    - MetricNet (occlusion/reliability metric)
    rife.onnx      - IFNet (RIFE-based interpolation)
    fusionnet.onnx - GridNet (multi-scale fusion)
"""

import argparse
import os
import sys

import torch
import torch.nn as nn
import torch.nn.functional as F


def parse_args():
    p = argparse.ArgumentParser(description="Export GMFSS sub-networks to ONNX")
    p.add_argument("--gmfss-repo", required=True, help="Path to GMFSS_Fortuna repository")
    p.add_argument("--model-dir", required=True, help="Path to model weights directory (e.g. fortuna_union_ft_animerun)")
    p.add_argument("--output-dir", required=True, help="Output directory for ONNX files")
    p.add_argument("--height", type=int, default=720, help="Reference height for export (default: 720)")
    p.add_argument("--width", type=int, default=1280, help="Reference width for export (default: 1280)")
    p.add_argument("--opset", type=int, default=17, help="ONNX opset version (default: 17)")
    return p.parse_args()


# --- Wrapper modules for clean ONNX export ---

class FeatureNetExport(nn.Module):
    """Wraps FeatureNet for ONNX export with 3 named outputs."""
    def __init__(self, feat_ext):
        super().__init__()
        self.feat_ext = feat_ext

    def forward(self, x):
        f1, f2, f3 = self.feat_ext(x)
        return f1, f2, f3


def _onnx_compatible_roll(input, shifts, dims):
    """ONNX-compatible replacement for torch.roll using Slice+Cat.
    torch.roll is not in the ONNX standard opset, but can be decomposed into
    slice and concatenate operations along each dimension."""
    if not isinstance(shifts, (tuple, list)):
        shifts = (shifts,)
    if not isinstance(dims, (tuple, list)):
        dims = (dims,)
    result = input
    for shift, dim in zip(shifts, dims):
        size = result.shape[dim]
        # Normalize negative shift
        shift = shift % size
        if shift == 0:
            continue
        # Split into two parts and concatenate in reversed order
        part1 = torch.narrow(result, dim, 0, size - shift)
        part2 = torch.narrow(result, dim, size - shift, shift)
        result = torch.cat([part2, part1], dim=dim)
    return result


class GMFlowExport(nn.Module):
    """Wraps GMFlow for single-direction flow export.
    Input must already be padded to multiples of 64 (required by Swin
    Transformer with attn_splits=8 on 1/8-resolution backbone features).

    Pre-computes all Swin Transformer attention masks as registered buffers
    so ONNX trace mode doesn't encounter dynamic negative-index slicing
    or in-place masked_fill operations."""
    def __init__(self, flownet, trace_h, trace_w):
        super().__init__()
        self.flownet = flownet
        # Pre-compute and cache attention masks for the fixed trace resolution.
        # This patches the transformer layers to use constant masks during export.
        self._precompute_attn_masks(trace_h, trace_w)

    def _precompute_attn_masks(self, h, w):
        """Run a dummy forward pass to let the transformer generate and cache
        all attention masks, then freeze them as constant tensors."""
        import sys
        gmfss_repo = None
        for p in sys.path:
            if 'GMFSS_Fortuna' in p:
                gmfss_repo = p
                break

        # Import the mask generation function
        from model.gmflow.transformer import generate_shift_window_attn_mask
        from model.gmflow.utils import split_feature, merge_splits

        # The backbone produces features at 2 scales:
        # Scale 0 (1/8): h/8, w/8  — attn_splits=2
        # Scale 1 (1/4): h/4, w/4  — attn_splits=8
        # For each scale, if there's a shifted window attention, a mask is needed.
        device = torch.device('cpu')

        # Monkey-patch: replace generate_shift_window_attn_mask with a version
        # that returns pre-computed constant tensors
        import model.gmflow.transformer as tf_module
        _orig_gen_mask = tf_module.generate_shift_window_attn_mask

        # Cache of pre-computed masks
        self._cached_masks = {}

        def _precompute_mask(input_resolution, window_size_h, window_size_w,
                             shift_size_h, shift_size_w, device=device):
            key = (tuple(input_resolution), window_size_h, window_size_w,
                   shift_size_h, shift_size_w)
            if key not in self._cached_masks:
                mask = _orig_gen_mask(input_resolution, window_size_h, window_size_w,
                                      shift_size_h, shift_size_w, device=device)
                self._cached_masks[key] = mask.detach()
            return self._cached_masks[key]

        # Patch and do a warmup forward pass to populate the cache
        tf_module.generate_shift_window_attn_mask = _precompute_mask
        with torch.no_grad():
            dummy0 = torch.randn(1, 3, h, w)
            dummy1 = torch.randn(1, 3, h, w)
            self.flownet(dummy0, dummy1)

        # Now register all cached masks as buffers (constant in ONNX)
        for i, (key, mask) in enumerate(self._cached_masks.items()):
            buf_name = f'_attn_mask_{i}'
            self.register_buffer(buf_name, mask)

        # Create a lookup from key -> buffer name
        self._mask_key_to_buf = {}
        for i, key in enumerate(self._cached_masks.keys()):
            self._mask_key_to_buf[key] = f'_attn_mask_{i}'

        # Replace the mask function to return registered buffers
        parent = self
        def _return_cached_mask(input_resolution, window_size_h, window_size_w,
                                shift_size_h, shift_size_w, device=None):
            key = (tuple(input_resolution), window_size_h, window_size_w,
                   shift_size_h, shift_size_w)
            buf_name = parent._mask_key_to_buf.get(key)
            if buf_name:
                return getattr(parent, buf_name)
            # Fallback: shouldn't happen for the traced resolution
            return _orig_gen_mask(input_resolution, window_size_h, window_size_w,
                                   shift_size_h, shift_size_w,
                                   device=device or torch.device('cpu'))

        tf_module.generate_shift_window_attn_mask = _return_cached_mask

        # Monkey-patch torch.roll → ONNX-compatible Slice+Cat decomposition
        # (aten::roll is not in standard ONNX opset)
        import model.gmflow.transformer as tf
        tf.torch.roll = _onnx_compatible_roll

    def forward(self, img0, img1):
        return self.flownet(img0, img1)


class MetricNetExport(nn.Module):
    """Wraps MetricNet with inlined forward_backward_consistency_check
    and backwarp to avoid external state / cached grids."""
    def __init__(self, metricnet):
        super().__init__()
        self.metric_in = metricnet.metric_in
        self.metric_net1 = metricnet.metric_net1
        self.metric_net2 = metricnet.metric_net2
        self.metric_net3 = metricnet.metric_net3
        self.metric_out = metricnet.metric_out

    @staticmethod
    def _backwarp(tenIn, tenflow):
        B, C, H, W = tenIn.shape
        tenHor = torch.linspace(-1.0, 1.0, W, dtype=tenflow.dtype, device=tenflow.device).view(1, 1, 1, -1).expand(B, 1, H, W)
        tenVer = torch.linspace(-1.0, 1.0, H, dtype=tenflow.dtype, device=tenflow.device).view(1, 1, -1, 1).expand(B, 1, H, W)
        grid = torch.cat([tenHor, tenVer], 1)
        flow_norm = torch.cat([
            tenflow[:, 0:1] / ((W - 1.0) / 2.0),
            tenflow[:, 1:2] / ((H - 1.0) / 2.0)
        ], 1)
        grid = (grid + flow_norm).permute(0, 2, 3, 1)
        return F.grid_sample(tenIn, grid, mode='bilinear', padding_mode='zeros', align_corners=True)

    @staticmethod
    def _flow_warp(feature, flow):
        B, C, H, W = feature.shape
        yy, xx = torch.meshgrid(torch.arange(H, dtype=flow.dtype, device=flow.device),
                                torch.arange(W, dtype=flow.dtype, device=flow.device),
                                indexing='ij')
        grid = torch.stack([xx, yy], dim=0).unsqueeze(0).expand(B, -1, -1, -1) + flow
        x_grid = 2 * grid[:, 0] / (W - 1) - 1
        y_grid = 2 * grid[:, 1] / (H - 1) - 1
        grid_norm = torch.stack([x_grid, y_grid], dim=-1)
        return F.grid_sample(feature, grid_norm, mode='bilinear', padding_mode='zeros', align_corners=True)

    @staticmethod
    def _fwd_bwd_check(fwd_flow, bwd_flow, alpha=0.01, beta=0.5):
        flow_mag = torch.norm(fwd_flow, dim=1) + torch.norm(bwd_flow, dim=1)
        warped_bwd = MetricNetExport._flow_warp(bwd_flow, fwd_flow)
        warped_fwd = MetricNetExport._flow_warp(fwd_flow, bwd_flow)
        diff_fwd = torch.norm(fwd_flow + warped_bwd, dim=1)
        diff_bwd = torch.norm(bwd_flow + warped_fwd, dim=1)
        threshold = alpha * flow_mag + beta
        fwd_occ = (diff_fwd > threshold).float()
        bwd_occ = (diff_bwd > threshold).float()
        return fwd_occ, bwd_occ

    def forward(self, img0, img1, flow01, flow10):
        # Manual L1 loss (aten::l1_loss is not ONNX-exportable)
        metric0 = torch.abs(img0 - self._backwarp(img1, flow01)).mean(1, keepdim=True)
        metric1 = torch.abs(img1 - self._backwarp(img0, flow10)).mean(1, keepdim=True)
        fwd_occ, bwd_occ = self._fwd_bwd_check(flow01, flow10)
        H, W = flow01.shape[2], flow01.shape[3]
        flow01_norm = torch.cat([flow01[:, 0:1] / ((W - 1.0) / 2.0), flow01[:, 1:2] / ((H - 1.0) / 2.0)], 1)
        flow10_norm = torch.cat([flow10[:, 0:1] / ((W - 1.0) / 2.0), flow10[:, 1:2] / ((H - 1.0) / 2.0)], 1)
        img = torch.cat((img0, img1), 1)
        metric = torch.cat((-metric0, -metric1), 1)
        flow = torch.cat((flow01_norm, flow10_norm), 1)
        occ = torch.cat((fwd_occ.unsqueeze(1), bwd_occ.unsqueeze(1)), 1)
        feat = self.metric_in(torch.cat((img, metric, flow, occ), 1))
        feat = self.metric_net1(feat) + feat
        feat = self.metric_net2(feat) + feat
        feat = self.metric_net3(feat) + feat
        out = self.metric_out(feat)
        out = torch.tanh(out) * 10
        return out[:, :1], out[:, 1:2]


class IFNetExport(nn.Module):
    """Wraps IFNet for ONNX export with inlined warp (no cached grid)."""
    def __init__(self, ifnet):
        super().__init__()
        self.block0 = ifnet.block0
        self.block1 = ifnet.block1
        self.block2 = ifnet.block2
        self.block3 = ifnet.block3

    @staticmethod
    def _warp(tenInput, tenFlow):
        B, C, H, W = tenInput.shape
        # Crop flow to match input size (IFBlock may produce slightly larger flow
        # when input H/W is not divisible by scale*4)
        tenFlow = tenFlow[:, :, :H, :W]
        tenHor = torch.linspace(-1.0, 1.0, W, dtype=tenFlow.dtype, device=tenFlow.device).view(1, 1, 1, W).expand(B, 1, H, W)
        tenVer = torch.linspace(-1.0, 1.0, H, dtype=tenFlow.dtype, device=tenFlow.device).view(1, 1, H, 1).expand(B, 1, H, W)
        flow_norm = torch.cat([
            tenFlow[:, 0:1] / ((W - 1.0) / 2.0),
            tenFlow[:, 1:2] / ((H - 1.0) / 2.0)
        ], 1)
        g = (torch.cat([tenHor, tenVer], 1) + flow_norm).permute(0, 2, 3, 1)
        return F.grid_sample(tenInput, g, mode='bilinear', padding_mode='border', align_corners=True)

    def forward(self, imgs, timestep):
        """
        imgs: [B, 6, H, W] - concatenated img0 and img1
        timestep: [B, 1, 1, 1] - scalar timestep as tensor
        """
        img0 = imgs[:, :3]
        img1 = imgs[:, 3:]
        H, W = img0.shape[2], img0.shape[3]
        timestep_map = timestep.expand(-1, 1, H, W)
        flow = None
        scale_list = [8, 4, 2, 1]
        blocks = [self.block0, self.block1, self.block2, self.block3]
        for i in range(4):
            if flow is None:
                flow, mask = blocks[i](
                    torch.cat((img0, img1, timestep_map), 1), None, scale=scale_list[i]
                )
                flow = flow[:, :, :H, :W]
                mask = mask[:, :, :H, :W]
            else:
                f0, m0 = blocks[i](
                    torch.cat((warped_img0, warped_img1, timestep_map, mask), 1),
                    flow, scale=scale_list[i]
                )
                flow = flow + f0[:, :, :H, :W]
                mask = mask + m0[:, :, :H, :W]
            # Crop flow/mask to input spatial size (IFBlock may produce
            # slightly larger output when H/W isn't divisible by scale*4)
            warped_img0 = self._warp(img0, flow[:, :2])
            warped_img1 = self._warp(img1, flow[:, 2:4])
        mask = torch.sigmoid(mask[:, :, :H, :W])
        merged = warped_img0 * mask + warped_img1 * (1 - mask)
        return merged


class FusionNetExport(nn.Module):
    """Thin wrapper for clean ONNX export of GridNet (FusionNet_u)."""
    def __init__(self, fusionnet):
        super().__init__()
        self.fusionnet = fusionnet

    def forward(self, x, x1, x2, x3):
        return self.fusionnet(x, x1, x2, x3)


def export_model(model, inputs, input_names, output_names, path, opset,
                 dynamic_axes=None, use_dynamo=False):
    print(f"  Exporting to {path}...")
    if use_dynamo:
        # Use TorchDynamo-based exporter for complex models (e.g. GMFlow
        # with dynamic Swin Transformer slicing that trace mode can't handle)
        export_output = torch.onnx.export(
            model,
            inputs,
            dynamo=True,
        )
        export_output.save(path)
    else:
        torch.onnx.export(
            model,
            inputs,
            path,
            input_names=input_names,
            output_names=output_names,
            dynamic_axes=dynamic_axes,
            opset_version=opset,
            do_constant_folding=True,
        )
    size_mb = os.path.getsize(path) / (1024 * 1024)
    print(f"  Done: {path} ({size_mb:.1f} MB)")


def export_model_jit(model, inputs, input_names, output_names, path, opset,
                     dynamic_axes=None):
    """Export via torch.jit.trace → ONNX. More robust for models with
    dynamic control flow that has fixed shapes at trace time."""
    print(f"  Exporting via JIT trace to {path}...")
    traced = torch.jit.trace(model, inputs, strict=False)
    torch.onnx.export(
        traced,
        inputs,
        path,
        input_names=input_names,
        output_names=output_names,
        dynamic_axes=dynamic_axes,
        opset_version=opset,
        do_constant_folding=True,
        operator_export_type=torch.onnx.OperatorExportTypes.ONNX_ATEN_FALLBACK,
    )
    size_mb = os.path.getsize(path) / (1024 * 1024)
    print(f"  Done: {path} ({size_mb:.1f} MB)")


def main():
    args = parse_args()

    # Add GMFSS repo to path so model imports work
    sys.path.insert(0, args.gmfss_repo)

    from model.FeatureNet import FeatureNet
    from model.gmflow.gmflow import GMFlow
    from model.MetricNet import MetricNet as MetricNetOrig
    from model.IFNet_HDv3 import IFNet
    from model.FusionNet_u import GridNet

    device = torch.device("cpu")
    os.makedirs(args.output_dir, exist_ok=True)

    H, W = args.height, args.width
    # All processing happens at half resolution
    Hh, Wh = H // 2, W // 2

    # --- Load weights ---
    print("Loading model weights...")
    feat_ext = FeatureNet()
    feat_ext.load_state_dict(torch.load(os.path.join(args.model_dir, "feat.pkl"), map_location=device))
    feat_ext.eval()

    flownet = GMFlow()
    flownet.load_state_dict(torch.load(os.path.join(args.model_dir, "flownet.pkl"), map_location=device))
    flownet.eval()

    metricnet_orig = MetricNetOrig()
    metricnet_orig.load_state_dict(torch.load(os.path.join(args.model_dir, "metric.pkl"), map_location=device))
    metricnet_orig.eval()

    ifnet = IFNet()
    ifnet.load_state_dict(torch.load(os.path.join(args.model_dir, "rife.pkl"), map_location=device))
    ifnet.eval()

    fusionnet = GridNet()
    fusionnet.load_state_dict(torch.load(os.path.join(args.model_dir, "fusionnet.pkl"), map_location=device))
    fusionnet.eval()

    opset = args.opset

    # Common dynamic axes for variable resolution
    dyn_hw = {0: "batch", 2: "height", 3: "width"}
    dyn_hw_half = {0: "batch", 2: "height_half", 3: "width_half"}
    dyn_hw_quarter = {0: "batch", 2: "height_quarter", 3: "width_quarter"}
    dyn_hw_eighth = {0: "batch", 2: "height_eighth", 3: "width_eighth"}

    # --- 1. FeatureNet ---
    print("\n[1/5] FeatureNet")
    feat_model = FeatureNetExport(feat_ext).eval()
    feat_input = torch.randn(1, 3, H, W)
    export_model(
        feat_model, (feat_input,),
        input_names=["input"],
        output_names=["feat1", "feat2", "feat3"],
        path=os.path.join(args.output_dir, "feat.onnx"),
        opset=opset,
        dynamic_axes={
            "input": dyn_hw,
            "feat1": dyn_hw_half,
            "feat2": dyn_hw_quarter,
            "feat3": dyn_hw_eighth,
        },
    )

    # --- 2. GMFlow ---
    # GMFlow's Swin Transformer requires input H,W to be multiples of 64
    # (backbone 1/8 downsample × attn_splits=8). Pad trace input accordingly.
    # At runtime, the C code must pad input and crop the output flow.
    print("\n[2/5] GMFlow (flownet)")
    flow_pad_h = (64 - Hh % 64) % 64
    flow_pad_w = (64 - Wh % 64) % 64
    flow_Hh = Hh + flow_pad_h
    flow_Wh = Wh + flow_pad_w
    print(f"  GMFlow trace size: {Hh}x{Wh} -> padded {flow_Hh}x{flow_Wh}")
    flow_model = GMFlowExport(flownet, flow_Hh, flow_Wh).eval()
    flow_img0 = torch.randn(1, 3, flow_Hh, flow_Wh)
    flow_img1 = torch.randn(1, 3, flow_Hh, flow_Wh)
    export_model(
        flow_model, (flow_img0, flow_img1),
        input_names=["img0", "img1"],
        output_names=["flow"],
        path=os.path.join(args.output_dir, "flownet.onnx"),
        opset=opset,
        dynamic_axes={
            "img0": dyn_hw_half,
            "img1": dyn_hw_half,
            "flow": dyn_hw_half,
        },
    )

    # --- 3. MetricNet ---
    print("\n[3/5] MetricNet")
    metric_model = MetricNetExport(metricnet_orig).eval()
    m_img0 = torch.randn(1, 3, Hh, Wh)
    m_img1 = torch.randn(1, 3, Hh, Wh)
    m_flow01 = torch.randn(1, 2, Hh, Wh)
    m_flow10 = torch.randn(1, 2, Hh, Wh)
    export_model(
        metric_model, (m_img0, m_img1, m_flow01, m_flow10),
        input_names=["img0", "img1", "flow01", "flow10"],
        output_names=["metric0", "metric1"],
        path=os.path.join(args.output_dir, "metric.onnx"),
        opset=opset,
        dynamic_axes={
            "img0": dyn_hw_half, "img1": dyn_hw_half,
            "flow01": dyn_hw_half, "flow10": dyn_hw_half,
            "metric0": dyn_hw_half, "metric1": dyn_hw_half,
        },
    )

    # --- 4. IFNet (RIFE) ---
    # Clear any global warp grid caches left by GMFlow warmup (different resolution)
    import model.warplayer as _wp
    _wp.backwarp_tenGrid.clear()
    import model.MetricNet as _mn
    _mn.backwarp_tenGrid.clear()

    # Reload IFNet fresh to avoid any state pollution from GMFlow warmup
    ifnet = IFNet()
    ifnet.load_state_dict(torch.load(os.path.join(args.model_dir, "rife.pkl"), map_location=device))
    ifnet.eval()

    print("\n[4/5] IFNet (rife)")
    rife_model = IFNetExport(ifnet).eval()
    rife_imgs = torch.randn(1, 6, Hh, Wh)
    rife_ts = torch.tensor([[[[0.5]]]])  # [1,1,1,1]
    export_model(
        rife_model, (rife_imgs, rife_ts),
        input_names=["imgs", "timestep"],
        output_names=["output"],
        path=os.path.join(args.output_dir, "rife.onnx"),
        opset=opset,
        dynamic_axes={
            "imgs": dyn_hw_half,
            "output": dyn_hw_half,
        },
    )

    # --- 5. FusionNet (GridNet) ---
    print("\n[5/5] FusionNet (GridNet)")
    fusion_model = FusionNetExport(fusionnet).eval()
    f_x = torch.randn(1, 9, Hh, Wh)          # I1t + rife + I2t
    f_x1 = torch.randn(1, 128, Hh, Wh)       # cat(feat1t1, feat2t1) = 64+64
    f_x2 = torch.randn(1, 256, Hh // 2, Wh // 2)  # cat(feat1t2, feat2t2) = 128+128
    f_x3 = torch.randn(1, 384, Hh // 4, Wh // 4)  # cat(feat1t3, feat2t3) = 192+192
    export_model(
        fusion_model, (f_x, f_x1, f_x2, f_x3),
        input_names=["x", "x1", "x2", "x3"],
        output_names=["output"],
        path=os.path.join(args.output_dir, "fusionnet.onnx"),
        opset=opset,
        dynamic_axes={
            "x": dyn_hw_half,
            "x1": dyn_hw_half,
            "x2": dyn_hw_quarter,
            "x3": dyn_hw_eighth,
            "output": dyn_hw_half,
        },
    )

    print(f"\nAll 5 ONNX models exported to: {args.output_dir}")
    print("Files: feat.onnx, flownet.onnx, metric.onnx, rife.onnx, fusionnet.onnx")


if __name__ == "__main__":
    main()
