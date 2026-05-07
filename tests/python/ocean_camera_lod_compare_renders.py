# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import os
import sys
from dataclasses import asdict
from pathlib import Path

sys.path.append(os.path.dirname(os.path.realpath(__file__)))

from modules import ocean_camera_lod_metrics as ocean_metrics


def create_argparser():
    parser = argparse.ArgumentParser(
        description="Compare two ocean benchmark render images and write error metrics."
    )
    parser.add_argument("--reference", required=True, help="Dense baseline render path.")
    parser.add_argument("--candidate", required=True, help="Dense optimized or LOD optimized render path.")
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--label", default="candidate")
    parser.add_argument(
        "--no-diff-images",
        action="store_true",
        help="Only write the JSON metrics report; skip PNG diff visualizations.",
    )
    return parser


def main():
    parser = create_argparser()
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = parser.parse_args(argv)

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    ref_width, ref_height, ref_rgb = ocean_metrics.load_rgb_pixels(args.reference)
    cand_width, cand_height, cand_rgb = ocean_metrics.load_rgb_pixels(args.candidate)
    if (ref_width, ref_height) != (cand_width, cand_height):
        raise RuntimeError(
            f"Render sizes differ: reference {(ref_width, ref_height)} candidate {(cand_width, cand_height)}"
        )

    report, rgba_abs, rgba_gradient = ocean_metrics.compute_render_report(
        cand_rgb, ref_rgb, ref_width, ref_height
    )
    diff_abs = outdir / f"{args.label}_diff_abs.png"
    diff_gradient = outdir / f"{args.label}_diff_gradient.png"
    if not args.no_diff_images:
        ocean_metrics.save_rgba_image(str(diff_abs), ref_width, ref_height, rgba_abs)
        ocean_metrics.save_rgba_image(str(diff_gradient), ref_width, ref_height, rgba_gradient)

    result = {
        "reference": args.reference,
        "candidate": args.candidate,
        "label": args.label,
        "width": ref_width,
        "height": ref_height,
        "report": asdict(report),
        "diff_abs": None if args.no_diff_images else str(diff_abs),
        "diff_gradient": None if args.no_diff_images else str(diff_gradient),
    }
    ocean_metrics.write_json(outdir / f"{args.label}_render_compare.json", result)


if __name__ == "__main__":
    main()
