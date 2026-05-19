# Write examples — supported delta operations

Runnable SQL examples for every write operation the DuckDB `delta` extension supports today. For the inverse — what is *not* supported — see [`PROJECT_STATUS.md`](PROJECT_STATUS.md).

Each example assumes the extension is loaded:

```sql
INSTALL delta;
LOAD delta;
```

Examples use local paths for clarity; the same SQL works against `s3://`, `gs://`, `abfss://`, etc. when the matching httpfs / cloud extensions are configured.

## Append into an existing delta table

```sql
ATTACH '/path/to/existing/delta_table' AS t (TYPE delta);

INSERT INTO t VALUES (1), (2), (3);
INSERT INTO t SELECT i FROM range(10, 20) AS r(i);

SELECT count(*) FROM t;
```

Each `INSERT` produces one parquet data file plus one log entry (one `add` action per file). Inserts auto-commit unless wrapped in a transaction (see below).

## Append into a partitioned table

Partition columns are auto-detected from the existing table's metadata. The parquet copy is partition-aware — data is sharded by partition value, and the partition columns themselves are not stored inside the parquet files.

```sql
ATTACH '/path/to/partitioned_table' AS t (TYPE delta);

-- Suppose t has columns (i BIGINT, part BIGINT) with part as the partition column.
INSERT INTO t VALUES (10, 1), (11, 1), (20, 2);
```

## CREATE TABLE AS SELECT

Use the `allow_create true` ATTACH option to create new tables under the attached catalog. The catalog alias and the table inside it are addressed as `<alias>.<table>`.

```sql
ATTACH '/path/for/new/table' AS t (TYPE delta, allow_create true);

CREATE TABLE t.t AS
SELECT id, name FROM source_table;

SELECT count(*) FROM t.t;
```

CTAS supports complex types, partitioning (`PARTITIONED BY`), `TIMESTAMP_NTZ`, and empty `SELECT`s. `CREATE OR REPLACE TABLE AS SELECT` is not supported.

## Multi-statement transactions

Multiple `INSERT`s inside a single `BEGIN`/`COMMIT` commit atomically:

```sql
BEGIN TRANSACTION;
INSERT INTO t VALUES (100);
INSERT INTO t VALUES (101);
INSERT INTO t VALUES (102);
COMMIT;
```

All three inserts become visible together, in a single delta log version. Each `INSERT` produces its own parquet file.

## Rollback

```sql
BEGIN TRANSACTION;
INSERT INTO t VALUES (200);
INSERT INTO t VALUES (201);
ROLLBACK;   -- or: ABORT;
```

Staged parquet files are cleaned up from disk on rollback; no delta log entry is written. `ABORT` is an alias for `ROLLBACK`.

## Read-your-own-writes within a transaction

A `SELECT` inside an open transaction sees the rows inserted earlier in the same transaction:

```sql
BEGIN TRANSACTION;
INSERT INTO t VALUES (777);
SELECT count(*) FROM t;   -- includes 777
COMMIT;
```

Other connections only see the new rows after `COMMIT`.

## Idempotent writes

The Delta protocol's transaction identifier (`appId` + `version`) lets a producer mark a commit so it is not double-applied on retry. The extension exposes this via two table functions:

```sql
-- inside a transaction, mark the upcoming commit:
BEGIN TRANSACTION;
CALL delta_set_transaction_version('t', 'my_app', 5::UBIGINT, 4::UBIGINT);
--                                  table alias   app id   new ver    expected previous ver (NULL on first commit)
INSERT INTO t SELECT * FROM batch_5_input;
COMMIT;

-- read back the last-committed version for an app id:
SELECT version FROM delta_get_transaction_version('t', 'my_app');
```

If `expected_previous_version` does not match what was last committed for that `app_id`, the `COMMIT` fails with a `TransactionContext Error` and no row is added. On `ROLLBACK`/`ABORT` the version stays at its previous value.

Note: within one transaction, calling `delta_set_transaction_version` twice for the same `app_id` keeps the *first* value (subsequent calls are no-ops). Use one call per app per transaction.

## Catalog-managed Commits v2 (Unity Catalog)

When `unity_catalog true` is set on `ATTACH`, the actual commit is delegated to a `__internal_delta_ccv2_commit_staged` table function on a parent catalog (typically a Unity Catalog attachment). `unity_table_id` is required — the committer rejects commits whose `io.unitycatalog.tableId` does not match the registered table. The producer-side SQL is unchanged — `INSERT` / CTAS / etc. — only the ATTACH options differ:

```sql
-- parent unity catalog already attached as `uc`
ATTACH '/path/to/delta_table' AS t (
  TYPE delta,
  unity_catalog true,
  parent_catalog 'uc',
  unity_table_id '01234567-89ab-cdef-0123-456789abcdef'
);

INSERT INTO t VALUES (1);   -- routes through uc's commit function
```

`parent_commit true` is accepted as a deprecated alias for `unity_catalog true`.

## Checkpoint

```sql
ATTACH '/path/to/delta_table' AS t (TYPE delta);
INSERT INTO t VALUES (1);
INSERT INTO t VALUES (2);

CHECKPOINT t;
```

Produces a `_delta_log/<v>.checkpoint.parquet` plus a `_last_checkpoint` pointer so future readers can skip the JSON log files up to version `v`. `CHECKPOINT` on a table that already has a checkpoint at the current version is a no-op; `FORCE CHECKPOINT` is accepted but currently equivalent (the kernel does not yet honor a force flag).

## What is not supported

`UPDATE`, `DELETE`, `MERGE`, `INSERT OR REPLACE` / overwrite, `CREATE OR REPLACE TABLE`, `DROP TABLE`, `ALTER TABLE`, `OPTIMIZE`, `VACUUM`, `INSERT ... RETURNING`, `INSERT ... ON CONFLICT`, schema evolution on append, deletion-vector writes, change data feed emission, and liquid clustering all throw at plan time. See [`PROJECT_STATUS.md`](PROJECT_STATUS.md) for the full matrix.
