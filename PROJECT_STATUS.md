# Project status — write operations

Status of the DuckDB `delta` extension's write surface, mapped against the Apache Spark Delta Lake reference implementation.

The writer is intentionally narrow today. The planner entry points in `src/storage/delta_catalog.cpp` make this explicit: `PlanInsert` (line 368) and `PlanCreateTableAs` (line 114) have real implementations; `PlanDelete` (line 220) and `PlanUpdate` (line 224) just `throw NotImplementedException`. On the DDL side, `DeltaSchemaEntry::CreateView`, `CreateIndex`, `Alter`, and `DropEntry` all throw (`src/storage/delta_schema_entry.cpp:109-237`); `DeltaSchemaEntry::CreateTable` only throws for `CREATE OR REPLACE TABLE` and otherwise returns `nullptr` to route through `PlanCreateTableAs`.

## Implemented

| Spark / Delta op | DuckDB form | Where |
|---|---|---|
| `df.write.format("delta").mode("append").save(...)` — append/blind insert | `INSERT INTO t SELECT ...` against an `ATTACH`-ed delta table | `DeltaInsert` + `DeltaCatalog::PlanInsert` (`src/storage/delta_insert.cpp`) |
| Append into partitioned tables (partition columns auto-detected, parquet copy is partition-aware) | Same `INSERT INTO`; partition columns are read via `GetPartitionColumns()` in PlanInsert | `src/storage/delta_insert.cpp:405-463` |
| Idempotent transactional writes (`SetTransaction(appId, version)`) | Helper table functions for the `appId`/`version` protocol | `src/functions/delta_transaction_utils/idempotency_helpers.cpp`, tests `test/sql/generated/writing/idempotent_writes.test`, `test/sql/main/writing/idempotent_writes.test` |
| Catalog-managed Commits v2 (Unity Catalog) | `unity_catalog true` ATTACH option (legacy alias `parent_commit true`) delegating to `__internal_delta_ccv2_commit_staged` | `src/storage/delta_transaction.cpp:703-715`, `src/delta_extension.cpp:52-117` |
| Transactional multi-statement appends | `BEGIN; INSERT…; INSERT…; COMMIT;` | `test/sql/main/writing/transaction_multi_insert.test` |
| Stats collection for appended files (min/max/null counts, including nested / list) | Wired through the parquet copy + Delta `add` action stats | `test/sql/generated/writing/append/write_stats*.test` |
| `CREATE TABLE … AS SELECT` (CTAS) into a fresh delta path, including partitioned, complex types, `TIMESTAMP_NTZ`, empty `SELECT`, and CCv2 | `ATTACH '…' AS t (TYPE delta, allow_create true); CREATE TABLE t.t AS SELECT …` | `DeltaCatalog::PlanCreateTableAs` (`src/storage/delta_catalog.cpp:114`), `test/sql/main/writing/ctas/` |
| `CHECKPOINT` (writes `_last_checkpoint` + a `<v>.checkpoint.parquet` so future readers can skip JSON log files up to version `v`) | `CHECKPOINT t` / `FORCE CHECKPOINT t` (force currently equivalent — kernel does not honor a force flag) | `test/sql/main/writing/checkpoint.test` |

## Not implemented (throws today)

| Spark / Delta op | DuckDB form that would trigger it | Status |
|---|---|---|
| `df.write.format("delta").mode("overwrite")` and overwrite-by-partition | `INSERT OR REPLACE`, `INSERT INTO … OVERWRITE` | No path — blind insert only; `op.on_conflict_info.action_type != THROW` is rejected at `src/storage/delta_insert.cpp:373-375` |
| `DELETE FROM t WHERE …` | `DELETE FROM delta_t WHERE …` | `PlanDelete` throws (`src/storage/delta_catalog.cpp:220-222`) |
| `UPDATE t SET …` | `UPDATE delta_t SET …` | `PlanUpdate` throws (`src/storage/delta_catalog.cpp:224-226`) |
| `MERGE INTO t USING s ON … WHEN MATCHED …` | DuckDB has no native `MERGE`, but it would route through `PlanUpdate`/`PlanDelete`/`PlanInsert` anyway | Blocked by missing update/delete |
| `CREATE TABLE … USING delta` (managed or external, schema-only — no `AS SELECT`) | `CREATE TABLE t.t(...)` against a delta-attached DB | `DeltaSchemaEntry::CreateTable` returns `nullptr` (`src/storage/delta_schema_entry.cpp:88-92`); there is no `PhysicalCreateTable` for the delta catalog, so only the CTAS path produces a usable table |
| `REPLACE TABLE` / `CREATE OR REPLACE TABLE` (and CTAS variant) | `CREATE OR REPLACE TABLE t.t [AS SELECT …]` | Rejected at `DeltaCatalog::PlanCreateTableAs` (`src/storage/delta_catalog.cpp:118-120`) and `DeltaSchemaEntry::CreateTable` (`src/storage/delta_schema_entry.cpp:37-39`) |
| `ALTER TABLE` — add column, rename column, set tblproperties, set partition, change schema, column-mapping changes | `ALTER TABLE …` | `DeltaSchemaEntry::Alter` throws (`src/storage/delta_schema_entry.cpp:150-152`) |
| `DROP TABLE` | `DROP TABLE t` | `DeltaSchemaEntry::DropEntry` throws (`src/storage/delta_schema_entry.cpp:236-237`) |
| `OPTIMIZE` / bin-packing compaction, `ZORDER BY` | n/a | Not implemented |
| `VACUUM` (file cleanup) | n/a | Not implemented |
| Generated columns / `CHECK` constraints on write | n/a | Not implemented (table-creation path is blocked anyway) |
| `INSERT … RETURNING` | `INSERT … RETURNING *` | Explicitly rejected at `src/storage/delta_insert.cpp:370-372` |
| `INSERT … ON CONFLICT` (any non-THROW action) | `INSERT … ON CONFLICT DO …` | Rejected at `src/storage/delta_insert.cpp:373-375` |
| Schema evolution on append (`mergeSchema=true`, `overwriteSchema=true`) | n/a — schema must match | Not implemented |
| Change Data Feed write side (emitting `cdc` actions on updates/deletes/merges) | n/a | Tied to update/delete, neither exists |
| Deletion-vector writes (writes that emit DVs rather than rewriting files for delete/merge) | n/a | Read side handles DVs; writer never emits them |
| Liquid clustering (`CLUSTER BY`) | n/a | Not implemented |
| `CREATE VIEW` / `CREATE INDEX` on delta | same | Throw (`src/storage/delta_schema_entry.cpp:109-120`) |

## Summary

Reads cover the bulk of Delta's reader protocol (including DVs, partitioning, time travel, CCv2-flavored catalogs). On the write side, append into an existing table and CTAS (including partitioned, complex types, `TIMESTAMP_NTZ`, and CCv2) are implemented, plus the txn-idempotency and CCv2-commit plumbing that wraps them — everything else in Spark's Delta DML and DDL surface (overwrite, update, delete, merge, alter, drop, optimize, vacuum, schema evolution) currently throws at plan time.
