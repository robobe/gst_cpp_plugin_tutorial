#!/usr/bin/env python3
"""Export the official LightTrack-Mobile checkpoint as fixed-shape ONNX graphs."""
import argparse
import collections.abc
import subprocess
import sys
import types
from pathlib import Path

import onnx
import torch

# LightTrack's pinned source imports this module removed by modern PyTorch.
legacy_torch_six = types.ModuleType("torch._six")
legacy_torch_six.container_abcs = collections.abc
sys.modules.setdefault("torch._six", legacy_torch_six)

UPSTREAM_COMMIT = "39c426f48ee674795cdf0e00301a0b4ad0785d2a"
ARCHITECTURE = "back_04502514044521042540+cls_211000022+reg_100000111_ops_32"


class Template(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.features = model.features

    def forward(self, image):
        return self.features(image)


class Search(Template):
    pass


class Head(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.neck = model.neck
        self.feature_fusor = model.feature_fusor
        self.head = model.head

    def forward(self, template_features, search_features):
        template_features, search_features = self.neck(template_features, search_features)
        output = self.head(self.feature_fusor(template_features, search_features))
        return output["cls"], output["reg"]


def export(module, inputs, path, input_names, output_names):
    module.eval()
    with torch.no_grad():
        torch.onnx.export(module, inputs, path, input_names=input_names, output_names=output_names,
                          opset_version=13, do_constant_folding=True)
    onnx.checker.check_model(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lighttrack-root", type=Path, required=True,
                        help="checkout of github.com/researchmm/LightTrack at the pinned commit")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).parent / "onnx")
    args = parser.parse_args()
    root = args.lighttrack_root.resolve()
    commit = subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"], text=True).strip()
    if commit != UPSTREAM_COMMIT:
        raise SystemExit(f"LightTrack checkout must be {UPSTREAM_COMMIT}, got {commit}")
    sys.path.insert(0, str(root))
    from lib.models.models import LightTrackM_Subnet

    model = LightTrackM_Subnet(ARCHITECTURE)
    checkpoint = torch.load(root / "snapshot/LightTrackM/LightTrackM.pth", map_location="cpu",
                            weights_only=False)
    model.load_state_dict(checkpoint["state_dict"])
    args.output_dir.mkdir(parents=True, exist_ok=True)
    export(Template(model), torch.zeros(1, 3, 128, 128), args.output_dir / "lighttrack_template.onnx",
           ["image"], ["features"])
    export(Search(model), torch.zeros(1, 3, 256, 256), args.output_dir / "lighttrack_search.onnx",
           ["image"], ["features"])
    export(Head(model), (torch.zeros(1, 96, 8, 8), torch.zeros(1, 96, 16, 16)),
           args.output_dir / "lighttrack_head.onnx", ["template_features", "search_features"], ["cls", "reg"])


if __name__ == "__main__":
    main()
