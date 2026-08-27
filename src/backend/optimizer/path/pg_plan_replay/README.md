# pg_plan_replay 离线采集与顺序回放

这个版本不需要启动推理服务。一个 `plan_replay.mode` 同时控制两类数据：

- Path 代价协议：采集候选 `Path` 树，按候选出现顺序回放
  `startup_cost` 和 `total_cost`。
- 基数协议：采集基础表和连接的基数估计输入，按估计调用顺序回放 `rows`。

## 编译与安装

```bash
make PG_CONFIG=/home/pgdev/postgresql-18.4/Debug/install/bin/pg_config
make install PG_CONFIG=/home/pgdev/postgresql-18.4/Debug/install/bin/pg_config
```

## 统一采集

```sql
LOAD 'pg_plan_replay';
SET plan_replay.path_capture_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/query.paths.jsonl';
SET plan_replay.cardinality_capture_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/cardinality.jsonl';
SET plan_replay.mode = 'capture';
EXPLAIN ANALYZE SELECT * FROM pg_catalog.pg_class;
SET plan_replay.mode = 'off';
```

开始规划顶层语句时，采集文件会被清空。默认使用紧凑格式 v3，每一行对应一个
候选 Path，并带有从 0 开始的 `sequence` 序号：

```json
{
  "format_version": 3,
  "sequence": 0,
  "cardinality_sequence": 0,
  "partial": false,
  "relids": [1],
  "path": {
    "op_name": "Seq Scan",
    "startup_cost": 0,
    "total_cost": 20.225,
    "rows": 150,
    "width": 4,
    "pathkeys": 0,
    "parallel_aware": false,
    "parallel_workers": 0,
    "relids": [1],
    "relations": [
      {"rti": 1, "alias": "c", "schema": "pg_catalog", "table": "pg_class"}
    ],
    "children": []
  }
}
```

`path` 会递归记录子 Path。索引 Path 额外包含 `index_name`，连接 Path 额外包含
`join_type`。默认格式不再重复输出查询文本、所有 planner GUC、全表列定义、
`RelOptInfo`、`PathTarget`、`raw_query`、`raw_path` 和节点专属内部 dump。
过滤条件和连接条件属于独立的基数采集协议，不在 Path 代价文件中重复记录。

排查 PostgreSQL 内部节点时可以临时恢复旧的完整 v2 格式：

```sql
SET plan_replay.path_capture_verbose = on;
```

正常生成模型输入时应保持 `plan_replay.path_capture_verbose = off`。

启用采集或回放时，每个候选 Path 都记录两个独立序列号：

- `sequence`：候选 Path 序号，对应 Path JSONL 和 `.costs` 的行号。
- `cardinality_sequence`：生成该 Path 行数估计的序号，对应 cardinality JSONL 和
  `.rows` 的行号；没有对应基数估计时为 `null`。

这两个序列号会传递到由该 Path 生成的最终 Plan 节点。`EXPLAIN ANALYZE` 文本格式
分别显示 `plan_replay_sequence=N` 和 `plan_replay_cardinality_sequence=N`，JSON
格式分别显示 `"Plan Replay Sequence": N` 和
`"Plan Replay Cardinality Sequence": N`。规划器额外插入、没有直接对应 Path 的
Plan 节点不会显示相应字段。

## 保存真实执行结果

在 capture 会话中，可以把同一条查询的真实执行结果保存为 JSON：

```sql
LOAD 'pg_plan_replay';
SET plan_replay.path_capture_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/query.paths.jsonl';
SET plan_replay.cardinality_capture_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/cardinality.jsonl';
SET plan_replay.mode = 'capture';

\pset format unaligned
\pset tuples_only on
\o /home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/query.explain.json

EXPLAIN (ANALYZE, BUFFERS OFF, FORMAT JSON)
SELECT * FROM pg_catalog.pg_class;

\o
SET plan_replay.mode = 'off';
```

`BUFFERS OFF` 去掉当前任务不需要的 buffer/block 字段。这样一次执行会得到：

- `query.paths.jsonl`：所有候选 Path 的原始基数和代价估计。
- `cardinality.jsonl`：基础关系、普通连接和参数化连接的原始基数估计。
- `query.explain.json`：最终执行计划节点的真实行数和执行时间。

`query.explain.json` 中的 `Plan Replay Sequence` 可以直接定位
`query.paths.jsonl` 和 `.costs` 中对应的候选 Path。可以使用节点的
`Actual Startup Time`、`Actual Total Time` 覆盖该 Path 的两项代价。按照当前实验
约定，真实时间与 PostgreSQL cost 不做额外单位换算。

`Plan Replay Cardinality Sequence` 可以直接定位 `cardinality.jsonl` 和 `.rows`
中对应的基数估计，不再需要按关系集合、连接类型或计划树位置做启发式匹配。生成真实
`.rows` 时，把该节点的 `Actual Rows` 写入对应序号的行即可。`Actual Rows` 是每次
loop 的平均输出行数，这与规划器的节点行数语义一致，因此注入基数时不要乘以
`Actual Loops`。

当前基数 hook 覆盖基础关系、普通连接和参数化连接。参数化基础关系以及 Aggregate、
Limit 等上层节点没有独立的基数采集记录，因此相应 Path 和 Plan 节点不会显示
cardinality 序列号；不能为这类节点凭空改写 `.rows`。

## 生成回放文件

采集完成后，使用附带的 Python 工具把两个 JSONL 分别转换成 `.costs` 和
`.rows`：

```bash
python3 tools/jsonl_to_replay.py \
  test/query.paths.jsonl \
  test/cardinality.jsonl
```

默认生成 `test/query.costs` 和 `test/cardinality.rows`。需要自定义位置时使用
`--costs-output` 和 `--rows-output`。工具会检查 JSON 格式、连续的 `sequence`、
数值范围以及 `startup_cost <= total_cost`，验证全部通过后才写出文件。

## 统一回放

回放文件必须为每个已采集的候选提供且仅提供一行数据：

```text
startup_cost total_cost
```

然后在相同的规划器环境下执行同一条语句：

```sql
SET plan_replay.path_replay_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/query.costs';
SET plan_replay.cardinality_replay_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/cardinality.rows';
SET plan_replay.mode = 'replay';
EXPLAIN SELECT * FROM pg_catalog.pg_class;
SET plan_replay.mode = 'off';
```

两个代价值必须是有限的非负数，并满足 `startup_cost <= total_cost`。如果回放
文件缺少数据、格式错误或包含多余数据，当前规划会立即报错终止。一组采集和回放
文件只对应一条顶层语句，不能由多个 backend 并发读写。

## 基数文件

基数 JSONL 每行对应一次基数估计调用，并包含从 0 开始的 `sequence`。`kind`
有三种取值：

- `base`：基础关系估计，记录原始表行数 `table_tuples`、原生估计
  `native_rows`、关系身份和实际参与估计的 `filter_clause`。
- `join`：普通连接关系估计，记录连接类型、两侧 relids、关系别名、两侧实际输入
  `outer_rows`/`inner_rows`、原生输出估计和 `join_clause`。
- `parameterized_join`：参数化连接估计。字段与 `join` 相同，但两侧行数来自本次
  被估价的具体 Path，而不一定等于 RelOptInfo 的普通行数。

`filter_clause` 和 `join_clause` 都是数组。每个元素包含可读的 `expression`、
PostgreSQL 内部表示 `raw` 和该条件依赖的 `required_relids`。其中
`join_clause` 直接来自 PostgreSQL 本次连接基数计算实际接收的 `restrictlist`，
因此也覆盖外连接场景下在该层应用的 pushed-down 条件。

基数回放文件必须按 JSONL 中的 `sequence` 顺序，为每次估计提供且仅提供一个值：

```text
rows
```

`rows` 必须是有限的非负数。缺行、多行、非法数字都会使当前规划立即报错。
PostgreSQL 原生的 `clamp_row_est()` 仍会在 hook 返回后执行，因此除允许为 0 的
特殊情况外，最终参与后续规划的估计至少为 1。

这部分移植自 PostgreSQL 13.1 实验代码，但针对 PostgreSQL 18.4 做了接口适配：
没有保留固定长度数组、全局手工序号和工作目录相对文件，而是通过一个窄的核心 hook
交给 `pg_plan_replay` 逐行读写；同时区分普通连接与参数化连接，并补充原实现缺少的
`join_clause`。

## 测试产物

测试使用的采集和回放文件统一保存在源码目录下的 `test/`：

```text
src/backend/optimizer/path/pg_plan_replay/test/<case>.paths.jsonl
src/backend/optimizer/path/pg_plan_replay/test/<case>.costs
src/backend/optimizer/path/pg_plan_replay/test/<case>.cardinality.jsonl
src/backend/optimizer/path/pg_plan_replay/test/<case>.rows
```

同一用例使用相同的 `<case>` 前缀。不要把用于检查或交付的 `.jsonl`、`.costs`
文件放到 `/tmp`；临时错误注入文件也应在测试结束后从 `test/` 清理。

当前版本提供 `plan_replay_last_timing()`。在线服务相关的时间字段
固定为 0，`paths` 和 `batches` 返回最近一次成功采集或回放所处理的 Path 和批次数量。
`plan_replay_reset_stats()` 用于清空这些统计信息。
