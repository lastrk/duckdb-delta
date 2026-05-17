# Code Review: delta-kernel-rs v0.21.0 → v0.23.0 bump (Step A)

## Review Summary

The bump is mechanically sound. The three forced API changes — `get_write_context` rename,
`commit()` return-type change, and `visit_literal_null` signature change — are all handled
correctly. The new RAII typedefs (`KernelWriteContext`, `KernelCommittedTransaction`) follow
the existing `TemplatedUniqueKernelPointer` pattern exactly. The commit-flow safety analysis
is clean: `TryUnpackResult` returns `ErrorData` and never throws, so there is no exception
window between the raw-pointer unpack and the RAII wrap. One High issue stands out: the
entire `GetWriteSchema` method — including the new `get_unpartitioned_write_context` call —
has no callers in the codebase, meaning the new kernel path is never exercised at runtime or
by the test suite. Two further issues (unused-parameter suppression missing, typed NULL gap)
are documented below at appropriate severity.

---

## Critical (must fix before merge)

_None._

---

## High (should fix)

### [H1] `GetWriteSchema` has no callers — new `get_unpartitioned_write_context` path is dead and untested

- **Location**: `src/storage/delta_transaction.cpp:302-313` and `src/include/storage/delta_transaction.hpp:46`
- **Standard / Issue**: A grep across the entire source tree (`src/`, including all `.cpp` and `.hpp` files outside `build/`) finds zero call sites for `DeltaTransaction::GetWriteSchema`. The rename from `ffi::get_write_context` to `ffi::get_unpartitioned_write_context` is the highest-risk mechanical change in this bump; yet the resulting code path is never executed, so neither the test suite nor the release binary exercises the new kernel call. If the call site, argument order, or RAII ownership semantics are subtly wrong, the bug will surface only when a caller is eventually added. The implementation log confirms the 564-assertion run exercised only paths reachable from INSERT and CTAS — neither of which calls `GetWriteSchema`.
- **Fix**: Either add a call site (if `GetWriteSchema` is intentionally used by some upcoming change that is in flight), or add a `// NOTE: no callers yet; first caller must exercise this path under test` comment and file a follow-up issue. Minimally, add a sqllogic test that triggers the code path once a caller exists. As a non-code option, add an `explicit_call_test` that invokes `GetWriteSchema` directly via a unit test or a new table function exercised from a `.test` file.

### [H2] Test environment caveat: debug build not tested; D_ASSERTs not exercised

- **Location**: Implementation log, section 4.1; `src/storage/delta_transaction.cpp:511` (`D_ASSERT(table_entry)`)
- **Standard / Issue**: The implementation log explicitly states the debug build failed to link due to disk exhaustion, so all 564 assertions were verified against the release binary only. DuckDB's `D_ASSERT` expands to a no-op in release builds. The two `D_ASSERT` sites in `delta_transaction.cpp` (lines 248 and 511) could mask invariant violations that would crash a debug build. This is not a defect in the code itself but is a gap in the validation of this PR.
- **Fix**: Before merge, verify `make debug` links cleanly on a machine with sufficient disk. Run `make test_debug` at minimum over `test/sql/main/writing/` and `test/sql/dat/`. The CI matrix should already do this; confirm the CI run for this PR includes a debug job.

---

## Medium (nice to fix)

### [M1] `VisitNullLiteral` drops type information — typed NULL literals produce `SQLNULL` instead of typed NULLs

- **Location**: `src/delta_utils.cpp:251-256`
- **Standard / Issue**: In v0.23 the kernel can emit `Scalar::Null(data_type)` for any of the 14 primitive types plus `NonPrimitive`. Our implementation discards `type_tag`, `precision`, and `scale` and emits `Value()` (DuckDB's `SQLNULL`, logical type `SQLNULL`). For the current pushdown paths (partition-column transforms and filter round-trips) this is benign: the kernel uses `visit_is_null` for IS-NULL predicates, so typed NULLs in literal position mostly appear in partition-value expressions where DuckDB's type-promotion rules produce the same result as a typed NULL. However, for decimal partition nulls the precision/scale is silently dropped; if the kernel ever uses this for filter-range pushdown on decimal partitions the result could be wrong. The comment correctly documents the intent, but no invariant protects against the kernel sending a typed null on a new code path added in v0.22-v0.23.
- **Fix (minimal)**: Replace the silent discard with a logged warning when `type_tag != 0` (i.e., not `NonPrimitive`), so the first time a typed NULL appears in a real workload it is visible. The full fix — mapping `NullTypeTag` to a DuckDB `LogicalType` and calling `Value(logical_type)` — is the right long-term solution and can be a follow-up PR.

### [M2] Unused parameters `type_tag`, `precision`, `scale` are named but never suppressed

- **Location**: `src/delta_utils.cpp:251-252`
- **Standard / Issue**: DuckDB's contributor guide states that intentionally-unused parameters should be cast to `(void)` to suppress compiler warnings. The three parameters are declared with names in both the header declaration and the implementation but are never read. Under `-Wunused-parameter` (which many DuckDB CI configurations enable) this will produce warnings that can hide real issues and fail strict builds.
- **Fix**: Add `(void)type_tag; (void)precision; (void)scale;` at the top of the function body, matching the project convention:
  ```cpp
  void ExpressionVisitor::VisitNullLiteral(void *state, uintptr_t sibling_list_id,
                                           uint8_t type_tag, uint8_t precision, uint8_t scale) {
      (void)type_tag;
      (void)precision;
      (void)scale;
      // We emit an untyped NULL; the kernel's NullTypeTag information is not yet used for pushdown.
      auto expression = make_uniq<ConstantExpression>(Value());
      static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
  }
  ```

---

## Low (style suggestions)

### [L1] `write_entry.get()->snapshot` chain is evaluated twice in `GetWriteSchema`

- **Location**: `src/storage/delta_transaction.cpp:307-311`
- **Standard / Issue**: `write_entry.get()->snapshot` is dereferenced on lines 308, 309, and 311, producing three identical pointer-chase chains. This is not a bug (optional_ptr::get() is a cheap null-pointer check), but it is harder to read and maintain than a named local.
- **Fix**: Introduce a local reference before the calls:
  ```cpp
  auto &snapshot = *write_entry.get()->snapshot;
  KernelWriteContext write_context = snapshot.TryUnpackKernelResult(
      ffi::get_unpartitioned_write_context(kernel_transaction.get(), snapshot.extern_engine.get()));
  auto result = SchemaVisitor::VisitWriteContextSchema(snapshot.extern_engine.get(), write_context.get());
  ```

### [L2] `committed_txn` scope-exit comment is redundant with the code

- **Location**: `src/storage/delta_transaction.cpp:491`
- **Standard / Issue**: The comment `// committed_txn goes out of scope here and frees the ExclusiveCommittedTransaction handle` states what C++ RAII already guarantees. It adds noise without adding information that the type name and destructor contract do not already convey. DuckDB's style guide prefers comments that explain *why*, not *what*.
- **Fix**: Remove the comment. The `KernelCommittedTransaction` name and the inline comment on line 482 (`// Wrap the handle in RAII so it is always freed on scope exit, even on error paths`) are sufficient.

---

## What's Done Well

1. **RAII wrap-before-check pattern in `Commit`**: The sequence `TryUnpackResult → nullptr init → RAII wrap → HasError check` is correctly ordered. Because `TryUnpackResult` returns `ErrorData` rather than throwing, there is no exception window between the raw-pointer extraction and the RAII construction. The `nullptr` initializer on `committed_txn_ptr` ensures the RAII destructor is a safe no-op if the kernel returns an error and the Ok field is never written.

2. **New typedef placement and style**: `KernelWriteContext` and `KernelCommittedTransaction` are inserted at the correct point in the typedef block (after the existing six), use the same `TemplatedUniqueKernelPointer<KernelType, FreeFunction>` pattern as every sibling typedef, and both free-function names (`ffi::free_write_context`, `ffi::free_committed_transaction`) have been verified against the generated FFI header.

3. **Transaction non-consumption verified**: The implementation log correctly identifies and documents (section 4.3) that `get_unpartitioned_write_context` uses `txn.as_ref()` in Rust and therefore does not consume the `ExclusiveTransaction`. This means `kernel_transaction.get()` (not `release()`) is the correct call, which is what the code does. The transaction handle remains valid for the subsequent `ffi::commit(kernel_transaction.release(), …)` call.
