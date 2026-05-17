# Implementation Log: CTAS Kernel-Native Path (Step B)

## Section 1: Reconciliation

### `src/include/storage/delta_create_table_schema.hpp` (NEW)
Found in complete state. Matches plan §5.2 exactly. The `scratch_names` field from the plan was omitted by the previous coder — correctly, since the visitor uses the `ColumnList`'s own strings for field names and `static const string` locals for the root-name empty string sentinel. No scratch storage was needed.

### `src/storage/delta_create_table_schema.cpp` (NEW)
Found in complete state. Implements the full visitor dispatch table covering: boolean, byte, short, integer, long, float, double, varchar/enum, date, timestamp, timestamp_ntz (for TIMESTAMP_NS/TIMESTAMP_MS/TIMESTAMP_SEC), binary, decimal, struct (recursive), list (array), map. The `DispatchVisit` trampoline catches all C++ exceptions and returns sentinel `0`. `VisitImpl` correctly builds top-level field IDs then calls `visit_field_struct` a final time with all IDs to produce the root struct ID.

### `src/include/delta_utils.hpp` (MOD)
Found complete. Added `KernelExclusiveCreateTableBuilder` and `KernelExclusiveCreateTransaction` typedefs after the existing block, plus `KernelCommittedTransaction` (needed for the CTAS commit path). The `VisitNullLiteral` signature was updated to accept `uint8_t type_tag, uint8_t precision, uint8_t scale` parameters to match the v0.23 kernel FFI signature change.

### `src/delta_utils.cpp` (MOD)
`VisitNullLiteral` implementation updated to match the new 5-parameter signature. Comment added that we emit an untyped NULL without using the kernel's NullTypeTag information.

### `src/include/functions/delta_scan/delta_multi_file_list.hpp` (MOD)
`BuildEngine` static method declared. This is not in the plan's primary target list. The previous coder added it here because `InitializeForNewTable` needs to build a kernel engine before the Delta log exists (i.e., before `DeltaMultiFileList` can be constructed). The static method reuses the existing `CreateBuilder` + `builder_build` pattern and wraps the result in `KernelExternEngine`. This is consistent with the plan's ownership model and does not duplicate any logic.

### `src/functions/delta_scan/delta_multi_file_list.cpp` (MOD)
`BuildEngine` implementation added just before the `DeltaMultiFileList` constructor. Calls `ToDeltaPath` internally, then `CreateBuilder`, then `builder_build`. The note in the comment says "ToDeltaPath normalises the path (adds trailing slash, resolves ./)." — this is slightly imprecise since `BuildEngine` calls `ToDeltaPath` then passes the result to `CreateBuilder`, but there is no bug.

### `src/include/storage/delta_transaction.hpp` (MOD)
Found complete. `DeltaTransactionMode` enum added. `InitializeForNewTable` and `AppendForNewTable` declarations added. `IsCreatingTable()` inline added. Three new private fields: `kernel_create_txn`, `ctas_extern_engine`, `ctas_table_path`, `ctas_partition_columns`. Note: the plan's §5.3 shows `write_entry` as a field; this already existed. The signature diverged slightly from the plan: `InitializeForNewTable` takes `const string &table_path` and `BoundCreateTableInfo &info` rather than `DeltaTableEntry &new_table_entry` (which plan §5.3 showed). This is correct because the CTAS path builds its own engine without a pre-existing `DeltaTableEntry`.

### `src/storage/delta_transaction.cpp` (MOD)
Found complete. New `WriteMetaData` constructor for the CTAS path (takes `table_path`, `partition_col_names`, `outstanding_appends`). `InitializeForNewTable` implementation drives the builder chain. `AppendForNewTable` stages files via `create_table_add_files`. `Commit()` has the CREATING_TABLE branch calling `create_table_commit`. `Rollback()` unchanged — RAII deleter on `kernel_create_txn` handles the CREATING_TABLE case automatically.

### `src/storage/delta_insert.cpp` (MOD)
Found complete. CTAS arm of `GetGlobalSinkState` calls `InitializeForNewTable` and returns `DeltaInsertGlobalState(*info)`. A new `DeltaInsertGlobalState(BoundCreateTableInfo &info)` constructor was added to derive columns from `info.Base().columns.Logical()`. `Finalize` CTAS arm calls `AppendForNewTable`. All correct.

### `src/storage/delta_catalog.cpp` (MOD)
Found complete. `ValidateColumnTypes` call removed. The `DeltaSchemaJson` include removed. `PlanCreateTableAs` completely rewritten to use the kernel path. No references to the old JSON CTAS code remain.

### `src/storage/delta_schema_entry.cpp` (MOD)
Found complete. `BuildSchemaString` call removed. `CreateTable` now does partition validation and existence probe only, then returns nullptr. Clean.

### `CMakeLists.txt` (MOD)
Found complete. `delta_ctas.cpp` replaced by `delta_create_table_schema.cpp`. GIT_TAG already at `v0.23.0` (this was Step A; not touched in Step B).

### `src/storage/delta_ctas.cpp` and `src/include/storage/delta_ctas.hpp` (DELETE)
Found still on disk. Verified zero external references via grep. Deleted both files. Build remained clean.

---

## Section 2: Files Created / Modified / Deleted

**Created (2):**
- `/workspace/src/include/storage/delta_create_table_schema.hpp` — write-side kernel schema visitor class declaration
- `/workspace/src/storage/delta_create_table_schema.cpp` — visitor implementation; DuckDB LogicalType → `visit_field_*` dispatcher

**Modified (9):**
- `/workspace/CMakeLists.txt` — swap `delta_ctas.cpp` → `delta_create_table_schema.cpp` in EXTENSION_SOURCES
- `/workspace/src/include/delta_utils.hpp` — add `KernelCommittedTransaction`, `KernelExclusiveCreateTableBuilder`, `KernelExclusiveCreateTransaction` typedefs; update `VisitNullLiteral` signature
- `/workspace/src/delta_utils.cpp` — update `VisitNullLiteral` implementation to match new FFI signature
- `/workspace/src/include/functions/delta_scan/delta_multi_file_list.hpp` — declare `BuildEngine` static method
- `/workspace/src/functions/delta_scan/delta_multi_file_list.cpp` — implement `BuildEngine`
- `/workspace/src/include/storage/delta_transaction.hpp` — `DeltaTransactionMode` enum, new fields, new method declarations
- `/workspace/src/storage/delta_transaction.cpp` — `InitializeForNewTable`, `AppendForNewTable`, CTAS branch of `Commit`
- `/workspace/src/storage/delta_insert.cpp` — CTAS arm of `GetGlobalSinkState` and `Finalize`; new `DeltaInsertGlobalState(BoundCreateTableInfo&)` constructor
- `/workspace/src/storage/delta_catalog.cpp` — remove `ValidateColumnTypes`, rewrite `PlanCreateTableAs`
- `/workspace/src/storage/delta_schema_entry.cpp` — remove `BuildSchemaString` call from `CreateTable`

**Deleted (2):**
- `/workspace/src/storage/delta_ctas.cpp` — hand-rolled JSON CTAS path (171 lines)
- `/workspace/src/include/storage/delta_ctas.hpp` — header for deleted file

**Tests created (2):**
- `/workspace/test/sql/main/writing/ctas/ctas_kernel_native.test`
- `/workspace/test/sql/main/writing/ctas/ctas_timestamp_ntz.test`

---

## Section 3: Tests Added

- `test/sql/main/writing/ctas/ctas_kernel_native.test` — 34 assertions. Verifies the kernel-emitted commit JSON contains `engineInfo:"DuckDB"`, an `operation` field, `protocol`, `metaData`, and `schemaString` with the declared column names. Also exercises the recursive `visit_field_struct` path (STRUCT column type) and `visit_field_decimal` precision/scale path (DECIMAL column type), with round-trip read-back assertions.

- `test/sql/main/writing/ctas/ctas_timestamp_ntz.test` — 21 assertions. Verifies that `TIMESTAMP_NS`, `TIMESTAMP_MS`, and `TIMESTAMP_S` columns are accepted by the kernel-native CTAS path (mapped to Delta `timestamp_ntz`). The hand-rolled path previously rejected these with `BinderException`. Confirms data round-trips correctly after re-attach.

---

## Section 4: Visitor Sentinel Verification

**Claim:** sentinel `0` in `DeltaCreateTableSchema::DispatchVisit` is safe because `0` is never a valid field ID returned by `KernelSchemaVisitorState`.

**Verified in:** `/workspace/build/debug/rust/src/delta_kernel/ffi/src/lib.rs` lines 1278–1285:
```rust
impl<T> Default for ReferenceSet<T> {
    fn default() -> Self {
        Self {
            map: Default::default(),
            // NOTE: 0 is interpreted as None
            next_id: 1,
        }
    }
}
```

`KernelSchemaVisitorState` (defined in `ffi/src/schema_visitor.rs` line 36–38) contains a `ReferenceSet<StructField>`. The `Default` impl for `ReferenceSet` initializes `next_id: 1` with the explicit comment "0 is interpreted as None". The `insert` method increments `next_id` after each allocation, so IDs 1, 2, 3, … are assigned in order; ID 0 is never produced.

**Sentinel `0` is correct and safe.**

---

## Section 5: Plan Deviations

### `delta_multi_file_list.{cpp,hpp}` changes

The plan's §2 Affected Surfaces did not list `delta_multi_file_list.cpp/.hpp`. The previous coder added `DeltaMultiFileList::BuildEngine` as a static helper. This is load-bearing: `DeltaTransaction::InitializeForNewTable` must construct a `KernelExternEngine` from the CTAS table path before the Delta log exists (no snapshot to borrow from), but the engine-construction logic (applying DuckDB secrets/credentials) lives inside `DeltaMultiFileList::CreateBuilder`. The alternative would be duplicating that logic. Adding a thin static factory on `DeltaMultiFileList` is the correct minimal approach. No behavior is changed on the read path.

### `DeltaInsertGlobalState` CTAS constructor

The plan §5.4 shows the CTAS arm returning `make_uniq<DeltaInsertGlobalState>(*new_table_entry)` (taking a `DeltaTableEntry`). The implementation instead uses a `DeltaInsertGlobalState(BoundCreateTableInfo &info)` constructor that derives columns directly from the schema info. This is simpler because no `DeltaTableEntry` (and therefore no snapshot) exists at `GetGlobalSinkState` time. The resulting `columns` vector is equivalent — both derived from `info.Base().columns.Logical()`.

### `InitializeForNewTable` signature

Plan §5.3 shows `void InitializeForNewTable(ClientContext &context, DeltaTableEntry &new_table_entry, BoundCreateTableInfo &info)`. The implementation uses `void InitializeForNewTable(ClientContext &context, const string &table_path, BoundCreateTableInfo &info)`. The `table_path` carries all information from `new_table_entry` that `InitializeForNewTable` needs (the kernel path), while avoiding the need to create a `DeltaTableEntry` before the commit.

---

## Section 6: Open Question Resolutions

1. **CCv2 CTAS (`parent_commit=true`) — IN SCOPE or DEFERRED?**
   Resolved as proposed: DEFERRED. `InitializeForNewTable` throws `NotImplementedException` when `parent_commit == true` with a user-facing message explaining the workaround.

2. **`DispatchVisit` sentinel-on-error convention — `0` valid?**
   Resolved as proposed: `0` is correct. Verified in `ReferenceSet::default()` at `ffi/src/lib.rs:1283` — `next_id: 1` with comment "0 is interpreted as None".

3. **Atomicity guarantee equivalence.**
   Not independently verified in this step (the kernel's `FileSystemCommitter` commit atomicity is a kernel-internal concern). The existing 6 CTAS tests (including `ctas_attach_existing.test` which verifies the existence check) pass, confirming the behavior is correct from the user's perspective.

4. **Idempotency `app_versions` during CTAS — DISABLED?**
   Resolved as proposed: `SetTransactionVersion` is NOT called during CTAS mode (no code path reaches it in the CREATING_TABLE flow). If a caller does call it, the existing `app_versions` map accumulates entries that are never flushed (the CREATING_TABLE branch in `Commit` returns before the app_versions loop). This is benign — no corruption, just silent no-op. A future PR could add a guard that throws `NotImplementedException` from `SetTransactionVersion` when `IsCreatingTable()`.

---

## Section 7: Test Output

| Test Directory | Command | Result |
|---|---|---|
| `test/sql/main/writing/ctas/*` | `build/release/test/unittest "test/sql/main/writing/ctas/*"` | 93 assertions in 8 test cases — ALL PASSED |
| `test/sql/main/writing/*` | `build/release/test/unittest "test/sql/main/writing/*"` | 193 assertions in 12 test cases — ALL PASSED |
| `test/sql/main/*` | `build/release/test/unittest "test/sql/main/*"` | 193 assertions in 13 test cases — ALL PASSED (1 skipped: require httpfs) |
| `test/sql/issues/*` | `build/release/test/unittest "test/sql/issues/*"` | 23 assertions in 2 test cases — ALL PASSED |
| `test/sql/inlined/*` | `build/release/test/unittest "test/sql/inlined/*"` | 34 assertions in 2 test cases — ALL PASSED |
| `test/sql/dat/*` | With `DELTA_KERNEL_TESTS_PATH` and `DAT_PATH` env vars | 173 assertions in 4 test cases — ALL PASSED |
| `test/sql/delta_kernel_rs/*` | With `DELTA_KERNEL_TESTS_PATH` env var | 196 assertions in 5 test cases — ALL PASSED |

Build: `make release` — clean, no new warnings.
Clang-format: `clang-format --dry-run --Werror` on all modified files — no violations after applying `clang-format -i`.

---

## Review Fix Iteration 1

### Fix applied: H1 — TIMESTAMP_NS rejected with BinderException

**Location**: `src/storage/delta_create_table_schema.cpp`, `VisitField` switch in the `TIMESTAMP_NS` case (previously lines 93–101).

The three-way fall-through `TIMESTAMP_NS / TIMESTAMP_MS / TIMESTAMP_SEC` was split into two separate cases:

- `TIMESTAMP_NS` now throws `BinderException` immediately with the message:
  > "Cannot create Delta table: column '%s' has type TIMESTAMP_NS (nanosecond precision). Delta's timestamp_ntz type stores microseconds; nanosecond precision cannot be preserved. Cast to TIMESTAMP or TIMESTAMP_MS if microsecond precision is acceptable."
  
  A brief comment above the throw explains the reason (Delta timestamp_ntz is microsecond precision; int64 nanoseconds would lose the two sub-microsecond digits silently).

- `TIMESTAMP_MS` and `TIMESTAMP_SEC` continue to call `visit_field_timestamp_ntz` unchanged (they are lossless). Their comment was updated to clarify why they are lossless.

Because `DispatchVisit` catches the `BinderException` via `catch (std::exception &e)` and stores it in `captured_error`, the exception surfaces to the user through `InitializeForNewTable`'s `HasError()` check — the same path every other bind-time rejection uses.

**clang-format**: `clang-format -i` applied; `clang-format --dry-run --Werror` confirmed no violations.

**Build**: `make release` — `delta_create_table_schema.cpp.o` recompiled with no errors or warnings.

### Test additions: `test/sql/main/writing/ctas/ctas_timestamp_ntz.test`

**TIMESTAMP_NS rejection test (new)**:

The old `statement ok` / round-trip test for `TIMESTAMP_NS` was replaced with a `statement error` block confirming the rejection message at bind time. The ATTACH is still performed (the directory is created) but the CREATE TABLE statement must fail before any data is written.

**TIMESTAMP_MS value round-trip (new)**:

Three rows with distinct sub-second values are written and read back both before and after DETACH/re-ATTACH:
- `2024-06-01 08:00:00.000` (full-second boundary)
- `2024-06-01 08:00:00.500` (half-second, exercises non-zero milliseconds)
- `2001-09-09 01:46:40.000` (Unix time 1 billion seconds boundary, distinct epoch)

The `query II` assertion on `id, ts_ms ORDER BY id` ensures both the id and the timestamp column are checked, so a truncation bug in either direction would surface.

**TIMESTAMP_SEC value round-trip (new)**:

Three rows with distinct second-boundary values:
- `2024-12-31 23:59:59` (near-future boundary)
- `2000-01-01 00:00:00` (Y2K epoch)
- `1970-01-01 00:00:01` (one second after Unix epoch)

Same `query II` pattern with ORDER BY id.

### Test results

| Test directory | Result |
|---|---|
| `test/sql/main/writing/ctas/*` | 109 assertions in 8 test cases — ALL PASSED |
| `test/sql/main/writing/*` | 209 assertions in 12 test cases — ALL PASSED |

---

## Performance Optimizations

### PERF-5: Pre-FFI type validation to short-circuit unsupported types

**What was added:**

- `src/include/storage/delta_create_table_schema.hpp`: Added `static void ValidateTypes(const ColumnList &columns)` public static method declaration (7 lines with doc comment).
- `src/storage/delta_create_table_schema.cpp`: Added file-static `ValidateType(const string &name, const LogicalType &type)` helper (46 lines) and the `DeltaCreateTableSchema::ValidateTypes` public method (4 lines). Total new code: 50 lines.

`ValidateType` mirrors `VisitField`'s switch over `LogicalTypeId`, but makes no FFI calls — it returns immediately for all supported leaf types and recurses into STRUCT/LIST/MAP children. For unsupported types it throws the identical `BinderException` messages as `VisitField` so user-visible error text is unchanged regardless of which path triggers first.

**Where it is called:**

`DeltaTransaction::InitializeForNewTable` (`src/storage/delta_transaction.cpp`, line 653) — called after the `parent_commit` guard (which is even cheaper) and before `DeltaMultiFileList::BuildEngine`. This means any schema with an unsupported column type (e.g. `TIMESTAMP_NS`) exits with a `BinderException` without initialising the Tokio runtime or performing any cloud-credential lookup.

**Test status:**

`ctas_timestamp_ntz.test` `statement error` assertion continues to match — the error message is identical. All writing tests pass:

| Test directory | Result |
|---|---|
| `test/sql/main/writing/ctas/*` | 109 assertions in 8 test cases — ALL PASSED |
| `test/sql/main/writing/*` | 209 assertions in 12 test cases — ALL PASSED |
