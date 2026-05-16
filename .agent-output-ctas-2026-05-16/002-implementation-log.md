# Implementation Log: CTAS for the delta Extension

## Section 1: Reconciliation

### `src/include/storage/delta_ctas.hpp` (new file, 33 lines)
**State found:** v1 hand-rolled JSON approach. Declares `DeltaSchemaJson` with `BuildSchemaString` and `BuildCommitJson`.
**Decision:** Keep. See Section 4 — the v2 kernel-native path is architecturally impossible at v0.21.0 (no `SharedSchema` constructor exists in the FFI).

### `src/storage/delta_ctas.cpp` (new file, 171 lines)
**State found:** v1 hand-rolled JSON approach. Implements `DeltaSchemaJson::BuildSchemaString` (DuckDB type → Delta JSON schema string) and `DeltaSchemaJson::BuildCommitJson` (produces `00000000000000000000.json` content with commitInfo + metaData + protocol actions).
**Decision:** Keep. The JSON is correct Delta protocol; the kernel parses it fine at CTAS read-back time (confirmed by all tests passing).

### `CMakeLists.txt`
**State found:** Added `src/storage/delta_ctas.cpp` to `EXTENSION_SOURCES`.
**Decision:** Keep. Correct and necessary.

### `src/delta_extension.cpp`
**State found:** Added `allow_create` option parsing (reads bool from attach options, sets `res->allow_create`).
**Decision:** Keep. Correct per v2 plan §5.5.

### `src/include/storage/delta_catalog.hpp`
**State found:** Added `bool allow_create = false` field to `DeltaCatalog`.
**Decision:** Keep. Correct per v2 plan §5.5.

### `src/include/storage/delta_schema_entry.hpp`
**State found:** Added private helper `CreateTableEntryForNewTable(ClientContext &context, BoundCreateTableInfo &info)`.
**Decision:** Keep. Correct per v2 plan §5.3.

### `src/include/storage/delta_transaction.hpp`
**State found:** Added three methods (`SetPendingCreateTable`, `TakePendingCreateTable`, `HasPendingCreateTable`) and a `unique_ptr<DeltaTableEntry> pending_create_table` field. No `DeltaTransactionMode` enum (v2 plan proposed it but the implementation opted for a simpler `pending_create_table` nullable pattern).
**Decision:** Keep. The simpler nullable-pointer pattern achieves the same result as the mode enum without the CREATING_TABLE vs REGULAR complexity. The implementation is semantically equivalent.

### `src/storage/delta_catalog.cpp`
**State found:** Full implementation of `PlanCreateTableAs`. Handles `CREATE OR REPLACE TABLE` rejection, parquet copy function lookup, partition column resolution, directory creation, `PhysicalCopyToFile` binding, and `DeltaInsert` construction.
**Decision:** Keep. Mirrors `PlanInsert` pattern correctly. Also fixed a pre-existing `int64_t` vs `idx_t` issue in the INSERT loop (changed two `int64_t` loop counters to `idx_t`).

### `src/storage/delta_schema_entry.cpp`
**State found:** Full implementation of `CreateTable` (validation + pending-create entry) and `CreateTableEntryForNewTable` (builds a snapshot-less `DeltaTableEntry`). Also added `allow_create` + no-snapshot guard in `LookupEntry`.
**Decision:** Keep. Logic is correct. The path check using `00000000000000000000.json` existence is the right heuristic for v1 (instead of trying to initialize a kernel snapshot, which would fail on an empty directory).

### `src/storage/delta_insert.cpp`
**State found:** CTAS arm of `GetGlobalSinkState` (writes version-0 JSON, then calls `InitializeTableEntry`) and CTAS arm of `Finalize` (uses `schema->catalog` instead of `table->catalog`). Also added includes for `FileSystem`, `FileOpenFlags`, `UUID`, `Timestamp`, `ColumnRefExpression`.
**Decision:** Keep. Correct split between the INSERT path and CTAS path.

### `src/storage/delta_transaction.cpp`
**State found:** Three new methods (`SetPendingCreateTable`, `TakePendingCreateTable`, `HasPendingCreateTable`) at end of file. Also fixed a self-referential `unordered_map<string, StatNode>` ODR issue by adding `StatNodeMap` typedef and changing `StatNode::children` from inline `unordered_map` to `unique_ptr<StatNodeMap>` (this was a pre-existing latent bug — the struct was not truly self-referential before because the children were in a separate unordered_map, but the forward declaration was missing).
**Decision:** Keep. The `StatNode` fix is correct and necessary (incomplete type in `unordered_map` value is UB).

## Section 2: Files Created or Modified

### New files
- `/workspace/src/include/storage/delta_ctas.hpp` — `DeltaSchemaJson` struct declaration: `BuildSchemaString` (DuckDB ColumnList → Delta StructType JSON escaped string) and `BuildCommitJson` (produces version-0 log file content).
- `/workspace/src/storage/delta_ctas.cpp` — Implementation of `DeltaSchemaJson` including `LogicalTypeToDeltaType` type mapper (boolean, byte, short, integer, long, float, double, string, date, timestamp, binary, decimal, struct, array, map; throws `BinderException` for unsupported types).

### Modified files
- `/workspace/CMakeLists.txt` — Added `delta_ctas.cpp` to `EXTENSION_SOURCES`.
- `/workspace/src/delta_extension.cpp` — Added `allow_create` attach option parsing.
- `/workspace/src/include/storage/delta_catalog.hpp` — Added `bool allow_create = false` field.
- `/workspace/src/include/storage/delta_schema_entry.hpp` — Added private `CreateTableEntryForNewTable` helper declaration.
- `/workspace/src/include/storage/delta_transaction.hpp` — Added `SetPendingCreateTable`, `TakePendingCreateTable`, `HasPendingCreateTable` declarations and `pending_create_table` field.
- `/workspace/src/storage/delta_catalog.cpp` — Implemented `PlanCreateTableAs`; fixed loop variable types from `int64_t` to `idx_t` in `PlanInsert`.
- `/workspace/src/storage/delta_schema_entry.cpp` — Implemented `CreateTable` and `CreateTableEntryForNewTable`; added `allow_create` guard in `LookupEntry`.
- `/workspace/src/storage/delta_insert.cpp` — Implemented CTAS arms in `GetGlobalSinkState` and `Finalize`.
- `/workspace/src/storage/delta_transaction.cpp` — Added three pending-create methods; fixed `StatNode::children` self-referential map to use `unique_ptr<StatNodeMap>`.

## Section 3: Tests Added or Kept

All 6 test files under `test/sql/main/writing/ctas/` were on disk; all kept (no deletions or additions):

- `basic_ctas.test` — ATTACH with `allow_create=true`, CTAS 3 rows, verify row count, DETACH+re-ATTACH persistence check (38 assertions total across all 6 tests).
- `ctas_attach_existing.test` — CTAS to a path with an existing Delta table must fail with `CatalogException` containing "Delta table already exists".
- `ctas_empty_select.test` — CTAS with `WHERE false` produces zero rows; table schema is present; `count(*) = 0`.
- `ctas_or_replace_unsupported.test` — `CREATE OR REPLACE TABLE` throws error containing "CREATE OR REPLACE TABLE is not supported for Delta tables".
- `ctas_then_insert.test` — CTAS followed by `INSERT INTO` on the same table; verifies commit-0 + commit-1 pattern works.
- `ctas_type_coverage.test` — CTAS with BOOLEAN, TINYINT, SMALLINT, INTEGER, BIGINT, FLOAT, DOUBLE, VARCHAR, DATE, DECIMAL(10,3); verifies row count and a scalar value.

## Section 4: Deviations from v2 Plan and Why

### Critical deviation: v2 kernel-native FFI path is impossible at v0.21.0

The v2 plan proposes using `ffi::get_create_table_builder(path, Handle<SharedSchema>, ...)`. This requires constructing a `Handle<SharedSchema>` for a brand-new (not-yet-existing) table.

After inspecting `build/release/codegen/include/generated_delta_kernel_ffi.hpp`, **there is no FFI function that creates a `SharedSchema` from scratch**. Every function returning `Handle<SharedSchema>` requires an existing snapshot, scan, or table_changes handle:
- `logical_schema(Handle<SharedSnapshot>)`
- `scan_logical_schema(Handle<SharedScan>)`
- `scan_physical_schema(Handle<SharedScan>)`
- `table_changes_schema(Handle<ExclusiveTableChanges>)`
- `get_write_schema(Handle<SharedWriteContext>)` — requires existing `ExclusiveTransaction`/`ExclusiveCreateTransaction`

The chicken-and-egg problem: `get_create_table_builder` needs a `SharedSchema`, but `SharedSchema` can only come from an existing table, but the table doesn't exist yet. The `get_write_schema` approach via `create_table_get_write_context` also requires a `Handle<ExclusiveCreateTransaction>` first, which requires the schema handle.

The only schema-creation FFI at v0.21.0 is `visit_schema` (read-only visitor pattern, not a builder). No `schema_from_json` or `schema_builder` entry point exists.

**Consequence:** The entire kernel-native CTAS path (`get_create_table_builder` → `create_table_builder_build` → `create_table_add_files` → `create_table_commit`) is not implementable at v0.21.0. The hand-rolled JSON approach (v1) is the correct implementation for this kernel version.

The v1 JSON writer produces a valid Delta protocol commit file that the kernel reads back correctly on the next snapshot initialization. All CTAS tests confirm this round-trip works.

### Minor deviation: No `DeltaTransactionMode` enum

The v2 plan proposed a `DeltaTransactionMode` enum (`REGULAR` / `CREATING_TABLE`). The implementation uses a simpler pattern: `pending_create_table` is non-null during the CTAS bind phase, and the `HasPendingCreateTable()` check replaces the mode check. This is functionally equivalent and simpler.

### Minor deviation: `KernelExclusiveCreateTableBuilder` / `KernelExclusiveCreateTransaction` typedefs not added to `delta_utils.hpp`

Since the kernel-native path is impossible, these RAII typedefs serve no purpose and were not added. The v1 implementation does not hold any `ExclusiveCreateTableBuilder` or `ExclusiveCreateTransaction` handles.

### Exact schema-handle FFI entry point name

The relevant symbols confirmed in the generated header:
- `ffi::SharedSchema` — struct name correct
- `ffi::free_schema(Handle<SharedSchema>)` — free function name correct
- `ffi::get_create_table_builder(KernelStringSlice path, Handle<SharedSchema> schema, KernelStringSlice engine_info, Handle<SharedExternEngine> engine)` — entry point exists but requires `SharedSchema` input (blocked as described above)
- `ffi::create_table_builder_build(Handle<ExclusiveCreateTableBuilder>, Handle<SharedExternEngine>)` — exists
- `ffi::create_table_builder_build_with_committer(...)` — exists
- `ffi::create_table_add_files(Handle<ExclusiveCreateTransaction>, Handle<ExclusiveEngineData>)` — exists
- `ffi::create_table_commit(Handle<ExclusiveCreateTransaction>, Handle<SharedExternEngine>)` — exists
- `ffi::create_table_free_transaction(Handle<ExclusiveCreateTransaction>)` — exists
- `ffi::free_create_table_builder(Handle<ExclusiveCreateTableBuilder>)` — exists

All v2 plan FFI names are confirmed correct. The blocker is schema construction only.

## Section 5: Test Output

### CTAS tests
```
build/release/test/unittest "test/sql/main/writing/ctas/*"
All tests passed (38 assertions in 6 test cases)
```

### All writing tests (CTAS + pre-existing)
```
build/release/test/unittest "test/sql/main/writing/*"
All tests passed (138 assertions in 10 test cases)
```
(6 CTAS tests + 4 pre-existing: `incremental_snapshot`, `non_nullable`, `checkpoint`, `transaction_multi_insert`)

### All main tests
```
build/release/test/unittest "test/sql/main/*"
All tests passed (1 skipped test, 138 assertions in 11 test cases)
```
(1 skipped: requires httpfs)

### Inlined tests
```
build/release/test/unittest "test/sql/inlined/*"
All tests passed (34 assertions in 2 test cases)
```

### Issue regression tests
```
build/release/test/unittest "test/sql/issues/*"
All tests passed (23 assertions in 2 test cases)
```

### Tests that could not be run
- `test/sql/dat/*` — requires `DAT_PATH` environment variable (kernel test data)
- `test/sql/delta_kernel_rs/*` — requires `DELTA_KERNEL_TESTS_PATH`
- `test/sql/generated/*` — requires `GENERATED_DATA_AVAILABLE=1` and Java+PySpark
- `test/sql/cloud/*` — requires cloud backend servers
- `test/sql/golden_tests/*` — requires `GOLDEN_TABLES_PATH`

### Build
- Release build: clean, no new warnings, no errors.
- Debug build: linker OOM (signal 9) — machine memory constraint, not a code defect. The extension C++ sources compiled cleanly (confirmed by `cmake --build build/debug -- -j2 2>&1 | grep "^/workspace/src/.*error:"` returning no output).
- `clang-format --dry-run -Werror` on all modified files: clean after `clang-format -i` pass.

## Review Fix Iteration 1

### C1 — D_ASSERT on user-controlled partition expression type

**Files changed:** `src/storage/delta_catalog.cpp`, `src/storage/delta_insert.cpp`

- `delta_catalog.cpp` (`PlanCreateTableAs`): Replaced the two `D_ASSERT(pk->type == ExpressionType::COLUMN_REF)` and `D_ASSERT(found)` guards with proper `if (pk->type != ExpressionType::COLUMN_REF) { throw BinderException(...) }` and `if (!found) { throw BinderException(...) }`.
- `delta_insert.cpp` (`GetGlobalSinkState`): Replaced the single `D_ASSERT(pk->type == ExpressionType::COLUMN_REF)` with `if (pk->type != ExpressionType::COLUMN_REF) { throw BinderException(...) }`.
- Also added `#include "storage/delta_ctas.hpp"` to `delta_catalog.cpp` (needed for H1's `BuildSchemaString` call, see below).

### C2 — timestamp_ntz maps to wrong protocol version

**Files changed:** `src/storage/delta_ctas.cpp`

- In `LogicalTypeToDeltaType`, the `TIMESTAMP_NS`, `TIMESTAMP_MS`, `TIMESTAMP_SEC` case that previously returned `"timestamp_ntz"` now throws `BinderException` explaining that `timestamp_ntz` requires `minReaderVersion=3`/`minWriterVersion=7` (not yet supported), and directs users to use `TIMESTAMP` or `TIMESTAMP WITH TIME ZONE`.

### H1 — CreateTable validation bypassed on happy-path CTAS

**Files changed:** `src/storage/delta_catalog.cpp`

- Added `DeltaSchemaJson::BuildSchemaString(columns)` call in `PlanCreateTableAs` (before any I/O) to validate all column types have Delta representations at bind time.
- Added partition-key expression-type and column-existence checks in `PlanCreateTableAs` (replacing the D_ASSERT-based checks that were erroneously assumed to have run via `CreateTable`).
- Also added `#include "storage/delta_ctas.hpp"` to `delta_catalog.cpp` to make `DeltaSchemaJson` available.
- The duplicate validation in `CreateTable` is kept (for the "already exists" error path) since that code is still reachable by DuckDB's `PhysicalCreateTable` routing.

### H2 — Dead pending_create_table machinery removed

**Files changed:** `src/include/storage/delta_transaction.hpp`, `src/storage/delta_transaction.cpp`, `src/storage/delta_schema_entry.cpp`, `src/include/storage/delta_ctas.hpp`

- Removed `SetPendingCreateTable`, `TakePendingCreateTable`, `HasPendingCreateTable` declarations from `delta_transaction.hpp`.
- Removed `pending_create_table` field from `delta_transaction.hpp`.
- Removed the three method implementations from `delta_transaction.cpp`.
- In `delta_schema_entry.cpp::CreateTable`: removed the `SetPendingCreateTable` call and the `CreateTableEntryForNewTable` call after validation passes. The function now returns `nullptr` after validation (this branch is unreachable during CTAS since DuckDB routes CTAS through `PlanCreateTableAs`).
- In `delta_schema_entry.cpp::LookupEntry`: removed the `HasPendingCreateTable()` guard. The allow_create path now simply checks filesystem existence and returns `nullptr` when the log file doesn't exist.
- Removed the now-unused `GetDeltaTransaction` free function from `delta_schema_entry.cpp`; its equivalent is now inlined directly at the top of the `LookupEntry` if-block as `auto &delta_transaction = transaction.transaction->Cast<DeltaTransaction>()`.

### H3 — Orphaned version-0 file on InitializeTableEntry failure

**Files changed:** `src/storage/delta_insert.cpp`

- Wrapped the version-0 file write (step 1) and `InitializeTableEntry` call (step 2) in `GetGlobalSinkState` in a `try/catch(...)` block.
- On any exception, calls `fs.TryRemoveFile(version0_path)` (using the `fs` reference already in scope) then re-throws. This ensures a failed CTAS leaves no `00000000000000000000.json` artifact, so a retry to the same path will not fail with "Delta table already exists".

## Review Fix Iteration 2

### Dead code removal: `CreateTableEntryForNewTable`

**Files changed:** `src/include/storage/delta_schema_entry.hpp`, `src/storage/delta_schema_entry.cpp`

The H2 cleanup in iteration 1 removed all callers of `CreateTableEntryForNewTable` but left the declaration and implementation in place. This iteration removes both:

- Removed the private declaration (lines ~50-52) from `delta_schema_entry.hpp`, including its comment block.
- Removed the 21-line implementation (lines 99-119) from `delta_schema_entry.cpp`.

Confirmed zero callers via `grep -rn CreateTableEntryForNewTable src/` before deletion. Both files passed `clang-format -i` with no changes. `cmake --build build/debug --target delta_extension` and `cmake --build build/release --target delta_extension` both built clean with no new errors or warnings. `build/release/test/unittest "test/sql/main/writing/*"` passed all 138 assertions in 10 test cases.

### Test files touched

None — all existing 6 CTAS tests and 4 pre-existing writing tests continue to pass without modification (138 assertions in 10 test cases). The C2 fix (TIMESTAMP_NS/MS/SEC now throws) is consistent with `ctas_type_coverage.test` which does not test those types.

### Build results

- Release build: clean, no new warnings, no errors. `[100%] Built target unittest`.
- Debug build: compilation clean; linker OOM (signal 9) — machine memory constraint, not a code defect.
- `clang-format-11 --dry-run -Werror` on all 8 touched files: clean (no output).

## Performance Optimizations

Applied all five optimizations from `004-perf-findings.md`. Each was built and tested in sequence against `test/sql/main/writing/*` (10 test cases, 138 assertions).

### OPT-1 (HIGH) — applied
Moved the `allow_create` `FileExists` stat inside the `lock` scope and after both fast paths (`GetTableEntry` and `cached_table` present). The check now runs at most once per schema entry lifetime instead of once per query.
File: `src/storage/delta_schema_entry.cpp`

### OPT-2 (HIGH) — applied
Added `if (append_files.empty()) { return; }` at the top of `DeltaTransaction::Append`. Zero-row CTAS no longer initializes a kernel FFI transaction.
File: `src/storage/delta_transaction.cpp`

### OPT-3 (MEDIUM) — applied
Added `DeltaSchemaJson::ValidateColumnTypes(const ColumnList &)` that calls `LogicalTypeToDeltaType` per column and discards results (no JSON string built). Updated `PlanCreateTableAs` in `delta_catalog.cpp` to call `ValidateColumnTypes` instead of `BuildSchemaString`; `GetGlobalSinkState` still uses `BuildSchemaString` when the JSON is actually needed.
Files: `src/include/storage/delta_ctas.hpp`, `src/storage/delta_ctas.cpp`, `src/storage/delta_catalog.cpp`

### OPT-4 (MEDIUM) — applied
Replaced the hand-rolled `DirectoryExists` + `CreateDirectory` loop (up to `path_depth` round-trips) with a single `fs.CreateDirectoriesRecursive(delta_path)` call.
File: `src/storage/delta_catalog.cpp`

### OPT-5 (MEDIUM) — applied
Added `reserve` calls to `fields_json` in `BuildRawSchemaJson` (~70 bytes × column count) and in the STRUCT case of `LogicalTypeToDeltaType` (~70 bytes × child count). Rewrote `BuildCommitJson` to use a single pre-reserved `result` string with `+=` appends instead of `operator+` chains; also pre-reserved `part_cols_json` (~24 bytes × partition column count).
File: `src/storage/delta_ctas.cpp`

### Final test count

`build/release/test/unittest "test/sql/main/writing/*"` — All tests passed (138 assertions in 10 test cases).
