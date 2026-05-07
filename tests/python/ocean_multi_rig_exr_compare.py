# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path
from typing import Any


def create_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare two OpenEXR images with oiiotool and write parsed metrics as JSON."
    )
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    parser.add_argument("--oiiotool", default="oiiotool")
    return parser


def run_command(command: list[str]) -> tuple[int, str]:
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return proc.returncode, proc.stdout


def parse_info(text: str) -> dict[str, Any]:
    match = re.search(
        r":\s+(\d+)\s+x\s+(\d+),\s+(\d+)\s+channel,\s+([^\s]+)\s+([^\n]+)", text
    )
    if match is None:
        return {}
    return {
        "width": int(match.group(1)),
        "height": int(match.group(2)),
        "channels": int(match.group(3)),
        "data_type": match.group(4),
        "format": match.group(5).strip(),
    }


def parse_diff(text: str) -> dict[str, Any]:
    result: dict[str, Any] = {}
    patterns = {
        "mean_error": r"Mean error = ([^\s]+)",
        "rms_error": r"RMS error = ([^\s]+)",
        "peak_snr": r"Peak SNR = ([^\s]+)",
        "max_error": r"Max error\s+= ([^\s]+)",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if match is not None:
            result[key] = float(match.group(1))

    max_match = re.search(r"Max error\s+= [^\n]* @ \(([^)]+)\)", text)
    if max_match is not None:
        result["max_error_location"] = max_match.group(1)

    threshold_matches = re.findall(r"(\d+) pixels \(([0-9.]+)%\) over ([^\s]+)", text)
    if threshold_matches:
        count, percent, threshold = threshold_matches[0]
        result["pixels_over_threshold"] = int(count)
        result["percent_over_threshold"] = float(percent)
        result["threshold"] = float(threshold)

    result["passed"] = "FAILURE" not in text
    return result


def main() -> None:
    args = create_argparser().parse_args()
    info_code, info_text = run_command([args.oiiotool, "--info", str(args.reference)])
    diff_code, diff_text = run_command(
        [args.oiiotool, str(args.reference), str(args.candidate), "--diff"]
    )
    result = {
        "reference": str(args.reference),
        "candidate": str(args.candidate),
        "info_returncode": info_code,
        "diff_returncode": diff_code,
        "info_stdout": info_text,
        "diff_stdout": diff_text,
        "image": parse_info(info_text),
        "diff": parse_diff(diff_text),
    }
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
