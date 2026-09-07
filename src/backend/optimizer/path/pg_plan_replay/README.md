# pg_plan_replay 采集、稳定 ID 回放与在线代价预测

离线采集和回放不需要推理服务。在线 Path 代价预测通过独立 Python 服务完成。
`plan_replay.mode` 保留为同时控制两个离线通道的兼容入口；需要独立实验、迭代采集
或在线预测时使用 `plan_replay.path_mode` 和
`plan_replay.cardinality_mode`：

- Path 代价协议：采集候选 `Path` 树，以 128 位稳定 Path ID 稀疏回放
  `startup_cost` 和 `total_cost`；候选出现顺序只用于诊断。
- 基数协议：采集基础表和连接的基数估计输入，按估计调用顺序回放 `rows`。

两个通道都支持 `inherit`、`off`、`capture`、`replay` 和 `capture_replay`；
Path 通道另外支持 `predict` 和 `capture_predict`。默认 `inherit` 继承
`plan_replay.mode`。`capture_replay` 在同一次
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

## 在线模型代价预测

C 端按职责拆成三层：`pg_plan_replay.c` 只负责模式、离线通道和 planner 生命周期；
`online_prediction.c` 负责模型 Path 序列化、固定批量、原子代价应用及推理统计；
`model_client.c` 只负责 Unix socket 收发和顺序 costs 数组解析。

### 代码与模型资产边界

本目录不自带 E2E、QueryFormer、ZeroShot、DACE 或 QPPNet 的完整模型代码和
checkpoint。它提供的是 PostgreSQL 接入层、Path JSON 序列化、模型服务框架，以及把
Path JSON 转换为各模型原生 collator 输入的 adapter。

`pg_lcm.zip` 本身同样只包含 PostgreSQL 接入层、server 和 adapter，并不包含 `.pt`
checkpoint 或五种模型的完整源码。当前在线服务复用了它的对接方式，但仍必须通过
`--repository` 指定一个完整的外部 `lcm-eval` 仓库。服务运行时会从该仓库加载：

- `src/models` 和 `src/classes`：模型实现、原生 collator 和配置类。
- `data/models/<model>/<database>/<model>_<seed>.pt`：真实模型 checkpoint。
- `data/runs` 及 dataset 目录：特征统计、表列统计和 E2E/QueryFormer 所需的词向量等资产。

因此，服务进程确实会在本地直接加载 checkpoint 并执行模型 forward，而不是再把请求
转发给另一个推理服务；但模型实现和资产属于外部 `lcm-eval` 依赖。

`/home/pgdev/postgresql-18.4/Debug/lcm-eval` 是当前开发机上已准备好的一份
`lcm-eval` 工作目录，不是 `pg_plan_replay` 的编译产物，也不会由上面的
`make`/`make install` 生成。删除或移动该目录后，除非将 `--repository` 改为另一份完整的
`lcm-eval` 目录，否则真实模型服务无法启动。如果把模型服务部署到远端，这些模型代码、
checkpoint 和统计资产只需存在于远端模型机器。

### 启动本地模型服务

一个服务进程只加载一种模型；切换模型时停止旧进程再使用新的 `--model` 启动。
以当前开发机上的外部 `lcm-eval` 工作目录为例：

```bash
export LCM_EVAL_ROOT=/home/pgdev/postgresql-18.4/Debug/lcm-eval
cd "$LCM_EVAL_ROOT"
export PYTHONPATH=/home/pgdev/postgresql-18.4/src/backend/optimizer/path:$LCM_EVAL_ROOT/src:$LCM_EVAL_ROOT/.deps
export DGLBACKEND=pytorch
export DGLDEFAULTDIR=/tmp/pg-plan-replay-dgl
export MPLCONFIGDIR=/tmp/pg-plan-replay-matplotlib
python -m pg_plan_replay.service.server \
  --model e2e \
  --repository "$LCM_EVAL_ROOT" \
  --database imdb \
  --socket /tmp/pg_plan_replay.sock \
  --device cpu \
  --seed 0 \
  --output-scale 1000
```

PostgreSQL 端启用：

```sql
LOAD 'pg_plan_replay';
SET plan_replay.path_mode = 'predict';
SET plan_replay.cardinality_mode = 'off';
SET plan_replay.model_socket_path = '/tmp/pg_plan_replay.sock';
EXPLAIN (FORMAT JSON) SELECT * FROM pg_catalog.pg_class;
SELECT plan_replay_last_timing();
```

`capture_predict` 还需要配置 `path_capture_file`，并把模型生效后的候选 Path 写入
JSONL。在线基数预测不在当前范围内，但离线基数回放可与 `path_mode=predict` 组合。

预测协议使用一问一答的单行 JSON。请求中的 `paths` 与响应中的 `costs` 完全按数组
位置对应，不发送或校验 request ID、index、stable Path ID。后端只保留 costs 数组
可解析、数量相等和代价为有限正数三项最低安全检查。模型仅覆盖 `total_cost`，启动代价取：

```text
startup_cost = min(native_startup_cost, predicted_total_cost)
```

当前实现固定使用 60 秒单批绝对超时和 256 个 Path 的最大批量。连接、超时、协议
或预测结果异常都会直接终止规划。多个子批次全部成功后才会应用预测，不会产生
半批注入。

每个在线 Path 除原生代价、行数、宽度、关系、并行属性和递归子 Path 外，还会发送
以下可选字段：

- `filter_conditions`：从基础关系的 `baserestrictinfo`、参数化 Path 的
  `ppi_clauses` 和连接 Path 的 `joinrestrictinfo` 提取。结构化节点包含操作符、
  物理表名、物理列名、schema、SQL alias、常量值及其类型 OID；支持
  `AND`/`OR`/`NOT`、普通二元比较、BooleanTest、`IS [NOT] NULL` 和
  `= ANY(array)`（序列化为 `IN`）。`NOT` 会通过操作符 negator 和 De Morgan
  规则转换，不在请求中引入新的 NOT 类别。
- `join_conditions`：同一批 qual 中可以解析为 `Var op Var` 的连接条件。
- `output_columns`：从 `PathTarget` 提取基础列，并标记 checkpoint 支持的
  `COUNT`、`SUM`、`MIN`、`MAX` 和 `AVG` 聚合。
- `sort_keys`、`scan_direction`、`join_type`、`aggregate_strategy` 和
  `partial_mode`：从对应的专用 Path 结构提取。Sort Key 还保留 ASC/DESC、
  NULLS FIRST/LAST、collation OID 和 operator-family OID。
- `relation_identities`：保留每个关系的 RT index、schema、物理表名和 SQL alias；
  `relation_names` 继续保留为旧 adapter 使用的物理表名数组。

qual 的入口与 `end-to-end/benchmark.patch` 一致，都是实际
`RestrictInfo->clause`，并通过 planner range table 解析 Var。在线协议不发送
`FuncExpr` 的文本近似：现有 checkpoint 只能把过滤映射到统计信息中的基础列，无法
准确表达函数结果，因而这类以及其他不支持的表达式会被跳过。`OR` 只在所有分支均可
表达且能映射到模型统计字典时才会使用，避免改变布尔语义。

adapter 只消费对应 checkpoint 已有 featurization 能接收的字段：E2E 使用真实过滤和
输出列，QueryFormer 使用表、过滤和已知连接条件，ZeroShot 使用过滤和输出列，DACE
仍只使用操作符/代价/基数，QPPNet 使用可获得的 scan、join、aggregate、sort 属性。
训练时依赖执行期采样、Sort Method 等 planner 阶段没有的数据仍使用原有默认值；
`sample_vec` 继续使用全零向量。因此新增字段是向后兼容的可选输入，不改变协议版本。

### 当前 checkpoint 不消费的请求字段

请求 JSON 可以比已发布 checkpoint 的输入张量更丰富。下列字段用于保持 planner
语义、诊断或未来模型适配；它们不会因为出现在 JSON 中就自动进入当前模型：

| 请求字段 | 当前不消费它的模型或说明 |
|---|---|
| `path_tag`、`plan_tag` | 五个模型都不消费；模型使用归一化后的 `op_name` |
| `startup_cost` | 五个模型都不消费；DACE/QPPNet 只使用 total cost |
| `parallel_aware`、`parallel_safe` | 五个模型都不直接消费；ZeroShot 使用 `parallel_workers`，QPPNet 依赖并行 operator 名称 |
| `pathkeys` 数量 | 五个模型都不消费；QPPNet 只消费可映射到字典的首个 `Sort Key` |
| `relids`、`required_outer` | 五个模型都不消费 |
| `relation_identities` 以及条件中的 schema/alias | 五个模型都不消费；当前映射仍以物理表名和列名查静态统计字典 |
| `literal_type_oid` | 五个模型都不消费；literal 类型由模型侧 column statistics 决定 |
| Sort Key 的 direction、NULLS、collation/opfamily | 五个模型都不消费；QPPNet 只编码 Sort Key 类别和默认 Sort Method |
| `join_conditions` | 只有 QueryFormer 消费，而且只接受 checkpoint 字典中已有的连接字符串 |
| `output_columns` | ZeroShot 消费列和聚合；E2E 只消费列 ID；QueryFormer、DACE、QPPNet 不消费 |
| `filter_conditions` | E2E、QueryFormer、ZeroShot 消费；QPPNet 仅把它转成 Filter 以推导 Min/Max/Mean；DACE 不消费 |
| `native_total_cost` | DACE 和 QPPNet 消费；E2E、QueryFormer、ZeroShot 不消费 |

因此，增加上述字段本身不会提升不消费它的 checkpoint；要使用它们必须修改
featurization/collator、训练数据和 checkpoint 输入维度并重新训练。

### 模型接受但在线输入仍缺失或使用默认值

下面才是当前实现仍存在的模型输入缺口：

| 模型输入 | 当前在线行为 | 原因或后续方向 |
|---|---|---|
| E2E/QueryFormer 的 1000 位 `sample_vec` | 有过滤时仍发送全零向量 | 训练值来自对基础表样本实际执行谓词；planner Path 本身没有样本命中位图，需要服务持有同版本样本并执行谓词 |
| QueryFormer 的未知 table/join 类别 | table 使用 dummy，未知 join condition 丢弃；最多保留 checkpoint 允许的 6 个过滤节点和 5 个 join | checkpoint 是固定 categorical dictionary 和固定张量形状，不能通过新增 JSON 类别扩容 |
| ZeroShot/E2E/QueryFormer 使用的 table/column statistics | 不从当前 PostgreSQL 实时发送；adapter 读取 lcm-eval repository 中的静态 statistics | 当前数据库统计变化后可能与训练资产不一致；若要补，需要定义统计快照协议并保证与 checkpoint 的表列编号一致 |
| QPPNet `Sort Method` | 使用 checkpoint 字典中的 `quicksort` 或首个默认值 | Sort Method 由 executor 决定，Path 阶段不可知 |
| QPPNet `Hash Buckets`、`Peak Memory Usage` | 当前 Path 树通常没有独立 Hash 节点，也没有填入这两个值 | 它们是执行期指标；planner 只有部分估计量，不能等价替代真实值 |
| QPPNet 根 Join 的 `Parent Relationship` | 根节点没有父角色，使用 `Inner` 兼容值；嵌套 Join 已按 child 位置使用 Outer/Inner | checkpoint 把该字段定义为必需类别，但根节点语义上不存在 parent relationship |

本轮已经补上模型原本接受、且能从现有输入准确计算的 `literal_feature`：`IN`
沿用训练 parser 的逗号数，`LIKE`/`NOT LIKE` 使用 `%` 通配符数量。其余普通比较为 0。

### 表达式协议边界（不等同于模型输入缺口）

以下表达式目前不会被结构化为 filter/join leaf；这表示 C 端协议尚不能准确表达它们，
而不是某个 checkpoint 少接收了一个现成字段：

- 函数或算术结果参与比较，例如 `lower(name) = 'x'`、`a + b > 10`、
  `coalesce(a, 0) = 1`。
- `Param`、SubPlan 或其他非 Const 值，例如 `column = $1`、`column IN (subquery)`。
- `CoerceViaIO`、domain/array coercion 等复杂 cast；目前只透明剥离 `RelabelType`。
- `RowCompareExpr`、`DistinctExpr`、CASE、COALESCE、NULLIF、全文检索、JSON/range/
  spatial 专用表达式。
- `<> ALL(array)`/`NOT IN`、非等值 ANY/ALL，以及包含非常量元素的数组表达式。
- join leaf 只支持直接的 `Var op Var`；函数连接、算术连接、row join、OR join 和
  Subquery/CTE 输出列连接尚不能表达。schema/alias 已保留在 JSON 中，但
  QueryFormer 仍按物理表列和已有 join 字典映射，因此 self join 不能获得新的模型类别。
- Var 必须来自当前查询层级的普通 `RTE_RELATION` 且 `varattno > 0`；系统列、whole-row
  Var、Subquery/CTE/VALUES/Function RTE 输出不支持。

对于 AND，不能表达或不能映射的 conjunct 可以被省略，模型看到的是可表达子集；
对于 OR，任一分支失败就整体回退，避免改变 OR 语义。

### 其他尚未进入在线模型输入的 Path 语义

这些信息 PostgreSQL planner 中部分可取，但当前五个 checkpoint 没有对应编码，所以
本轮没有为了“字段齐全”而重复发送：

- IndexClause、derived/lossy index qual、Index Cond/Recheck Cond 与普通 Filter 的用途区分，
  index selectivity、index total cost 和 index order-by 表达式。
- Hash/Merge 使用的专属 join clauses、Hash `num_batches`、Merge 两侧 sort keys、
  presorted-key 数和 materialize/mark-restore 决策。
- join 的 `inner_unique`、SEMI/ANTI early-stop 因子及不同 child 的 rescan cost。
- Aggregate 的 group keys、`numGroups`、transition/final cost；Window、Unique、SetOp
  等 upper Path 的类型专属字段。
- Limit 的 offset/count 估计，Memoize 的 cache keys/calls/entries，Append/MergeAppend
  的分区及裁剪语义，Gather 的 single-copy 等属性。
- 参数化 Path 目前只有 `required_outer` relids，没有参数表达式和参数来源；也没有
  单独发送 page/tuple/selectivity、PathTarget 表达式文本、typmod 或表达式 collation。

若将来训练新的 checkpoint，应先从这一节选择字段并同时修改 C 协议、adapter、
collator 和训练数据，而不是只扩展请求 JSON。

## 统一采集

```sql
LOAD 'pg_plan_replay';
SET plan_replay.path_capture_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/query.paths.jsonl';
SET plan_replay.cardinality_capture_file = '/home/pgdev/postgresql-18.4/src/backend/optimizer/path/pg_plan_replay/test/cardinality.jsonl';
SET plan_replay.mode = 'capture';
EXPLAIN ANALYZE SELECT * FROM pg_catalog.pg_class;
SET plan_replay.mode = 'off';
```

开始规划顶层语句时，采集文件会被清空。默认使用紧凑格式 v6，每一行对应一个
候选 Path，并带有从 0 开始的 `sequence` 序号：

```json
{
  "format_version": 6,
  "sequence": 0,
  "path_id": "7ddc6720f8e6eec1354813d528c9d1f3",
  "cost_replay_applied": false,
  "model_prediction_applied": false,
  "cost_source": "native",
  "native_startup_cost": 0,
  "native_total_cost": 20.225,
  "effective_startup_cost": 0,
  "effective_total_cost": 20.225,
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
`join_type`。当前格式不重复输出查询文本、所有 planner GUC、全表列定义、
`RelOptInfo`、`PathTarget`、`raw_query`、`raw_path` 和节点专属内部 dump。
过滤条件和连接条件属于独立的基数采集协议，不在 Path 代价文件中重复记录。

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

当前版本提供 `plan_replay_last_timing()`。`paths` 和 `batches` 返回最近一次成功规划
处理的 Path 和关系批次数量；`inference_ms`、`model_batches`、`model_paths`、
`model_applied` 和 `fallbacks` 记录在线预测工作。
`plan_replay_reset_stats()` 用于清空这些统计信息。返回结果还包含
`cost_replay_entries`、`cost_replay_applied` 和 `cost_replay_unmatched`，分别表示
稀疏文件中的 ID 数、当前候选命中次数以及本轮未出现的文件 ID 数。
