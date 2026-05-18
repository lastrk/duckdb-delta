# DELETE for Delta tables — architecture plan

Status: WIP — substantial implementation already on `main`; this revision
captures the audit and the concrete completion tasks.
Author: architect agent
Target file to retire: `src/storage/delta_catalog.cpp` `PlanDelete`
NotImplementedException stub (already removed; replaced by the
implementation in `src/storage/delta_delete.cpp`).

---

## 1. Scope decision: file-level DELETE only, with strict "all-or-nothing" guard

**Decision: Scenario (I) — file-level DELETE only in this PR.** Predicate must
prove every row in a candidate file matches (file is *fully covered* by the
filter); any row-level match that does not align with file boundaries is
rejected with `NotImplementedException` pointing at CTAS as the workaround.

### Rationale (Layer 3 → 2 → 1)

**Layer 3 (domain).** Kernel v0.23's `remove_files` is unambiguously
**file-granular**: the `selection_vector` chooses rows of the *file-metadata
table* returned by `scan_metadata_next_arrow`, not rows of user data. The
kernel exposes no writer-side deletion-vector primitive and no
"rewrite file" primitive at this version. Copy-on-Write (CoW) would
require us to implement, in C++, a Delta-aware mini-engine that scans
matched files, applies the predicate, writes the survivors, and stages
an Add+Remove pair — a parallel subsystem to the INSERT pipeline.
Outside this PR.

**Layer 2 (design).** The single most load-bearing user case is
"DELETE FROM t WHERE …". The largest cohort of useful DELETEs in
analytics workloads is on partition keys ("drop a day", "drop a tenant"),
which **are file-aligned by construction**. Implementing only
file-level DELETE captures that cohort with code that is symmetric to
existing INSERT/CTAS plumbing (RAII handles, transaction lifecycle,
CCv2 commit path, error mapping). The CoW v2 — which would land later
— does not need to change the v1 plan or the user-facing operator; it
only changes the "fallback" branch of the file-selection algorithm.

**Layer 1 (mechanics).** v1 reuses every existing kernel handle
wrapper (`KernelExclusiveTransaction`, `KernelEngineData`,
`KernelExternEngine`, `KernelCommittedTransaction`). The only new RAII
wrapper introduced is for `ScanMetadataArrowResult*` (file-local in
`delta_multi_file_list.cpp`).

### v1 acceptance contract

`DELETE FROM t WHERE <predicate>` succeeds when the predicate is
provably file-level — i.e. every active row of every selected file is
matched, and no row of any *unselected* file is matched. Otherwise
`NotImplementedException` is thrown with a message naming the workaround.

The tests pin the exact substrings the error messages must contain:
- `delete_read_only.test`              → `Cannot delete from a read-only Delta table`
- `delete_returning_rejected.test`     → `RETURNING clause is not yet supported`
- `delete_row_level_rejected.test`     → `Row-level DELETE is not yet supported`

### Explicit non-goals (in this PR)

- Row-level DELETE / Copy-on-Write file rewrites
- Writer-side deletion vectors (kernel does not expose them at v0.23)
- `MERGE`
- `UPDATE` (still throws NotImplementedException as before)
- `RETURNING` clause for DELETE (throws BinderException)
- `DELETE FROM t USING …` (refused; see Open Question OQ-A on whether a
  dedicated `delete_using_rejected.test` is required)

---

## WIP Audit

### What is DONE (already on disk)

**Source files**

- `src/include/storage/delta_delete.hpp` — `FileLevelDeletePlan` POD
  struct and `DeltaDelete` operator class declaration are in place and
  match the architecture intent.
- `src/storage/delta_delete.cpp` — implements:
  - `DeltaDelete` constructor and sink/source lifecycle methods.
  - `DeltaDeleteGlobalState` (sink global state with `delete_count`).
  - `Sink` returns `NEED_MORE_INPUT` and ignores chunks (correct for v1).
  - `Finalize` calls `DeltaTransaction::RemoveFiles` (once) when
    `files_to_remove` is non-empty; copies the precomputed
    `total_rows_removed` into the global state.
  - `GetDataInternal` emits a one-row, one-column BIGINT.
  - Free helpers `ExpressionRefersOnlyPartitionColumns` /
    `ExpressionsAllReferOnlyPartitionColumns` (file-local statics).
  - `DeltaCatalog::PlanDelete` — performs RETURNING / READ_ONLY /
    USING guards, walks the child plan, distinguishes
    `predicate_was_true` (no WHERE) from the partition-column path,
    runs `ComplexFilterPushdown` and reads file_number/cardinality from
    `DeltaFileMetaData` to build `FileLevelDeletePlan.files_to_remove`.
  - Note: the file currently constructs the candidate file index from
    `meta.file_number` of each remaining (post-pushdown) file (not
    from the pruned-list position). That is correct, **provided**
    `meta.file_number` is the index into the original snapshot's
    `resolved_files`, which is how `ScanDataCallBack::VisitCallbackInternal`
    sets it. The mapping is intentional but fragile — a one-line
    invariant comment in `DeltaFileMetaData::file_number` should
    reinforce this in `Completion Task 8`.

- `src/include/storage/delta_transaction.hpp` — adds `RemoveFiles(...)`
  declaration and a private `idx_t outstanding_removes = 0` counter.
  Doc comment is aligned with the architecture plan.
- `src/storage/delta_transaction.cpp` — implements:
  - `RemoveFiles`: short-circuits on empty; D_ASSERTs not-CTAS; lazily
    calls `InitializeTransaction`; delegates the kernel work to
    `DeltaMultiFileList::StageRemoveFiles`; bumps `outstanding_removes`.
  - `Commit`: the gate is now `!outstanding_appends.empty() ||
    outstanding_removes > 0`, matching the architecture spec.

- `src/include/functions/delta_scan/delta_multi_file_list.hpp` —
  declares `StageRemoveFiles(ClientContext &, const vector<idx_t> &,
  KernelExclusiveTransaction &)`. **Note:** this is a different shape
  from the originally-planned `BuildRemoveFilesEngineData(...)` out-param
  helper. The implemented shape calls `ffi::remove_files` from inside
  the helper rather than returning the engine_data to the caller. This
  is a cleaner ownership split and **the implemented shape wins**.

- `src/functions/delta_scan/delta_multi_file_list.cpp` —
  `StageRemoveFiles` implementation:
  - Opens a fresh `ffi::scan(snapshot, engine, nullptr, nullptr)` (no
    predicate visitor; the iterator visits every file in snapshot order).
  - Iterates `scan_metadata_next_arrow` until exhausted.
  - For each Arrow batch, builds a per-batch `vector<uint8_t>`
    selection vector where row `i` is `1` iff
    `(global_file_idx + i)` is in the requested removal set.
  - Calls `ffi::get_engine_data` to convert the batch into an
    `ExclusiveEngineData` handle, then `ffi::remove_files` (only when
    the batch contains at least one selected file).
  - Uses a file-local `ScanMetadataArrowResultGuard` for the
    `ScanMetadataArrowResult*` and **explicitly nulls
    `raw_result->arrow_data.array.release` after `get_engine_data`**
    to avoid a double-free of the ArrowArray (the kernel's
    `free_scan_metadata_arrow_result` would otherwise re-release it).
    This is the empirical resolution of original OQ-3.

- `src/include/storage/delta_table_entry.hpp` — adds
  `GetRowIdColumns() const override { return {}; }`. This prevents
  DuckDB's `BindRowIdColumns` from projecting `COLUMN_IDENTIFIER_ROW_ID`
  into the scan, which would otherwise fail because the parquet
  multifile reader does not produce that virtual column. With this
  override, the v1 contract (static file-level proof at plan time) is
  consistent with the bound child plan.

- `src/storage/delta_table_entry.cpp` — sets
  `this->snapshot->table_entry = this;` in `GetScanFunctionInternal` so
  that `DeltaScanGetBindInfo` can return a `BindInfo(table_entry)` and
  DuckDB's DELETE/UPDATE planner can resolve the catalog entry.
- `src/functions/delta_scan/delta_scan.cpp` — adds
  `DeltaScanGetBindInfo` and wires it into the table function
  (`function.get_bind_info = DeltaScanGetBindInfo`). Falls back to
  `BindInfo(ScanType::EXTERNAL)` for the bare `delta_scan('…')`
  function path.

- `src/storage/delta_catalog.cpp` — removes the `PlanDelete` stub and
  adds a comment pointing at `delta_delete.cpp` (the implementation
  lives there alongside `DeltaDelete`).

**Build**

- `CMakeLists.txt` — `src/storage/delta_delete.cpp` is in
  `EXTENSION_SOURCES`. Kernel `GIT_TAG` is `v0.23.0` (original OQ-6
  resolved — no further bump needed).

**Tests**

- `test/sql/main/writing/delete/delete_full_table.test` — three parts
  (non-partitioned, partitioned, empty), each with re-attach to verify
  persistence.
- `test/sql/main/writing/delete/delete_partition_aligned.test` — six
  parts: single value, multi-row partition, no-match, multi-column
  partition, sequential DELETEs, NULL-partition delete.
- `test/sql/main/writing/delete/delete_read_only.test` — error contract.
- `test/sql/main/writing/delete/delete_returning_rejected.test` —
  error contract.
- `test/sql/main/writing/delete/delete_row_level_rejected.test` —
  three rejection cases (data column, mixed predicate, non-partitioned
  table with data column).
- `test/sql/main/writing/delete/delete_then_insert.test` — DELETE then
  INSERT in the same session, re-attach for persistence.
- `test/sql/main/writing/delete/delete_transaction.test` — rollback +
  commit round-trips with explicit transactions.

### What is PARTIAL

1. **`StageRemoveFiles` — partition pruning interaction.**
   `StageRemoveFiles` always opens a *fresh* `ffi::scan` over the
   snapshot with **no filters**. This is correct because the
   `file_indices` it receives are already pre-pruned. However, the
   `global_file_idx` counter advances over **every file in the
   unfiltered snapshot**, while `file_indices` are
   `DeltaFileMetaData::file_number` values from the *unfiltered*
   snapshot (set during the initial `ScanDataCallBack::VisitCallbackInternal`
   sweep). The two enumerations must agree row-for-row.

   **Risk:** if the kernel ever batches `scan_metadata_next_arrow`
   differently from `scan_metadata_next` (e.g., applies a different
   row order, or filters inactive Add actions differently), the indices
   could drift. This is implicitly assumed correct today. Worth adding
   an explicit invariant assertion when the test suite is green: when
   the iterator is exhausted, `global_file_idx` must equal
   `snapshot.GetTotalFileCount()`. See Completion Task 2.

2. **Logging.** `DeltaTransaction::Commit` only emits the
   `delta.Commit` debug log when going through the kernel commit
   path. The `delta_kernel_logging` setting is not propagated to
   the new `StageRemoveFiles` path; today no log line announces a
   DELETE staging event. Not required for correctness; flagged for
   Completion Task 9 (nice-to-have).

3. **Test for empty-table DELETE.** `delete_full_table.test` Part 3
   covers the "DELETE on empty table → 0 rows, no commit" case. The
   architecture's OQ-1 short-circuit decision (no commit when
   `outstanding_appends.empty() && outstanding_removes == 0`) is
   implicitly tested by `SELECT count(*)` post-DELETE returning 0,
   but **no test asserts that no new log entry was written**. If the
   short-circuit is to be a guaranteed observable behavior, add a
   probe via `delta_file_list` to check the log version did not
   advance. See Completion Task 7.

4. **Sequential DELETE under one transaction.** The architecture spec
   says multiple `RemoveFiles` calls in one DeltaTransaction must share
   one `kernel_transaction`, with `outstanding_removes` accumulating.
   That is what the code does. However, `delete_partition_aligned.test`
   Part 5 only covers sequential **auto-commit** DELETEs. There is no
   test where multiple DELETEs occur inside an explicit
   `BEGIN…COMMIT` block (the architecture's Transaction Lifecycle
   diagram shows `DELETE p1; DELETE p2; INSERT;` all under one
   commit). See Completion Task 6.

5. **CCv2 / `parent_commit=true`.** The plan calls for a
   `test/sql/main/writing/delete/ccv2/delete_ccv2.test` mirroring
   `ctas_ccv2.test`. **It is missing.** The transaction commit path
   already routes through the UC committer when `parent_commit` is set
   (no code branch needed in DELETE; `remove_files` does not care
   about the committer), so this is a pure test addition. See
   Completion Task 5.

### What is MISSING (in priority order)

- **M1.** `_temp_test.test` is a scratch file (description: "temp"),
  attaches as `t1` and runs `DELETE FROM t1.t1`. It should be
  deleted before merge — it duplicates `delete_full_table.test` Part 1
  with weaker coverage.
- **M2.** `delete_using_rejected.test` (referenced in §10 of the
  original plan) is **not** present. The `DELETE … USING` rejection
  is wired in `PlanDelete` (the cross-product walk throws
  `NotImplementedException`), but there is no test fixing the
  contract. Decision needed in OQ-A: add the test, or drop the
  intent and document that USING is rejected indirectly by the
  "cross-product child not supported" guard.
- **M3.** `delete/ccv2/delete_ccv2.test` (see PARTIAL #5).
- **M4.** `delete_transaction.test` does not cover the
  multi-operation case `BEGIN; DELETE…; INSERT…; COMMIT;` — see
  PARTIAL #4.
- **M5.** No assertion that "0 rows deleted" produces no log advance
  — see PARTIAL #3.
- **M6.** No TSAN run was recorded for the new code paths. The
  architecture plan calls for `SANITIZER_MODE=thread make debug && make
  test_debug` over the multi-op transaction test.

### What CONTRADICTS the original plan, and which wins

- **`BuildRemoveFilesEngineData` → `StageRemoveFiles`.** The plan
  specified a helper that returned engine_data and a selection vector
  by out-params; the implementation moved the entire batched call into
  the helper. **Implemented shape wins** — it keeps the kernel handle
  inside the file that owns the snapshot lock, avoids leaking
  `ExclusiveEngineData` ownership to the transaction layer, and
  matches the per-batch streaming model that `scan_metadata_next_arrow`
  actually exposes (multiple batches → multiple `remove_files` calls).
  Update the plan text to reflect this (done above).
- **`OQ-3` (ArrowArray double-free).** The implementation chose the
  "extract array by value, null the release callback in the struct,
  let the guard drop the rest" pattern. **This is the correct empirical
  resolution.** Resolved.
- **`OQ-6` (kernel v0.23 bump).** Already bumped on `main`. Resolved.

---

## Completion Tasks (in dependency order)

Each task is implementable independently by the coder agent. After every
task: `make debug && make test_debug` should pass.

### Task 1 — Sanity sweep on the current implementation

- Compile-test the current WIP: `make debug` (no source changes).
- Run the new test set: `build/debug/test/unittest "test/sql/main/writing/delete/*"`.
- File-level smoke: confirm `delete_full_table`, `delete_partition_aligned`,
  `delete_read_only`, `delete_returning_rejected`,
  `delete_row_level_rejected`, `delete_then_insert`, `delete_transaction`
  all pass.
- **Do not change behavior.** If anything fails here, root-cause and
  fix the minimum needed to make the current contract green before
  proceeding.

### Task 2 — Invariant assertion in `StageRemoveFiles`

After the `scan_metadata_next_arrow` loop exits in
`DeltaMultiFileList::StageRemoveFiles`, add:

```cpp
D_ASSERT(global_file_idx == GetTotalFileCountInternal());
```

(holding the snapshot lock for the read). This pins the invariant that
the unfiltered scan iteration order matches the original
`ScanDataCallBack::VisitCallbackInternal` enumeration that produced
`file_number`. If this ever fires under a future kernel bump, the
DELETE → file_number mapping has drifted and must be revisited.

### Task 3 — Delete scratch `_temp_test.test`

Remove `test/sql/main/writing/delete/_temp_test.test`. Its three
non-trivial assertions are already covered by `delete_full_table.test`
Part 1.

### Task 4 — Decide on `delete_using_rejected.test` (see OQ-A)

If the user wants the USING rejection covered:
add `test/sql/main/writing/delete/delete_using_rejected.test`:

```
statement ok
ATTACH '__TEST_DIR__/delete_using' AS t (TYPE delta, allow_create true);

statement ok
CREATE TABLE t.t AS SELECT range AS id FROM range(5);

statement ok
CREATE TABLE other AS SELECT 1 AS id;

statement error
DELETE FROM t USING other WHERE t.id = other.id;
----
DELETE ... USING / join is not yet supported

statement ok
DETACH t;
```

If the user does **not** want this test, no work needed; the guard in
`PlanDelete` is already present.

### Task 5 — Add `delete_transaction.test` Part 3: multi-op transaction

Extend `test/sql/main/writing/delete/delete_transaction.test` with a
third part that exercises the shared-`kernel_transaction` codepath:

```
ATTACH '…' AS t3 (TYPE delta, allow_create true);
CREATE TABLE t3.t3 PARTITIONED BY (region) AS
SELECT * FROM (VALUES (1,'east'),(2,'west')) v(id,region);

BEGIN;
DELETE FROM t3 WHERE region = 'east';   # 1 row
INSERT INTO t3 VALUES (3,'north');
COMMIT;

# Expect 2 rows total: (2,'west'),(3,'north')
# Re-attach to verify a SINGLE log version advance.
```

To assert single-commit semantics, use
`SELECT max(version) FROM delta_file_list('…/_delta_log')` (or whichever
helper is conventional in the test suite — check `delete_full_table` and
existing CTAS tests for the idiom). If no convenient helper exists,
verifying behavior via re-attach and the row count is sufficient for v1.

### Task 6 — Add `delete/ccv2/delete_ccv2.test`

Mirror `test/sql/main/writing/ctas/ccv2/ctas_ccv2.test` exactly,
substituting a partition-aligned DELETE for the CTAS body:
1. Use the same `__internal_delta_test_ccv2_commit_staged` table
   function path.
2. ATTACH with `parent_commit=true`, `unity_table_id='…'`, etc.
3. CTAS-create a partitioned table.
4. DELETE FROM t WHERE part = 'x'.
5. Verify the staged commit lands and the table state matches.

If `ctas_ccv2.test` does not exist as a fixture today (check the
directory), defer to a future PR and update OQ-B with the decision.

### Task 7 — (optional, low priority) Empty-DELETE log assertion

Extend `delete_full_table.test` Part 3 to assert that the log version
did not advance when a DELETE removed zero rows. Pseudocode:

```
# before
SELECT max(version) FROM delta_file_list('…/_delta_log')
----
0

DELETE FROM t3
----
0

SELECT max(version) FROM delta_file_list('…/_delta_log')
----
0
```

If `delta_file_list` does not expose a max-version helper, skip this
task — the short-circuit is implicitly tested by the existing assertions.

### Task 8 — Documentation comment on `DeltaFileMetaData::file_number`

In `src/include/functions/delta_scan/delta_multi_file_list.hpp`,
strengthen the doc comment on `idx_t file_number`:

```
//! Position of this file within the *unfiltered* snapshot, as
//! enumerated by the initial scan_metadata_next visit order. Used by
//! StageRemoveFiles to address files in the kernel's selection vector.
//! Must remain in lock-step with the order produced by
//! scan_metadata_next_arrow over a no-filter scan of the same snapshot.
```

This makes the DELETE invariant explicit at the struct definition.

### Task 9 — (optional) Logging line for DELETE staging

In `DeltaTransaction::RemoveFiles` (post-StageRemoveFiles, before
`outstanding_removes` bump), emit:

```cpp
DUCKDB_LOG_INTERNAL(context, "delta.RemoveFiles", LogLevel::LOG_DEBUG,
    "Staged %s remove actions for %s", to_string(file_indices.size()),
    snapshot.GetPath());
```

Matches the precedent in `Append` / `Commit`.

### Task 10 — TSAN sweep

Run `SANITIZER_MODE=thread make debug && make test_debug` over the
DELETE test set:

```
build/debug/test/unittest "test/sql/main/writing/delete/*"
```

If `StageRemoveFiles`'s "release snapshot lock, then iterate" pattern
triggers a TSAN warning under multi-statement contention, revisit the
lock scope (the iteration is documented as safe because the iterator
is independent of the snapshot lock after creation).

---

## 2. Affected surfaces (after audit)

### Implemented and tracked (working tree changes)

- `src/include/storage/delta_delete.hpp` (NEW)
- `src/storage/delta_delete.cpp` (NEW; contains both `DeltaDelete` and
  `DeltaCatalog::PlanDelete`)
- `src/include/storage/delta_transaction.hpp` (MOD: `RemoveFiles` decl,
  `outstanding_removes` field)
- `src/storage/delta_transaction.cpp` (MOD: `RemoveFiles` impl, Commit
  gate update)
- `src/include/functions/delta_scan/delta_multi_file_list.hpp` (MOD:
  `StageRemoveFiles` decl)
- `src/functions/delta_scan/delta_multi_file_list.cpp` (MOD:
  `StageRemoveFiles` impl with `ScanMetadataArrowResultGuard`)
- `src/include/storage/delta_table_entry.hpp` (MOD: `GetRowIdColumns`
  override returning empty)
- `src/storage/delta_table_entry.cpp` (MOD: snapshot->table_entry
  back-pointer)
- `src/functions/delta_scan/delta_scan.cpp` (MOD: `DeltaScanGetBindInfo`)
- `src/storage/delta_catalog.cpp` (MOD: removed PlanDelete stub)
- `CMakeLists.txt` (MOD: delta_delete.cpp in EXTENSION_SOURCES)

### Test files (working tree)

- `test/sql/main/writing/delete/delete_full_table.test` (DONE)
- `test/sql/main/writing/delete/delete_partition_aligned.test` (DONE)
- `test/sql/main/writing/delete/delete_read_only.test` (DONE)
- `test/sql/main/writing/delete/delete_returning_rejected.test` (DONE)
- `test/sql/main/writing/delete/delete_row_level_rejected.test` (DONE)
- `test/sql/main/writing/delete/delete_then_insert.test` (DONE)
- `test/sql/main/writing/delete/delete_transaction.test` (DONE; extend
  per Completion Task 5)
- `test/sql/main/writing/delete/_temp_test.test` (DELETE per Completion
  Task 3)
- `test/sql/main/writing/delete/delete_using_rejected.test` (depends
  on OQ-A)
- `test/sql/main/writing/delete/ccv2/delete_ccv2.test` (per
  Completion Task 6; depends on OQ-B)

### FFI / kernel

- **No changes to `scripts/ffi/prefix.inc` or `suffix.inc`.**
- Kernel `GIT_TAG` pinned at `v0.23.0` (already on `main`); exposes
  `remove_files`, `scan_metadata_next_arrow`,
  `free_scan_metadata_arrow_result`, `get_engine_data`.

---

## 3. Ownership map (as implemented)

### Kernel handles

```
DeltaTransaction
├── kernel_transaction : KernelExclusiveTransaction
│       (existing; INSERT and DELETE both stage actions onto it; consumed by ffi::commit)
├── kernel_create_txn  : KernelExclusiveCreateTransaction  (CTAS only — unchanged)
└── ctas_extern_engine : KernelExternEngine                (CTAS only — unchanged)

DeltaTableEntry
└── snapshot : shared_ptr<DeltaMultiFileList>
        ├── extern_engine : KernelExternEngine
        ├── snapshot      : shared_ptr<SharedKernelSnapshot>
        ├── scan          : KernelScan
        ├── scan_data_iterator : KernelScanDataIterator
        └── metadata[]    : vector<unique_ptr<DeltaFileMetaData>>

DeltaMultiFileList::StageRemoveFiles  (scope-local during one call)
├── fresh_scan          : KernelScan                   (no-filter scan; freed by RAII at end)
├── iter                : KernelScanDataIterator       (freed by RAII)
└── per-batch:
    ├── raw_result      : ScanMetadataArrowResult*     (freed by ScanMetadataArrowResultGuard)
    │     └── arrow_data.array  : moved to ffi::get_engine_data;
    │                              release pointer nulled before guard drops the result
    └── engine_data     : KernelEngineData             (consumed by ffi::remove_files; or
                                                        RAII-freed if no rows in the batch
                                                        are selected)
```

### Plain data

```
DeltaDelete (PhysicalOperator, owned by DuckDB PhysicalPlan)
├── table        : optional_ptr<TableCatalogEntry>   (non-owning; the DeltaTableEntry)
└── delete_plan  : FileLevelDeletePlan               (value-owned)
                   ├── files_to_remove   : vector<idx_t>
                   ├── total_rows_removed: idx_t
                   └── predicate_was_true: bool

DeltaDeleteGlobalState (sink global state; unique_ptr from GetGlobalSinkState)
└── delete_count : idx_t   (set in Finalize; emitted in GetDataInternal)
```

No `shared_ptr` introduced. No raw owning pointers.

---

## 4. Module layout (final)

```
src/include/storage/delta_delete.hpp          NEW   FileLevelDeletePlan + DeltaDelete decl
src/storage/delta_delete.cpp                  NEW   DeltaDelete impl + DeltaCatalog::PlanDelete
src/storage/delta_catalog.cpp                 MOD   stub removed; comment points at delta_delete.cpp
src/storage/delta_transaction.cpp             MOD   RemoveFiles + Commit gate update
src/include/storage/delta_transaction.hpp     MOD   RemoveFiles decl; outstanding_removes
src/include/storage/delta_table_entry.hpp     MOD   GetRowIdColumns override (empty)
src/storage/delta_table_entry.cpp             MOD   snapshot->table_entry back-pointer
src/functions/delta_scan/delta_scan.cpp       MOD   DeltaScanGetBindInfo
src/functions/delta_scan/delta_multi_file_list.cpp           MOD   StageRemoveFiles impl
src/include/functions/delta_scan/delta_multi_file_list.hpp   MOD   StageRemoveFiles decl
CMakeLists.txt                                MOD   delta_delete.cpp in EXTENSION_SOURCES
test/sql/main/writing/delete/*.test           NEW   coverage (see §2 above)
```

---

## 5. Key types (as implemented)

```cpp
// src/include/storage/delta_delete.hpp

struct FileLevelDeletePlan {
    vector<idx_t> files_to_remove;     // indices into the unfiltered snapshot
    idx_t total_rows_removed;          // sum of cardinalities
    bool predicate_was_true;           // DELETE FROM t (no WHERE)
};

class DeltaDelete : public PhysicalOperator {
public:
    DeltaDelete(PhysicalPlan &plan, LogicalOperator &op,
                TableCatalogEntry &table, FileLevelDeletePlan delete_plan);

    optional_ptr<TableCatalogEntry> table;
    FileLevelDeletePlan delete_plan;

    SourceResultType GetDataInternal(ExecutionContext &, DataChunk &,
                                     OperatorSourceInput &) const override;
    bool IsSource() const override { return true; }

    SinkResultType Sink(ExecutionContext &, DataChunk &,
                        OperatorSinkInput &) const override;
    SinkFinalizeType Finalize(Pipeline &, Event &, ClientContext &,
                              OperatorSinkFinalizeInput &) const override;
    unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &) const override;
    bool IsSink() const override { return true; }
    bool ParallelSink() const override { return false; }

    string GetName() const override;
    InsertionOrderPreservingMap<string> ParamsToString() const override;
};
```

```cpp
// src/include/storage/delta_transaction.hpp (added members)

void RemoveFiles(ClientContext &context, DeltaMultiFileList &snapshot,
                 const vector<idx_t> &file_indices);

// private:
idx_t outstanding_removes = 0;
```

```cpp
// src/include/functions/delta_scan/delta_multi_file_list.hpp (added member)

void StageRemoveFiles(ClientContext &context, const vector<idx_t> &file_indices,
                      KernelExclusiveTransaction &kernel_transaction) const;
```

```cpp
// src/include/storage/delta_table_entry.hpp (added member)

vector<column_t> GetRowIdColumns() const override { return {}; }
```

`DeltaDeleteGlobalState` is a file-local `class … : public GlobalSinkState`
in `delta_delete.cpp` with a single `idx_t delete_count = 0` field.

`ScanMetadataArrowResultGuard` is a file-local RAII struct in
`delta_multi_file_list.cpp`. It owns the `ScanMetadataArrowResult*` and
calls `ffi::free_scan_metadata_arrow_result` on scope exit. The caller
explicitly nulls `raw_result->arrow_data.array.release` before the guard
fires whenever `ffi::get_engine_data` has consumed the array.

---

## 6. File-selection algorithm (as implemented)

`DeltaCatalog::PlanDelete` (in `src/storage/delta_delete.cpp`):

1. **Guard: RETURNING.** `op.return_chunk` → `BinderException("RETURNING
   clause is not yet supported for DELETE on Delta tables")`.
2. **Guard: READ_ONLY.** `access_mode == AccessMode::READ_ONLY` →
   `InvalidInputException("Cannot delete from a read-only Delta table")`.
3. **Guard: USING / non-trivial child.** Walk the child chain of `op`.
   Any node that is not `LogicalDelete`, `LogicalFilter`,
   `LogicalProjection`, or `LogicalGet` triggers
   `NotImplementedException("DELETE ... USING / join is not yet
   supported for Delta tables. …")`.
4. **Locate snapshot.** `op.table.Cast<DeltaTableEntry>().snapshot`.
   Force snapshot materialization by calling `GetTotalFileCount()` (and
   `GetPartitionColumns()` so partition names are populated).
5. **Find the optional `LogicalFilter`.** Descend one level. If the
   child is a `LogicalFilter`, capture it; otherwise `predicate_was_true`.
6. **`predicate_was_true` branch.** Enumerate `[0, total_files)` as
   `files_to_remove`; `total_rows_removed = sum cardinalities`.
7. **WHERE branch.** Reject if any filter expression references a
   non-partition column (`ExpressionsAllReferOnlyPartitionColumns`).
   Otherwise feed the filter expression copies through
   `DeltaMultiFileList::ComplexFilterPushdown` to get a pruned
   `MultiFileList`. For each surviving file in the pruned list, take
   `meta.file_number` (the original-snapshot index) and add it to
   `files_to_remove`. Sum the cardinalities.
8. **Build operator.** `planner.Make<DeltaDelete>(op, op.table,
   std::move(delete_plan_value));` and append `plan` as a child so
   DuckDB schedules the child pipeline (its rows are ignored in Sink).

---

## 7. Transaction lifecycle (as implemented)

Identical to the architecture intent (§7 of the original plan):
- One `kernel_transaction` per `DeltaTransaction` regardless of mix of
  RemoveFiles / Append calls.
- `outstanding_removes` is incremented by `RemoveFiles` after a
  successful `StageRemoveFiles` call.
- `Commit` proceeds iff `!outstanding_appends.empty() ||
  outstanding_removes > 0`. (The CTAS branch is mode-checked above
  and is independent.)
- `Rollback` simply destroys the `kernel_transaction` via RAII;
  remove_files has no on-disk side effects.
- CCv2 (`parent_commit=true`) is transparent: `InitializeTransaction`
  already wires the UC committer; `remove_files` does not interact
  with the committer choice.

---

## 8. Concurrency plan (as implemented)

- `DeltaDelete::ParallelSink()` returns `false`.
- `DeltaMultiFileList::lock` is held only during the construction of
  the fresh scan and iterator in `StageRemoveFiles`; released before
  any `scan_metadata_next_arrow` call.
- `DeltaTransaction::lock` is NOT held across any kernel call (matches
  Append's pattern).
- No new long-running kernel calls; no new cancellation points.

**Open invariant** (Completion Task 2): after the iteration loop in
`StageRemoveFiles`, `global_file_idx ==
GetTotalFileCountInternal()`. Add as `D_ASSERT`.

---

## 9. Error strategy (as implemented)

| Failure                                                  | Exception type              | Throwing site |
|----------------------------------------------------------|-----------------------------|---------------|
| DELETE on READ_ONLY attachment                           | `InvalidInputException`     | `PlanDelete` |
| DELETE … RETURNING                                       | `BinderException`           | `PlanDelete` |
| DELETE … USING / cross-product child                     | `NotImplementedException`   | `PlanDelete` |
| Predicate references any non-partition column            | `NotImplementedException`   | `PlanDelete` |
| Predicate prunes to zero files                           | (no exception; 0 rows; no commit) | n/a — short-circuited by RemoveFiles' empty-check and Commit's gate |
| Kernel error in `scan_metadata_next_arrow`               | `IOException`               | `StageRemoveFiles` |
| Kernel error in `get_engine_data`                        | `IOException`               | `StageRemoveFiles` |
| Kernel error in `remove_files`                           | `IOException`               | `StageRemoveFiles` |
| Kernel error / CCv2 conflict at commit                   | `IOException` or `TransactionException` | `DeltaTransaction::Commit` (unchanged) |
| DELETE invoked in `CREATING_TABLE` mode                  | (D_ASSERT)                  | `RemoveFiles` |
| Snapshot enumeration drift (Task 2 invariant fires)      | (D_ASSERT)                  | `StageRemoveFiles` |

---

## 10. Test plan (final)

All under `test/sql/main/writing/delete/` (fixture set: `main`):
the directory is consistent with `test/sql/main/writing/ctas/` and the
existing INSERT writing tests.

Already present (audit-verified): `delete_full_table.test`,
`delete_partition_aligned.test`, `delete_read_only.test`,
`delete_returning_rejected.test`, `delete_row_level_rejected.test`,
`delete_then_insert.test`, `delete_transaction.test`.

To add or modify (see Completion Tasks):
- Delete: `_temp_test.test` (Task 3).
- Extend: `delete_transaction.test` Part 3 (Task 5).
- Add: `delete_using_rejected.test` (Task 4 — depends on OQ-A).
- Add: `delete/ccv2/delete_ccv2.test` (Task 6 — depends on OQ-B).
- Optional: log-version probe in `delete_full_table.test` Part 3
  (Task 7 — depends on helper availability).

Also: a TSAN run (Task 10).

---

## 11. Open questions

- **OQ-A (test for USING).** The `PlanDelete` USING guard is wired and
  works; the only question is whether to land a dedicated
  `delete_using_rejected.test`. Recommendation: yes, because the guard
  is implemented as a generic "unexpected child node" check that could
  silently swallow future correctness regressions. **Confirm: add
  `delete_using_rejected.test`?**

- **OQ-B (CCv2 DELETE test).** Architecturally `parent_commit=true` is
  transparent to DELETE. Confirm we want a `delete/ccv2/delete_ccv2.test`
  in this PR (mirrors `ctas_ccv2.test`), or defer to a follow-up CCv2
  PR. Recommendation: include it, because the CCv2 commit callback
  exercises a separate Rust→C++ re-entrancy path that we want
  regression-covered for DELETE.

- **OQ-C (log-version probe).** Does the test suite already expose a
  helper (table function or scalar) to read the max log version for
  asserting "no commit happened"? If yes, point the coder at it for
  Task 7. If no, defer Task 7.

- **OQ-D (set_data_change).** Not called explicitly today; the kernel
  defaults to `data_change=true` for `add_files`/`remove_files`. INSERT
  doesn't call `ffi::set_data_change` either, so DELETE follows that
  precedent. **Confirm we follow INSERT precedent (recommend: yes).**

- **OQ-E (deletion-vector-active rows in cardinality).** When a
  source file already has an active deletion vector, the kernel's
  `Stats.num_records` (mirrored in `DeltaFileMetaData::cardinality`)
  could either be the physical or logical row count. For
  partition-aligned DELETE the difference is irrelevant to correctness
  (we remove the whole file either way), but the `Count` returned to
  the user is **physical row count** unless we adjust. Recommendation
  for v1: report the physical count (what `cardinality` gives us),
  matching `add_files`-side semantics. Flag as a future-tightening
  task once CoW v2 introduces per-row accounting.

- **OQ-F (scratch file removal).** `_temp_test.test` is a scratch
  file. Recommended action: delete (Task 3). **Confirm**.
