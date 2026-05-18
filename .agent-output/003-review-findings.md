# DELETE FROM — Code Review Findings (Iteration 2)

Reviewer: Claude (review agent)
Date: 2026-05-18
Commit base: main (before this change set)
Iteration: 2 (re-review after fix iteration 1)

Files reviewed: `src/include/storage/delta_delete.hpp`,
`src/storage/delta_delete.cpp`,
`src/functions/delta_scan/delta_multi_file_list.cpp`,
`src/include/functions/delta_scan/delta_multi_file_list.hpp`,
`src/storage/delta_transaction.cpp`,
`src/include/storage/delta_transaction.hpp`,
`src/storage/delta_table_entry.cpp`,
`src/include/storage/delta_table_entry.hpp`,
`src/functions/delta_scan/delta_scan.cpp`,
`src/storage/delta_catalog.cpp`,
all test files under `test/sql/main/writing/delete/`

---

## Review Summary

All five blocking issues from iteration 1 (C1, C2, H1, H2, H3) have been
correctly applied. The cardinality overflow guards follow the existing
`GetCardinality` pattern exactly. The post-loop `D_ASSERT` is now the
architecture-specified equality check, correctly re-acquiring `unique_lock<mutex>`
for a call to `GetTotalFileCountInternal` which does not itself take the lock —
no deadlock risk is introduced. The `const_cast` comment is present on the line
immediately above the cast. `ScanMetadataArrowResultGuard` is at file scope inside
the `duckdb` namespace with no name collisions. The `DeltaDelete` header has a
single `public:` section. The `INVALID_INDEX` guard does not suppress commits:
`files_to_remove` is populated before the cardinality accumulation, and
`outstanding_removes` is driven by `file_indices.size()`, so the commit path is
independent of `total_rows_removed`.

No new Critical or High issues were introduced by the fixes. The three Medium and
three Low items from iteration 1 remain unchanged and are all non-blocking.

---

## Resolved in Iteration 1

### [C1-RESOLVED] `BuildDeletePlan` accumulated `DConstants::INVALID_INDEX` cardinalities

Both accumulation sites in `delta_delete.cpp` now guard:
```cpp
if (card != DConstants::INVALID_INDEX) { total_rows += card; }
```
Pattern matches `GetCardinality()` line 1097.

### [C2-RESOLVED] Post-loop `D_ASSERT` in `StageRemoveFiles` was weaker than required

Replaced the `max_element` check with:
```cpp
{
    unique_lock<mutex> lck(lock);
    D_ASSERT(global_file_idx == GetTotalFileCountInternal());
}
```
`GetTotalFileCountInternal` does not acquire the lock; no recursive-lock risk.

### [H1-RESOLVED] No explanatory comment on `const_cast<LogicalGet &>`

Comment now appears immediately above the cast:
`// MultiFilePushdownInfo requires non-const LogicalGet even though it does not mutate it.`

### [H2-RESOLVED] `ScanMetadataArrowResultGuard` defined inside loop body

`struct ScanMetadataArrowResultGuard` is now at file scope (line 1153), inside
the `duckdb` namespace block, immediately above `StageRemoveFiles`. Single
`ScanMetadataArrowResultGuard guard(raw_result);` instantiation per loop iteration.

### [H3-RESOLVED] `DeltaDelete` double `public:` section

`src/include/storage/delta_delete.hpp` now has a single `public:` section
containing constructor, data members, and methods in order.

---

## Medium (nice to fix — non-blocking)

### [M1] Multi-operation transaction test lacks single-commit assertion

**Location:** `test/sql/main/writing/delete/delete_transaction.test`, Part 3

**Issue:** Part 3 exercises `BEGIN; DELETE; INSERT; COMMIT` and verifies row
data. It does not assert that only one log version was written (single commit).
A regression that committed DELETE and INSERT as two separate transactions would
pass the test. The architecture plan §7 calls out single-commit semantics as an
observable guarantee.

**Suggested addition** (at the end of Part 3, before `DETACH t3r`):
```sql
# Exactly one new log entry after the multi-op transaction (version 2, not 3)
query I
SELECT count(*) FROM glob('__TEST_DIR__/delete_txn/multi/_delta_log/00000000000000000002.json');
----
0
```
(Version 0 is CTAS, version 1 is the combined DELETE+INSERT. Version 2 must not exist.)

---

### [M2] `D_ASSERT(orig_sv_len == 0 || orig_sv_len == batch_rows)` relies on undocumented kernel internal

**Location:** `src/functions/delta_scan/delta_multi_file_list.cpp:1226`

**Issue:** The `FilteredEngineData` Rust type allows a selection vector shorter
than the data length (rows not covered are assumed selected). The assert treats
only length-0 or exact-match as valid. This is correct for v0.23.0 because the
kernel's `scan_metadata` path enforces `selection_vector.len() == actions.len()`
via an internal `require!` in `log_replay.rs`. The comment says "kernel guarantees
this" but cites no kernel source location. Under a future kernel bump the
assertion could become wrong without any indication of which invariant changed.

**Suggested fix:** Add a kernel source reference to the comment:
```cpp
// kernel guarantees this for scan_metadata: delta-kernel-rs/src/engine/arrow/log_replay.rs,
// EngineDataExtractor::with_selection — len must equal batch_rows or be 0 (all-selected).
```

---

### [M3] TSAN sweep skipped

**Issue:** `SANITIZER_MODE=thread make debug` was skipped due to linker OOM on the
test AArch64 machine. Three sharing patterns warrant TSAN validation before the
change can be called race-free:

1. `extern_engine` read in `StageRemoveFiles` after the snapshot lock is released.
   Logically safe (written once under lock), but not instrumented.
2. `outstanding_removes` incremented in `RemoveFiles` and read in `Commit` without
   `DeltaTransaction::lock`. Logically safe due to pipeline sequencing, but not
   instrumented.
3. `DeltaMultiFileList::metadata` read in `GetMetaData` (under lock) from
   `BuildDeletePlan` concurrently with potential scan threads.

None of these are likely actual races given the single-pipeline-executor model for
non-parallel sinks. TSAN validation should be run on the first CI machine that can
link the debug binary without OOM.

---

## Low (style suggestions — non-blocking)

### [L1] Pre-null-check assertion for `array_copy.release` missing

**Location:** `src/functions/delta_scan/delta_multi_file_list.cpp:1253`

The comment says the release pointer is nulled so the guard doesn't double-free.
A `D_ASSERT(array_copy.release != nullptr)` before the null assignment would make
the invariant self-documenting: we only null it when a release pointer was present.

---

### [L2] `DeltaDeleteGlobalState` default constructor should be `= default`

**Location:** `src/storage/delta_delete.cpp:27`

`DeltaDeleteGlobalState() { }` can be written as `= default`. Minor DuckDB style
consistency; the base class `GlobalSinkState` has a virtual destructor so this
is non-issue for correctness.

---

### [L3] `delete_using_rejected.test` uses a self-join as USING source

**Location:** `test/sql/main/writing/delete/delete_using_rejected.test:19`

The test self-joins the same table (`t1 USING t1 AS other`). The documented
use-case in the architecture plan is a separate table as the USING source. The
test works correctly and does exercise the guard; this is a readability-only note.

---

## What's Done Well

1. **C2 assertion correctness.** The lock scope in the post-loop assertion is
   minimal (a dedicated RAII block), and `GetTotalFileCountInternal` is correctly
   identified as the lock-free internal form. The assertion now pins exactly the
   invariant the architecture plan required.

2. **C1 guard placement.** Both cardinality accumulation sites are guarded, and
   the guard does not suppress commits — `files_to_remove` population and
   `outstanding_removes` tracking are independent of `total_rows_removed`, so
   the fix correctly reports 0 rows removed (not an error) when stats are absent.

3. **Guard struct hoisting.** `ScanMetadataArrowResultGuard` at file scope, inside
   the correct namespace, with explicit copy-constructor and assignment-operator
   deletions, and with move-semantics-safe design (no move ctor needed because the
   guard is never moved). This is the correct DuckDB pattern for FFI RAII helpers.

---

**APPROVED**

Blocking issues remaining: 0 Critical + 0 High = **0 blocking issues**.

Prior fixes correctly applied: yes — all 5 blockers (C1, C2, H1, H2, H3)
verified against source at the exact lines described in the implementation log.
