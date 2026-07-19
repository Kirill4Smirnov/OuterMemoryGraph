#!/usr/bin/env python3
"""Stream-validate PageRank CSV without loading it into memory."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections.abc import Iterator
from pathlib import Path


def output_rows(path: Path) -> Iterator[tuple[int, float]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["vertex", "rank"]:
            raise ValueError(f"{path}: expected header vertex,rank")
        for line_number, row in enumerate(reader, start=2):
            try:
                yield int(row["vertex"]), float(row["rank"])
            except (TypeError, ValueError) as error:
                raise ValueError(f"{path}:{line_number}: invalid row") from error


def reference_rows(path: Path) -> Iterator[tuple[int, float]]:
    with path.open(encoding="utf-8") as stream:
        first_data_line = True
        for line_number, raw_line in enumerate(stream, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [part.strip() for part in line.split(",")]
            if len(fields) == 1:
                fields = line.split()
            if first_data_line and fields[0].lower() == "vertex":
                first_data_line = False
                continue
            first_data_line = False
            if len(fields) != 2:
                raise ValueError(f"{path}:{line_number}: expected vertex and rank")
            try:
                yield int(fields[0]), float(fields[1])
            except ValueError as error:
                raise ValueError(
                    f"{path}:{line_number}: invalid reference row"
                ) from error


class KahanSum:
    def __init__(self) -> None:
        self.value = 0.0
        self.correction = 0.0

    def add(self, value: float) -> None:
        corrected = value - self.correction
        updated = self.value + corrected
        self.correction = (updated - self.value) - corrected
        self.value = updated


def validate(args: argparse.Namespace) -> dict[str, int | float | None]:
    expected = reference_rows(args.expected) if args.expected else None
    expected_iterator = iter(expected) if expected is not None else None
    next_expected = next(expected_iterator, None) if expected_iterator else None

    rank_sum = KahanSum()
    count = 0
    previous_vertex: int | None = None
    minimum_rank = math.inf
    maximum_rank = -math.inf
    maximum_absolute_error = 0.0
    maximum_relative_error = 0.0

    for vertex, rank in output_rows(args.output):
        if previous_vertex is not None and vertex <= previous_vertex:
            raise ValueError("output vertex IDs must be strictly increasing")
        if not math.isfinite(rank) or rank < 0.0:
            raise ValueError(f"vertex {vertex}: rank must be finite and non-negative")
        previous_vertex = vertex
        rank_sum.add(rank)
        minimum_rank = min(minimum_rank, rank)
        maximum_rank = max(maximum_rank, rank)
        count += 1

        if expected_iterator is not None:
            if next_expected is None:
                raise ValueError("output contains more vertices than the reference")
            expected_vertex, expected_rank = next_expected
            if vertex != expected_vertex:
                raise ValueError(
                    f"vertex mismatch: output={vertex}, reference={expected_vertex}"
                )
            absolute_error = abs(rank - expected_rank)
            relative_error = (
                absolute_error / abs(expected_rank)
                if expected_rank != 0.0
                else (0.0 if rank == 0.0 else math.inf)
            )
            maximum_absolute_error = max(maximum_absolute_error, absolute_error)
            maximum_relative_error = max(maximum_relative_error, relative_error)
            next_expected = next(expected_iterator, None)

    if count == 0:
        raise ValueError("output contains no ranks")
    if abs(rank_sum.value - 1.0) > args.sum_tolerance:
        raise ValueError(
            f"rank sum {rank_sum.value:.17g} exceeds tolerance {args.sum_tolerance}"
        )
    if expected_iterator is not None:
        if next_expected is not None:
            raise ValueError("reference contains more vertices than the output")
        if maximum_relative_error > args.relative_tolerance:
            raise ValueError(
                f"maximum relative error {maximum_relative_error} exceeds "
                f"{args.relative_tolerance}"
            )

    return {
        "vertices": count,
        "rank_sum": rank_sum.value,
        "minimum_rank": minimum_rank,
        "maximum_rank": maximum_rank,
        "maximum_absolute_error": maximum_absolute_error if args.expected else None,
        "maximum_relative_error": maximum_relative_error if args.expected else None,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="PageRank CSV produced by omg")
    parser.add_argument(
        "--expected", type=Path, help="optional sorted reference output"
    )
    parser.add_argument("--sum-tolerance", type=float, default=1e-10)
    parser.add_argument("--relative-tolerance", type=float, default=1e-4)
    return parser.parse_args()


def main() -> None:
    print(json.dumps(validate(parse_args()), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
