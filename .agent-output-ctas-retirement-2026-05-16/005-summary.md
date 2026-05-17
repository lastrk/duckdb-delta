# Feature Summary: CTAS retirement (route to kernel-native `get_create_table_builder`)

## What Was Built

Retired the hand-rolled `_delta_log/00000000000000000000.json` writer (`src/storage/delta_ctas.{cpp,hpp}`, ~242 lines deleted) and rewired Delta CTAS through the kernel-native `ffi::get_create_table_builder` flow exposed in delta-kernel-rs v0.22+ (we're on v0.23). DuckDB's `ColumnList` is now translated to a kernel `EngineSchema` via a visitor (`DeltaCreateTableSchema` in `src/storage/delta_create_table_schema.{cpp,hpp}`); the kernel walks the visitor synchronously to register fields, returns an `ExclusiveCreateTableBuilder`, which we chain with optional table properties, build into an `ExclusiveCreateTransaction`, stage parquet files into via `create_table_add_files`, and finalize with `create_table_commit`. Result: the version-0 commit JSON is now written by the kernel's `FileSystemCommitter`, not by us. We no longer maintain Delta-protocol JSON in C++.

## Architecture Decisions

- **Use the kernel's `FileSystemCommitter` for the version-0 commit.** This was the entire point of the retirement: we now share the commit path with every subsequent INSERT instead of having a divergent first-commit code path. Atomicity (put-if-absent) is now the kernel's responsibility; our `FILE_FLAGS_FILE_CREATE_NEW` workaround is gone with the JSON writer.
- **Add a `BuildEngine` static factory in `delta_multi_file_list.{cpp,hpp}`.** The read-side `CreateBuilder` requires an existing snapshot to construct an engine — CTAS doesn't have one yet. `BuildEngine` factors out engine-only construction (no snapshot dependency) while still reusing the credential / secret / option pickup paths. No code duplication; reviewer-flagged as load-bearing.
- **Visitor exception barrier via sentinel `0`.** `DispatchVisit` is called by the kernel through an `extern "C"` callback; a DuckDB exception (typically `BinderException` for unsupported types) must not unwind across the FFI boundary (UB). Pattern: `try { ... } catch (std::exception &) { stash; return 0; } catch (...) { stash; return 0; }`. The error is re-thrown in C++ after the FFI returns. Sentinel `0` is correct against v0.23 — verified at `build/debug/rust/src/delta_kernel/ffi/src/lib.rs:1283`: `ReferenceSet::default()` initializes `next_id: 1` with the comment "0 is interpreted as None".
- **Pre-FFI type validation** (PERF-5). `DeltaCreateTableSchema::ValidateTypes(const ColumnList &)` is a pure-C++ validator that runs the same `BinderException` checks the visitor would, BEFORE any kernel / engine construction. Saves 10–50 ms of wasted Tokio + credential setup on S3/Azure-backed catalogs when CTAS rejects an unsupported type. Same template as the prior pipeline's `DeltaSchemaJson::ValidateColumnTypes` (now deleted).
- **CCv2 (`parent_commit=true`) CTAS deferred to follow-up.** The kernel exposes `create_table_builder_build_with_committer` for this, and the existing INSERT-side `CommitCallback` is reusable. But no test fixture for CCv2 CTAS exists today — shipping it without tests means the first user discovers any bug. Architect's Open Question 1.
- **`TIMESTAMP_NS` rejected with `BinderException`.** The hand-rolled path explicitly rejected nanosecond timestamps. The first-cut of the kernel-native path silently truncated to microseconds (reviewer's H1 — genuine data-loss regression). The fix rejects `TIMESTAMP_NS` at bind time with a message directing users to `TIMESTAMP` or `TIMESTAMP_MS` if microsecond precision is acceptable. `TIMESTAMP_MS` and `TIMESTAMP_SEC` remain accepted (genuinely lossless).
- **Kept `DeltaTransactionMode` enum (reviewer M3 carried).** Reviewer flagged that `mode == CREATING_TABLE` is always equivalent to `kernel_create_txn != nullptr`, so the enum is technically redundant. Left as-is for now since (a) it makes the CTAS branch in `Commit` / `Rollback` self-documenting at call sites, and (b) leaves room for future modes (CCv2 CTAS would be a third).

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `src/include/storage/delta_ctas.hpp` | **Deleted** | 39 lines; hand-rolled JSON writer header |
| `src/storage/delta_ctas.cpp` | **Deleted** | 203 lines; hand-rolled JSON writer body |
| `src/include/storage/delta_create_table_schema.hpp` | Created | DuckDB-to-kernel schema-visitor declarations + `ValidateTypes` |
| `src/storage/delta_create_table_schema.cpp` | Created | Visitor trampoline + `VisitField` switch + `ValidateType` pre-FFI validator (~227 lines after PERF-5) |
| `CMakeLists.txt` | Modified | Source list: drop `delta_ctas.cpp`, add `delta_create_table_schema.cpp` |
| `src/delta_utils.cpp`, `src/include/delta_utils.hpp` | Modified | New RAII typedefs `KernelCreateTableBuilder` and `KernelCreateTransaction`, matching the existing `TemplatedUniqueKernelPointer` pattern |
| `src/functions/delta_scan/delta_multi_file_list.{cpp,hpp}` | Modified | New `BuildEngine` static factory (engine without snapshot dependency) |
| `src/include/storage/delta_transaction.hpp` | Modified | `DeltaTransactionMode` enum; `kernel_create_txn` + `ctas_extern_engine` + `ctas_partition_columns` fields; `InitializeForNewTable` / `AppendForNewTable` decls; CTAS branch in `Commit`/`Rollback` |
| `src/storage/delta_transaction.cpp` | Modified | Bulk of new logic (~183 inserted): `InitializeForNewTable` (validates types, builds engine, calls kernel builder), `AppendForNewTable` (packages parquet add-files), CTAS branch of `Commit` (`ffi::create_table_commit`) and `Rollback` (`ffi::create_table_free_transaction`) |
| `src/storage/delta_catalog.cpp` | Modified | Removed call to `DeltaSchemaJson::ValidateColumnTypes` (now handled by `DeltaCreateTableSchema::ValidateTypes` inside the transaction init) |
| `src/storage/delta_insert.cpp` | Modified | CTAS arm collapsed from ~75 lines (exclusive file create + cleanup) to a single `InitializeForNewTable` call |
| `src/storage/delta_schema_entry.cpp` | Modified | Removed call to `DeltaSchemaJson::BuildSchemaString` |

Net: **+260 / -328 lines, 12 files touched, 2 deleted, 2 created.**

## sqllogic Tests Added

- `test/sql/main/writing/ctas/ctas_kernel_native.test` — exercises the kernel-native commit path (34 assertions); confirms the version-0 JSON is now kernel-written
- `test/sql/main/writing/ctas/ctas_timestamp_ntz.test` — rewritten by the H1 fix: `statement error` for `TIMESTAMP_NS` with the expected message; `TIMESTAMP_MS` / `TIMESTAMP_SEC` round-trip multiple distinct values and assert preserved timestamps after re-ATTACH (the previous version only queried `count(*)`, so silent truncation was invisible)

The 6 pre-existing CTAS tests (`basic_ctas`, `ctas_attach_existing`, `ctas_empty_select`, `ctas_or_replace_unsupported`, `ctas_then_insert`, `ctas_type_coverage`) pass unchanged.

**Final test status (release build):**
| Suite | Result |
|---|---|
| `test/sql/main/writing/ctas/*` | 109 assertions, 8 test cases — ALL PASS |
| `test/sql/main/writing/*` | 209 assertions, 12 test cases — ALL PASS |
| `test/sql/main/*` | 193 assertions, 13 test cases (1 skipped: httpfs) — ALL PASS |
| `test/sql/issues/*` | 23 assertions, 2 test cases — ALL PASS |
| `test/sql/inlined/*` | 34 assertions, 2 test cases — ALL PASS |
| `test/sql/dat/*` | 173 assertions, 4 test cases — ALL PASS |
| `test/sql/delta_kernel_rs/*` | 196 assertions, 5 test cases — ALL PASS |

Total: **937 assertions pass.**

## Review Status

- **Verdict: APPROVED after 1 iteration (+ trailing H1 fix).**
- Iter 1 (APPROVED with 1 trailing High):
  - **H1** — `TIMESTAMP_NS` silently truncating to microseconds; tests didn't read back the timestamp column so the truncation was invisible. Fixed: bind-time `BinderException`, tests updated to assert preserved values. Same approach as the original CTAS C2 fix.
- **Outstanding Medium/Low items** (deliberately deferred — out of orchestrator scope):
  - **M1**: `DispatchVisit` is assigned to a C function pointer without `extern "C"` linkage. Codebase-wide pattern, technically UB per the C++ standard, no practical defect on current ABIs.
  - **M2**: Original `ctas_timestamp_ntz.test` didn't read back the timestamp column — partially addressed by the H1 fix (which now reads back `ts_ms` and `ts_sec`); a similar concern for fully-supported types still applies to other tests.
  - **M3**: `DeltaTransactionMode` enum is redundant with `kernel_create_txn` nullability. Kept for self-documentation and to leave room for CCv2 CTAS as a future mode.
  - **L1**: `UnpackFieldId`'s parameter named `context` collides with the DuckDB convention reserving that name for `ClientContext &`. Rename to `call_site` or `location`.
  - **L2**: `LIST` and `MAP` visitor paths not covered by any CTAS test. Add a test case with `[1,2,3]` and `MAP {'a':1}` columns.

## Performance

- **Verdict: OPTIMIZED (1 Medium finding actioned)**
- **Optimizations applied:**
  - **PERF-5** — Added `DeltaCreateTableSchema::ValidateTypes` pre-FFI validator (~46-line file-static `ValidateType` + 4-line entry). Called at the top of `InitializeForNewTable` BEFORE `BuildEngine`. Saves 10–50 ms of wasted Tokio runtime init + cloud-credential lookup on S3/Azure-backed catalogs when CTAS rejects an unsupported type. Same template as the prior pipeline's `DeltaSchemaJson::ValidateColumnTypes` (now deleted). Identical error messages preserved (verified by `ctas_timestamp_ntz.test`'s `statement error` block still matching).
- **Optimizations skipped (with reason):**
  - **PERF-1** (Visitor FFI vs. JSON construction): immeasurable — ~100–500 ns delta at 10 columns, dominated by builder/parquet I/O; CTAS is once-per-table.
  - **PERF-2** (`BuildEngine` duplication): no duplication; thin wrapper around shared `CreateBuilder`.
  - **PERF-3** (new `DeltaTransaction` fields): unavoidable per FFI contract; zero-cost on INSERT path.
  - **PERF-4** (`DeltaTransactionMode` branch on INSERT): once-per-transaction, perfectly predicted; not a perf issue.
  - **PERF-6** (function-local statics in `VisitField`): 1-instruction guard check; immeasurable next to FFI call.

## Items for Human Review

1. **CCv2 CTAS still deferred.** Architect's Open Question 1 — `create_table_builder_build_with_committer` is one line away from working, but no test fixture for CCv2 CTAS exists today. Strong recommend: file as follow-up so the next ticket can land both the path AND a test fixture together. Until then `parent_commit=true` continues to throw `NotImplementedException` early in `InitializeForNewTable`.
2. **`KNOWN_ISSUES.md` entry on `VisitNullLiteral` remains open** — the kernel-bump pipeline added a note about `type_tag`/`precision`/`scale` being discarded; this PR didn't touch that code path. The risk still applies whenever decimal-partition NULL pushdown is exercised.
3. **`LIST` and `MAP` visitor paths untested in CTAS** (L2). Add `[1,2,3]` + `MAP {'a':1}` column types to a CTAS test to lock in correctness, before someone refactors the visitor.
4. **Reviewer's M3** — `DeltaTransactionMode` enum carries a small redundancy. Defensible (self-documentation, room for CCv2 CTAS mode), but a future cleanup could collapse to nullable-pointer pattern if no second non-CCv2 mode emerges.
5. **Visitor sentinel verification methodology** — verified against the kernel Rust source for v0.23. If the kernel pin moves again, re-verify against the new tag's `ReferenceSet` (or whatever replaces it). Track as part of any future kernel-bump checklist.
