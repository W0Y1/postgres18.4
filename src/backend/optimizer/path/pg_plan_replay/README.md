# pg_plan_replay 离线采集与稳定 ID 回放

这个版本不需要启动推理服务。`plan_replay.mode` 保留为同时控制两类数据的兼容
入口；需要独立实验或迭代采集时使用 `plan_replay.path_mode` 和
`plan_replay.cardinality_mode`：

- Path 代价协议：采集候选 `Path` 树，以 128 位稳定 Path ID 稀疏回放
  `startup_cost` 和 `total_cost`；候选出现顺序只用于诊断。
- 基数协议：采集基础表和连接的基数估计输入，按估计调用顺序回放 `rows`。

两个通道模式均支持 `inherit`、`off`、`capture`、`replay` 和
`capture_replay`。默认 `inherit` 继承 `plan_replay.mode`。`capture_replay` 在同一次
规划中读取上一轮 replay 文件，并把本轮最终生效的值写入新的 JSONL；capture 与
replay 文件必须使用不同的绝对路径。

典型迭代配置如下：

| 实验 | `path_mode` | `cardinality_mode` |
|---|---|---|
| 仅基数注入 | `capture` | `capture_replay` |
| 仅代价注入 | `capture_replay` | `capture` |
| 联合注入 | `capture_replay` | `capture_replay` |

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

开始规划顶层语句时，采集文件会被清空。默认使用紧凑格式 v5，每一行对应一个
候选 Path，并带有从 0 开始的 `sequence` 序号：

```json
{
  "format_version": 5,
  "sequence": 0,
  "path_id": "7ddc6720f8e6eec1354813d528c9d1f3",
  "cost_replay_applied": false,
  "native_startup_cost": 0,
  "native_total_cost": 20.225,
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

根记录的 `native_startup_cost` 和 `native_total_cost` 是本轮回放前 PostgreSQL
计算的值；`path.startup_cost` 和 `path.total_cost` 是回放后最终生效的值。普通
capture 中两组值相同。`cost_replay_applied` 表示本候选是否命中了稀疏代价文件。
`path_id` 是当前物理 Path 的 128 位稳定 ID。`path` 会递归记录子 Path。索引 Path
额外包含 `index_name`，连接 Path 额外包含
`join_type`。默认格式不再重复输出查询文本、所有 planner GUC、全表列定义、
`RelOptInfo`、`PathTarget`、`raw_query`、`raw_path` 和节点专属内部 dump。
过滤条件和连接条件属于独立的基数采集协议，不在 Path 代价文件中重复记录。

排查 PostgreSQL 内部节点时可以临时使用完整 verbose 格式：

```sql
SET plan_replay.path_capture_verbose = on;
```

正常生成模型输入时应保持 `plan_replay.path_capture_verbose = off`。

启用采集或回放时，每个候选 Path 都记录稳定 ID 和两个独立序列号：

- `path_id`：跨轮匹配代价；由 Path 类型、关系身份、参数化、完整排序语义、
  并行属性、类型专属属性和递归子 Path ID 计算。
- `sequence`：本轮候选 Path 序号，只对应 Path JSONL 行序和诊断信息。
- `cardinality_sequence`：生成该 Path 行数估计的序号，对应 cardinality JSONL 和
  `.rows` 的行号；没有对应基数估计时为 `null`。

这些标识会传递到由该 Path 生成的最终 Plan 节点。`EXPLAIN ANALYZE` 文本格式
显示 `plan_replay_path_id=...`、`plan_replay_sequence=N` 和
`plan_replay_cardinality_sequence=N`，JSON 格式显示 `"Plan Replay Path ID"`、
`"Plan Replay Sequence": N` 和
`"Plan Replay Cardinality Sequence": N`。规划器额外插入、没有直接对应 Path 的
Plan 节点不会显示相应字段。

ID 使用 SHA-256 的前 128 位。描述符明确排除 cost、rows、selectivity、真实执行
时间、sequence、列表位置和内存地址。Join 的 outer/inner 子 Path 顺序保留，
BitmapAnd/BitmapOr 的子 ID 排序后再参与计算。同一规划中会保存 ID 到规范描述符的
映射；如果两个不同描述符产生相同 128 位 ID，规划立即报错。

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

`query.explain.json` 中的 `Plan Replay Path ID` 可直接作为稀疏 `.costs` 的键。
把最终计划节点的 `Actual Startup Time`、`Actual Total Time` 连同这个 ID 写入
`.costs`。`Plan Replay Sequence` 仍可定位该轮 `query.paths.jsonl`，但不能用于
跨轮代价匹配。按照当前实验约定，真实时间与 PostgreSQL cost 不做额外单位换算。

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
稳定 Path ID、数值范围以及 `startup_cost <= total_cost`，验证全部通过后才写出
文件。由于 Path JSONL 本身不知道哪些候选进入了最终执行计划，工具只初始化带格式
头的空稀疏 `.costs`；生成真实代价文件的调用方应从 EXPLAIN 取最终节点的 ID 和
实际时间，并只追加这些节点。

## 统一回放

代价回放使用 format 2 稀疏文件，只保存具有真实代价的最终计划节点：

```text
# pg_plan_replay_costs format=2
7ddc6720f8e6eec1354813d528c9d1f3 0.006 0.193
a1c23e5c2c978813f737dac41d99cd19 0.244 0.303
```

然后在相同的规划器环境下执行同一条语句：

```sql
SET plan_replay.path_replay_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/query.costs';
SET plan_replay.cardinality_replay_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/cardinality.rows';
SET plan_replay.mode = 'replay';
EXPLAIN SELECT * FROM pg_catalog.pg_class;
SET plan_replay.mode = 'off';
```

两个代价值必须是有限的非负数，并满足 `startup_cost <= total_cost`。当前候选 ID
命中时覆盖代价；新出现或未收录的候选保留 PostgreSQL 本轮原生代价；文件中已经
消失的 ID 被统计为 unmatched，不影响规划。旧的无 ID 两列文件、重复 ID、非法
格式都会立即报错，绝不退回 sequence 匹配。一组采集和回放文件只对应一条顶层
语句，不能由多个 backend 并发读写。

第一版稳定 ID 支持 Seq/Index/Index Only/Bitmap Scan、Nested Loop/Hash Join/
Merge Join、Material/Memoize、Sort/Aggregate/Limit、Append/MergeAppend 和
Gather/Gather Merge。遇到其他 Path 类型会 fail closed 并报告不支持，避免静默
产生不完整身份。

## 基数文件

基数 JSONL 每行对应一次基数估计调用，并包含从 0 开始的 `sequence`。格式 v2
同时包含回放前的 `native_rows` 和最终生效的 `effective_rows`；转换脚本优先使用
`effective_rows`，旧格式没有该字段时仍使用 `native_rows`。`kind`
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
`plan_replay_reset_stats()` 用于清空这些统计信息。返回结果还包含
`cost_replay_entries`、`cost_replay_applied` 和 `cost_replay_unmatched`，分别表示
稀疏文件中的 ID 数、当前候选命中次数以及本轮未出现的文件 ID 数。
