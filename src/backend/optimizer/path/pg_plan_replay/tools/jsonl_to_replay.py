#!/usr/bin/env python3

import argparse
import json
import math
import os
import re
import sys
import tempfile
from pathlib import Path


class ConversionError(ValueError):
    pass


PATH_ID_PATTERN = re.compile(r"[0-9a-f]{32}\Z")
COSTS_HEADER = "# pg_plan_replay_costs format=3\n"
CARDINALITIES_HEADER = "# pg_plan_replay_cardinalities format=2\n"


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


def relid_array(path, line_number, record, field):
    values = record.get(field)
    if (
        not isinstance(values, list)
        or any(
            isinstance(value, bool)
            or not isinstance(value, int)
            or value <= 0
            for value in values
        )
        or values != sorted(set(values))
    ):
        raise ConversionError(
            f"{path}:{line_number}: {field} must be a sorted array of "
            "unique positive integers"
        )
    return tuple(values)


def is_compact_capture(path):
    try:
        with path.open(encoding="utf-8") as source:
            first = json.loads(source.readline())
        return isinstance(first, dict) and first.get("record") == "header"
    except (ValueError, OSError):
        return False


def validate_compact_capture(path, kind):
    """Validate every complete planning frame without loading candidate trees."""
    header = None
    count = 0
    frames = 0
    version = 9 if kind == "path" else 4
    with path.open(encoding="utf-8") as source:
        for number, line in enumerate(source, 1):
            try:
                row = json.loads(line)
            except ValueError as error:
                raise ConversionError(f"{path}:{number}: invalid JSON") from error
            if not isinstance(row, dict):
                raise ConversionError(f"{path}:{number}: expected an object")
            if header is None:
                if row.get("record") != "header" or row.get("format_version") != version:
                    raise ConversionError(f"capture v{version} required; recapture legacy files")
                if kind == "path" and (row.get("path_id_version") != 2 or
                        type(row.get("parallel_leader_participation")) is not bool):
                    raise ConversionError("invalid Path identity or parallel header")
                header, count = row, 0
            elif row.get("record") == "end":
                if type(row.get("records")) is not int or row["records"] != count:
                    raise ConversionError("capture record count mismatch")
                if kind == "path":
                    stats = row.get("stats")
                    if not isinstance(stats, dict) or type(stats.get("paths")) is not int or stats["paths"] < count:
                        raise ConversionError("invalid planning frame statistics")
                header = None
                frames += 1
            else:
                key = row.get("path_id" if kind == "path" else "cardinality_id")
                if not isinstance(key, str) or not PATH_ID_PATTERN.fullmatch(key):
                    raise ConversionError("capture ID must be 32 lowercase hex digits")
                flags = ("partial", "cost_replay_applied", "path_prune_applied") if kind == "path" else ("cardinality_replay_applied",)
                allowed = {"path_id", "parallel_workers", *flags} if kind == "path" else {"cardinality_id", *flags}
                if set(row) - allowed:
                    raise ConversionError("unexpected compact capture fields")
                if kind == "path" and (type(row.get("parallel_workers")) is not int or row["parallel_workers"] < 0):
                    raise ConversionError("invalid parallel_workers")
                if any(type(row.get(flag, False)) is not bool for flag in flags):
                    raise ConversionError("invalid capture flag")
                count += 1
    if header is not None or not frames:
        raise ConversionError("incomplete capture (missing footer)")


def convert_costs(path):
    if is_compact_capture(path):
        validate_compact_capture(path, "path")
        return [COSTS_HEADER]
    output = [COSTS_HEADER]

    for expected, (line_number, record) in enumerate(load_jsonl(path)):
        validate_sequence(path, line_number, record, expected)
        if record.get("format_version") != 8 or record.get("path_id_version") != 2:
            raise ConversionError("Path ID v2 / capture v8 required; recapture legacy files")
        path_id = record.get("path_id")
        if not isinstance(path_id, str) or not PATH_ID_PATTERN.fullmatch(path_id):
            raise ConversionError(
                f"{path}:{line_number}: path_id must be 32 lowercase hex digits"
            )
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
    return output


def convert_rows(path):
    if is_compact_capture(path):
        validate_compact_capture(path, "cardinality")
        return [CARDINALITIES_HEADER]
    identities = {}
    kinds = {"base", "parameterized_base", "join", "parameterized_join"}

    # sequence 只用于验证 capture 文件完整、有序；稀疏 .rows 不保存或匹配它。
    for expected, (line_number, record) in enumerate(load_jsonl(path)):
        validate_sequence(path, line_number, record, expected)
        if record.get("format_version") != 3:
            raise ConversionError(
                f"{path}:{line_number}: format_version must be 3"
            )
        kind = record.get("kind")
        if kind not in kinds:
            raise ConversionError(
                f"{path}:{line_number}: invalid cardinality kind"
            )
        cardinality_id = record.get("cardinality_id")
        if (
            not isinstance(cardinality_id, str)
            or not PATH_ID_PATTERN.fullmatch(cardinality_id)
        ):
            raise ConversionError(
                f"{path}:{line_number}: cardinality_id must be 32 lowercase hex digits"
            )
        fingerprint = record.get("logical_qual_fingerprint")
        if (
            not isinstance(fingerprint, str)
            or not PATH_ID_PATTERN.fullmatch(fingerprint)
        ):
            raise ConversionError(
                f"{path}:{line_number}: logical_qual_fingerprint must be "
                "32 lowercase hex digits"
            )
        relids = relid_array(path, line_number, record, "relids")
        required_outer = relid_array(
            path, line_number, record, "required_outer"
        )
        replay_applied = record.get("cardinality_replay_applied")
        if not isinstance(replay_applied, bool):
            raise ConversionError(
                f"{path}:{line_number}: cardinality_replay_applied must be a boolean"
            )
        numeric_value(path, line_number, record, "native_rows")
        numeric_value(path, line_number, record, "effective_rows")
        identity = (
            kind,
            relids,
            fingerprint,
            required_outer,
        )
        # 同一 ID 可以重复出现，但每次必须指向一致的逻辑状态。
        previous = identities.setdefault(cardinality_id, identity)
        if previous != identity:
            raise ConversionError(
                f"{path}:{line_number}: cardinality_id is associated with "
                "inconsistent logical state"
            )
    # 工具只初始化安全的空稀疏文件；真实基数由 EXPLAIN ANALYZE 结果另行追加。
    return [CARDINALITIES_HEADER]


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
        "cardinality_jsonl", type=Path, nargs="?", help="Optional legacy cardinality capture JSONL"
    )
    parser.add_argument(
        "--costs-output", type=Path, help="Output .costs file"
    )
    parser.add_argument("--rows-output", type=Path, help="Output .rows file")
    return parser.parse_args()


def main():
    args = parse_args()
    costs_output = args.costs_output or default_costs_path(args.paths_jsonl)
    rows_output = args.rows_output or (default_rows_path(args.cardinality_jsonl)
                                       if args.cardinality_jsonl else costs_output.with_suffix(".rows"))

    input_paths = {args.paths_jsonl.resolve()}
    if args.cardinality_jsonl:
        input_paths.add(args.cardinality_jsonl.resolve())
    output_paths = {costs_output.resolve(), rows_output.resolve()}
    if len(output_paths) != 2 or input_paths & output_paths:
        raise ConversionError("input and output files must all have distinct paths")

    costs = convert_costs(args.paths_jsonl)
    rows = convert_rows(args.cardinality_jsonl) if args.cardinality_jsonl else [CARDINALITIES_HEADER]
    write_atomic(costs_output, costs)
    write_atomic(rows_output, rows)

    print(f"initialized sparse costs file at {costs_output}")
    print(f"initialized sparse cardinality file at {rows_output}")


if __name__ == "__main__":
    try:
        main()
    except (ConversionError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
