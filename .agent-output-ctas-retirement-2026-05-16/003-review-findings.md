# CTAS Kernel-Native Implementation — Code Review Findings

**Verdict: APPROVED with recommendations**  
**Critical: 0 | High: 1 | Medium: 3 | Low: 2**

---

## Review Summary

The implementation successfully retires the hand-rolled JSON writer in favour of the kernel-native `ffi::get_create_table_builder` API. The FFI boundary is handled carefully: `DispatchVisit` catches both `std::exception` and `...` before the sentinel return, `TryUnpackResult` frees kernel error objects via `IntoString()`, and all kernel handles are wrapped in `TemplatedUniqueKernelPointer` RAII wrappers. Handle ownership across the four-step CTAS chain (`get_create_table_builder` → `create_table_builder_build` → `create_table_add_files` → `create_table_commit`) is correct: `.release()` is used exactly at consuming call sites, `.get()` at borrowing ones. The CCv2 guard (`parent_commit=true` → `NotImplementedException`) is in place. Test coverage is reasonable — 8 CTAS tests including attach-existing, empty select, OR REPLACE rejection, and type coverage. The main concern is a silent precision-loss regression introduced by the TIMESTAMP_NS/MS/SEC behaviour change.

---

## High (should fix)

### [H1] TIMESTAMP_NS silently truncates nanoseconds to microseconds with no user warning or test verification
- **Location**: `src/storage/delta_create_table_schema.cpp:93-101`, `test/sql/main/writing/ctas/ctas_timestamp_ntz.test`
- **Standard / Issue**: The prior hand-rolled path rejected `TIMESTAMP_NS`, `TIMESTAMP_MS`, and `TIMESTAMP_SEC` with `BinderException`. This PR intentionally accepts them and maps all three to Delta `timestamp_ntz`. Delta `timestamp_ntz` is stored at microsecond precision. DuckDB's `TIMESTAMP_NS` carries nanosecond precision (int64 nanoseconds since epoch). The parquet writer writes INT64 nanoseconds; when the Delta reader reads back the file through the Delta schema (which declares `timestamp_ntz`, i.e. microsecond precision), the nanosecond sub-field is silently truncated. The two sub-microsecond digits disappear with no error, no warning, and no indication in the metadata. `ctas_timestamp_ntz.test` only reads `count(*)` and `id`, never the timestamp column itself, so the test cannot catch this precision loss. `TIMESTAMP_MS` and `TIMESTAMP_SEC` are genuinely lossless (milliseconds and seconds fit in microseconds without truncation); the concern is specific to `TIMESTAMP_NS`.
- **Fix (two parts)**:
  1. In `VisitField`, split the three cases and throw for `TIMESTAMP_NS` specifically, or document the lossy conversion explicitly in the `BinderException` message so the user is informed:
```cpp
case LogicalTypeId::TIMESTAMP_NS:
    throw BinderException(
        "Cannot create Delta table: column '%s' has type TIMESTAMP_NS (nanosecond precision). "
        "Delta's timestamp_ntz type stores microseconds; nanosecond precision would be silently lost. "
        "Cast to TIMESTAMP or TIMESTAMP_MS before creating the table.",
        name);
case LogicalTypeId::TIMESTAMP_MS:
case LogicalTypeId::TIMESTAMP_SEC:
    return UnpackFieldId(
        ffi::visit_field_timestamp_ntz(state, name_slice, nullable, DuckDBEngineError::AllocateError),
        "visit_field_timestamp_ntz");
```
  2. Extend `ctas_timestamp_ntz.test` to read back the `ts_ms` and `ts_sec` columns and assert their values are preserved. For `TIMESTAMP_NS`, add a `statement error` block that confirms the rejection.

---

## Medium (nice to fix)

### [M1] `DispatchVisit` is assigned to a C function pointer without `extern "C"` linkage
- **Location**: `src/storage/delta_create_table_schema.cpp:25`, `src/include/storage/delta_create_table_schema.hpp:46`
- **Standard / Issue**: `EngineSchema::visitor` has type `uintptr_t (*)(void *, KernelSchemaVisitorState *)` — a C function pointer. `DeltaCreateTableSchema::DispatchVisit` is a C++ static member function without `extern "C"` linkage. On all currently supported ABIs (System V AMD64, AAPCS64 for aarch64) C and C++ static functions with the same signature share identical calling conventions in practice, so this does not cause a runtime defect. However the C++ standard makes calling a C++ function through a C function pointer technically undefined behaviour, and a future ABI with different calling conventions for the two linkages would silently break. The entire existing codebase uses this pattern (see `delta_utils.cpp` lines 43, 57–65, 545), so this PR does not introduce new risk — it inherits the project-wide convention. Flag as Medium because the new code adds another instance without a comment acknowledging the trade-off.
- **Fix**: Add a comment at the assignment site that acknowledges the convention:
```cpp
// Assigned to a C function pointer; on all supported ABIs (System V AMD64, AAPCS64)
// C++ static member functions share the C calling convention for this signature.
result.visitor = &DeltaCreateTableSchema::DispatchVisit;
```
  For a stronger fix (deferred, given the codebase-wide scope): introduce a free function wrapper declared `extern "C"` and assign that instead.

### [M2] `ctas_timestamp_ntz.test` does not verify the round-tripped timestamp values for the lossless cases
- **Location**: `test/sql/main/writing/ctas/ctas_timestamp_ntz.test`
- **Standard / Issue**: The test creates tables with `TIMESTAMP_MS` and `TIMESTAMP_SEC` columns, reads them back, but only queries the `id` column, not the timestamp column. For these two types the write/read is lossless (millisecond and second precision fit within microsecond Delta storage), but if a future change accidentally breaks the mapping (e.g. maps these to the wrong kernel type), the test would still pass. The test as written proves only that the table is readable, not that values are preserved.
- **Fix**: Add value-checking queries for the lossless cases:
```sql
query T
SELECT ts_ms FROM ntz2r;
----
2024-06-01 08:00:00
```

### [M3] `DeltaTransactionMode` enum adds indirection where a null-pointer check would suffice
- **Location**: `src/include/storage/delta_transaction.hpp:25-28`, `src/storage/delta_transaction.cpp:444, 639, 692-693`
- **Standard / Issue**: The prior review (iter 2) noted that a nullable-pointer pattern on `kernel_create_txn` avoids a parallel mode state variable. The new code has a two-value `DeltaTransactionMode` enum (`REGULAR` / `CREATING_TABLE`) that is set at exactly the same time as `kernel_create_txn` is assigned. `mode == CREATING_TABLE` is equivalent to `kernel_create_txn.get() != nullptr`, so the enum carries no information the RAII wrapper does not already express. `IsCreatingTable()` could read `kernel_create_txn.get() != nullptr` directly. This is not a correctness issue — the invariant is properly maintained — but it violates DRY and was explicitly flagged in the prior review as the recommended simplification.
- **Fix**: Remove the `DeltaTransactionMode` enum and its `mode` field. Replace the three use-sites with `kernel_create_txn.get() != nullptr` (in `Commit`) and remove the `D_ASSERT(mode == DeltaTransactionMode::REGULAR)` guard (the `kernel_create_txn` RAII state already prevents double-init since assigning an owning wrapper twice would free the first handle). `IsCreatingTable()` becomes `return kernel_create_txn.get() != nullptr`.

---

## Low (style suggestions)

### [L1] `UnpackFieldId` parameter `context` shadows the DuckDB idiom name
- **Location**: `src/storage/delta_create_table_schema.cpp:10`
- **Standard / Issue**: The parameter `const char *context` in the static helper `UnpackFieldId` uses the name `context`, which DuckDB conventionally reserves for `ClientContext &context`. In this particular function there is no outer `context` variable to shadow, but the name is confusing to readers of DuckDB code who expect `context` to refer to the client session.
- **Fix**: Rename to `location` or `call_site`:
```cpp
static uintptr_t UnpackFieldId(ffi::ExternResult<uintptr_t> result, const char *call_site) {
```

### [L2] MAP and LIST visitor paths in `VisitField` are not covered by any CTAS test
- **Location**: `src/storage/delta_create_table_schema.cpp:124-144`, `test/sql/main/writing/ctas/`
- **Standard / Issue**: `ctas_kernel_native.test` covers STRUCT and DECIMAL; `ctas_type_coverage.test` covers the scalar primitives. Neither test exercises the `LIST` or `MAP` cases in `VisitField`. Both paths call `VisitField` recursively for element/key/value types and then call `visit_field_array` / `visit_field_map`. A kernel API change or a bug in the recursive element call would go undetected.
- **Fix**: Add a test case in `ctas_type_coverage.test` or `ctas_kernel_native.test`:
```sql
CREATE TABLE t.t AS
SELECT
    [1, 2, 3]                AS c_list,
    MAP {'a': 1, 'b': 2}     AS c_map;
```

---

## What's Done Well

1. **Comprehensive exception safety at the FFI boundary.** `DispatchVisit` catches both `std::exception &` and `...`, stores the captured error in `DeltaCreateTableSchema::captured_error`, and returns the sentinel `0`. The caller in `InitializeForNewTable` checks `schema_visitor.HasError()` first so the user sees the DuckDB-typed `BinderException` rather than a generic kernel `IOException`. The `DuckDBEngineError::IntoString()` / `delete this` pattern ensures kernel error allocations are not leaked even on the error path.

2. **RAII ownership is airtight across the four-step create-table chain.** `KernelExclusiveCreateTableBuilder` wraps the builder and is `.release()`d only at the single consuming call (`create_table_builder_build`). `KernelExclusiveCreateTransaction` wraps the returned transaction handle and is `.release()`d only at `create_table_commit`. Both RAII destructors guard `ptr && free` so a null pointer from an error path is a no-op. `KernelCommittedTransaction` is always constructed (even from null on error path) and freed on scope exit. There are no handle leaks on any code path.

3. **CCv2 / `parent_commit=true` is guarded immediately with `NotImplementedException`.** The check at the top of `InitializeForNewTable` (line 641–645 in `delta_transaction.cpp`) prevents the kernel path from being taken when the Unity Catalog managed-commit flow is active, matching Open Question 1's resolution. The guard is in the right place — before any state mutation — and produces a clear, actionable error message.

---

**Verdict: APPROVED**  
The implementation is correct and the FFI contract is respected. The single High issue (TIMESTAMP_NS silent precision loss) should be addressed before shipping the feature to users, either by rejecting TIMESTAMP_NS with a clear error or by adding an explicit caveat to the error message. The two Medium issues are clean-up recommendations.
