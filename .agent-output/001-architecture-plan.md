# DELETE for Delta tables — architecture plan

Status: design (no code yet)
Author: architect agent
Target file to retire: `src/storage/delta_catalog.cpp:220-223` (`PlanDelete` NotImplementedException stub)

---

## 1. Scope decision: file-level DELETE only, with strict "all-or-nothing" guard

**Decision: Scenario (I) — file-level DELETE only in this PR.** Predicate must
prove every row in a candidate file matches (file is *fully covered* by the
filter); any row-level match that does not align with file boundaries is
rejected with `NotImplementedException` pointing at CTAS as the workaround.

### Rationale (Layer 3 → 2 → 1)

**Layer 3 (domain).** Kernel v0.23's `remove_files` is unambiguously
**file-granular**: the `selection_vector` chooses rows of the *file-metadata
table* returned by `scan_metadata_next`, not rows of user data. The kernel
exposes no writer-side deletion-vector primitive and no "rewrite file"
primitive at this version. Copy-on-Write (CoW) would require us to implement,
in C++, a Delta-aware mini-engine that scans matched files, applies the
predicate, writes the survivors, and stages an Add+Remove pair — a parallel
subsystem to the INSERT pipeline. Outside this PR.

**Layer 2 (design).** The single-most-load-bearing user case the task brief
asks for is "DELETE FROM t WHERE ...". The largest cohort of useful DELETEs in
analytics workloads is on partition keys ("drop a day", "drop a tenant"),
which **are file-aligned by construction**. Implementing only file-level
DELETE captures that cohort with code that is symmetric to existing
INSERT / CTAS plumbing (RAII handles, transaction lifecycle, CCv2 commit
path, error mapping). The CoW v2 — which would land later — does not need to
change the v1 plan or the user-facing operator; it only changes the
"fallback" branch of the file-selection algorithm.

**Layer 1 (mechanics).** v1 reuses every existing kernel handle wrapper
(`KernelExclusiveTransaction`, `KernelEngineData`, `KernelExternEngine`,
`KernelCommittedTransaction`). The only new RAII wrapper introduced is for
`ScanMetadataArrowResult*` (returned by `scan_metadata_next_arrow`,
freed by `free_scan_metadata_arrow_result`).

### v1 acceptance contract

`DELETE FROM t WHERE <predicate>` succeeds when the predicate is provably
file-level (see §6 algorithm) — i.e. every active row of every selected file
is matched, and no row of any *unselected* file is matched. Otherwise
`NotImplementedException` is thrown with a message naming the workaround:

> Row-level DELETE is not yet supported for Delta tables. This DELETE
> would require rewriting individual files, which is not implemented in
> this version. Workaround: use CTAS to write the surviving rows into a
> new Delta table, e.g.
>   `CREATE TABLE t_new AS SELECT * FROM t WHERE NOT (<predicate>);`
> File-level DELETE (e.g. `WHERE partition_col = 'x'`) is supported.

This is a Layer-3 guardrail: we'd rather refuse a query than silently
produce wrong data or corrupt the log.

### Explicit non-goals (in this PR)

- Row-level DELETE / Copy-on-Write file rewrites
- Writer-side deletion vectors (kernel does not expose them at v0.23)
- `MERGE`
- `UPDATE` (still throws NotImplementedException as before)
- `RETURNING` clause for DELETE (throws BinderException)
- `DELETE FROM t USING …` (the binder *allows* this; we refuse it
  with NotImplementedException to keep v1 surface tight)

---

## 2. Affected surfaces

### `src/include/storage/`
- `delta_catalog.hpp` — no change to declarations; behavior changes in `.cpp`
- `delta_delete.hpp` (NEW) — `DeltaDelete` physical operator class
- `delta_transaction.hpp` — add `RemoveFiles(...)` declaration, add
  `outstanding_removes` field, document its lifetime

### `src/storage/`
- `delta_catalog.cpp` — implement `PlanDelete` (replaces `:220-223` stub)
- `delta_delete.cpp` (NEW) — `DeltaDelete` operator (sink-only; no source
  output other than the BIGINT delete count, mirroring INSERT)
- `delta_transaction.cpp` — implement `RemoveFiles`, extend `Commit` to also
  handle the "remove-only" case, extend `Rollback` / `CleanUpFiles`
  symmetrically (RemoveFiles itself does not create files; nothing to
  clean up — but the kernel transaction handle still needs freeing on
  rollback, which RAII already covers)

### `src/include/functions/delta_scan/`
- `delta_multi_file_list.hpp` — expose a NEW helper
  `BuildRemoveFilesEngineData(...)` (file-local naming: see §4) that:
  - re-runs `scan_metadata_next_arrow` for the scope of this transaction
  - returns the `KernelEngineData` plus a `vector<uint8_t>` selection
    vector keyed by file_index
  - throws `IOException` on kernel failure

  Alternative considered and rejected: build the engine_data from the
  cached `DeltaFileMetaData` we already accumulate via `ScanDataCallBack`.
  Rejected because (a) the kernel doc explicitly says the engine_data
  must come from `scan_metadata_next`, and (b) the row schema is owned
  by the kernel and is not stable engine-side. Re-using the kernel's
  arrow batch is the contract.

### `src/functions/delta_scan/`
- `delta_multi_file_list.cpp` — implement `BuildRemoveFilesEngineData`.

### `scripts/ffi/`
- **No changes.** `remove_files`, `set_data_change`, `with_engine_info`,
  `commit`, `scan_metadata_next_arrow`, and
  `free_scan_metadata_arrow_result` are all already in the generated
  header.

### `CMakeLists.txt`
- Add `src/storage/delta_delete.cpp` to `EXTENSION_SOURCES`.

### `test/sql/main/writing/`
- `delete/` (NEW directory)
  - `basic_delete.test` — partition-aligned DELETE, simple sanity
  - `delete_full_table.test` — `DELETE FROM t` (no WHERE)
  - `delete_partition_aligned.test` — `WHERE partition_col = …` and `IN`
  - `delete_row_level_rejected.test` — verifies the
    `NotImplementedException` message for non-aligned predicates
  - `delete_read_only.test` — DELETE on `AS t (TYPE delta, READ_ONLY)`
    raises `InvalidInputException`
  - `delete_returning_rejected.test` — `DELETE … RETURNING` raises
    `BinderException`
  - `delete_using_rejected.test` — `DELETE … USING …` raises
    `NotImplementedException`
  - `delete_transaction.test` — `BEGIN; DELETE…; INSERT…; COMMIT;`
    mixed-operation transaction; verifies a single commit covers both
- `delete/ccv2/` (NEW subdir, optional gating same as CTAS)
  - `delete_ccv2.test` — DELETE under `parent_commit=true` (file-level)

(See §9 for the test plan in detail.)

---

## 3. Ownership map

### Kernel handles

```
DeltaTransaction
├── kernel_transaction : KernelExclusiveTransaction        (existing; INSERT+DELETE both consume it via commit)
├── kernel_create_txn  : KernelExclusiveCreateTransaction  (existing; CTAS only)
└── ctas_extern_engine : KernelExternEngine                (existing; CTAS only)

DeltaTableEntry
└── snapshot : shared_ptr<DeltaMultiFileList>
              └── extern_engine : KernelExternEngine       (existing; used for remove_files engine arg)
              └── snapshot      : shared_ptr<SharedKernelSnapshot>
              └── scan          : KernelScan              (created lazily — reused by RemoveFiles)
              └── scan_data_iterator : KernelScanDataIterator
              └── metadata[]    : unique_ptr<DeltaFileMetaData>  (existing)

DeltaTransaction::RemoveFiles (scope-local during call)
├── remove_engine_data : KernelEngineData        (new; built per call; consumed by ffi::remove_files)
└── (No new long-lived state.)
```

### Plain data

```
DeltaDelete (physical operator, value-type, owned by DuckDB PhysicalPlan)
├── table : optional_ptr<TableCatalogEntry>     (the DeltaTableEntry, non-owning)
├── row_id_index : idx_t                        (per LogicalDelete: index into Sink chunk
│                                                where the rowid expression lives — but for
│                                                v1 we ignore it; see §6)
├── file_level_predicate_summary :              (see §6 — the "proof of file alignment"
│   FileLevelDeletePlan                          carried from PlanDelete into Sink)
└── return_chunk : bool                         (must be false in v1; BinderException otherwise)

DeltaDeleteGlobalState (sink global state, unique_ptr from GetGlobalSinkState)
├── delete_count       : idx_t                  (running total of rows removed; reported by GetData)
├── files_to_remove    : vector<idx_t>          (file_index values; deduped at finalize)
└── files_total_rows   : idx_t                  (sum of cardinalities of files_to_remove;
                                                  becomes delete_count if filter == TRUE)
```

### DuckDB handles

```
PhysicalPlanGenerator → planner.Make<DeltaDelete>(op, table_entry, ...)
                                  ^ allocates DeltaDelete on the plan arena (DuckDB-owned)
```

No `shared_ptr` introduced. No raw owning pointers.

---

## 4. Module layout

```
src/include/storage/delta_delete.hpp          NEW   Physical op class declaration
src/storage/delta_delete.cpp                  NEW   PlanDelete in DeltaCatalog + DeltaDelete operator
src/storage/delta_catalog.cpp                 MOD   replace PlanDelete stub with call into DeltaDelete plan helper (mirror INSERT)
src/storage/delta_transaction.cpp             MOD   add RemoveFiles(); Commit() handles remove-only flow
src/include/storage/delta_transaction.hpp     MOD   declare RemoveFiles(); add outstanding_removes
src/functions/delta_scan/delta_multi_file_list.cpp           MOD   add BuildRemoveFilesEngineData()
src/include/functions/delta_scan/delta_multi_file_list.hpp   MOD   declare BuildRemoveFilesEngineData()
CMakeLists.txt                                MOD   add delta_delete.cpp to EXTENSION_SOURCES
test/sql/main/writing/delete/*                NEW   sqllogic coverage (see §9)
```

Notes on placement:
- Following the INSERT precedent, `DeltaCatalog::PlanDelete` is implemented
  in `delta_delete.cpp`, not in `delta_catalog.cpp`. The
  `delta_catalog.cpp:220-223` stub goes away and is *replaced* by the new
  cpp file's contribution (the function is declared in the catalog header).
- `BuildRemoveFilesEngineData` lives in `delta_multi_file_list.cpp` because
  it must touch `extern_engine`, `snapshot`, and the scan iterator — all
  private/protected state of `DeltaMultiFileList`. It is a member function
  of `DeltaMultiFileList` (not a free helper) for that reason.

---

## 5. Key types

### `src/include/storage/delta_delete.hpp` (NEW)

```cpp
#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/index_vector.hpp"

namespace duckdb {

class DeltaTableEntry;

//! Compile-time proof that a DELETE's predicate aligns to file boundaries.
//! Computed at PlanDelete time and embedded into the operator. The
//! invariant is: every file_index in `files_to_remove` has every active
//! row matched by the predicate; no file outside the set has any matched row.
//! The kernel's `remove_files` is FILE-granular, so this proof is what
//! makes the v1 contract sound.
struct FileLevelDeletePlan {
    //! file_index values (as enumerated by DeltaMultiFileList::GetFile)
    //! to mark as Remove in the kernel transaction.
    vector<idx_t> files_to_remove;
    //! Sum of cardinalities (per DeltaFileMetaData::cardinality) of the
    //! files in `files_to_remove`. Used as the BIGINT return of DELETE.
    idx_t total_rows_removed;
    //! True iff the original predicate was the boolean literal TRUE
    //! (`DELETE FROM t` without WHERE). The Sink path can then skip
    //! verifying child rowids and just commit the file set.
    bool predicate_was_true;
};

//! Physical operator for file-level DELETE on a Delta table.
//!
//! Lifecycle mirrors DeltaInsert:
//!   GetGlobalSinkState → Sink(chunks of rowids; verifies coverage) →
//!   Finalize(adds Remove actions to kernel transaction) →
//!   GetData(emits BIGINT delete count).
//!
//! Sink is intentionally NOT parallel. The DELETE child plan may emit
//! row_ids in any order; aggregating set-of-files-to-remove is trivial
//! and cheap, and we keep the global state simple by serialising. INSERT
//! does the same (see DeltaInsert::ParallelSink() == false).
class DeltaDelete : public PhysicalOperator {
public:
    DeltaDelete(PhysicalPlan &plan, LogicalOperator &op, TableCatalogEntry &table,
                FileLevelDeletePlan delete_plan);

    optional_ptr<TableCatalogEntry> table;
    FileLevelDeletePlan delete_plan;

public:
    SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                     OperatorSourceInput &input) const override;
    bool IsSource() const override { return true; }

    SinkResultType Sink(ExecutionContext &context, DataChunk &chunk,
                        OperatorSinkInput &input) const override;
    SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                              OperatorSinkFinalizeInput &input) const override;
    unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
    bool IsSink() const override { return true; }
    bool ParallelSink() const override { return false; }

    string GetName() const override;
    InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
```

### `src/include/storage/delta_transaction.hpp` (MOD — added members)

```cpp
//! v1 file-level DELETE: stages Remove actions for the given file_index
//! values via ffi::remove_files. file_indices are positions into the
//! current DeltaMultiFileList resolved_files. Calls
//! InitializeTransaction() lazily on first use, identical to Append().
//! No-op when file_indices is empty.
//!
//! Must NOT be called when mode == CREATING_TABLE. (v1 doesn't allow
//! DELETE inside CTAS; the bind layer never produces that combination.)
void RemoveFiles(ClientContext &context, DeltaMultiFileList &snapshot,
                 const vector<idx_t> &file_indices);
```

And one new private field:

```cpp
//! True once at least one RemoveFiles call staged any Remove actions on
//! kernel_transaction. Used by Commit() so that we still commit when
//! outstanding_appends.empty() but outstanding_removes > 0.
//! Counts files removed (cumulative across multiple DELETEs in one txn).
idx_t outstanding_removes = 0;
```

### `src/include/functions/delta_scan/delta_multi_file_list.hpp` (MOD — added member)

```cpp
//! Re-issue scan_metadata_next_arrow over this snapshot and produce
//! a KernelEngineData ready to feed to ffi::remove_files, together with
//! a per-row (= per-file) selection vector aligned to file_indices.
//!
//! file_indices values are positions into the resolved_files vector
//! built up by ScanDataCallBack. Each entry's selection_vector[i] = 1 if
//! file resolved_files[i] is in file_indices, else 0.
//!
//! Returns the engine_data and the selection vector by out-params so
//! the caller can pass `selection_vector.data()` directly to
//! ffi::remove_files without copy.
//!
//! Throws IOException on any kernel failure.
//!
//! Locking: takes the internal `lock` for the duration of the kernel
//! iteration. Does NOT hold the lock across ffi::remove_files itself
//! (callers do).
void BuildRemoveFilesEngineData(ClientContext &context,
                                const vector<idx_t> &file_indices,
                                KernelEngineData &out_engine_data,
                                vector<uint8_t> &out_selection_vector) const;
```

### New RAII wrapper in `delta_multi_file_list.cpp` (file-local, anon namespace)

```cpp
//! Wraps a ScanMetadataArrowResult* (returned by ffi::scan_metadata_next_arrow)
//! so it is always freed exactly once on scope exit, even on error paths.
struct ScanMetadataArrowResultGuard {
    ffi::ScanMetadataArrowResult *ptr = nullptr;
    explicit ScanMetadataArrowResultGuard(ffi::ScanMetadataArrowResult *p) : ptr(p) {}
    ~ScanMetadataArrowResultGuard() {
        if (ptr) { ffi::free_scan_metadata_arrow_result(ptr); }
    }
    ScanMetadataArrowResultGuard(const ScanMetadataArrowResultGuard &) = delete;
    ScanMetadataArrowResultGuard &operator=(const ScanMetadataArrowResultGuard &) = delete;
    // No release(): the kernel always consumes the engine_data inside via the
    // arrow array's own release callback when free_scan_metadata_arrow_result fires.
    // We extract the engine_data via ffi::get_engine_data BEFORE this guard
    // disposes the result (see implementation note below).
};
```

Note: `ScanMetadataArrowResult::arrow_data` is an `ArrowFFIData` (array+schema).
We convert it to a `KernelEngineData` via `ffi::get_engine_data(array, &schema,
DuckDBEngineError::AllocateError)` — same pattern as `WriteMetaData::ToArrow`
→ `get_engine_data` in `delta_transaction.cpp:649-650`. After
`get_engine_data` returns successfully, **the kernel owns the engine_data**;
the local `ScanMetadataArrowResult` still owns the arrow data and the
`CTransforms` (which we don't use here), and the guard frees both on scope
exit.

`get_engine_data` consumes the arrow array (zero-copy hand-off). Whether
freeing the ScanMetadataArrowResult attempts to re-release the same
ArrowArray is a kernel-side question — see OQ-3 below. If it does, the
implementation must extract the array onto the stack first and clear it
inside the result struct, or copy. We'll determine the right pattern from
the kernel source / existing usage during implementation.

### `DeltaCatalog::PlanDelete` skeleton (`src/storage/delta_delete.cpp`)

```cpp
PhysicalOperator &DeltaCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                            LogicalDelete &op, PhysicalOperator &plan);
```

(Body in §6.)

---

## 6. File-selection algorithm — the load-bearing piece

This is what makes v1 sound. We have to translate the DuckDB-bound
`LogicalDelete` (with its `LogicalFilter` child wrapping a `LogicalGet`
over the Delta table) into a proven file-level set.

### High-level algorithm (run inside `PlanDelete`, NOT inside Sink)

1. **Reject the surfaces we don't support:**
   - `op.return_chunk == true` → `throw BinderException("RETURNING not yet supported for DELETE on Delta tables")`
   - Walk `op.children` (the plan tree): if any node above `LogicalGet`
     is not in {`LogicalFilter`, `LogicalProjection`, the `LogicalGet`
     itself}, raise `NotImplementedException("DELETE … USING / join is
     not yet supported for Delta tables")`. (LogicalCrossProduct is the
     give-away for USING; see `duckdb/src/planner/binder/statement/bind_delete.cpp:48`.)
   - Access mode: `if (access_mode == AccessMode::READ_ONLY) throw
     InvalidInputException(...)` (mirror `DeltaTransaction::InitializeTransaction`).

2. **Extract the predicate.** The child plan from `bind_delete.cpp:58-61`
   is: `LogicalDelete → LogicalFilter(condition) → LogicalGet(delta table)`,
   or `LogicalDelete → LogicalGet` when there is no WHERE.

3. **The "no WHERE" case (`predicate_was_true`).**
   - `files_to_remove := all file_indices in the current snapshot`
   - `total_rows_removed := sum of cardinalities`
   - Skip the child plan entirely — emit a `PhysicalDummyScan`-equivalent
     child OR pass `plan` through as a no-op source whose data is
     discarded in Sink. Mirror the INSERT pattern: `DeltaDelete::Sink`
     simply ignores incoming chunks when `delete_plan.predicate_was_true`.
   - Rationale: avoids reading the whole table.

4. **The "WHERE …" case — push the predicate through ComplexFilterPushdown.**
   - Take the `LogicalFilter::expressions` and feed them through the
     same `DeltaMultiFileList::ComplexFilterPushdown` path that read
     scans use today. This produces a pruned `DeltaMultiFileList`
     containing exactly the files that *might* match the predicate
     (kernel partition-value pruning; nothing else).
   - Let `candidate_files := pruned list`.
   - Let `surviving_files := original list \ pruned list`. Every row of
     these is guaranteed not to match the predicate (the kernel says so
     via pushdown). They are NOT to be removed.

5. **The all-or-nothing proof for each candidate file.** A candidate
   file is selected for removal IFF we can prove every active row of it
   matches. For v1 we accept two kinds of proof, both static (no scan):

   **(a) Predicate is a conjunction over PARTITION COLUMNS only.**
   Detected by `ExpressionsAllReferOnlyPartitionColumns(predicate,
   snapshot.partitions)`. Because partition columns are constants
   per-file, if the kernel's `ComplexFilterPushdown` admits the file
   into `candidate_files`, the predicate is provably TRUE for every row
   of that file. This covers `WHERE part = 'x'`, `WHERE part IN (...)`,
   `WHERE part_a = 'x' AND part_b > 5`.

   **(b) Predicate is the literal TRUE (already handled in step 3).**

   Any other case (predicate mentions any non-partition column) →
   **`NotImplementedException`** with the message from §1. We *do not*
   attempt to read the data to verify; that's CoW territory.

6. **Build `FileLevelDeletePlan`.**
   - `files_to_remove := [file.file_index for file in candidate_files]`
   - `total_rows_removed := sum(file.cardinality for file in candidate_files)`
   - `predicate_was_true := false` (true only in step 3)

7. **Build the physical plan.**
   ```
   DeltaDelete(delete_plan)
   └── plan  (the existing child PhysicalOperator passed in; ignored in v1
              because Sink does no per-row work — it's there only so that
              DuckDB's pipeline scheduling treats DELETE as a sink with a
              source it can drain. We may switch to PhysicalDummyScan for
              efficiency; see OQ-5.)
   ```

### Why we don't trust the row-id stream

`bind_delete.cpp` calls `BindRowIdColumns` to add a virtual ROW_ID
column to the child get. The default `TableCatalogEntry::GetRowIdColumns`
returns `COLUMN_IDENTIFIER_ROW_ID` (a virtual column of type
`ROW_TYPE`), but the Delta scan path does NOT generate ROW_ID values
in its output (the parquet multifile reader exposes `file_index` and
`file_row_number` as separate virtuals — not the unified ROW_ID).
Verifying coverage by *counting* matched-rows-per-file at Sink time
would require:
  - DeltaTableEntry overriding `GetRowIdColumns` / `GetVirtualColumns`
    to expose `(file_index, file_row_number)` as a struct ROW_TYPE
  - DeltaDelete::Sink aggregating per file_index how many rows arrived
  - Comparing against `DeltaFileMetaData::cardinality` at Finalize
  - Handling deletion-vector-active rows in the original snapshot
    (cardinality must mean "active rows" not "physical rows")

That's a meaningful chunk of work AND adds new public state to the
table entry (the row-id virtuals leak into every other query path).
For v1 the **static proof at PlanDelete time** is sufficient,
simpler, and avoids the read pass. The CoW v2 will need the row-id
mechanism anyway, so this is also natural sequencing: v1 builds nothing
that v2 has to throw away.

---

## 7. Transaction lifecycle

```
SESSION                    DELTA TRANSACTION (DeltaTransactionState)
───────                    ─────────────────
BEGIN                      TRANSACTION_NOT_YET_STARTED
DELETE FROM t WHERE p1     ┐
                           │ first writer touch:
                           │   DeltaCatalog::PlanDelete builds delete_plan
                           │   DeltaDelete::Finalize calls
                           │   transaction.RemoveFiles(snapshot, files1)
                           │   ↳ InitializeTransaction (if not already started)
                           │   ↳ ffi::scan_metadata_next_arrow → engine_data
                           │   ↳ ffi::remove_files(kernel_transaction, engine_data,
                           │                       sel_vec, len, engine)
                           │   ↳ outstanding_removes += files1.size()
                           ↓
                           TRANSACTION_STARTED
DELETE FROM t WHERE p2     ┐
                           │   transaction.RemoveFiles(snapshot, files2)
                           │   (kernel_transaction already alive — only stage)
                           │   outstanding_removes += files2.size()
                           ↓
INSERT INTO t VALUES ...   ┐
                           │   transaction.Append(...) — same kernel_transaction
                           │   outstanding_appends grows
                           ↓
COMMIT                     ┐
                           │   transaction.Commit:
                           │     if (outstanding_appends.empty() &&
                           │         outstanding_removes == 0) return
                           │     run app_versions check (existing code)
                           │     ffi::commit(kernel_transaction.release(), engine)
                           ↓
                           TRANSACTION_FINISHED
```

Key invariants:
- **One kernel `ExclusiveTransaction` per DeltaTransaction**, shared by
  multiple `Append` + `RemoveFiles` calls.
- `set_data_change(true)` is the implicit default — DELETE *is* a data
  change. We can defer calling it explicitly; only set it explicitly if
  we ever need `data_change=false` (e.g. compaction). Out of v1 scope.
  See OQ-2.
- `with_engine_info("DuckDB")` already runs in `InitializeTransaction`
  — no need to repeat per call.
- Commit gate updates from `if (!outstanding_appends.empty())` to
  `if (!outstanding_appends.empty() || outstanding_removes > 0)`.
- Rollback: `kernel_transaction` is freed by RAII destructor. Nothing
  to undo on disk — `remove_files` only stages an in-memory action set.

### CCv2 (`parent_commit=true`) integration

CCv2 is **transparent**: `InitializeTransaction` already wires the
`ffi::transaction_with_committer` path with `get_uc_committer` when
`parent_commit` is true (`delta_transaction.cpp:583-597`). The
`kernel_transaction` is the same handle whether the committer is
filesystem or UC; `add_files` and `remove_files` are unaware of
committer choice. The CommitCallback path triggers identically on
`ffi::commit`. We need only a single CCv2 sqllogic test
(`delete_ccv2.test`) to assert that the CCv2 path produces the same
observable behavior as the filesystem path. No new branching in code.

---

## 8. Concurrency plan

### Sink parallelism
DeltaDelete::ParallelSink() returns `false` — same as DeltaInsert. The
work in Sink is trivial (count incoming rows for the count source), and
the aggregation into `files_to_remove` is statically computed at plan
time, so serial Sink is fine and removes lock contention.

### Lock ordering and kernel re-entry
- `DeltaMultiFileList::lock` is held in `BuildRemoveFilesEngineData`
  during the `scan_metadata_next_arrow` iteration (mirrors
  `GetFileInternal`). The kernel does NOT re-enter our code from
  `scan_metadata_next_arrow` (it returns an arrow batch — no
  engine-visitor callback). Safe to hold.
- `DeltaMultiFileList::lock` is **released** before `ffi::remove_files`
  fires (the caller in `DeltaTransaction::RemoveFiles` takes the engine
  data out of `BuildRemoveFilesEngineData`, then calls
  `ffi::remove_files` outside the snapshot lock). `ffi::remove_files`
  does not re-enter our code.
- `DeltaTransaction::lock` (existing) guards `outstanding_removes` and
  `outstanding_appends`. We do NOT hold this across `ffi::remove_files`
  either — match the existing pattern in `Append()` which calls
  `ffi::add_files` without holding the transaction lock (it's never
  taken in Append today; we keep that behavior for symmetry).

### Cancellation
No long-running kernel calls are added. `scan_metadata_next_arrow` is
already on the read path; `remove_files` is bounded by the count of
files being removed (typically O(thousands), not O(millions)). No new
polling points needed.

---

## 9. Error strategy

| Failure                                                    | Exception type             | Rationale |
|------------------------------------------------------------|----------------------------|-----------|
| DELETE on `AS t (TYPE delta, READ_ONLY)`                  | `InvalidInputException`    | Matches `DeltaTransaction::InitializeTransaction` for INSERT |
| `RETURNING` clause                                         | `BinderException`          | Matches INSERT's RETURNING handling (delta_insert.cpp:370) |
| `DELETE … USING <table>`                                   | `NotImplementedException`  | Join-like DELETE; out of scope; reuses CoW workaround language |
| Predicate references any non-partition column              | `NotImplementedException`  | Row-level DELETE; per §1 contract; message names CTAS workaround |
| Predicate provably false (e.g. `WHERE 1=0`)                | (no exception; succeed with 0 rows removed) | Kernel pushdown yields empty candidate set; Commit short-circuits because both `outstanding_appends` empty and `outstanding_removes == 0` |
| Kernel error from `scan_metadata_next_arrow`               | `IOException`              | Existing pattern (`KernelUtils::TryUnpackResult` + `.Throw()`) |
| Kernel error from `remove_files`                           | `IOException`              | Existing pattern |
| Kernel error from `commit`                                 | `IOException` (or `TransactionException` if conflict) | Existing pattern from INSERT path |
| CCv2 commit conflict (CommitCallback returns Some(error))  | `TransactionException`     | Existing pattern (delta_transaction.cpp:434 throws via active_error which the kernel wraps) |
| DELETE in CTAS context (`mode == CREATING_TABLE`)          | `InternalException`        | Programmer error — binder shouldn't produce this combo |
| `op.children` contains anything other than LogicalGet/Filter/Projection | `NotImplementedException` | Catch-all for unanticipated plan shapes |

Non-fatal "filter pruned everything" → `total_rows_removed == 0`,
`files_to_remove.empty()`. The Sink path skips RemoveFiles entirely
(short-circuit in `RemoveFiles` when `file_indices.empty()` — mirror
of `Append`'s short-circuit on empty). No exception. DELETE returns
`Count = 0`.

---

## 10. Test plan

All tests under `test/sql/main/writing/delete/`, fixture pattern
mirroring `test/sql/main/writing/ctas/`.

### `basic_delete.test`
- Setup: copy inlined simple_table; attach; verify count=10.
- `DELETE FROM t WHERE i = 1;` → expect `NotImplementedException`
  (non-partition column).
- Reset, then `DELETE FROM t;` → expect Count=10, then `SELECT count(*)
  FROM t` returns 0. Verify all parquet files now have Remove actions in
  `_delta_log/0000…0001.json` (count via `delta_file_list` or
  `glob('**/*.parquet')` still shows 1 file but a snapshot read returns 0).

### `delete_full_table.test`
- `DELETE FROM t` on a partitioned table; expect Count=full,
  post-DELETE scan returns 0 rows, but all parquet files still exist on
  disk (Remove ≠ physical delete; vacuum is a separate concern, not in
  scope).

### `delete_partition_aligned.test`
- Setup: CTAS a partitioned table with `partition_col IN {'a','b','c'}`.
- `DELETE FROM t WHERE partition_col = 'b'` → expect Count = rows in
  partition 'b', `SELECT DISTINCT partition_col` returns `{'a','c'}`.
- `DELETE FROM t WHERE partition_col IN ('a','c')` → expect Count = rest,
  scan returns 0.
- `DELETE FROM t WHERE partition_col = 'no_such'` → Count=0, no commit
  (verify no new entry in `_delta_log`).

### `delete_row_level_rejected.test`
- Partitioned table. `DELETE FROM t WHERE value > 5;` → expect
  `NotImplementedException` matching the CTAS workaround message.
- Mixed: `DELETE FROM t WHERE partition_col = 'a' AND value > 5;`
  → also rejected (any non-partition reference).

### `delete_read_only.test`
- `ATTACH '...' AS t (TYPE delta, READ_ONLY);`
- `DELETE FROM t WHERE part = 'x'` → `InvalidInputException`.

### `delete_returning_rejected.test`
- `DELETE FROM t RETURNING *;` → `BinderException`.

### `delete_using_rejected.test`
- `DELETE FROM t USING other_table WHERE …` → `NotImplementedException`.

### `delete_transaction.test`
- `BEGIN; INSERT…; DELETE FROM t WHERE part='x'; INSERT…; COMMIT;`
- Verify a SINGLE log entry advances the version (not three) and
  contains both Add and Remove actions.
- `BEGIN; DELETE FROM t WHERE part='x'; ROLLBACK;` — verify version
  unchanged and partition still present.

### `delete/ccv2/delete_ccv2.test`
- Attach with `parent_commit=true`, `unity_table_id='...'` etc., using
  the same CCv2 wiring as `ctas_ccv2.test`.
- Run a partition-aligned DELETE; verify the staged commit lands via
  the test commit table function (`__internal_delta_test_ccv2_commit_staged`).

### Non-test verification
- TSAN run (`SANITIZER_MODE=thread make debug && make test_debug`)
  with at least the multi-INSERT-DELETE transaction test enabled.

---

## 11. Migration order (incremental build/test gates)

1. **Step 1 — operator skeleton.** Add `delta_delete.hpp` and an
   `delta_delete.cpp` that contains `PlanDelete` *still throwing*
   `NotImplementedException` but routed through the new file (and a
   skeleton `DeltaDelete` operator that never executes). Wire into
   `CMakeLists.txt`. **Build green; existing tests green.**

2. **Step 2 — full-table DELETE (no WHERE).** Implement
   `DeltaTransaction::RemoveFiles`, `DeltaMultiFileList::BuildRemoveFilesEngineData`,
   and the `predicate_was_true` branch of `PlanDelete`. Add
   `delete_full_table.test`. **Build + the new test green.**

3. **Step 3 — partition-aligned WHERE.** Implement the
   ComplexFilterPushdown path of `PlanDelete`. Add
   `delete_partition_aligned.test`, `delete_row_level_rejected.test`.
   **All new tests green.**

4. **Step 4 — guards.** Add RETURNING / USING / READ_ONLY rejection;
   add the corresponding tests. **All green.**

5. **Step 5 — transactions.** Mixed-op transaction test
   (`delete_transaction.test`). **Green.**

6. **Step 6 — CCv2.** `delete_ccv2.test`. **Green.**

Each step keeps `git status` clean of in-flight changes between steps —
they are pre-mergeable individually.

---

## 12. Open questions (architect → user)

- **OQ-1 (semantics).** Should `DELETE FROM t` (no WHERE) on an empty
  table produce a commit at all? v1 design: no — the
  `outstanding_appends.empty() && outstanding_removes == 0` gate
  short-circuits. This matches Spark's behavior on empty tables but
  diverges from some engines that always advance the version. **Confirm
  short-circuit is desired.**

- **OQ-2 (`set_data_change`).** The kernel defaults appear to be
  `data_change = true` (Add/Remove actions). Should v1 explicitly call
  `ffi::set_data_change(txn, true)` before commit? Today's INSERT path
  doesn't call it either. **Confirm we follow INSERT precedent and skip
  the explicit call.**

- **OQ-3 (arrow array release ownership).** Does
  `ffi::free_scan_metadata_arrow_result` attempt to re-release an
  `ArrowArray` whose contents have already been handed to
  `ffi::get_engine_data`? This is a kernel-side implementation detail.
  Implementation will determine empirically during Step 2 (test under
  ASAN), or by reading kernel source. **Flag for cpp-coder to verify
  empirically.**

- **OQ-4 (row-id virtuals long-term).** When CoW v2 lands it will need
  Delta tables to expose `(file_index, file_row_number)` row-id
  virtuals. This is a future-breaking-ish API change to
  `DeltaTableEntry`. **Confirm v1 deliberately does not introduce
  these** (so we don't half-bake the surface), and that CoW v2 is
  allowed to add them later.

- **OQ-5 (child plan in `predicate_was_true` branch).** For `DELETE
  FROM t` (no WHERE), the bind layer still generates a `LogicalGet`
  child. The physical pipeline will scan and stream rows to our Sink
  for nothing. Options:
    (a) accept the read cost in v1 (simplest, matches INSERT pattern
        where the child plan is always real),
    (b) substitute a `PhysicalDummyScan` for the child when
        `predicate_was_true` (we are within `PlanDelete` and own the
        plan construction).
  Recommendation: option (a) for v1; revisit if benchmarks show it
  matters. **Confirm acceptable.**

- **OQ-6 (kernel-version assumption).** The task brief states `remove_files`
  is already exposed at v0.23. Current `CMakeLists.txt` pins kernel at
  `v0.21.0`. **A kernel bump from v0.21 → v0.23 is required before
  this PR can build.** This is a load-bearing prerequisite; calling it
  out explicitly per the architect rubric.

- **OQ-7 (test gating for CCv2).** Today CCv2 tests use the
  `__internal_delta_test_ccv2_commit_staged` table function for testability.
  The delete CCv2 test will use the same. Is there any CCv2 test
  fixture that won't carry over from CTAS? (Most likely not — CCv2 is
  agnostic to Add/Remove split — but flag for review.)
