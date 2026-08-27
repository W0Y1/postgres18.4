#!/usr/bin/env python3

import argparse
import json
import math
import os
import sys
import tempfile
from pathlib import Path


class ConversionError(ValueError):
    pass


def default_costs_path(path):
    suffix = ".paths.jsonl"
    if path.name.endswith(suffix):
        return path.with_name(path.name[:-len(suffix)] + ".costs")
    return path.with_suffix(".costs")


def default_rows_path(path):
    return path.with_suffix(".rows")


def load_jsonl(path):
    records = []

    with path.open("r", encoding="utf-8") as input_file:
        for line_number, line in enumerate(input_file, start=1):
            if not line.strip():
                raise ConversionError(f"{path}:{line_number}: empty line")
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise ConversionError(
                    f"{path}:{line_number}: invalid JSON: {error.msg}"
                ) from error
            if not isinstance(record, dict):
                raise ConversionError(
                    f"{path}:{line_number}: expected a JSON object"
                )
            records.append((line_number, record))

    if not records:
        raise ConversionError(f"{path}: input file is empty")
    return records


def validate_sequence(path, line_number, record, expected):
    sequence = record.get("sequence")
    if isinstance(sequence, bool) or not isinstance(sequence, int):
        raise ConversionError(
            f"{path}:{line_number}: sequence must be an integer"
        )
    if sequence != expected:
        raise ConversionError(
            f"{path}:{line_number}: expected sequence {expected}, got {sequence}"
        )


def numeric_value(path, line_number, container, field):
    value = container.get(field)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConversionError(
            f"{path}:{line_number}: {field} must be a number"
        )
    value = float(value)
    if not math.isfinite(value) or value < 0:
        raise ConversionError(
            f"{path}:{line_number}: {field} must be finite and nonnegative"
        )
    return value


def convert_costs(path):
    output = []

    for expected, (line_number, record) in enumerate(load_jsonl(path)):
        validate_sequence(path, line_number, record, expected)
        path_record = record.get("path")
        if not isinstance(path_record, dict):
            raise ConversionError(
                f"{path}:{line_number}: path must be a JSON object"
            )
        startup_cost = numeric_value(
            path, line_number, path_record, "startup_cost"
        )
        total_cost = numeric_value(path, line_number, path_record, "total_cost")
        if startup_cost > total_cost:
            raise ConversionError(
                f"{path}:{line_number}: startup_cost exceeds total_cost"
            )
        output.append(f"{startup_cost:.17g} {total_cost:.17g}\n")
    return output


def convert_rows(path):
    output = []

    for expected, (line_number, record) in enumerate(load_jsonl(path)):
        validate_sequence(path, line_number, record, expected)
        native_rows = numeric_value(path, line_number, record, "native_rows")
        output.append(f"{native_rows:.17g}\n")
    return output


def write_atomic(path, lines):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name = None

    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="ascii",
            dir=path.parent,
            prefix=f".{path.name}.",
            delete=False,
        ) as output_file:
            temporary_name = output_file.name
            output_file.writelines(lines)
        os.replace(temporary_name, path)
    except BaseException:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass
        raise


def parse_args():
    parser = argparse.ArgumentParser(
        description="Convert pg_plan_replay JSONL captures to replay files."
    )
    parser.add_argument("paths_jsonl", type=Path, help="Path capture JSONL")
    parser.add_argument(
        "cardinality_jsonl", type=Path, help="Cardinality capture JSONL"
    )
    parser.add_argument(
        "--costs-output", type=Path, help="Output .costs file"
    )
    parser.add_argument("--rows-output", type=Path, help="Output .rows file")
    return parser.parse_args()


def main():
    args = parse_args()
    costs_output = args.costs_output or default_costs_path(args.paths_jsonl)
    rows_output = args.rows_output or default_rows_path(args.cardinality_jsonl)

    input_paths = {args.paths_jsonl.resolve(), args.cardinality_jsonl.resolve()}
    output_paths = {costs_output.resolve(), rows_output.resolve()}
    if len(output_paths) != 2 or input_paths & output_paths:
        raise ConversionError("input and output files must all have distinct paths")

    costs = convert_costs(args.paths_jsonl)
    rows = convert_rows(args.cardinality_jsonl)
    write_atomic(costs_output, costs)
    write_atomic(rows_output, rows)

    print(f"wrote {len(costs)} costs to {costs_output}")
    print(f"wrote {len(rows)} rows to {rows_output}")


if __name__ == "__main__":
    try:
        main()
    except (ConversionError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
