# Feature Summary: DELETE FROM for Delta tables

## What Was Built

File-level `DELETE FROM` for Delta Lake tables, wired through DuckDB's
catalog + planner + transaction path and committed via the kernel's
`remove_files` FFI. The predicate must be **provably file-level** —
every active row of every selected file must match — or the statement
is rejected with `NotImplementedException` pointing at CTAS as the
workaround. Partition-aligned deletes are the load-bearing case
("drop a day", "drop a tenant"); row-level matches that do not
align with file boundaries are explicitly rejected. DELETE coexists
with INSERT in one `DeltaTransaction` via a shared
`kernel_transaction` and a new `outstanding_removes` counter, and
flows through the existing CCv2 (`parent_commit=true`) committer
unchanged.

## Architecture Decisions

- **File-granular only in v1** — Kernel v0.23's `remove_files` is
  file-level by design; row-level / CoW would be a parallel
  subsystem out of scope. Decision lets v1 reuse every existing
  kernel-handle RAII wrapper.
- **Plan-time guards over runtime errors** — `PlanDelete` rejects
  READ_ONLY, RETURNING, USING/cross-product children, and any
  non-partition column predicate up-front with the appropriate
  DuckDB exception type, so the operator never reaches execution
  with an unsupported shape.
- **Path-based original-index lookup** — Partition pushdown reorders
  the active-file enumeration; using `meta.file_number` from the
  *pruned* `DeltaMultiFileList` against the unfiltered scan iterator
  silently mismaps files. `BuildDeletePlan` instead builds a
  `path → original_index` map once from the unfiltered snapshot and
  looks each candidate up by path.
- **Single `kernel_transaction` across mixed ops** — `RemoveFiles`
  stages onto the same kernel transaction object as `Append`; the
  commit gate fires iff `!outstanding_appends.empty() ||
  outstanding_removes > 0`. A 0-row DELETE writes no log entry.
- **CCv2 via `SetParentTableEntry(op.table)` in `PlanDelete`** —
  Mirrors the existing INSERT path so the UC committer callback
  receives the parent-table back-pointer at plan time, not execute
  time.
- **Selection-vector + arrow-batch FFI** — `StageRemoveFiles` opens
  a fresh, no-filter `scan_metadata_next_arrow` iterator, builds a
  per-batch `vector<uint8_t>` selection vector against the
  pre-computed `files_to_remove` set, and calls
  `ffi::remove_files` per batch. The ArrowArray double-free is
  resolved by extracting the array by value and nulling the
  `release` pointer in the original struct before the
  `ScanMetadataArrowResultGuard` fires.

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `src/include/storage/delta_delete.hpp` | Created | `FileLevelDeletePlan` POD + `DeltaDelete` PhysicalOperator decl |
| `src/storage/delta_delete.cpp` | Created | `DeltaDelete` impl + `DeltaCatalog::PlanDelete` + `BuildDeletePlan` |
| `src/include/functions/delta_scan/delta_multi_file_list.hpp` | Modified | `StageRemoveFiles` decl + `GetAllCardinalities` / `BuildPathIndexMap` / `GetFilePath` helpers + `file_number` doc comment |
| `src/functions/delta_scan/delta_multi_file_list.cpp` | Modified | `StageRemoveFiles` impl, `ScanMetadataArrowResultGuard` RAII (file scope), bulk-read helpers, post-loop enumeration invariant |
| `src/functions/delta_scan/delta_scan.cpp` | Modified | `DeltaScanGetBindInfo` (snapshot wiring) |
| `src/include/storage/delta_catalog.hpp` | Modified | `PlanDelete` declaration |
| `src/storage/delta_catalog.cpp` | Modified | `PlanDelete` stub removed; comment points at `delta_delete.cpp` |
| `src/include/storage/delta_table_entry.hpp` | Modified | `GetRowIdColumns()` override returning empty |
| `src/storage/delta_table_entry.cpp` | Modified | Snapshot→table_entry back-pointer |
| `src/include/storage/delta_transaction.hpp` | Modified | `RemoveFiles` decl + `outstanding_removes` counter |
| `src/storage/delta_transaction.cpp` | Modified | `RemoveFiles` impl + Commit gate + log line |
| `CMakeLists.txt` | Modified | `delta_delete.cpp` added to `EXTENSION_SOURCES` |
| `.gitignore` | Modified | Build artefact ignores |

## sqllogic Tests Added

Under `test/sql/main/writing/delete/`:

- `delete_full_table.test` — `DELETE FROM t` end-to-end; Part 3 covers empty-table DELETE + log-version probe asserting no commit
- `delete_partition_aligned.test` — single- and multi-partition DELETEs; sequential auto-commit DELETEs
- `delete_read_only.test` — pinned error: `Cannot delete from a read-only Delta table`
- `delete_returning_rejected.test` — pinned error: `RETURNING clause is not yet supported`
- `delete_row_level_rejected.test` — pinned error: `Row-level DELETE is not yet supported`
- `delete_using_rejected.test` — pinned error for `DELETE FROM t USING …` (OQ-A: included)
- `delete_then_insert.test` — DELETE followed by INSERT in separate statements
- `delete_transaction.test` — Part 1: within-transaction visibility (count=2); Part 2: rollback; Part 3: `BEGIN; DELETE; INSERT; COMMIT` multi-op single-commit
- `ccv2/delete_ccv2.test` — partition-aligned DELETE through `__internal_delta_test_ccv2_commit_staged` (gated by `require debug`)

Scratch files removed: `_temp_test.test`, `gen_log.test`, `test_delete_temp.test`.

## Review Status

- **Verdict:** APPROVED after **1 review iteration**
- 2 Critical + 3 High blockers caught in round 1, all fixed in round 2:
  - C1: cardinality accumulation guarded against `DConstants::INVALID_INDEX`
  - C2: post-loop `D_ASSERT(global_file_idx == GetTotalFileCountInternal())` restored to architecture-mandated strength
  - H1: `const_cast<LogicalGet &>` explained with a one-line comment
  - H2: `ScanMetadataArrowResultGuard` hoisted to file scope
  - H3: double `public:` section in `DeltaDelete` merged
- **Outstanding Medium/Low items (non-blocking):**
  - **M1** (Medium): `delete_transaction.test` Part 3 does not probe single-version-advance — only verifies post-commit row data
  - **M2** (Medium): `D_ASSERT(orig_sv_len == 0 || orig_sv_len == batch_rows)` relies on an undocumented kernel internal that should be re-verified on kernel bumps
  - **M3** (Medium): TSAN sweep skipped (debug unittest OOMs at link on this AArch64 machine)
  - **L1**: pre-null-check assertion on `array_copy.release` not added
  - **L2**: `DeltaDeleteGlobalState()` empty body vs `= default` (style)
  - **L3**: `delete_using_rejected.test` self-joins; a distinct USING table would be clearer

## Performance

- **Verdict:** HAS_OPPORTUNITIES → 2 of 5 optimizations applied
- **Applied (Medium priority):**
  - **P1** — `BuildDeletePlan`'s no-WHERE branch was acquiring the snapshot mutex N times via `GetMetaData(file_idx)`. Replaced with `GetAllCardinalities` (single-lock bulk read of `metadata` cardinalities). Eliminates N mutex round-trips at plan time; scales with file count.
  - **P2** — WHERE branch was making two full `vector<OpenFileInfo>` copies via `GetAllFiles()` (~2N path-string copies per DELETE plan call). Replaced with `BuildPathIndexMap` (lock-once map build from `resolved_files` directly) and per-candidate `GetFilePath(idx_t)` lookups.
- **Skipped (Low priority / speculative):**
  - **P3** — Per-batch `remove_sv` reserve outside the loop. Below measurement noise at typical DELETE scales (tens to hundreds of files).
  - **P4** — `unordered_set` → sorted-vector + `std::binary_search` swap in `StageRemoveFiles`. Speculative; no profiler data; crossover point depends on `file_indices.size()`.
  - **P5** — Style-only `= default` on `DeltaDeleteGlobalState`. Tracked in review L2.

## Items for Human Review

- **TSAN sweep was skipped** because the debug unittest binary cannot link on this AArch64 + ASAN host (ld OOM at 2.4 GB). The `StageRemoveFiles` lock-scope and `outstanding_removes` access patterns are logically safe under DuckDB's pipeline model and `ParallelSink()` returns `false`, but a TSAN run on another host should be scheduled before merge to a release branch.
- **`delete_transaction.test` Part 3** verifies multi-op transaction *data* but not that exactly one log version was written. Consider adding a `glob('…/_delta_log/*')` count probe to assert single-version-advance for the BEGIN; DELETE; INSERT; COMMIT case — Open Question OQ-C / Medium finding M1.
- **CCv2 DELETE test** (`ccv2/delete_ccv2.test`) is gated by `require debug` because `__internal_delta_test_ccv2_commit_staged` is debug-only. Same gating as the CCv2 INSERT test. If CI does not run the debug suite, this path is untested by automation.
- **Kernel `GIT_TAG`** stays at `v0.23.0`. No bump performed or required for this feature; bumping the kernel in a future PR should re-verify the `scan_metadata_next_arrow` enumeration invariant in `StageRemoveFiles` (the new `D_ASSERT(global_file_idx == GetTotalFileCountInternal())` will fire if it drifts).
- **`D_ASSERT(orig_sv_len == 0 || orig_sv_len == batch_rows)`** (Medium M2) relies on `delta-kernel-rs`'s internal `require!` in `log_replay.rs` that `selection_vector.len() == actions.len()`. This is an undocumented contract from the kernel side — flag for re-verification on kernel bumps.
