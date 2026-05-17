# Architecture Plan: bump delta-kernel-rs v0.21.0 -> v0.23.0

## 1. Scope of bump

The bump crosses two minor releases (v0.21.0 -> v0.22.0 -> v0.23.0). v0.22 is the dominant one for our FFI surface; v0.23 is much smaller for engines.

**v0.22.0 (high impact for us):**
- Robust partitioned-write APIs (#2356): `get_write_context` -> `get_unpartitioned_write_context` / partitioned variant. Old symbol removed.
- `get_create_table_builder` now takes `&EngineSchema` visitor (#2378). This is the API we were missing in v0.21 that forced `delta_ctas.cpp`'s hand-rolled JSON path.
- Typed null literals (#2375): `visit_expression_literal_null` / `EngineExpressionVisitor::visit_literal_null` now take a `NullTypeTag` byte (+ precision/scale for decimal).
- Default to relative paths in `add.path` (#2410): kernel now writes `abc.parquet` not `s3://bucket/.../abc.parquet`.
- `scan_table_changes_next` returns `*mut ArrowFFIData` (#2430): we don't call this, but it's worth noting if we add CDF later.
- Arrow batch-mode scan metadata FFI (#2395) and new `create_table_*` / `remove_files` family (#2296/#2297) added.
- FFI module reorg: `ffi/src/expressions/{mod,kernel}.rs` split into `expressions/{mod, engine_visitor, kernel_visitor}.rs`. C symbol names (the ones cbindgen exports) are unchanged.

**v0.23.0 (low impact for us):**
- `delta.parquet.compression.codec` table property (#2523) - internal Rust struct, doesn't touch the FFI we use.
- `ExpressionTransform` / `SchemaTransform` carriers (#2151) - internal Rust trait, doesn't touch FFI.
- New FFI extras we don't strictly need yet: `with_operation` (#2532), `CommittedTransaction` handles (#2488), `IncrementalScanBuilder` (#2519). Worth keeping in mind for follow-ups.

**Feature flags** (`scripts`/`CMakeLists.txt`):
- We pass `default-engine-rustls,arrow,test-ffi,delta-kernel-unity-catalog,tracing`. All five still exist in v0.23.0 (`ffi/Cargo.toml` confirmed). No rename or removal.
- `arrow` is now an alias for `arrow-58`; the kernel raised the minimum supported arrow to 57 in v0.21 (#2116). DuckDB's vendored arrow remains compatible.

**Test data layout**: `kernel/tests/data/` and `acceptance/tests/dat/` directory structure unchanged between v0.21 and v0.23. The Makefile env exports keep working without changes.

## 2. Breakage inventory

Per affected file in our tree, with the fix shape.

### 2.1 `src/storage/delta_transaction.cpp`

| Symbol used today | Status in v0.23 | Fix shape |
|---|---|---|
| `ffi::get_write_context(txn.get())` | **Removed.** Replaced by partition-aware split. | Call `ffi::get_unpartitioned_write_context(txn, engine)` (also returns `ExternResult<Handle<SharedWriteContext>>` and takes the engine handle - signature is not a drop-in rename). Wrap the result through `KernelUtils::TryUnpackResult` / `TryUnpackKernelResult`. Throws `IOException` on failure (write path). |
| `ffi::CommitRequest`, `ffi::Commit`, `ffi::OptionalValue<ffi::Commit>::Tag::Some` | **Stable** (struct layout matches v0.21). | No change. |
| `ffi::commit`, `ffi::transaction`, `ffi::transaction_with_committer`, `ffi::with_engine_info`, `ffi::with_transaction_id`, `ffi::get_app_id_version`, `ffi::add_files`, `ffi::get_uc_commit_client`, `ffi::get_uc_committer` | **Stable.** | No change. |
| `ffi::ArrowFFIData` | **Stable.** | No change. |
| `ffi::allocate_kernel_string` | **Stable.** | No change. |

Optional v0.23 sweetener (not strictly required by the bump but lands on the table):
- `commit` now returns `ExternResult<Handle<ExclusiveCommittedTransaction>>` (post-commit snapshot accessible via `committed_transaction_version` / `committed_transaction_post_commit_snapshot` / `free_committed_transaction`). We previously got back just the version. We can adopt this in a follow-up; for the bump itself we keep the value-only path (read version via the new accessor then free). **This is a forced API change**: the return type changed in v0.22. So we must adapt `DeltaTransaction::Commit` to: call `ffi::commit` -> unwrap -> read `committed_transaction_version` -> `free_committed_transaction`.

### 2.2 `src/storage/delta_ctas.cpp` + `src/include/storage/delta_ctas.hpp`

**The hand-rolled version-0 JSON path is now retireable.** v0.22 added `get_create_table_builder(path, &EngineSchema, engine_info, engine)` -> `Handle<ExclusiveCreateTableBuilder>`, with `create_table_builder_with_table_property` and `create_table_builder_build` to compose the builder, plus `create_table_commit` to commit. See section 4 below for the full plan.

### 2.3 `src/functions/delta_scan/delta_multi_file_list.cpp`

| Symbol | Status | Fix |
|---|---|---|
| `ffi::get_engine_builder`, `ffi::set_builder_option`, `ffi::set_builder_with_multithreaded_executor`, `ffi::builder_build` | Stable | No change. |
| `ffi::get_snapshot_builder`, `ffi::get_snapshot_builder_from`, `ffi::snapshot_builder_set_version`, `ffi::snapshot_builder_set_log_tail`, `ffi::snapshot_builder_build` | Stable | No change. |
| `ffi::scan`, `ffi::scan_metadata_iter_init`, `ffi::scan_metadata_next`, `ffi::visit_scan_metadata`, `ffi::scan_table_root`, `ffi::scan_logical_schema`, `ffi::scan_physical_schema`, `ffi::selection_vector_from_dv`, `ffi::version` | Stable | No change. |
| `ffi::get_partition_columns`, `ffi::get_partition_column_count` | Stable | No change. |
| `ScanDataCallBack::VisitCallback` / `ScanDataCallBack::VisitData` C-callback signatures (`KernelStringSlice path`, `Stats`, `CDvInfo`, `Expression`, `CStringMap`) | Stable | No change. |

### 2.4 `src/delta_utils.cpp` + `src/include/delta_utils.hpp`

| Symbol | Status | Fix |
|---|---|---|
| `ffi::EngineExpressionVisitor` (and all `visit_literal_*`, `visit_struct_literal`, `visit_array_literal`, etc.) | **Field added: `visit_literal_null` signature changed.** New signature: `extern "C" fn(data, sibling_list_id, type_tag: u8, precision: u8, scale: u8)`. | In `ExpressionVisitor::CreateVisitor`, when we wire the visitor table, the `visit_literal_null` slot must point at a callback with the new C signature. We do not appear to set this field today (review the struct init); if it was zero-initialized this is a latent UB that we should now fix by providing a real callback that maps `NullTypeTag` -> DuckDB `LogicalType` and produces a typed `Value`. |
| `ffi::EngineSchemaVisitor` and all `visit_string`, `visit_struct`, `visit_array`, `visit_map`, `visit_decimal`, `visit_variant` callbacks | Stable | No change. |
| `ffi::SchemaVisitor::VisitSnapshotSchema` / `VisitSnapshotGlobalReadSchema` / `VisitWriteContextSchema` | Stable | No change. |
| `ffi::KernelExpressionVisitorState`, `ffi::visit_predicate_*`, `ffi::visit_expression_*`, `ffi::visit_expression_column`, `ffi::visit_expression_literal_string`, `ffi::visit_expression_literal_decimal` | Stable | No change. |
| `ffi::Event`, `ffi::Level`, `ffi::enable_event_tracing` | Stable | No change. |

### 2.5 `src/storage/delta_insert.cpp`

| Use | Effect of bump |
|---|---|
| Writes the hand-rolled `00000000000000000000.json` then calls `InitializeTableEntry` to load the snapshot. | This file becomes simpler once CTAS is rerouted through `get_create_table_builder` (section 4). For the *minimum* bump (no CTAS retirement), only one change is needed inside this file: nothing - the FFI calls live in `DeltaTransaction::InitializeTransaction` and the version-0 JSON itself is local file I/O. |
| `add.path` is now relative (v0.22 #2410). | We don't write `add.path` directly today (we delegate to the kernel after `add_files`), so the relative-path change is a kernel-internal behavior shift. **Test impact**: golden tables and DAT fixtures that snapshot the on-disk log may show `path` values shaped differently than before. Mitigate by re-recording or relaxing string matches in `test/sql/main/writing/`. |

### 2.6 `src/storage/delta_transaction_manager.cpp`

| Use | Effect |
|---|---|
| `ffi::checkpoint_snapshot(snapshot_ref.GetPtr(), engine)` returning `ExternResult<bool>` | Stable. The internal `Snapshot::checkpoint` API became `CheckpointResult` in v0.21.11 (#2314), but the FFI continues to expose `ExternResult<bool>`. No change. |

### 2.7 `src/include/delta_utils.hpp` RAII handles

All wrappers stay valid:
- `KernelSnapshot` -> `ffi::SharedSnapshot` / `ffi::free_snapshot` (unchanged)
- `KernelExternEngine` -> `ffi::SharedExternEngine` / `ffi::free_engine` (unchanged)
- `KernelScan` -> `ffi::SharedScan` / `ffi::free_scan` (unchanged)
- `KernelScanMetadataIterator` -> `ffi::SharedScanMetadataIterator` / `ffi::free_scan_metadata_iter` (unchanged)
- `KernelExclusiveTransaction` -> `ffi::ExclusiveTransaction` / `ffi::free_transaction` (unchanged)
- `KernelEngineData` -> `ffi::ExclusiveEngineData` / `ffi::free_engine_data` (unchanged)

**New RAII handles to add** (only when we adopt v0.22 features in steps 4 and 2.1):
- `KernelCreateTableBuilder` -> `ffi::ExclusiveCreateTableBuilder` / `ffi::free_create_table_builder`
- `KernelCreateTableTransaction` -> `ffi::ExclusiveCreateTransaction` / `ffi::free_transaction` (note: same `free_transaction` accepts both - confirm cbindgen overload name in the generated header before adding a separate typedef; if cbindgen emits a distinct C symbol like `free_create_transaction`, use that)
- `KernelCommittedTransaction` -> `ffi::ExclusiveCommittedTransaction` / `ffi::free_committed_transaction`
- `KernelWriteContext` -> `ffi::SharedWriteContext` / `ffi::free_write_context`

### 2.8 `scripts/ffi/`

- `prefix.inc`, `suffix.inc`, and `generate_delta_kernel_ffi_header` are layout-agnostic - they only depend on `#pragma once`, `extern`, and `}.*namespace ffi`. cbindgen's `ffi` namespace structure is preserved across the bump. **No change needed.**

### 2.9 `CMakeLists.txt`

- Bump `GIT_TAG` from `v0.21.0` to `v0.23.0` (single line, around line 149).
- The `--features` line stays the same (line 160). All five feature flags exist in v0.23.0.

### 2.10 Tests (`test/sql/`)

- `test/sql/main/writing/ctas/` and `test/sql/main/writing/append/` (insert path) need re-running. The `add.path` change to relative paths is the highest-risk source of fixture drift if any test asserts on the absolute path string.
- `test/sql/dat/`, `test/sql/delta_kernel_rs/`, `test/sql/inlined/` should be neutral (read-only fixture trees).
- `test/sql/golden_tests/` is regenerated by `make unpack-golden-tables-release`; no kernel coupling.
- `test/sql/generated/` requires `make generate-data`; the generator is PySpark/delta-spark/delta-rs and does not depend on our kernel pin, so its outputs should not drift.

## 3. Affected surfaces

```
CMakeLists.txt
  - GIT_TAG v0.21.0 -> v0.23.0

src/
  storage/
    delta_transaction.cpp       (forced: get_write_context rename, commit() return type)
    delta_transaction.hpp       (forced: new RAII for committed-transaction handle)
    delta_transaction_manager.cpp (no change)
    delta_ctas.cpp              (optional/recommended: removed entirely)
    delta_ctas.hpp              (optional/recommended: removed entirely)
    delta_insert.cpp            (forced: minimal; recommended: drop hand-rolled JSON path)
    delta_catalog.cpp           (touch only if delta_ctas.cpp removed: drop the include)
    delta_schema_entry.cpp      (touch only if delta_ctas.cpp removed: drop the include)
  delta_utils.cpp               (forced: visit_literal_null callback)
  include/delta_utils.hpp       (forced: visit_literal_null typedef; recommended new RAII typedefs)
  functions/delta_scan/         (no change)
  delta_extension.cpp           (no change)

scripts/ffi/                    (no change)

test/sql/
  main/writing/ctas/            (re-run; relax path assertions if any)
  main/writing/append/          (re-run)
  dat/                          (re-run; should be neutral)
  delta_kernel_rs/              (re-run)
```

## 4. CTAS implications — verdict: **retire `delta_ctas.cpp`**

### Why
- v0.22 (#2378) made `get_create_table_builder` accept `&EngineSchema` + a visitor function. This is *exactly* the API we said was missing in commit 70a741f's commit message ("v0.21.0's FFI has no way to construct a Handle<SharedSchema> from a column list").
- The kernel-side validation that `delta_ctas.cpp` re-implements (column types, partition keys, table-property hand-rolling) is now done in `delta_kernel::transaction::create_table::create_table`, which also handles the protocol-level reader/writer version constraints we partly enforce manually today (e.g., the `timestamp_ntz` rejection in `LogicalTypeToDeltaType`).

### Plan
Phase out `delta_ctas.cpp` in two ordered steps, the second optional but strongly recommended:

**Step A (minimum to unlock the bump):**
- Keep `delta_ctas.cpp` as-is. The bump itself does not force its removal. The only forced fix in CTAS-land is `delta_insert.cpp::GetGlobalSinkState` does not call `ffi::get_write_context` directly - it goes through `DeltaTransaction`, which DOES need the rename (section 2.1). No CTAS-specific edit required for compilation.

**Step B (retirement, follow-up PR):**
1. Add new RAII wrapper typedefs (section 2.7):
   - `KernelCreateTableBuilder`
   - `KernelCreateTableTransaction`
   - `KernelCommittedTransaction`
2. Add a translation layer `BuildEngineSchema(const ColumnList&)` in `src/storage/delta_create_table_schema.{cpp,hpp}` that:
   - owns a `vector<duckdb::ColumnDefinition>` for the lifetime of the visitor call;
   - provides a C `extern "C"` visitor function that walks the columns and calls `ffi::visit_field_string` / `_long` / `_integer` / `_short` / `_byte` / `_float` / `_double` / `_boolean` / `_binary` / `_date` / `_timestamp` / `_timestamp_ntz` / `_decimal` / `_struct` / `_array` / `_map` / `_variant`, returning the root struct's `usize` id;
   - exposes `ffi::EngineSchema { schema = this, visitor = &CWrapper }`.
3. Rewrite `DeltaInsertGlobalState`'s CTAS branch in `delta_insert.cpp`:
   - Replace the version-0 JSON write with: `ffi::get_create_table_builder(path, &engine_schema, engine_info, engine)` -> optionally `create_table_builder_with_table_property` for each user-supplied property -> `create_table_builder_build` -> stash the `ExclusiveCreateTransaction` in the global state alongside the existing `DeltaTransaction`.
   - On commit, call `create_table_commit` instead of the regular `commit`.
4. Delete `src/storage/delta_ctas.cpp`, `src/include/storage/delta_ctas.hpp`, drop their includes from `delta_catalog.cpp`, `delta_schema_entry.cpp`, `delta_insert.cpp`, and remove them from `CMakeLists.txt`'s source list.
5. Replace the timestamp-NTZ rejection logic with whatever the kernel returns (kernel may now allow it; that's a bonus).

**Why Step B is recommended:** the hand-rolled JSON path is a maintenance burden that already accumulated edge cases (timestamp-NTZ rejection, JSON escaping, partition-column quoting). The kernel surface is exactly the right size for our use.

**Open question for Step B:** the kernel's create-table writer issues the version-0 commit through a `Committer`. For `child_catalog_mode=parent_commit` (CCv2), we need to thread our custom `FfiUCCommitClient` through `create_table_builder_build_with_committer`. The shape of that wiring matches what `DeltaTransaction::Commit` already does for the existing-table `transaction_with_committer` path, so it's a known pattern - but worth calling out explicitly so cpp-coder doesn't accidentally use `FileSystemCommitter` for the Unity case.

## 5. Test plan

### Forced (regression risk from breaking changes)
- **Unit-test the visit_literal_null path.** No existing test, since we silently passed a zero pointer. Either add a `test/sql/inlined/test_null_literal_filter.test` that exercises pushdown of `WHERE col = NULL` / `WHERE col IS NULL` against a partitioned typed column, OR confirm by inspection of `delta_utils.cpp` that null literals never reach this visitor (in which case we can leave a no-op `extern "C"` stub that throws `InternalException` if invoked).
- **Re-run `test/sql/main/writing/append/`** to confirm INSERT still works through the renamed `get_unpartitioned_write_context` path.
- **Re-run `test/sql/main/writing/ctas/`** to confirm CTAS still works (this validates Step A; for Step B we add more partition-key / nested-type cases).
- **Re-run `test/sql/dat/`** end-to-end. This is the primary read-path regression net.
- **Re-run `test/sql/delta_kernel_rs/`** end-to-end (uses kernel test data; covers deletion vectors, column mapping, variant, etc.).
- **Re-run `test/sql/issues/`** - any open-issue regression test should still pass.
- **Re-run `test/sql/inlined/`** - quick sanity check.

### Not requiring regeneration
- `make generate-data` does **not** need to re-run for the bump itself: the generator is PySpark + delta-spark + delta-rs, independent of our kernel pin. Only re-run if generator versions in the Makefile changed.
- `make unpack-golden-tables-release` does **not** need to re-run unless `data/unpacked_golden_tables` is missing.

### Cloud
- `test/sql/cloud/*` doesn't need rerunning unless the bump changes object-store credential/retry behavior. v0.23.0 bumps `object_store` 0.13.1 -> 0.13.2 (#2366) - patch release, should be neutral. Run if convenient.

## 6. Migration order — exact edits, in sequence

Each numbered step leaves the tree in a buildable state.

1. **Pre-bump audit (no code changes).** Confirm `visit_literal_null` is currently zero-initialized in `ExpressionVisitor::CreateVisitor` (`src/delta_utils.cpp` around line 39). If it is, decide now whether the new typed-null callback will be a no-op stub or a full implementation; this is a Layer-2 decision.

2. **Update `scripts/ffi/` (no change required, but verify by visual diff after step 4 below).** The generator does not need updating; only re-confirm after the kernel rebuild.

3. **Bump the pin in `CMakeLists.txt`**: change `GIT_TAG v0.21.0` to `GIT_TAG v0.23.0` (single line).

4. **Clean the fetched kernel tree**: `make clean_debug` (or `make clean_release` for the release build). This forces `ExternalProject_Add` to re-fetch and re-build, regenerating `build/<config>/codegen/include/generated_delta_kernel_ffi.hpp`.

5. **Attempt a build (`make debug`)**. Expected compile errors:
   - `ffi::get_write_context` not found in `src/storage/delta_transaction.cpp:307`.
   - Mismatched signature on `EngineExpressionVisitor::visit_literal_null` if we explicitly assign it (probably no error if zero-initialized).
   - `ffi::commit` return-type mismatch in `src/storage/delta_transaction.cpp:478` (if v0.22 already changed it - confirm by inspecting the regenerated header).

6. **Fix `src/storage/delta_transaction.cpp` (Layer-1 mechanical edits):**
   - Replace `ffi::get_write_context(kernel_transaction.get())` with `KernelUtils::TryUnpackResult(ffi::get_unpartitioned_write_context(kernel_transaction.get(), engine.get()), write_context)` (using whichever local engine handle is in scope - `table_entry->snapshot->extern_engine`).
   - Adapt the `ffi::commit` call site to consume the new `Handle<ExclusiveCommittedTransaction>`: unwrap, call `ffi::committed_transaction_version` to get the version, call `ffi::free_committed_transaction`.

7. **Fix `src/delta_utils.cpp`** (if needed): wire `visit_literal_null` to a stub that translates `NullTypeTag` to a DuckDB `LogicalType` and emits a typed NULL `Value`. For minimum risk, throw `InternalException("typed null literal not yet handled in extension")` if/when reached - we have not observed it in our pushdown paths, but a defensive stub is the right Layer-1 choice.

8. **Run `make test_debug`** narrowly: start with `test/sql/main/writing/append/`, `test/sql/main/writing/ctas/`, then `test/sql/dat/`, `test/sql/delta_kernel_rs/`, `test/sql/inlined/`, `test/sql/issues/`. Expect:
   - Possible test output drift on any test that asserts on `add.path` containing the absolute URL (v0.22 #2410 makes it relative). Update the assertions in place.
   - No other test failures from the bump alone.

9. **Run `make test_release`** for full confidence.

10. **(Optional) Step B from section 4: retire `delta_ctas.cpp`** in a separate follow-up commit/PR. Do not bundle with the bump - it's a meaningful behavior change worth reviewing on its own.

## 7. Risk + rollback

**Risks:**
- **`visit_literal_null` latent bug.** If the existing zero-initialized field is ever invoked by the kernel's expression-to-engine path, v0.22's signature change turns this from "always crashed" into "now ALSO crashes, but with extra args on the stack". Defensive stub closes this.
- **`add.path` relative vs absolute.** Any user code or test that introspects the on-disk Delta log expecting an absolute URL will break. Internal to the kernel and our reads, this is transparent.
- **`commit()` return type.** Our `DeltaTransaction::Commit` currently returns the version via `ffi::commit -> ExternResult<u64>`. v0.22 changes this to `ExternResult<Handle<ExclusiveCommittedTransaction>>`. Mechanical, but a missed call site = silent leak of the committed handle. Audit by grepping `ffi::commit(` after the bump.
- **`free_transaction` shared between `ExclusiveTransaction` and `ExclusiveCreateTransaction`.** Need to confirm in the regenerated header before adding the second RAII typedef. If the same C symbol applies to both, that's fine (kernel uses Rust's `Drop`), but our `TemplatedUniqueKernelPointer<KernelType, DeleteFunction>` will get two distinct typedefs pointing at the same delete fn - acceptable.

**Rollback:**
- All edits in this plan are localized. Reverting the `GIT_TAG` line in `CMakeLists.txt` + `make clean_<config>` + reverting the C++ changes returns to v0.21.0 cleanly. No on-disk Delta log format change between v0.21 and v0.23 - the kernel reads back what it wrote in either direction (modulo the `add.path` style which is normalized on read).
- If Step B has landed and we need to revert, restoring `delta_ctas.{cpp,hpp}` is straightforward (no other code depends on them being absent).

## 8. Error strategy (recap)

| Failure surface | DuckDB exception |
|---|---|
| Kernel returns error from `get_unpartitioned_write_context` | `IOException` (write path) via `TryUnpackKernelResult` |
| Kernel returns error from `get_create_table_builder` (Step B) | `BinderException` (schema validation at bind time) or `CatalogException` (table-already-exists, protocol mismatch) - inspect `KernelError` variant to pick |
| `visit_literal_null` callback fires unexpectedly | `InternalException("typed null literal not yet handled")` - this is an invariant violation, not user-triggerable in v0.23 |
| `commit` returns `RetryableTransaction` or `ConflictedTransaction` (still surfaced as `Error` at FFI per v0.23 code) | `TransactionException` (already what we throw on conflict) |

Non-fatal failures (filter pruned everything, deletion vector applied) continue to return values, not exceptions, per existing scan-path convention.

## 9. Concurrency plan

No concurrency-model changes from the bump.
- Bind data still holds the immutable `DeltaCatalog` / `DeltaTableEntry`.
- Global scan state holds the `SharedKernelSnapshot` and the iterator (`KernelScanMetadataIterator`).
- Local scan state per-thread reads pruned file lists with no further FFI.
- Write path (`DeltaTransaction`) is per-transaction, single-threaded by DuckDB's transaction model.
- Step B introduces no new threads; `create_table_builder_build` is a synchronous kernel call.

## 10. Open questions

1. **(Authorization)** This plan assumes the user authorized bumping to `v0.23.0`. The CLAUDE.md guidance requires explicit authorization to change `GIT_TAG`. The task spec says "update to delta-kernel-rs v0.23.0", which we read as authorization. Confirm.
2. **`free_transaction` vs distinct create-transaction free function.** Before adding the `KernelCreateTableTransaction` RAII typedef in Step B, inspect the regenerated `generated_delta_kernel_ffi.hpp` to see whether cbindgen emits a separate C symbol (e.g., `free_create_table_transaction`) for `ExclusiveCreateTransaction`'s drop, or whether `free_transaction` is overloaded. Affects only RAII naming, not correctness.
3. **`visit_literal_null` semantics.** Should we implement the full typed-null callback (mapping `NullTypeTag` -> DuckDB typed NULL `Value`) now, or leave a defensive `InternalException` stub and wire it up the first time we see it in pushdown? The conservative call is the stub; the principled call is a full implementation given the type tag is well-defined. Recommend: defensive stub for the bump PR, full implementation in a follow-up tied to a real test case.
4. **Step B CCv2 wiring.** Once we use `get_create_table_builder` for CTAS, do we want CCv2 (Unity) tables to be creatable through `delta_ctas` as well? Today the hand-rolled path doesn't support CCv2 CTAS (the catalog rejects it earlier). If we expand support, the right entry point is `create_table_builder_build_with_committer` with our `FfiUCCommitClient`. Out of scope for the bump itself.
5. **`materializePartitionColumns` writer feature** (v0.22 #2481). Affects the write_context's physical schema when enabled. We don't write partitioned tables today (blind-insert only on unpartitioned), so this doesn't affect us yet, but worth flagging as a follow-up when partitioned writes land.

