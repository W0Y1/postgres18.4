from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import signal
import socketserver
import threading
from collections import OrderedDict
from typing import Any

from .adapters import SUPPORTED_MODELS, CostModel, load_model


class PredictionCache:
    def __init__(self, capacity: int):
        self.capacity = max(0, capacity)
        self.data: OrderedDict[str, float] = OrderedDict()
        self.lock = threading.Lock()

    @staticmethod
    def key(path: dict[str, Any]) -> str:
        payload = json.dumps(path, sort_keys=True, separators=(",", ":"))
        return hashlib.blake2b(payload.encode(), digest_size=16).hexdigest()

    def get(self, key: str) -> float | None:
        with self.lock:
            value = self.data.get(key)
            if value is not None:
                self.data.move_to_end(key)
            return value

    def put(self, key: str, value: float) -> None:
        if not self.capacity:
            return
        with self.lock:
            self.data[key] = value
            self.data.move_to_end(key)
            while len(self.data) > self.capacity:
                self.data.popitem(last=False)


class PlanReplayServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True

    def __init__(self, socket_path: str, model: CostModel, cache_size: int, output_scale: float):
        self.model = model
        self.cache = PredictionCache(cache_size)
        self.output_scale = output_scale
        super().__init__(socket_path, PlanReplayRequestHandler)


class PlanReplayRequestHandler(socketserver.StreamRequestHandler):
    server: PlanReplayServer

    def handle(self) -> None:
        # V1 intentionally maps output costs to input Paths by array order.
        request = json.loads(self.rfile.readline())
        paths = request.get("paths")
        if not isinstance(paths, list):
            raise ValueError("request.paths must be an array")

        costs = predict_costs(
            paths, self.server.model, self.server.cache, self.server.output_scale
        )
        response = json.dumps(
            {"version": 1, "type": "predict_cost_result", "costs": costs},
            separators=(",", ":"),
        ) + "\n"
        self.wfile.write(response.encode())


def predict_costs(
    paths: list[dict[str, Any]],
    model: CostModel,
    cache: PredictionCache,
    output_scale: float,
) -> list[float]:
    """Predict a Path batch while preserving its positional mapping."""

    costs: list[float | None] = [None] * len(paths)
    misses: list[dict[str, Any]] = []
    miss_indexes: list[int] = []
    keys: list[str] = []
    for index, path in enumerate(paths):
        key = cache.key(path)
        keys.append(key)
        cached = cache.get(key)
        if cached is None:
            misses.append(path)
            miss_indexes.append(index)
        else:
            costs[index] = cached

    if misses:
        predictions = model.predict(misses)
        if len(predictions) != len(misses):
            raise ValueError("model returned the wrong prediction count")
        for index, prediction in zip(miss_indexes, predictions):
            cost = max(float(prediction) * output_scale, 1e-9)
            if not math.isfinite(cost):
                raise ValueError("model returned a non-finite cost")
            costs[index] = cost
            cache.put(keys[index], cost)

    if any(cost is None for cost in costs):
        raise ValueError("model did not produce every ordered cost")
    return [float(cost) for cost in costs]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Serve lcm-eval models to pg_plan_replay")
    parser.add_argument("--socket", default="/tmp/pg_plan_replay.sock")
    parser.add_argument("--model", choices=SUPPORTED_MODELS, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--database", default="imdb")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--cache-size", type=int, default=4096)
    parser.add_argument(
        "--socket-mode",
        default="0777",
        help="Unix-socket permissions; PostgreSQL may run as another user",
    )
    parser.add_argument(
        "--output-scale",
        type=float,
        default=1000.0,
        help="multiply model output before assigning Path.total_cost",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    model = load_model(args.model, args.repository, args.database, args.seed, args.device)
    if os.path.exists(args.socket):
        os.unlink(args.socket)
    server = PlanReplayServer(args.socket, model, args.cache_size, args.output_scale)
    os.chmod(args.socket, int(args.socket_mode, 8))
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=server.shutdown).start())
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if os.path.exists(args.socket):
            os.unlink(args.socket)


if __name__ == "__main__":
    main()
