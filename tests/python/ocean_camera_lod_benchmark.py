# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import os
import sys

sys.path.append(os.path.dirname(os.path.realpath(__file__)))

from modules import ocean_camera_lod_metrics as ocean_metrics


def create_argparser():
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark Ocean Camera LOD against the dense reference, "
            "writing JSON summaries and render-difference artifacts."
        )
    )
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--scenarios", default="all")
    parser.add_argument("--devices", default="CPU")
    parser.add_argument("--mode", default="report", choices={"report", "strict"})
    parser.add_argument("--samples", default=4, type=int)
    parser.add_argument("--resolution", default=64, type=int)
    parser.add_argument("--repeat-eval", default=3, type=int)
    parser.add_argument("--repeat-render", default=3, type=int)
    parser.add_argument("--keep-intermediates", action="store_true")
    return parser


def main():
    parser = create_argparser()
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = parser.parse_args(argv)

    scenario_names = ocean_metrics.resolve_scenarios(args.scenarios)
    device_names = ocean_metrics.resolve_devices(args.devices)

    summary = ocean_metrics.run_benchmark_suite(
        outdir=args.outdir,
        scenario_names=scenario_names,
        device_names=device_names,
        mode=args.mode,
        samples=args.samples,
        resolution=args.resolution,
        repeat_eval=args.repeat_eval,
        repeat_render=args.repeat_render,
        keep_intermediates=args.keep_intermediates,
    )

    raise SystemExit(ocean_metrics.benchmark_exit_code(summary))


if __name__ == "__main__":
    main()
