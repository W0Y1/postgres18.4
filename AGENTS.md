# PostgreSQL 本地开发命令

以下命令均在仓库根目录 `/home/pgdev/postgresql-18.4` 中执行。

## Debug 环境

- 安装目录：`Debug/install`
- 数据目录：`Debug/data`
- 日志文件：`Debug/postgres.log`
- 开发端口：`5433`

如需在当前终端直接使用 PostgreSQL 命令，先设置：

```bash
export PATH=/home/pgdev/postgresql-18.4/Debug/install/bin:$PATH
```

## 初始化数据库

数据目录只需初始化一次。若 `Debug/data/PG_VERSION` 已存在，不要再次运行
`initdb`，否则可能破坏现有数据库。

```bash
initdb -D /home/pgdev/postgresql-18.4/Debug/data
```

## 启动数据库

```bash
pg_ctl \
  -D /home/pgdev/postgresql-18.4/Debug/data \
  -l /home/pgdev/postgresql-18.4/Debug/postgres.log \
  -o "-p 5433" \
  -w start
```

## 查看运行状态

```bash
pg_ctl -D /home/pgdev/postgresql-18.4/Debug/data status
```

## 连接数据库

```bash
psql -p 5433 -d postgres
```

退出 `psql` 客户端但保持数据库运行：

```text
\q
```

## 关闭数据库

开发环境默认使用 fast 模式安全关闭：

```bash
pg_ctl \
  -D /home/pgdev/postgresql-18.4/Debug/data \
  -m fast \
  -w stop
```

不要使用 `kill -9` 关闭数据库，除非正在处理无法正常关闭的故障。

## 查看日志

```bash
tail -f /home/pgdev/postgresql-18.4/Debug/postgres.log
```

如果以后编译 Release 版本，不得让 Debug 和 Release 实例同时使用同一个
数据目录。切换二进制版本前，必须先用 `pg_ctl ... -m fast -w stop` 完整关闭
当前实例。
