from __future__ import annotations

import copy
import json
import sys
import threading
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Protocol


SUPPORTED_MODELS = ("e2e", "query_former", "zeroshot", "dace", "qppnet")


class CostModel(Protocol):
    def predict(self, paths: list[dict[str, Any]]) -> list[float]: ...


def _categorical_size(statistics: dict, feature: str, size: int) -> None:
    entry = statistics[feature]
    values = entry["value_dict"]
    next_index = max(values.values(), default=-1) + 1
    while next_index < size:
        values[f"__online_unknown_{next_index}"] = next_index
        next_index += 1
    entry["no_vals"] = size


class LearnedCostModel:
    """Adapt PostgreSQL Path JSON to one released lcm-eval checkpoint.

    The checkpoints were trained on completed PostgreSQL plans.  During
    optimization only Path fields exist, so this bridge uses the common
    operator tree, estimated cost/cardinality/width, relation/index names and
    parallelism.  Unavailable plan-only features use the baseline adapters'
    empty or structural placeholder values.
    """

    def __init__(
        self,
        model_name: str,
        repository: str,
        database: str = "imdb",
        seed: int = 0,
        device: str = "cpu",
    ):
        if model_name not in SUPPORTED_MODELS:
            raise ValueError(f"unsupported model {model_name!r}")
        self.model_name = model_name
        self.repository = Path(repository).resolve()
        if not self.repository.is_dir():
            raise FileNotFoundError(f"lcm-eval repository does not exist: {self.repository}")
        self.database = database
        self.seed = seed
        self.device = device
        self.lock = threading.Lock()
        source = str(self.repository / "src")
        if source not in sys.path:
            sys.path.insert(0, source)
        self._load()

    def _json(self, relative: str) -> dict:
        path = self.repository / relative
        if not path.is_file():
            raise FileNotFoundError(f"required model asset does not exist: {path}")
        return json.loads(path.read_text(encoding="utf-8"))

    def _checkpoint(self):
        import torch

        path = self.repository / "data" / "models" / self.model_name / self.database / (
            f"{self.model_name}_{self.seed}.pt"
        )
        if not path.is_file():
            raise FileNotFoundError(f"model checkpoint does not exist: {path}")
        checkpoint = torch.load(path, map_location=self.device)
        if "val_median_q_error_50_best_model" in checkpoint:
            return checkpoint["val_median_q_error_50_best_model"]
        return checkpoint["model"]

    def _database_statistics(self, baseline: bool):
        from cross_db_benchmark.benchmark_tools.utils import load_json

        kind = "augmented_plans_baseline" if baseline else "parsed_plans"
        path = self.repository / "data" / "runs" / kind / self.database / "workload_100k_s1_c8220.json"
        if not path.is_file():
            raise FileNotFoundError(f"database statistics do not exist: {path}")
        return load_json(path).database_stats

    def _parsed_feature_statistics(self, zeroshot: bool) -> dict:
        preferred = self.repository / "data/runs/parsed_plans/statistics_combined_regen.json"
        fallback = self.repository / "data/runs/parsed_plans/statistics_workload_combined.json"
        path = preferred if preferred.is_file() else fallback
        if not path.is_file():
            raise FileNotFoundError(
                f"neither parsed-plan statistics file exists: {preferred}, {fallback}"
            )
        statistics = json.loads(path.read_text(encoding="utf-8"))
        if zeroshot:
            # Released ZeroShot weights expect these encoder dimensions.  The
            # compatibility entries are added only to this in-memory copy.
            statistics.setdefault(
                "workers_planned",
                {"scale": 1.0, "center": 0.0, "type": "numeric"},
            )
            _categorical_size(statistics, "data_type", 10)
        return statistics

    def _load(self) -> None:
        import torch
        from dotenv import load_dotenv

        # lcm-eval imports training-host settings while defining its model
        # configs.  Resolve those settings from the selected repository rather
        # than requiring the service process to have that directory as cwd.
        load_dotenv(self.repository / ".env", override=False)
        from classes.classes import (
            DACEModelConfig,
            QPPNetModelConfig,
            QueryFormerModelConfig,
            TPoolModelConfig,
            ZeroShotModelConfig,
        )

        torch.set_num_threads(1)
        common = dict(device=self.device, seed=self.seed, num_workers=0, batch_size=256)
        self.column_statistics = self._json(
            f"src/cross_db_benchmark/datasets/{self.database}/column_statistics.json"
        )
        self.db_statistics = self._database_statistics(
            baseline=self.model_name in {"e2e", "query_former"}
        )
        self.table_indexes = {
            table.relname: index for index, table in enumerate(self.db_statistics.table_stats)
        }
        self.columns_by_relation: dict[str, list[int]] = {}
        self.column_indexes: dict[tuple[str, str], int] = {}
        for index, column in enumerate(self.db_statistics.column_stats):
            self.columns_by_relation.setdefault(column.tablename, []).append(index)
            self.column_indexes[(column.tablename, column.attname)] = index

        if self.model_name in {"e2e", "query_former"}:
            from gensim.models import KeyedVectors
            from models.workload_driven.dataset.dataset_creation import get_collate_func
            from models.workload_driven.model.e2e_model import E2EModel
            from models.query_former.model import QueryFormer

            self.feature_statistics = self._json(
                f"data/runs/augmented_plans_baseline/{self.database}/statistics_combined.json"
            )
            self.word_embeddings = KeyedVectors.load(
                str(self.repository / "data" / "runs" / "sentences" / self.database / "word2vec.m"),
                mmap="r",
            )
            if self.model_name == "e2e":
                self.config = TPoolModelConfig(**common)
                self.feature_statistics["columns"]["max"] = 358
                self.feature_statistics["column"]["max"] = 358
                self.feature_statistics["table"]["max"] = 20
            else:
                self.config = QueryFormerModelConfig(**common)
            self.collate, input_dims = get_collate_func(
                model_config=self.config,
                database_statistics={0: self.db_statistics},
                feature_statistics=self.feature_statistics,
                column_statistics=self.column_statistics,
                word_embeddings=self.word_embeddings,
                dim_word_hash=self.word_embeddings.vector_size,
                dim_word_emdb=self.word_embeddings.vector_size,
                dim_bitmaps=1000,
            )
            if self.model_name == "e2e":
                self.model = E2EModel(self.config, input_dims, device=self.device)
            else:
                self.model = QueryFormer(
                    config=self.config,
                    input_dims=input_dims,
                    label_norm=None,
                    feature_statistics=self.feature_statistics,
                )

        elif self.model_name == "dace":
            from models.dace.dace_dataset import dace_collator
            from models.dace.dace_model import DACELora

            self.config = DACEModelConfig(**common)
            self.feature_statistics = self._parsed_feature_statistics(zeroshot=False)
            self.collate = lambda plans: dace_collator(
                plans, feature_statistics=self.feature_statistics, config=self.config
            )
            self.model = DACELora(self.config)

        elif self.model_name == "zeroshot":
            from main import readout_hyperparameters
            from models.zeroshot.postgres_plan_batching import postgres_plan_collator
            from models.zeroshot.specific_models.postgres_zero_shot import PostgresZeroShotModel

            tuned = readout_hyperparameters(
                self.repository / "src/conf/zeroshot_hyperparameters/tune_est_best_config.json"
            )
            tuned.update(common)
            self.config = ZeroShotModelConfig(**tuned)
            self.feature_statistics = self._parsed_feature_statistics(zeroshot=True)
            _categorical_size(self.feature_statistics, "aggregation", 6)
            _categorical_size(self.feature_statistics, "operator", 11)
            self.collate = lambda plans: postgres_plan_collator(
                plans,
                db_statistics={0: self.db_statistics},
                feature_statistics=self.feature_statistics,
                plan_featurization=self.config.featurization,
            )
            self.model = PostgresZeroShotModel(
                model_config=self.config,
                feature_statistics=self.feature_statistics,
                label_norm=None,
            )

        else:
            from classes.workload_runs import WorkloadRuns
            from models.qppnet.qppnet_dataloader import qppnet_collator
            from models.qppnet.qppnet_model import QPPNet

            self.config = QPPNetModelConfig(**common)
            self.feature_statistics = self._json(
                f"data/runs/json/{self.database}/feature_statistics_combined.json"
            )
            _categorical_size(self.feature_statistics, "Index Name", 21)
            self.collate = lambda plans: qppnet_collator(
                plans,
                db_statistics={0: self.db_statistics},
                feature_statistics=self.feature_statistics,
                column_statistics=self.column_statistics,
                plan_featurization=self.config.featurization,
            )
            self.model = QPPNet(
                model_config=self.config,
                workload_runs=WorkloadRuns([], []),
                feature_statistics=self.feature_statistics,
                label_norm=None,
            )

        self.model.load_state_dict(self._checkpoint(), strict=True)
        self.model.to(self.device)
        self.model.eval()

    def _op_name(self, value: str) -> str:
        values = self.feature_statistics.get("op_name", {}).get("value_dict", {})
        if value in values:
            return value
        aliases = {"Result": "Materialize", "Unknown": "Materialize"}
        replacement = aliases.get(value, "Materialize")
        return replacement if replacement in values else next(iter(values))

    def _dummy_filter(self, relation: str | None, leaf: bool) -> SimpleNamespace | None:
        if self.model_name != "e2e":
            return None
        candidates = [
            (index, column)
            for index, column in enumerate(self.db_statistics.column_stats)
            if column.tablename == relation
        ]
        if not candidates and leaf:
            candidates = list(enumerate(self.db_statistics.column_stats[:1]))
        if not candidates:
            return None
        column_index, column = candidates[0]
        stats = self.column_statistics.get(relation, {}).get(column.attname, {})
        return SimpleNamespace(
            column=column_index,
            operator="=",
            literal=stats.get("min", 0),
            literal_feature=0,
            children=[],
        )

    def _literal(self, column_index: int, value: Any) -> Any:
        if isinstance(value, list):
            return [self._literal(column_index, item) for item in value]
        if value is None:
            return None
        column = self.db_statistics.column_stats[column_index]
        statistics = self.column_statistics.get(column.tablename, {}).get(column.attname, {})
        datatype = statistics.get("datatype")
        if datatype in {"int", "float"}:
            try:
                number = float(value)
                return int(number) if datatype == "int" else number
            except (TypeError, ValueError):
                return statistics.get("min", 0)
        return str(value)

    @staticmethod
    def _operator_alias(operator: str | None) -> str | None:
        return {
            "<>": "!=",
            ">": ">=",
            "<": "<=",
            "~~": "LIKE",
            "!~~": "NOT LIKE",
        }.get(operator, operator)

    @staticmethod
    def _literal_feature(operator: str | None, value: Any) -> int:
        if operator == "IN" and isinstance(value, list):
            # Match lcm-eval's released parser, which stores comma count.
            return max(len(value) - 1, 0)
        if operator in {"LIKE", "NOT LIKE"}:
            return str(value).count("%")
        return 0

    def _filter_node(
        self, item: dict[str, Any], budget: list[int] | None
    ) -> SimpleNamespace | None:
        operator = self._operator_alias(item.get("operator"))
        children = item.get("children") or []
        if children:
            if operator not in {"AND", "OR"}:
                return None
            original_budget = budget[0] if budget is not None else None
            if budget is not None:
                if budget[0] <= 0:
                    return None
                budget[0] -= 1
            converted = []
            for raw in children:
                child = self._filter_node(raw, budget)
                if child is None:
                    if operator == "OR":
                        if budget is not None:
                            budget[0] = original_budget
                        return None
                    continue
                converted.append(child)
            if not converted:
                if budget is not None:
                    budget[0] = original_budget
                return None
            if len(converted) == 1:
                return converted[0]
            return SimpleNamespace(
                column=None,
                operator=operator,
                literal=None,
                literal_feature=0,
                children=converted,
            )

        operator_values = self.feature_statistics.get("operator", {}).get("value_dict", {})
        relation = item.get("relation")
        column_index = self.column_indexes.get((relation, item.get("column")))
        if column_index is None or operator not in operator_values:
            return None
        if budget is not None:
            if budget[0] <= 0:
                return None
            budget[0] -= 1
        return SimpleNamespace(
            column=column_index,
            operator=operator,
            literal=self._literal(column_index, item.get("literal")),
            literal_feature=self._literal_feature(operator, item.get("literal")),
            children=[],
        )

    def _filter_columns(
        self, path: dict[str, Any], relation: str | None, leaf: bool
    ) -> SimpleNamespace | None:
        if self.model_name not in {"e2e", "query_former", "zeroshot"}:
            return None
        conditions = path.get("filter_conditions") or []
        if len(conditions) == 1:
            root = conditions[0]
        else:
            root = {"operator": "AND", "children": conditions}
        budget = None
        if self.model_name == "query_former":
            budget = [int(getattr(self.config, "max_num_filters", 6))]
        converted = self._filter_node(root, budget) if conditions else None
        if converted is not None:
            return converted
        return self._dummy_filter(relation, leaf)

    def _output_columns(
        self, path: dict[str, Any], relation: str | None, leaf: bool
    ) -> list[SimpleNamespace]:
        if self.model_name not in {"e2e", "zeroshot"}:
            return []
        aggregation_values = self.feature_statistics.get("aggregation", {}).get(
            "value_dict", {}
        )
        fallback_aggregation = (
            "None" if "None" in aggregation_values else next(iter(aggregation_values), "None")
        )
        result: list[SimpleNamespace] = []
        for item in path.get("output_columns") or []:
            columns = []
            for column in item.get("columns") or []:
                index = self.column_indexes.get(
                    (column.get("relation"), column.get("column"))
                )
                if index is not None and index not in columns:
                    columns.append(index)
            aggregation = item.get("aggregation", "None")
            if aggregation not in aggregation_values:
                aggregation = fallback_aggregation
            if columns or aggregation != "None":
                result.append(SimpleNamespace(aggregation=aggregation, columns=columns))
        if result:
            return result
        if self.model_name == "zeroshot":
            columns = self.columns_by_relation.get(relation, []) if relation is not None else []
            if not columns and leaf:
                columns = [0] if self.db_statistics.column_stats else []
            if columns:
                return [
                    SimpleNamespace(
                        aggregation=fallback_aggregation,
                        columns=[columns[0]],
                    )
                ]
        return []

    @staticmethod
    def _normalize_join_condition(value: str) -> str:
        return " ".join(value.replace('"', "").replace(";", " ").split())

    def _known_join_conditions(self, path: dict[str, Any]) -> list[str]:
        if self.model_name != "query_former":
            return []
        values = self.feature_statistics.get("join_conds", {}).get("value_dict", {})
        normalized = {
            self._normalize_join_condition(condition): condition for condition in values
        }
        result = []
        reverse_operator = {"<": ">", ">": "<", "<=": ">=", ">=": "<="}
        for item in path.get("join_conditions") or []:
            if isinstance(item, str):
                candidates = [item]
            else:
                operator = item.get("operator", "=")
                left = f'"{item.get("left_relation")}"."{item.get("left_column")}"'
                right = f'"{item.get("right_relation")}"."{item.get("right_column")}"'
                candidates = [
                    f"{left} {operator} {right}",
                    f"{right} {reverse_operator.get(operator, operator)} {left}",
                ]
            known = next(
                (
                    normalized[self._normalize_join_condition(candidate)]
                    for candidate in candidates
                    if self._normalize_join_condition(candidate) in normalized
                ),
                None,
            )
            if known is not None and known not in result:
                result.append(known)
            if len(result) == 5:
                break
        return result

    def _plan(self, path: dict[str, Any]) -> SimpleNamespace:
        children = [self._plan(child) for child in path.get("children", [])]
        relation_names = path.get("relation_names") or []
        relation = relation_names[0] if len(relation_names) == 1 else None
        table = self.table_indexes.get(relation) if relation is not None else None
        if self.model_name == "e2e" and table is not None:
            table = min(table, 20)
        child_rows = 1.0
        for child in children:
            child_rows *= max(float(child.plan_parameters.est_card), 1.0)
        parameters = SimpleNamespace(
            op_name=self._op_name(path.get("op_name", "Materialize")),
            est_cost=float(path.get("native_total_cost", 0.0)),
            est_card=max(float(path.get("rows", 1.0)), 1.0),
            est_width=float(path.get("width", 0.0)),
            workers_planned=int(path.get("parallel_workers", 0)),
            est_children_card=child_rows,
            output_columns=self._output_columns(path, relation, leaf=not children),
        )
        if table is not None:
            parameters.table = table
        if self.model_name == "query_former" and relation is not None:
            table_values = self.feature_statistics.get("tablename", {}).get("value_dict", {})
            if relation in table_values:
                parameters.tablename = table_values[relation]
        filter_columns = self._filter_columns(path, relation, leaf=not children)
        if filter_columns is not None:
            parameters.filter_columns = filter_columns
            parameters.sample_vec = [0] * 1000
        join_conds = self._known_join_conditions(path)
        for child in children:
            for condition in child.join_conds:
                if condition not in join_conds:
                    join_conds.append(condition)
                if len(join_conds) == 5:
                    break
            if len(join_conds) == 5:
                break
        return SimpleNamespace(
            plan_parameters=parameters,
            children=children,
            plan_runtime=1.0,
            database_id=0,
            join_conds=join_conds,
        )

    def _qpp_property(self, feature: str, fallback: str | None = None):
        values = self.feature_statistics[feature]["value_dict"]
        if fallback in values:
            return fallback
        return next(iter(values))

    @staticmethod
    def _qpp_literal(value: Any) -> str:
        if isinstance(value, list):
            return "(" + ", ".join(LearnedCostModel._qpp_literal(item) for item in value) + ")"
        if value is None:
            return "NULL"
        if isinstance(value, (int, float)):
            return str(value)
        return "'" + str(value).replace("'", "''") + "'"

    def _qpp_filter_text(self, item: dict[str, Any]) -> str | None:
        operator = self._operator_alias(item.get("operator"))
        children = [
            text
            for child in item.get("children") or []
            if (text := self._qpp_filter_text(child)) is not None
        ]
        if children:
            if operator not in {"AND", "OR"}:
                return None
            return "(" + f" {operator} ".join(children) + ")"
        column = item.get("column")
        if not column or not operator:
            return None
        if operator in {"IS NULL", "IS NOT NULL"}:
            return f'"{column}" {operator}'
        return f'"{column}" {operator} {self._qpp_literal(item.get("literal"))}'

    def _qpp_sort_key(self, path: dict[str, Any]) -> str:
        values = self.feature_statistics["Sort Key"]["value_dict"]
        keys = path.get("sort_keys") or []
        if keys:
            relation = keys[0].get("relation")
            column = keys[0].get("column")
            candidates = [f'"{relation}"."{column}"', f"{relation}.{column}", column]
            for candidate in candidates:
                if candidate in values:
                    return candidate
        return self._qpp_property("Sort Key")

    def _qpp_json(
        self, path: dict[str, Any], parent_relationship: str | None = None
    ) -> dict[str, Any]:
        op = path.get("op_name", "Result")
        source_children = path.get("children", [])
        supported = self.config.featurization.QPP_NET_OPERATOR_TYPES
        if op not in supported:
            if "Join" in op or op == "Nested Loop" or len(source_children) >= 2:
                op = "Join"
            elif len(source_children) == 1:
                op = "Materialize"
            else:
                op = "Result"
        relations = path.get("relation_names") or []
        relation = relations[0] if len(relations) == 1 else None
        properties: dict[str, Any] = {
            "Node Type": op,
            "Plan Width": float(path.get("width", 0.0)),
            "Plan Rows": max(float(path.get("rows", 1.0)), 1.0),
            "Total Cost": float(path.get("native_total_cost", 0.0)),
            "Actual Total Time": 0.0,
        }
        if "Scan" in op:
            if relation not in self.feature_statistics["Relation Name"]["value_dict"]:
                relation = self._qpp_property("Relation Name")
            properties["Relation Name"] = relation
            filters = [
                value
                for item in path.get("filter_conditions") or []
                if (value := self._qpp_filter_text(item)) is not None
            ]
            if filters:
                properties["Filter"] = " AND ".join(filters)
        if "Index" in op:
            index_name = path.get("index_name")
            if index_name not in self.feature_statistics["Index Name"]["value_dict"]:
                index_name = self._qpp_property("Index Name")
            properties["Index Name"] = index_name
            properties["Scan Direction"] = self._qpp_property(
                "Scan Direction", path.get("scan_direction", "Forward")
            )
        if op == "Join":
            properties["Join Type"] = self._qpp_property(
                "Join Type", path.get("join_type", "Inner")
            )
            properties["Parent Relationship"] = self._qpp_property(
                "Parent Relationship", parent_relationship or "Inner"
            )
        if "Aggregate" in op:
            properties["Strategy"] = self._qpp_property(
                "Strategy", path.get("aggregate_strategy", "Plain")
            )
            partial = path.get(
                "partial_mode",
                "Partial"
                if op == "Partial Aggregate"
                else "Finalize"
                if op == "Finalize Aggregate"
                else "Simple",
            )
            properties["Partial Mode"] = self._qpp_property("Partial Mode", partial)
        if op == "Sort":
            properties["Sort Key"] = [self._qpp_sort_key(path)]
            properties["Sort Method"] = self._qpp_property("Sort Method", "quicksort")
        if op == "Join":
            source_children = source_children[:2]
        elif op == "Bitmap Heap Scan" or ("Scan" not in op and op != "Result"):
            source_children = source_children[:1]
        else:
            source_children = []
        properties["Plans"] = [
            self._qpp_json(
                child, "Outer" if child_index == 0 else "Inner"
            )["Plan"]
            for child_index, child in enumerate(source_children)
        ]
        return {"Plan": properties, "Execution Time": 0.0, "Planning Time": 0.0}

    def predict(self, paths: list[dict[str, Any]]) -> list[float]:
        import torch

        with self.lock, torch.inference_mode():
            if self.model_name == "qppnet":
                samples = [
                    (index, SimpleNamespace(**self._qpp_json(copy.deepcopy(path))))
                    for index, path in enumerate(paths)
                ]
            else:
                samples = [(index, self._plan(path)) for index, path in enumerate(paths)]
            batch = self.collate(samples)
            model_input, _, sample_indexes = self.config.batch_to_func(
                batch, self.device, self.model.label_norm
            )
            output = self.model(model_input).detach().cpu().reshape(-1).tolist()
            if len(output) != len(paths) or list(sample_indexes) != list(range(len(paths))):
                raise ValueError("collator changed the online Path batch")
            return [max(float(value), 1e-9) for value in output]


def load_model(
    model_name: str,
    repository: str,
    database: str,
    seed: int,
    device: str,
) -> CostModel:
    return LearnedCostModel(model_name, repository, database, seed, device)
