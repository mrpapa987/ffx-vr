"""vda_model.py - the depth model behind the converter's small model interface: Video-Depth-Anything-Small on CUDA.

SPDX-License-Identifier: MIT (this file; part of the FFX VR repository). Video-Depth-Anything itself (code Apache-2.0,
github.com/DepthAnything/Video-Depth-Anything) is NOT in this repository: tools/convert-fmv-depth.ps1 downloads it at the
pinned commit below, and its Small weights (Apache-2.0) from Hugging Face. Only the Small model is supported: the Base and
Large weights are CC-BY-NC-4.0.

The interface: model(frames) with 32 RGB uint8 frames (H x W x 3, already at the model's input size) returns float32
[32, H, W] relative disparity at the model's own resolution after prepare(..., full=True) (the converter's edge path,
flip averaging and shrink then run in numpy: convert.DepthAtOutput), or [32, out_h, out_w] area-shrunk on the GPU
otherwise. torch is imported only when a VdaSmall is made, so the rest of the converter (and its tests) runs without
it; everything after the model call is numpy.
"""
import hashlib
import os
import sys
import types

import numpy as np

VDA_COMMIT = '4f5ae23172ba60fd7bc11ef671cca678842c7072'   # 7 Oct 2025, "Update README.md (#96)"
WEIGHTS_NAME = 'video_depth_anything_vits.pth'
WEIGHTS_URL = 'https://huggingface.co/depth-anything/Video-Depth-Anything-Small/resolve/main/' + WEIGHTS_NAME
VITS_CONFIG = {'encoder': 'vits', 'features': 64, 'out_channels': [48, 96, 192, 384]}
MEAN = (0.485, 0.456, 0.406)
STD = (0.229, 0.224, 0.225)

# SHA-256 of the files we import from the VDA checkout, at VDA_COMMIT (computed from a git clone of that commit).
VDA_FILES = {
    'LICENSE': '43070e2d4e532684de521b885f385d0841030efa2b1a20bafb76133a5e1379c1',
    'utils/util.py': 'f02cf85f5a9543ce0f832a4fde1492076026650c8eb25c6a8cc4315bf67a8ed2',
    'video_depth_anything/dinov2.py': 'd22df8d0da44c8525ebed898fd9c33b3acfcda919a84d577cc2a08bfd3d50ac1',
    'video_depth_anything/dinov2_layers/__init__.py': 'e3c28c7b775165b6844000eb69f44d104d6c3803ac7da5d59f6ac3ecfb46d9d8',
    'video_depth_anything/dinov2_layers/attention.py': '0d2410ce8b8b4499a6b0cdab939ae2bceab8971efd8660d999b0f3088f28cbf7',
    'video_depth_anything/dinov2_layers/block.py': '093266ea08084c7cce7b8ddd779ecfaaa13055735faa8a7d68b5924f385b395b',
    'video_depth_anything/dinov2_layers/drop_path.py': '949100aac4d9c19c9d65319dbd362886c5d46a72d7586ba4fd500e9f1d51570c',
    'video_depth_anything/dinov2_layers/layer_scale.py': '356817b5d1390737128cbcc03a5333fd3060b73beae478772282ccb851d0ef85',
    'video_depth_anything/dinov2_layers/mlp.py': 'c18c5b3f021408597a473bb710070ec3fb3b39f5a2e42e88ae83ced425a608b5',
    'video_depth_anything/dinov2_layers/patch_embed.py': 'b3f82e19926c9cfb63f10136aca5e8a723bde1ad62dae1d04f90d405f9439baf',
    'video_depth_anything/dinov2_layers/swiglu_ffn.py': 'df81d9901240214edc1831ce1b77558dcd6227b29a2f000fc5f546df55d2d09a',
    'video_depth_anything/dpt.py': '4751fac16206892889d74fd7f564f275e0efa3a903172c9350a1b55ed850c7d8',
    'video_depth_anything/dpt_temporal.py': 'b80c448f4fe66d0ca7092be887c0ea3925a4ce7c03cffa0083480e7e2f55805d',
    'video_depth_anything/motion_module/attention.py': '15e4049641f04f73027ea520ada3ba2c81196cea1339f0d6a6063d944df61c8b',
    'video_depth_anything/motion_module/motion_module.py': '73d6a9cee40681532ae2603dd426405e840dc5c592a313005fa367f6fe4c69e8',
    'video_depth_anything/util/blocks.py': '8a9fde84bddbfa2498b5f5ef06895ddc941c1fc12c5919e9891fad481997b7dd',
    'video_depth_anything/util/transform.py': 'a97df1dbb687457a40c387fdc3482edee7ddc5c142c7d2735ca6235a8ae2885f',
    'video_depth_anything/video_depth.py': '208e882d60a41434b3a0d9935a7025b01cce0756c17b3d889c6db0c4c945eb29',
}


def sha256_file(path, bufsize=1 << 20):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


# Python files at VDA_COMMIT that we do not import (so not hashed) but that may be there. Any other .py file in the two
# imported package folders (an added __init__.py above all: it would run on import) fails the check.
VDA_OTHER_PY = {'utils/dc_utils.py', 'video_depth_anything/video_depth_stream.py'}
# Every file in those two folders at VDA_COMMIT: `git ls-tree -r --name-only 4f5ae23 -- video_depth_anything utils`
# on a clone of the official repository (3 Nov 2026). All are .py. VDA_FILES (minus LICENSE) + VDA_OTHER_PY must be
# exactly this list (tests/test_vda_files.py), and `python vda_model.py verify <that clone>` passes.
VDA_TREE_AT_COMMIT = (
    'utils/dc_utils.py', 'utils/util.py',
    'video_depth_anything/dinov2.py',
    'video_depth_anything/dinov2_layers/__init__.py', 'video_depth_anything/dinov2_layers/attention.py',
    'video_depth_anything/dinov2_layers/block.py', 'video_depth_anything/dinov2_layers/drop_path.py',
    'video_depth_anything/dinov2_layers/layer_scale.py', 'video_depth_anything/dinov2_layers/mlp.py',
    'video_depth_anything/dinov2_layers/patch_embed.py', 'video_depth_anything/dinov2_layers/swiglu_ffn.py',
    'video_depth_anything/dpt.py', 'video_depth_anything/dpt_temporal.py',
    'video_depth_anything/motion_module/attention.py', 'video_depth_anything/motion_module/motion_module.py',
    'video_depth_anything/util/blocks.py', 'video_depth_anything/util/transform.py',
    'video_depth_anything/video_depth.py', 'video_depth_anything/video_depth_stream.py',
)
VDA_PACKAGES = ('video_depth_anything', 'utils')


def verify_vda(vda_dir):
    """[] if the VDA checkout has exactly the pinned files we import, and no unexpected .py file in the imported
    packages, else a list of problems."""
    bad = []
    for rel, want in VDA_FILES.items():
        p = os.path.join(vda_dir, *rel.split('/'))
        if not os.path.isfile(p):
            bad.append(f'{rel}: missing')
        elif sha256_file(p) != want:
            bad.append(f'{rel}: differs from commit {VDA_COMMIT[:7]}')
    known = set(VDA_FILES) | VDA_OTHER_PY
    for pkg in VDA_PACKAGES:
        for d, _dirs, files in os.walk(os.path.join(vda_dir, pkg)):
            for f in files:
                if f.endswith(('.py', '.pyc', '.pyd', '.so')) and '__pycache__' not in d:
                    rel = os.path.relpath(os.path.join(d, f), vda_dir).replace(os.sep, '/')
                    if rel not in known:
                        bad.append(f'{rel}: not in commit {VDA_COMMIT[:7]}')
    return bad


class OutOfVram(RuntimeError):
    pass


def is_oom(e):
    n = type(e).__name__
    return n == 'OutOfMemoryError' or isinstance(e, OutOfVram) or 'CUDA out of memory' in str(e)


class VdaSmall:
    """Video-Depth-Anything-Small, fp16 autocast on CUDA (fp32 with fp32=True), eval mode, no gradients.

    attention='sdpa' (default) replaces the DINOv2 encoder's plain attention (q @ k^T, softmax: ~2.4k tokens per frame
    at 518x924, 32 frames a window: gigabytes of score matrices) by torch's scaled_dot_product_attention, the same
    maths with a memory-efficient kernel. xformers is not needed (and not installed)."""

    model_name = 'vda-small'

    def __init__(self, vda_dir, weights, device='cuda', fp32=False, attention='sdpa'):
        import torch
        import torch.nn.functional as F
        self.torch, self.F = torch, F
        if device == 'cuda' and not torch.cuda.is_available():
            raise RuntimeError('torch sees no CUDA GPU (torch %s, CUDA %s): update the NVIDIA driver, or reinstall the '
                               'converter venv (delete it; convert-fmv-depth.ps1 makes it again)'
                               % (torch.__version__, torch.version.cuda))
        vda_dir = os.path.abspath(vda_dir)
        if 'utils' not in sys.modules:          # VDA's utils/ has no __init__.py: make sure no other 'utils' wins
            m = types.ModuleType('utils')
            m.__path__ = [os.path.join(vda_dir, 'utils')]
            sys.modules['utils'] = m
        # The verified checkout first on sys.path while its package is imported (nothing else named
        # video_depth_anything can win), then off it again (its top-level files cannot shadow our own modules later).
        if vda_dir in sys.path:
            sys.path.remove(vda_dir)
        sys.path.insert(0, vda_dir)
        try:
            from video_depth_anything.video_depth import VideoDepthAnything
        finally:
            if vda_dir in sys.path:
                sys.path.remove(vda_dir)
        if attention == 'sdpa':
            self._patch_attention()
        net = VideoDepthAnything(**VITS_CONFIG)
        sd = torch.load(weights, map_location='cpu', weights_only=True)
        net.load_state_dict(sd, strict=True)
        self.net = net.to(device).eval()
        self.device = device
        self.fp32 = fp32
        self.attention = attention
        self.mean = torch.tensor(MEAN, device=device).view(1, 3, 1, 1)
        self.std = torch.tensor(STD, device=device).view(1, 3, 1, 1)
        self.out_w = self.out_h = None

    @staticmethod
    def _patch_attention():
        # forward() below is adapted from DINOv2's Attention.forward (video_depth_anything/dinov2_layers/attention.py
        # at VDA commit 4f5ae23; Copyright (c) Meta Platforms, Inc. and affiliates, Apache-2.0): the same projections,
        # with the explicit softmax(q @ k^T) @ v replaced by torch's scaled_dot_product_attention.
        import torch.nn.functional as F
        from video_depth_anything.dinov2_layers import attention as A

        def forward(self, x):
            B, N, C = x.shape
            qkv = self.qkv(x).reshape(B, N, 3, self.num_heads, C // self.num_heads).permute(2, 0, 3, 1, 4)
            q, k, v = qkv[0], qkv[1], qkv[2]
            x = F.scaled_dot_product_attention(q, k, v)       # default scale 1/sqrt(head_dim) == self.scale
            x = x.transpose(1, 2).reshape(B, N, C)
            return self.proj_drop(self.proj(x))
        A.Attention.forward = forward       # MemEffAttention falls back to Attention.forward without xformers

    def prepare(self, in_w, in_h, out_w, out_h, full=False):
        """full: return the depth at the model's own (input) resolution, for the converter's edge path (numpy, on the
        CPU); otherwise area-shrunk to out_w x out_h on the GPU."""
        self.in_w, self.in_h, self.out_w, self.out_h, self.full = in_w, in_h, out_w, out_h, full

    def __call__(self, frames):
        torch, F = self.torch, self.F
        x = torch.from_numpy(np.stack(frames)).to(self.device)           # [T, H, W, 3] uint8
        x = x.permute(0, 3, 1, 2).float().div_(255.0)
        x = ((x - self.mean) / self.std).unsqueeze(0)                     # [1, T, 3, H, W]
        try:
            with torch.inference_mode():
                with torch.autocast(device_type='cuda', dtype=torch.float16, enabled=not self.fp32):
                    d = self.net(x)                                       # [1, T, H, W], relu'd
                d = d.float()[0]                                          # [T, H, W]
                if not self.full:
                    d = F.interpolate(d.unsqueeze(1), size=(self.out_h, self.out_w), mode='area')[:, 0]
                out = d.cpu().numpy()
        except RuntimeError as e:
            if is_oom(e):
                raise OutOfVram(str(e)) from None
            raise
        return out

    def peak_vram(self):
        return int(self.torch.cuda.max_memory_allocated())

    def reset_peak(self):
        self.torch.cuda.reset_peak_memory_stats()

    def describe(self):
        t = self.torch
        name = t.cuda.get_device_name(0) if self.device == 'cuda' else self.device
        total = t.cuda.get_device_properties(0).total_memory if self.device == 'cuda' else 0
        return (f'torch {t.__version__} (CUDA {t.version.cuda}), {name} {total / 2**30:.1f} GB, '
                f'{"fp32" if self.fp32 else "fp16 autocast"}, attention {self.attention}')


class FakeModel:
    """Test / pipeline-check model: no torch, no GPU. 'Depth' = a blurred luma ramp, deterministic per frame
    (frames that are numpy images) - it exercises the pipeline, it is not depth."""

    model_name = 'fake'

    def prepare(self, in_w, in_h, out_w, out_h, full=False):
        self.out_w, self.out_h, self.full = out_w, out_h, full

    def __call__(self, frames):
        import depthproc
        if self.full:
            h, w = frames[0].shape[:2]
            ramp = np.linspace(0.5, 1.5, h, dtype=np.float32)[:, None]
            return np.stack([depthproc.luma(f) / np.float32(255.0) * ramp + np.float32(0.1) for f in frames])
        out = np.empty((len(frames), self.out_h, self.out_w), np.float32)
        ramp = np.linspace(0.5, 1.5, self.out_h, dtype=np.float32)[:, None]
        for i, f in enumerate(frames):
            y = depthproc.luma(f, 2)
            out[i] = depthproc.area_resize(y, self.out_w, self.out_h) / 255.0 * ramp + 0.1
        return out

    def peak_vram(self):
        return 0

    def reset_peak(self):
        pass

    def describe(self):
        return 'fake model (pipeline check only: the output is not depth)'


def main(argv):
    """python vda_model.py verify <VDA checkout>   (used by convert-fmv-depth.ps1 after the download)"""
    if len(argv) == 3 and argv[1] == 'verify':
        bad = verify_vda(argv[2])
        for b in bad:
            print('  ' + b)
        print(f'Video-Depth-Anything at {argv[2]}: ' + ('NOT the pinned commit' if bad else f'commit {VDA_COMMIT[:7]}, ok'))
        return 1 if bad else 0
    print(main.__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
