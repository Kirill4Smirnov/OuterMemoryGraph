#!/usr/bin/env python3
"""Measure PageRank iteration throughput for a fixed list of thread counts."""

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


ITERATION_PATTERN = re.compile(
    r"pagerank: iteration=(?P<iteration>\d+).*?seconds=(?P<seconds>[0-9.eE+-]+)"
    r".*?edges_per_second=(?P<throughput>[0-9.eE+-]+)"
)


def one_run(
    args: argparse.Namespace, threads: int, repetition: int
) -> dict[str, object]:
    output = args.output_directory / (
        f"pagerank-benchmark-{threads}-threads-{repetition}.csv"
    )
    if output.exists():
        raise FileExistsError(output)

    command = [
        str(args.executable),
        "pagerank",
        "--graph-dir",
        str(args.graph_directory),
        "--output",
        str(output),
        "--memory-mb",
        str(args.memory_mb),
        "--threads",
        str(threads),
        "--damping",
        str(args.damping),
        "--iterations",
        str(args.iterations),
    ]
    started = time.perf_counter()
    process = subprocess.run(command, text=True, capture_output=True, check=False)
    wall_seconds = time.perf_counter() - started
    try:
        if process.returncode != 0:
            raise RuntimeError(
                f"command failed with {process.returncode}\n{process.stdout}\n{process.stderr}"
            )
        iterations = [
            {
                "iteration": int(match.group("iteration")),
                "seconds": float(match.group("seconds")),
                "edges_per_second": float(match.group("throughput")),
            }
            for match in ITERATION_PATTERN.finditer(process.stderr)
        ]
        if len(iterations) != args.iterations:
            raise RuntimeError("could not parse every iteration from stderr")
        return {
            "threads": threads,
            "repetition": repetition,
            "wall_seconds_including_csv": wall_seconds,
            "iterations": iterations,
        }
    finally:
        output.unlink(missing_ok=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, default=Path("build-release/omg"))
    parser.add_argument("--graph-dir", dest="graph_directory", type=Path, required=True)
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8, 16])
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--memory-mb", type=int, default=128)
    parser.add_argument("--damping", type=float, default=0.85)
    parser.add_argument(
        "--output-directory", type=Path, default=Path(tempfile.gettempdir())
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    args.executable = args.executable.resolve()
    args.graph_directory = args.graph_directory.resolve()
    args.output_directory.mkdir(parents=True, exist_ok=True)

    runs = [
        one_run(args, threads, repetition)
        for threads in args.threads
        for repetition in range(1, args.repetitions + 1)
    ]
    summaries = []
    for threads in args.threads:
        selected = [run for run in runs if run["threads"] == threads]
        iteration_times = [
            iteration["seconds"] for run in selected for iteration in run["iterations"]
        ]
        throughputs = [
            iteration["edges_per_second"]
            for run in selected
            for iteration in run["iterations"]
        ]
        summaries.append(
            {
                "threads": threads,
                "median_iteration_seconds": statistics.median(iteration_times),
                "median_edges_per_second": statistics.median(throughputs),
                "median_wall_seconds_including_csv": statistics.median(
                    run["wall_seconds_including_csv"] for run in selected
                ),
            }
        )

    print(json.dumps({"runs": runs, "summary": summaries}, indent=2))


if __name__ == "__main__":
    main()
