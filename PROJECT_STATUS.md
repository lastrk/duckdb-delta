# Project status — write operations

Status of the DuckDB `delta` extension's write surface, mapped against the Apache Spark Delta Lake reference implementation.

The writer is intentionally narrow today. The planner entry points in `src/storage/delta_catalog.cpp:102-113` make this explicit: `PlanInsert` has a real implementation (`src/storage/delta_insert.cpp:304`), while `PlanCreateTableAs`, `PlanDelete`, and `PlanUpdate` all just `throw NotImplementedException`. `DeltaSchemaEntry::CreateTable`, `CreateView`, `CreateIndex`, and `Alter` likewise throw (`src/storage/delta_schema_entry.cpp:36-96`).

## Implemented

| Spark / Delta op | DuckDB form | Where |
|---|---|---|
| `df.write.format("delta").mode("append").save(...)` — append/blind insert | `INSERT INTO t SELECT ...` against an `ATTACH`-ed delta table | `DeltaInsert` + `DeltaCatalog::PlanInsert` (`src/storage/delta_insert.cpp`) |
| Append into partitioned tables (partition columns auto-detected, parquet copy is partition-aware) | Same `INSERT INTO`; partition columns are read via `GetPartitionColumns()` in PlanInsert | `src/storage/delta_insert.cpp:341-353` |
| Idempotent transactional writes (`SetTransaction(appId, version)`) | Helper table functions for the `appId`/`version` protocol | `src/functions/delta_transaction_utils/idempotency_helpers.cpp`, test `test/sql/generated/writing/idempotent_writes.test` |
| Catalog-managed Commits v2 (Unity Catalog) | `parent_commit` ATTACH option delegating to `__internal_delta_ccv2_commit_staged` | `src/storage/delta_transaction.cpp:509` |
| Transactional multi-statement appends | `BEGIN; INSERT…; INSERT…; COMMIT;` | `test/sql/main/writing/transaction_multi_insert.test` |
| Stats collection for appended files (min/max/null counts, including nested / list) | Wired through the parquet copy + Delta `add` action stats | `test/sql/generated/writing/append/write_stats*.test` |

## Not implemented (throws today)

| Spark / Delta op | DuckDB form that would trigger it | Status |
|---|---|---|
| `df.write.format("delta").mode("overwrite")` and overwrite-by-partition | `INSERT OR REPLACE`, `INSERT INTO … OVERWRITE` | No path — blind insert only; `op.on_conflict_info.action_type != THROW` is rejected at `src/storage/delta_insert.cpp:309` |
| `DELETE FROM t WHERE …` | `DELETE FROM delta_t WHERE …` | `PlanDelete` throws (`src/storage/delta_catalog.cpp:106-108`) |
| `UPDATE t SET …` | `UPDATE delta_t SET …` | `PlanUpdate` throws (`src/storage/delta_catalog.cpp:110-112`) |
| `MERGE INTO t USING s ON … WHEN MATCHED …` | DuckDB has no native `MERGE`, but it would route through `PlanUpdate`/`PlanDelete`/`PlanInsert` anyway | Blocked by missing update/delete |
| `CREATE TABLE … USING delta` (managed or external, schema-only) | `CREATE TABLE t(...)` against a delta-attached DB | `DeltaSchemaEntry::CreateTable` throws (`src/storage/delta_schema_entry.cpp:36-38`) |
| `CREATE TABLE … AS SELECT` (CTAS) | `CREATE TABLE t AS SELECT …` | `PlanCreateTableAs` throws (`src/storage/delta_catalog.cpp:102-104`) |
| `REPLACE TABLE` / `CREATE OR REPLACE TABLE` | same | Not wired |
| `ALTER TABLE` — add column, rename column, set tblproperties, set partition, change schema, column-mapping changes | `ALTER TABLE …` | `DeltaSchemaEntry::Alter` throws (`src/storage/delta_schema_entry.cpp:95-97`) |
| `DROP TABLE` | `DROP TABLE t` | `DeltaSchemaEntry::DropEntry` throws (`src/storage/delta_schema_entry.cpp:169`) |
| `OPTIMIZE` / bin-packing compaction, `ZORDER BY` | n/a | Not implemented |
| `VACUUM` (file cleanup) | n/a | Not implemented |
| Generated columns / `CHECK` constraints on write | n/a | Not implemented (table-creation path is blocked anyway) |
| `INSERT … RETURNING` | `INSERT … RETURNING *` | Explicitly rejected at `src/storage/delta_insert.cpp:306-308` |
| `INSERT … ON CONFLICT` (any non-THROW action) | `INSERT … ON CONFLICT DO …` | Rejected at `src/storage/delta_insert.cpp:309-311` |
| Schema evolution on append (`mergeSchema=true`, `overwriteSchema=true`) | n/a — schema must match | Not implemented |
| Change Data Feed write side (emitting `cdc` actions on updates/deletes/merges) | n/a | Tied to update/delete, neither exists |
| Deletion-vector writes (writes that emit DVs rather than rewriting files for delete/merge) | n/a | Read side handles DVs; writer never emits them |
| Liquid clustering (`CLUSTER BY`) | n/a | Not implemented |
| `CREATE VIEW` / `CREATE INDEX` on delta | same | Throw (`src/storage/delta_schema_entry.cpp:54-64`) |

## Summary

Reads cover the bulk of Delta's reader protocol (including DVs, partitioning, time travel, CCv2-flavored catalogs). On the write side only append into an existing table is implemented, plus the txn-idempotency and CCv2-commit plumbing that wraps it — everything else in Spark's Delta DML and DDL surface (overwrite, update, delete, merge, CTAS, alter, drop, optimize, vacuum, schema evolution) currently throws at plan time.
