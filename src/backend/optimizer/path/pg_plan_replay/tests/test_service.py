from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from pg_plan_replay.service.adapters import SUPPORTED_MODELS, LearnedCostModel
from pg_plan_replay.service.server import PredictionCache, predict_costs
from pg_plan_replay.tests.mock_server import FixedCostModel


class FakeModel:
    def predict(self, paths):
        return [float(path["rows"]) for path in paths]


class WrongCountModel:
    def predict(self, paths):
        return []


class NonFiniteModel:
    def predict(self, paths):
        return [float("nan")] * len(paths)


class ServiceTests(unittest.TestCase):
    @staticmethod
    def _adapter(model_name):
        model = LearnedCostModel.__new__(LearnedCostModel)
        model.model_name = model_name
        model.config = SimpleNamespace(max_num_filters=6)
        model.db_statistics = SimpleNamespace(
            table_stats=[SimpleNamespace(relname="title")],
            column_stats=[
                SimpleNamespace(tablename="title", attname="id"),
                SimpleNamespace(tablename="title", attname="production_year"),
            ],
        )
        model.table_indexes = {"title": 0}
        model.columns_by_relation = {"title": [0, 1]}
        model.column_indexes = {
            ("title", "id"): 0,
            ("title", "production_year"): 1,
        }
        model.column_statistics = {
            "title": {
                "id": {"datatype": "int", "min": 1, "max": 10},
                "production_year": {"datatype": "int", "min": 1880, "max": 2020},
            }
        }
        model.feature_statistics = {
            "op_name": {
                "value_dict": {
                    "Seq Scan": 0,
                    "Index Scan": 1,
                    "Nested Loop": 2,
                    "Aggregate": 3,
                    "Sort": 4,
                    "Materialize": 5,
                }
            },
            "operator": {
                "value_dict": {
                    "AND": 0,
                    "OR": 1,
                    "=": 2,
                    "!=": 3,
                    ">=": 4,
                    "<=": 5,
                    "IN": 6,
                    "IS NULL": 7,
                    "IS NOT NULL": 8,
                }
            },
            "aggregation": {
                "value_dict": {"COUNT": 0, "SUM": 1, "None": 2}
            },
            "tablename": {"value_dict": {"title": 4}},
            "join_conds": {
                "value_dict": {'"title"."id" = "title"."production_year"': 0}
            },
        }
        return model

    def test_only_requested_models_are_public(self):
        self.assertEqual(
            SUPPORTED_MODELS,
            ("e2e", "query_former", "zeroshot", "dace", "qppnet"),
        )

    def test_literal_feature_matches_released_filter_parser(self):
        self.assertEqual(LearnedCostModel._literal_feature("IN", [1, 2, 3]), 2)
        self.assertEqual(LearnedCostModel._literal_feature("LIKE", "%foo%%"), 3)
        self.assertEqual(LearnedCostModel._literal_feature("=", 1), 0)

    def test_cache_is_canonical_and_lru_bounded(self):
        cache = PredictionCache(1)
        left = {"op_name": "Seq Scan", "rows": 10}
        reordered = {"rows": 10, "op_name": "Seq Scan"}
        self.assertEqual(cache.key(left), cache.key(reordered))
        self.assertNotEqual(cache.key(left), cache.key({**left, "rows": 11}))
        cache.put(cache.key(left), 3.0)
        self.assertEqual(cache.get(cache.key(reordered)), 3.0)
        cache.put(cache.key({"op_name": "Index Scan"}), 2.0)
        self.assertIsNone(cache.get(cache.key(left)))

    def test_server_returns_costs_in_request_order(self):
        paths = [{"rows": 3}, {"rows": 1}, {"rows": 2}]
        costs = predict_costs(paths, FakeModel(), PredictionCache(8), output_scale=10)
        self.assertEqual(costs, [30.0, 10.0, 20.0])

    def test_mock_server_can_record_serialized_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            request_log = Path(directory) / "requests.jsonl"
            paths = [{"filter_conditions": [{"operator": "="}]}]
            model = FixedCostModel(7.0, str(request_log))

            self.assertEqual(model.predict(paths), [7.0])
            self.assertEqual(
                json.loads(request_log.read_text(encoding="utf-8")), paths
            )

    def test_wrong_prediction_count_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "wrong prediction count"):
            predict_costs([{"rows": 1}], WrongCountModel(), PredictionCache(0), 1)

    def test_non_finite_prediction_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "non-finite"):
            predict_costs([{"rows": 1}], NonFiniteModel(), PredictionCache(0), 1)

    def test_zeroshot_statistics_are_patched_only_in_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "data/runs/parsed_plans/statistics_workload_combined.json"
            target.parent.mkdir(parents=True)
            original = {
                "data_type": {
                    "value_dict": {f"type_{index}": index for index in range(9)},
                    "no_vals": 9,
                    "type": "categorical",
                }
            }
            target.write_text(json.dumps(original), encoding="utf-8")
            model = LearnedCostModel.__new__(LearnedCostModel)
            model.repository = root
            patched = model._parsed_feature_statistics(zeroshot=True)
            self.assertEqual(patched["data_type"]["no_vals"], 10)
            self.assertIn("workers_planned", patched)
            self.assertEqual(json.loads(target.read_text(encoding="utf-8")), original)

    def test_qpp_path_tree_is_normalized_to_fixed_model_arity(self):
        model = LearnedCostModel.__new__(LearnedCostModel)
        operators = {name: [] for name in ("Result", "Materialize", "Join")}
        model.config = SimpleNamespace(
            featurization=SimpleNamespace(QPP_NET_OPERATOR_TYPES=operators)
        )
        model.feature_statistics = {
            "Join Type": {"value_dict": {"Inner": 0}},
            "Parent Relationship": {"value_dict": {"Inner": 0, "Outer": 1}},
        }
        leaf = {"op_name": "Unknown", "rows": 1, "children": []}
        unary = {"op_name": "Limit", "rows": 1, "children": [leaf]}
        binary = {"op_name": "Append", "rows": 1, "children": [leaf, leaf, leaf]}
        self.assertEqual(len(model._qpp_json(unary)["Plan"]["Plans"]), 1)
        self.assertEqual(len(model._qpp_json(binary)["Plan"]["Plans"]), 2)

    def test_real_filters_and_outputs_replace_e2e_placeholders(self):
        model = self._adapter("e2e")
        path = {
            "op_name": "Seq Scan",
            "relation_names": ["title"],
            "native_total_cost": 42,
            "rows": 7,
            "width": 12,
            "parallel_workers": 0,
            "filter_conditions": [
                {
                    "operator": ">",
                    "relation": "title",
                    "column": "production_year",
                    "literal": "2000",
                    "children": [],
                },
                {
                    "operator": "IN",
                    "relation": "title",
                    "column": "id",
                    "literal": ["1", "2"],
                    "children": [],
                },
            ],
            "output_columns": [
                {
                    "aggregation": "None",
                    "columns": [{"relation": "title", "column": "production_year"}],
                }
            ],
            "children": [],
        }

        plan = model._plan(path)
        parameters = plan.plan_parameters
        self.assertEqual(parameters.filter_columns.operator, "AND")
        self.assertEqual(parameters.filter_columns.children[0].column, 1)
        self.assertEqual(parameters.filter_columns.children[0].operator, ">=")
        self.assertEqual(parameters.filter_columns.children[0].literal, 2000)
        self.assertEqual(parameters.filter_columns.children[1].literal, [1, 2])
        self.assertEqual(parameters.filter_columns.children[1].literal_feature, 1)
        self.assertEqual(parameters.output_columns[0].columns, [1])
        self.assertEqual(parameters.sample_vec, [0] * 1000)

    def test_query_former_receives_table_filters_and_known_joins(self):
        model = self._adapter("query_former")
        leaf = {
            "op_name": "Seq Scan",
            "relation_names": ["title"],
            "rows": 10,
            "filter_conditions": [
                {
                    "operator": "IS NOT NULL",
                    "relation": "title",
                    "column": "id",
                    "literal": None,
                    "children": [],
                }
            ],
            "children": [],
        }
        join = {
            "op_name": "Nested Loop",
            "rows": 5,
            "join_conditions": [
                {
                    "left_relation": "title",
                    "left_column": "production_year",
                    "operator": "=",
                    "right_relation": "title",
                    "right_column": "id",
                }
            ],
            "children": [leaf, leaf],
        }

        plan = model._plan(join)
        self.assertEqual(plan.children[0].plan_parameters.tablename, 4)
        self.assertEqual(
            plan.children[0].plan_parameters.filter_columns.operator,
            "IS NOT NULL",
        )
        self.assertEqual(
            plan.join_conds,
            ['"title"."id" = "title"."production_year"'],
        )

    def test_unknown_or_branch_is_not_silently_dropped(self):
        model = self._adapter("query_former")
        path = {
            "op_name": "Seq Scan",
            "relation_names": ["title"],
            "rows": 10,
            "filter_conditions": [
                {
                    "operator": "OR",
                    "children": [
                        {
                            "operator": "=",
                            "relation": "title",
                            "column": "id",
                            "literal": "1",
                            "children": [],
                        },
                        {
                            "operator": "=",
                            "relation": "title",
                            "column": "unknown_column",
                            "literal": "2",
                            "children": [],
                        },
                    ],
                }
            ],
            "children": [],
        }

        plan = model._plan(path)
        self.assertFalse(hasattr(plan.plan_parameters, "filter_columns"))

    def test_dace_ignores_checkpoint_incompatible_features(self):
        model = self._adapter("dace")
        plan = model._plan(
            {
                "op_name": "Seq Scan",
                "relation_names": ["title"],
                "native_total_cost": 8,
                "rows": 3,
                "filter_conditions": [
                    {
                        "operator": "=",
                        "relation": "title",
                        "column": "id",
                        "literal": "1",
                    }
                ],
                "output_columns": [
                    {
                        "aggregation": "None",
                        "columns": [{"relation": "title", "column": "id"}],
                    }
                ],
                "children": [],
            }
        )
        self.assertFalse(hasattr(plan.plan_parameters, "filter_columns"))
        self.assertEqual(plan.plan_parameters.output_columns, [])
        self.assertEqual(plan.join_conds, [])

    def test_qpp_uses_real_path_properties(self):
        model = LearnedCostModel.__new__(LearnedCostModel)
        model.model_name = "qppnet"
        model.config = SimpleNamespace(
            featurization=SimpleNamespace(
                QPP_NET_OPERATOR_TYPES={
                    name: []
                    for name in (
                        "Index Scan",
                        "Join",
                        "Aggregate",
                        "Sort",
                        "Result",
                        "Materialize",
                    )
                }
            )
        )
        model.feature_statistics = {
            "Relation Name": {"value_dict": {"title": 0}},
            "Index Name": {"value_dict": {"title_pkey": 0}},
            "Scan Direction": {"value_dict": {"Forward": 0, "Backward": 1}},
            "Join Type": {"value_dict": {"Inner": 0, "Left": 1}},
            "Parent Relationship": {"value_dict": {"Inner": 0, "Outer": 1}},
            "Strategy": {"value_dict": {"Plain": 0, "Hashed": 1}},
            "Partial Mode": {"value_dict": {"Simple": 0, "Partial": 1}},
            "Sort Key": {"value_dict": {'"title"."id"': 0}},
            "Sort Method": {"value_dict": {"quicksort": 0}},
        }
        leaf = {
            "op_name": "Index Scan",
            "relation_names": ["title"],
            "index_name": "title_pkey",
            "scan_direction": "Backward",
            "rows": 1,
            "filter_conditions": [
                {
                    "operator": "=",
                    "column": "id",
                    "literal": "1",
                    "children": [],
                }
            ],
            "children": [],
        }
        scan = model._qpp_json(leaf)["Plan"]
        self.assertEqual(scan["Scan Direction"], "Backward")
        self.assertIn('"id" =', scan["Filter"])

        join = model._qpp_json(
            {"op_name": "Hash Join", "join_type": "Left", "rows": 1, "children": [leaf, leaf]}
        )["Plan"]
        self.assertEqual(join["Join Type"], "Left")

        nested_join = model._qpp_json(
            {
                "op_name": "Hash Join",
                "rows": 1,
                "children": [
                    {
                        "op_name": "Hash Join",
                        "rows": 1,
                        "children": [leaf, leaf],
                    },
                    leaf,
                ],
            }
        )["Plan"]
        self.assertEqual(
            nested_join["Plans"][0]["Parent Relationship"], "Outer"
        )

        aggregate = model._qpp_json(
            {
                "op_name": "Aggregate",
                "aggregate_strategy": "Hashed",
                "partial_mode": "Partial",
                "rows": 1,
                "children": [leaf],
            }
        )["Plan"]
        self.assertEqual(aggregate["Strategy"], "Hashed")
        self.assertEqual(aggregate["Partial Mode"], "Partial")

        sort = model._qpp_json(
            {
                "op_name": "Sort",
                "rows": 1,
                "sort_keys": [{"relation": "title", "column": "id"}],
                "children": [leaf],
            }
        )["Plan"]
        self.assertEqual(sort["Sort Key"], ['"title"."id"'])


if __name__ == "__main__":
    unittest.main()
