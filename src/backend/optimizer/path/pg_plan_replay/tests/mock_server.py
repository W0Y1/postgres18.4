#!/usr/bin/env python3
"""Small deterministic service for PostgreSQL integration tests."""

from __future__ import annotations

import argparse
import json
import os
import signal
import threading
from pathlib import Path

from pg_plan_replay.service.server import PlanReplayServer


class FixedCostModel:
    def __init__(self, cost: float, request_log: str | None = None):
        self.cost = cost
        self.request_log = Path(request_log) if request_log else None
        self.log_lock = threading.Lock()

    def predict(self, paths):
        if self.request_log is not None:
            with self.log_lock, self.request_log.open("a", encoding="utf-8") as output:
                output.write(json.dumps(paths, separators=(",", ":")) + "\n")
        return [self.cost] * len(paths)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default="/tmp/pg_plan_replay_mock.sock")
    parser.add_argument("--cost", type=float, default=1.0)
    parser.add_argument("--request-log")
    args = parser.parse_args()
    if os.path.exists(args.socket):
        os.unlink(args.socket)
    server = PlanReplayServer(
        args.socket, FixedCostModel(args.cost, args.request_log), 0, 1.0
    )
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
