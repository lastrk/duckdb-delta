# Architecture Plan: CREATE TABLE AS SELECT (CTAS) for the `delta` extension

Status: design only, no implementation.
Target seam: `DeltaCatalog::PlanCreateTableAs` + `DeltaSchemaEntry::CreateTable`
+ a CTAS path on the existing `DeltaInsert` operator.

---

## 1. Domain Constraints

### Delta Lake (protocol)
- A Delta table's identity is its `_delta_log/` directory at the table root.
  Creating a table means writing commit `00000000000000000000.json`
  containing, at minimum:
  - one `protocol` action (minReaderVersion / minWriterVersion + reader /
    writer features),
  - one `metaData` action (id, name, description, format,
    schemaString, partitionColumns, configuration, createdTime),
  - optionally zero or more `add` actions if the CTAS produced data rows.
- The commit must be atomic. On filesystems that don't support atomic
  rename (S3 without locking) the kernel relies on a commit coordinator or
  catalog-managed commit. For local FS, the standard rename-into-place
  contract applies.
- The order of actions inside the commit JSON has no semantic meaning, but
  the file must be a JSON-lines stream where each line is exactly one
  action object.
- Schema in the metadata is a serialized Delta StructType (not Arrow, not
  parquet). Field IDs are not required at v0 unless column mapping is
  enabled.
- Partition columns are top-level only; they live in the metadata's
  `partitionColumns` array and are *not* materialized inside the parquet
  files (same as the existing `INSERT` blind-append path).
- For `CREATE TABLE … AS SELECT` with zero rows, the commit is still
  valid (Protocol + Metadata only).

### DuckDB (engine API)
- The CTAS pipeline is fixed: parser binds `CreateTableInfo` →
  `LogicalCreateTable` → `PhysicalPlanGenerator::CreatePlan(LogicalCreateTable&)`
  invokes the child plan and then calls
  `op.schema.catalog.PlanCreateTableAs(...)`
  (see `/workspace/duckdb/src/execution/physical_plan/plan_create_table.cpp`).
- The contract for `PlanCreateTableAs` returns a single `PhysicalOperator`
  that BOTH (a) consumes the `BoundCreateTableInfo` (creating the table),
  AND (b) sinks the rows from `plan` into it. The reference for this
  pattern is `PhysicalInsert::GetGlobalSinkState` — it sees `info` is set,
  calls `catalog.CreateTable(...)` lazily at sink-init time, then proceeds
  as a normal insert (`/workspace/duckdb/src/execution/operator/persistent/physical_insert.cpp:104-119`).
- The schema-side hook is `SchemaCatalogEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo&)`.
  For `delta` we override this in `DeltaSchemaEntry`, currently throwing
  `BinderException`.
- `RETURNING` is not yet supported for delta inserts and stays unsupported
  for delta CTAS too — same `BinderException` precedent.
- `ON CONFLICT` (i.e. `CREATE OR REPLACE TABLE`) needs an explicit
  decision; see Open Questions.

### FFI boundary
- `delta-kernel-rs` v0.21.0 (pinned in `CMakeLists.txt:148`) is the
  contract. We already use:
  - `ffi::get_engine_builder` / `ffi::builder_build` to construct an
    engine on a path (path doesn't have to point to a valid snapshot).
  - `ffi::get_snapshot_builder` / `ffi::snapshot_builder_build` to load
    an existing snapshot. **This fails when no `_delta_log` exists.**
  - `ffi::transaction(path_slice, engine)` to start a transaction on an
    existing table (loads the latest snapshot under the hood).
  - `ffi::with_engine_info`, `ffi::add_files`, `ffi::commit`,
    `ffi::free_transaction`.
- **Critical gap**: in v0.21 the kernel does NOT expose an FFI entry
  point for creating a brand-new table (no `ffi::create_table`,
  `ffi::transaction_create`, `ffi::empty_snapshot`, or similar — the FFI
  surface enumerated in `src/include/delta_utils.hpp` and used throughout
  `src/` has no such call). The kernel's Rust API
  (`delta_kernel::transaction::Transaction::new`) does support creating
  the first commit when the snapshot is fresh, but this is not exported
  through the FFI at v0.21.
- This means a "pure-kernel" CTAS implementation requires either
  (a) a kernel version bump that adds the create-table FFI, or
  (b) writing the initial Protocol+Metadata commit manually from C++
  (using DuckDB's filesystem layer and a hand-rolled JSON writer), then
  letting the existing `ffi::transaction` / `ffi::add_files` / `ffi::commit`
  pipeline handle the data add.
- Strategy (b) is the recommended path because (i) it keeps the kernel
  pin unchanged and (ii) the JSON for a v1/v2 first commit is small and
  stable. See FFI Plan section.

### ATTACH semantics
- `ATTACH 'path' AS x (TYPE delta)` today assumes the path exists and
  has a valid `_delta_log`. The `DeltaCatalogAttach` callback in
  `src/delta_extension.cpp:21` doesn't currently care, but
  `DeltaSchemaEntry::LookupEntry` will fail when the snapshot can't be
  loaded.
- Two distinct CTAS scenarios must be supported:
  1. **"Adopt path"**: user does `ATTACH 'path' AS db (TYPE delta);
     CREATE TABLE db.x AS SELECT …` where the path does not yet contain
     a delta table. Today this fails at lookup time. We need ATTACH to
     tolerate a missing snapshot when no version is pinned, *and* the
     default-table lookup must not run snapshot init eagerly.
  2. **"Function-form" CTAS** like `CREATE TABLE foo AS FROM
     delta_scan(...)` — this is just a DuckDB-managed CTAS into the
     default DuckDB catalog and is already supported. Not in scope.
- We pick scenario 1 only. The `child_catalog_mode`/`parent_commit` flag
  in `delta_insert.cpp:315` already handles Unity-managed catalogs — for
  Unity-managed CTAS the parent catalog must be the one allocating the
  table id, so this design defers that to a follow-up (see Open Questions).

---

## 2. Affected Surfaces

### `src/storage/`
- `delta_catalog.cpp` — fill in `PlanCreateTableAs`; small change to
  `LookupSchema` is not needed (we keep the single-schema model).
- `delta_schema_entry.cpp` — replace `CreateTable` body with a real
  implementation that builds a `DeltaTableEntry` for a brand-new table
  and arranges for the initial commit to be staged in the
  `DeltaTransaction`.
- `delta_insert.cpp` — extend the CTAS code path of the existing
  `DeltaInsert` operator (the second constructor at lines 34-38 already
  takes `info` for this).
- `delta_transaction.cpp` — extend `DeltaTransaction` with a
  "transaction-for-fresh-table" initialization mode; this is where the
  initial Protocol+Metadata commit is staged. Also a new
  `CreateTableMode` for the kernel-transaction-vs-bootstrap-JSON split.

### `src/include/storage/`
- `delta_catalog.hpp` — no signature change (override already declared);
  add a single private helper if needed.
- `delta_schema_entry.hpp` — add `CreateTableEntryForNewTable(...)` or
  similar (see Key Types).
- `delta_insert.hpp` — already supports the CTAS constructor; add
  bookkeeping fields if needed (none expected).
- `delta_transaction.hpp` — extend with a `CreateTable(...)` /
  `InitializeForNewTable(...)` entry point that produces the initial
  commit blob, plus state to remember the new table's schema until
  commit time.

### `src/functions/delta_scan/` (read-only consequences)
- `delta_multi_file_list.hpp` / `.cpp` — no API change. However, we need
  to verify that creating a `DeltaMultiFileList` against a path whose
  `_delta_log` was just freshly written by us still works for the
  subsequent first scan in the same session. This is just behavior
  validation, no code change. (The kernel reloads on `ATTACH` lookup.)

### `src/`
- New file: `src/storage/delta_initial_commit.cpp` — owns the
  Protocol+Metadata JSON synthesis. Pure C++, no FFI; uses DuckDB
  `FileSystem` for atomic-rename creation of `00000000000000000000.json`.
  This is the deliberate compromise for the kernel gap.

### `src/include/storage/`
- New file: `src/include/storage/delta_initial_commit.hpp` — declares
  the `DeltaInitialCommitWriter` type and a `DeltaSchemaSerializer`
  helper that turns a DuckDB `ColumnList` + constraints into a Delta
  StructType JSON string.

### `src/delta_extension.cpp`
- `DeltaCatalogAttach` — tolerate ATTACH-on-missing-path when (a) access
  mode is `READ_WRITE` and (b) `version` is not pinned. New attach option
  `allow_create = true` (default `false`) gates the relaxation explicitly
  so we never silently mask a typo. See Open Questions.
- `DeltaSchemaEntry::LookupEntry` must NOT throw when the schema's
  default table doesn't yet exist; today the path is implicit (it just
  doesn't find the table). Verify on read.

### `scripts/ffi/`
- No changes. We deliberately avoid touching the generated FFI header
  or the prefix/suffix.inc.

### `test/sql/`
- New directory: `test/sql/main/writing/ctas/` with:
  - `basic_ctas.test` — happy path on a brand-new path.
  - `ctas_with_partitions.test` — `PARTITION BY` columns become Delta
    partition columns.
  - `ctas_empty_select.test` — zero-row CTAS still produces a valid
    table (Protocol+Metadata only commit).
  - `ctas_attach_existing.test` — CTAS against a path that already has a
    `_delta_log` must fail with `CatalogException`.
  - `ctas_unsupported_types.test` — error messages for unsupported types
    (variant w/ NOT NULL in array, etc.) match the INSERT error paths.
  - `ctas_in_transaction.test` — CTAS inside a `BEGIN`/`COMMIT` and the
    `ROLLBACK` case (no files left behind, no `_delta_log` created).
- New directory: `test/sql/issues/` not used here (no GH issue yet).
- New directory: `test/sql/generated/writing/ctas/` reserved for tests
  that need PySpark to consume the resulting table — keep this empty in
  the first PR; cross-engine round-trip can land in a follow-up.

### `CMakeLists.txt`
- Add the two new files (`delta_initial_commit.cpp` and its header) to
  the extension sources list. No new dependencies.

---

## 3. Ownership Map

```
DeltaCatalog                                                 (catalog)
└── DeltaSchemaEntry          unique_ptr                     (catalog)
    └── DeltaTableEntry       unique_ptr (transient, new)    (created at PlanCreateTableAs time)
        └── DeltaMultiFileList shared_ptr (DuckDB API mandates shared)
            ├── KernelExternEngine        UniqueKernelPointer<SharedExternEngine, free_engine>
            └── shared_ptr<SharedKernelSnapshot>   (NULL until first scan after commit;
                                                     deferred snapshot for fresh tables)

DeltaTransaction              (per client-context)
├── kernel_transaction        KernelExclusiveTransaction (unique, RAII)
├── write_entry               optional_ptr<DeltaTableEntry>   (borrow only)
├── outstanding_appends       vector<DeltaDataFile>           (owned)
├── pending_initial_commit    unique_ptr<DeltaInitialCommit>  (NEW — owned)
└── parent_table_entry        optional_ptr<TableCatalogEntry> (borrow, for CCV2)

DeltaInsert (PhysicalOperator, CTAS arm)
├── schema                    optional_ptr<SchemaCatalogEntry> (borrow)
├── info                      unique_ptr<BoundCreateTableInfo>  (owned, consumed at sink init)
└── physical_copy children    standard DuckDB ownership

DeltaInitialCommitWriter (new, owned by DeltaTransaction::pending_initial_commit)
├── schema_json               string                          (owned, computed once)
├── partition_columns         vector<string>                  (owned)
├── protocol_version          uint32_t reader/writer (plain values)
└── (no kernel handles)        ← deliberate: stays off the FFI
```

**No `shared_ptr` is added**. The only `shared_ptr<DeltaMultiFileList>`
that exists is the one DuckDB's `TableCatalogEntry` already mandates; for
a fresh CTAS the table entry's `snapshot` field is created lazily after
commit (so a reader running in the same session can `FROM x`).

**No kernel handle is double-owned**. `kernel_transaction` lives only in
`DeltaTransaction`. The new `DeltaInitialCommitWriter` deliberately holds
zero kernel handles.

---

## 4. Module Layout

```
src/
├── delta_extension.cpp                       MOD: tolerate ATTACH on a path with no _delta_log,
│                                                  gated by new attach option `allow_create`.
├── storage/
│   ├── delta_catalog.cpp                     MOD: implement PlanCreateTableAs.
│   ├── delta_schema_entry.cpp                MOD: implement CreateTable; add helper
│   │                                              CreateTableEntryForNewTable().
│   ├── delta_insert.cpp                      MOD: CTAS arm of GetGlobalSinkState + Finalize.
│   ├── delta_transaction.cpp                 MOD: new InitializeForNewTable() path,
│   │                                              CommitInitialIfPending(), Rollback cleanup of
│   │                                              partially-created _delta_log dir.
│   ├── delta_initial_commit.cpp              NEW: synthesizes the v0 commit JSON and writes it
│   │                                              atomically through DuckDB's FileSystem.
│   ├── delta_table_entry.cpp                 unchanged (constructor already accepts CreateTableInfo).
│   ├── delta_transaction_manager.cpp         unchanged.
├── include/storage/
│   ├── delta_initial_commit.hpp              NEW: declares DeltaInitialCommitWriter +
│   │                                              DeltaSchemaSerializer.
│   ├── delta_schema_entry.hpp                MOD: add CreateTableEntryForNewTable() signature.
│   ├── delta_transaction.hpp                 MOD: add InitializeForNewTable() and
│   │                                              SetPendingInitialCommit().
│   ├── delta_catalog.hpp                     unchanged (override declared).
│   └── delta_insert.hpp                      unchanged.

test/sql/main/writing/ctas/
├── basic_ctas.test                                NEW
├── ctas_with_partitions.test                      NEW
├── ctas_empty_select.test                         NEW
├── ctas_attach_existing.test                      NEW
├── ctas_unsupported_types.test                    NEW
└── ctas_in_transaction.test                       NEW
```

---

## 5. Key Types (skeletons only — no method bodies)

### 5.1 Initial-commit writer (no FFI)

```cpp
// src/include/storage/delta_initial_commit.hpp

namespace duckdb {

class ColumnList;
class FileSystem;

//! Describes the actions written to commit 00000000000000000000.json
//! when a Delta table is created. Does NOT touch the kernel.
//! Lives in DeltaTransaction; constructed at CreateTable time, consumed
//! at Commit time.
class DeltaInitialCommit {
public:
	DeltaInitialCommit(string table_path, string table_name, string table_id,
	                   vector<string> partition_columns, string schema_string,
	                   uint32_t min_reader_version, uint32_t min_writer_version);

	//! Atomically write _delta_log/00000000000000000000.json to disk.
	//! Throws IOException on FS failure or if the commit file already exists.
	void WriteCommit(ClientContext &context, FileSystem &fs,
	                 const vector<DeltaDataFile> &add_files,
	                 timestamp_t commit_timestamp) const;

	//! Rollback support: removes the freshly created _delta_log directory
	//! if and only if it was created by this object and is still empty.
	void TryCleanup(FileSystem &fs) const noexcept;

	const string &table_path;
	const string &table_name;
	const string &table_id;           // freshly generated UUID
	const vector<string> &partition_columns;
	const string &schema_string;      // Delta-protocol StructType JSON
	uint32_t min_reader_version;
	uint32_t min_writer_version;

private:
	string log_dir_path;              // <table_path>/_delta_log
	string commit_file_path;          // <log_dir_path>/00000000000000000000.json
	mutable bool dir_was_created_by_us = false;
};

//! Converts a DuckDB ColumnList + NOT NULL constraints into the Delta
//! protocol's StructType JSON. Pure function; no I/O, no kernel.
class DeltaSchemaSerializer {
public:
	//! Returns the schemaString to embed in the metaData action. Throws
	//! BinderException if a type can't be represented in Delta.
	static string SerializeSchema(const ColumnList &columns,
	                              const vector<unique_ptr<Constraint>> &constraints);

	//! Returns the set of partition columns, validated to be top-level scalars.
	//! Throws BinderException for nested or unsupported partition keys.
	static vector<string> ResolvePartitionColumns(
	    const ColumnList &columns,
	    const vector<unique_ptr<ParsedExpression>> &partition_keys);
};

} // namespace duckdb
```

### 5.2 Transaction extension

```cpp
// addition to src/include/storage/delta_transaction.hpp

enum class DeltaTransactionMode : uint8_t {
	REGULAR,           // table exists; ffi::transaction(path) path
	CREATING_TABLE     // table is being created in this transaction
};

class DeltaTransaction : public Transaction {
public:
	// ... existing API ...

	//! Variant of InitializeTransaction() for CTAS. We delay starting the
	//! kernel transaction until we know the writer can succeed (after the
	//! _delta_log/00.json is staged at Finalize time).
	void InitializeForNewTable(ClientContext &context,
	                           DeltaTableEntry &new_table_entry,
	                           unique_ptr<DeltaInitialCommit> initial_commit);

	bool HasPendingInitialCommit() const;

private:
	DeltaTransactionMode mode = DeltaTransactionMode::REGULAR;
	unique_ptr<DeltaInitialCommit> pending_initial_commit; // CTAS only
};
```

### 5.3 Schema entry extension

```cpp
// addition to src/include/storage/delta_schema_entry.hpp

class DeltaSchemaEntry : public SchemaCatalogEntry {
public:
	// ... existing API ...

	//! Constructs the in-memory DeltaTableEntry for a CTAS target.
	//! Does NOT touch the filesystem. Validates schema types and partition
	//! columns and assigns a freshly generated table_id. The caller (the
	//! DeltaTransaction) is responsible for actually writing 00.json at
	//! commit time.
	unique_ptr<DeltaTableEntry>
	CreateTableEntryForNewTable(ClientContext &context,
	                            BoundCreateTableInfo &info,
	                            unique_ptr<DeltaInitialCommit> &out_initial_commit);
};
```

### 5.4 No new attach option type
The single new attach option `allow_create` (Boolean, default false) is
stored as `bool DeltaCatalog::allow_create_on_attach`. No new type needed.

---

## 6. FFI Plan

### What does NOT change
- The generated FFI header is untouched. `prefix.inc` / `suffix.inc` /
  generator script: untouched.
- `delta-kernel-rs` `GIT_TAG` stays at `v0.21.0`. **A kernel bump is not
  authorized by this design** and is listed as an open question.

### What handles cross the FFI for CTAS
- *None new*, by design. We use the existing
  `ffi::get_engine_builder` → `ffi::builder_build` to get an engine for
  the (freshly created) table path. Then, just like the INSERT case:
  - `ffi::transaction(path_slice, engine)` — at this point the path now
    has a `_delta_log/00.json` we wrote ourselves, so the kernel sees a
    valid v0 snapshot and constructs a transaction whose base version is
    0. The next commit we issue (`ffi::commit`) writes commit `01.json`
    containing the `add` actions for the CTAS-produced parquet files.
  - `ffi::add_files`, `ffi::with_engine_info`, `ffi::commit`,
    `ffi::free_transaction`.

### What the FFI does NOT do
- The Protocol + Metadata initial commit is written by us, not the kernel.
  This is the explicit kernel gap mitigation. The synthesized JSON is
  small and stable; the format is defined in the Delta protocol spec
  (`Protocol` and `Metadata` action types) and is what every Delta writer
  produces.

### Why we don't try to fold the Protocol+Metadata into a kernel
`ffi::transaction` call
- v0.21's `ffi::transaction` requires the path to already be a Delta
  table; it calls `Snapshot::try_new()` internally and errors out if
  there is no `_delta_log`.
- Writing the initial commit on the C++ side is simpler, deterministic,
  and contains no FFI risk (no kernel handles to leak).
- When the kernel grows a `create_table` FFI we can flip the
  implementation to use it transparently, since `DeltaInitialCommit` is
  the only call site.

### Callback re-entrancy
- We add zero new C-callbacks-from-Rust. The existing
  `DeltaTransaction::CommitCallback` (Unity Catalog) path is *not* touched
  for the first PR — see Open Questions about CTAS under
  `parent_commit=true`.

### String lifetimes
- All strings we hand to the kernel (table path, app_id, engine info)
  follow the existing `KernelUtils::ToDeltaString` contract: the string
  buffer must outlive the FFI call. No change.

---

## 7. Concurrency Plan

### State split
- **Bind data**: the CTAS bind data is what DuckDB constructs for
  `LogicalCreateTable` — namely `BoundCreateTableInfo`. Immutable after
  bind. Lives in `DeltaInsert::info`.
- **Global sink state** (`DeltaInsertGlobalState`): single instance.
  Today it holds `written_files`, `insert_count`, `columns`. For CTAS
  the same struct is reused — we just compute `columns` from `info`
  instead of from a pre-existing `DeltaTableEntry`. Guarded by
  `ParallelSink() == false` (already the case).
- **Local sink state**: not used today; no change.

### Lock ordering
- `DeltaTransaction::lock` is the existing pinch point. New CTAS calls
  must enter at most one of:
  - `lock` to set `pending_initial_commit` and `write_entry`,
  - then release before invoking the kernel.
- **Never** hold `lock` across `DeltaInitialCommit::WriteCommit` (which
  does FS I/O), nor across `ffi::transaction` / `ffi::commit`.

### Kernel re-entry
- Same rule as today: no `DeltaTransaction::lock` held across any `ffi::*`
  call. The CTAS path strengthens this because `WriteCommit` may sleep
  on FS.

### Cancellation
- `DeltaInsert::ParallelSink() == false`, so no per-thread cancellation
  fan-out is needed.
- `WriteCommit` should poll DuckDB's `InterruptState` between
  (a) creating `_delta_log/`, (b) writing the temp file, (c) renaming —
  three poll points are sufficient and cheap.
- A cancellation between (a) and (c) must trigger
  `DeltaInitialCommit::TryCleanup`, which is invoked from
  `DeltaTransaction::Rollback` (and from `~DeltaInitialCommit` as a last
  resort, noexcept).

---

## 8. Error Strategy

| Failure                                                               | Exception type          | Where         |
|-----------------------------------------------------------------------|-------------------------|---------------|
| Path already contains a `_delta_log`                                  | `CatalogException`      | `CreateTable` |
| Path exists and is not a directory                                    | `IOException`           | `CreateTable` |
| Path's parent doesn't exist / can't be created                        | `IOException`           | `WriteCommit` |
| Schema contains a DuckDB type that has no Delta representation        | `BinderException`       | `DeltaSchemaSerializer::SerializeSchema` |
| Partition column refers to a nested or non-existent field             | `BinderException`       | `DeltaSchemaSerializer::ResolvePartitionColumns` |
| `RETURNING` clause requested                                          | `BinderException`       | `PlanCreateTableAs` |
| `OR REPLACE` requested (decide: refuse v1)                            | `BinderException`       | `PlanCreateTableAs` |
| ATTACH path doesn't exist and `allow_create=false`                    | `CatalogException`      | `DeltaCatalogAttach` |
| ATTACH succeeded but lookup before CTAS finds no table                | (returns nullptr, not throw) | `LookupEntry` |
| Catalog access mode is `READ_ONLY`                                    | `InvalidInputException` | `CreateTable` (matches existing append behavior) |
| `parent_commit=true` (Unity CCV2) CTAS                                | `NotImplementedException` v1 | `PlanCreateTableAs` |
| FS rename failed during commit write                                  | `IOException`           | `WriteCommit` |
| Kernel `ffi::transaction` fails after we wrote 00.json                | `IOException` via existing `KernelUtils::TryUnpackResult` | `InitializeTransaction` |
| Concurrent CTAS race (someone else wrote `00.json` between our check and our rename) | `TransactionException` | `WriteCommit` |
| Zero-row CTAS                                                          | (no exception — valid)  | n/a           |

Non-fatal:
- Zero `written_files` at finalize is **not** an error for CTAS. The
  initial commit's `add` action list is just empty.

---

## 9. Test Plan

All tests use `__TEST_DIR__/...` paths and the `require notwindows`
pattern matches the existing writing tests.

### `test/sql/main/writing/ctas/basic_ctas.test`
- ATTACH a brand-new path with `(TYPE delta, allow_create=true)`.
- `CREATE TABLE foo AS SELECT range AS i FROM range(10);`
- Verify `_delta_log/00000000000000000000.json` exists.
- Verify there is one or more `.parquet` file.
- `FROM foo` returns 10 rows.
- Re-attach in a fresh session and read again — confirms persisted state.

### `test/sql/main/writing/ctas/ctas_with_partitions.test`
- CTAS with `PARTITION BY (p)` and verify `_delta_log/00.json` contains
  `partitionColumns: ["p"]`.
- Read the table and verify partition pruning still works.

### `test/sql/main/writing/ctas/ctas_empty_select.test`
- CTAS from `SELECT … WHERE FALSE`.
- Verify `00.json` exists, no parquet files, `count(*) = 0`.

### `test/sql/main/writing/ctas/ctas_attach_existing.test`
- ATTACH an existing delta table path with `allow_create=true`.
- `CREATE TABLE foo AS ...` must fail with `CatalogException`
  (table already exists).

### `test/sql/main/writing/ctas/ctas_unsupported_types.test`
- CTAS of a column whose type has no Delta mapping fails with
  `BinderException`.
- `RETURNING` raises `BinderException`.

### `test/sql/main/writing/ctas/ctas_in_transaction.test`
- `BEGIN; CTAS; ROLLBACK;` leaves no `_delta_log` and no parquet.
- `BEGIN; CTAS; INSERT INTO new_table VALUES (...); COMMIT;` produces a
  single table with both the CTAS rows and the inserted rows, and the
  log has commits 00 (Protocol+Metadata+initial adds) and 01 (subsequent
  adds). Decide commit grouping; see Open Questions.

### `test/sql/main/writing/ctas/ctas_or_replace_unsupported.test`
- `CREATE OR REPLACE TABLE` fails with `BinderException` until we
  decide what semantics we want (see Open Questions).

Future (not in scope of v1):
- `test/sql/generated/writing/ctas/` — cross-engine round-trip with
  PySpark / delta-spark, gated on `GENERATED_DATA_AVAILABLE`.
- `test/sql/cloud/{minio_local,azurite}/ctas.test` — CTAS against object
  storage. Requires sorting out atomic-put semantics; deferred.

---

## 10. Open Questions

1. **Kernel bump for native `create_table` FFI.** Should we instead
   request that delta-kernel-rs export the table-creation flow through
   the FFI and bump our `GIT_TAG`? A kernel-native creation would be
   strictly better long-term (handles row-group statistics integration,
   future protocol changes, catalog-managed commits v2). But the user
   has not authorized a `GIT_TAG` bump, so v1 ships the
   `DeltaInitialCommit` workaround. **Requires explicit user
   authorization before changing CMakeLists.txt.**

2. **ATTACH UX for empty paths.** Are we comfortable adding a new
   attach option `allow_create` (default false), or do we prefer
   inferring from access mode (`READ_WRITE` + missing path ⇒ allow)?
   The explicit-flag option is safer (no silent typo-creates-a-table),
   but adds surface area.

3. **`CREATE OR REPLACE TABLE` semantics.** Replacing a Delta table in
   place would mean writing a metadata commit that changes the schema
   while still referencing the existing log — not a true replace. For
   v1 we propose `BinderException`. Alternatively we can treat it as
   "drop the `_delta_log` and rewrite from scratch" but that loses time
   travel; we should explicitly decide.

4. **Initial commit grouping inside an active transaction.**
   `BEGIN; CTAS; INSERT; COMMIT;` — should the inserted rows land in
   commit `00.json` (as `add` actions alongside Protocol+Metadata)
   or in commit `01.json`? Folding into one commit is friendlier for
   readers but more complex (we'd need to defer 00.json write until
   commit time and bundle all `add`s). For v1 we propose:
   write 00.json at Finalize of the CTAS sink (Protocol + Metadata + the
   CTAS adds), then 01.json for subsequent inserts in the same
   transaction. Confirm.

5. **Unity Catalog managed-commit (CCV2) CTAS.** When `parent_commit=
   true`, the parent catalog must allocate the new table id and the
   commit must go through `__internal_delta_ccv2_commit_staged`. v1
   refuses CCV2 CTAS with `NotImplementedException`. Confirm this is
   acceptable.

6. **Column mapping mode.** Should newly created tables enable
   `delta.columnMapping.mode = name` by default? It costs a writer
   feature flag but is what most modern producers do. Default for v1
   proposed: column mapping disabled (writer features minimal).

7. **Min protocol versions.** v1 will write
   `minReaderVersion = 1, minWriterVersion = 2` (no writer features
   required). If we want deletion vectors / column mapping later we'll
   bump to v3/v7 + features. Confirm.

8. **Time semantics of `createdTime` in metaData.** Use
   `Timestamp::GetCurrentTimestamp()`? Yes — but call this out so the
   test fixtures don't pin the value.

9. **Determinism of `table_id` (UUID).** Each CTAS generates a fresh
   UUID v4. This means tests can't assert on the value; they assert on
   structure only. Confirm.

10. **Sort keys / order keys.** `CreateTableInfo::sort_keys` is parsed
    by DuckDB but Delta does not have a first-class concept of sort
    keys in the protocol. Proposal: ignore them with a warning, OR
    raise `BinderException`. Confirm.
