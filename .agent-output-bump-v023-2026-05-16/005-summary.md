# Feature Summary: delta-kernel-rs v0.21.0 → v0.23.0 bump

## What Was Built

A minimal-viable kernel upgrade. Bumps the `delta-kernel-rs` pin from v0.21.0 to v0.23.0 (spanning v0.22 + v0.23) and adapts the three FFI surfaces that changed shape: `ffi::commit` now returns a `Handle<ExclusiveCommittedTransaction>` instead of `u64`, `visit_literal_null` callback gained three parameters (`type_tag`, `precision`, `scale`), and `ffi::get_write_context` was renamed to `ffi::get_unpartitioned_write_context` (this last one turned out to affect dead code — see Architecture Decisions). All other FFI symbols we depend on are stable. Net diff: 5 files, +15 / -19 lines.

## Architecture Decisions

- **Step A only — minimal-viable bump.** The architect proposed a two-step plan: Step A is just the kernel bump, Step B is retiring `src/storage/delta_ctas.{cpp,hpp}` (242 lines of hand-rolled version-0 JSON) using v0.22's new `get_create_table_builder(path, &EngineSchema, ...)` which takes a column list via visitor — the exact API the CTAS commit lamented missing. **Step B is recommended as a follow-up PR**, not bundled here, per the user's request scope ("address breaking changes if needed") and to keep this PR's risk surface tight.
- **`KernelCommittedTransaction` RAII wraps the new commit handle.** v0.23's `ffi::commit` allocates a Rust-heap object for the committed-transaction state and returns its handle; without RAII a thrown exception or early return between unpack and free would leak. New typedef follows the existing `TemplatedUniqueKernelPointer<KernelType, FreeFunction>` pattern (matches all six sibling typedefs). The two-step pattern (init `nullptr` → `TryUnpackResult` into raw pointer → wrap in RAII) is forced by `TryUnpackResult<T>`'s template-argument deduction; reviewer confirmed there's no exception window between unpack and wrap because `TryUnpackResult` returns `ErrorData` and never throws.
- **`VisitNullLiteral` ignores the new type info.** v0.23 passes the literal's type tag, precision, and scale to the visitor. Current pushdown paths emit an untyped DuckDB `Value()` and the discarded info is benign for IS-NULL predicates (which use `visit_is_null` instead) and for integer/string nulls. Decimal partition NULLs silently drop precision/scale — a Medium follow-up (see "Items for Human Review").
- **Dead `GetWriteSchema` left absent.** The architect flagged `ffi::get_write_context` (called from `DeltaTransaction::GetWriteSchema`) as the "biggest breakage." Reviewer's grep found `GetWriteSchema` had no callers anywhere in `src/`. By the time iter-1 fix ran the function was already absent — likely never present in the bumped state. Confirmed gone: no `GetWriteSchema`, no `KernelWriteContext`, no `get_*_write_context` calls remain.

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `CMakeLists.txt` | Modified | `GIT_TAG` `v0.21.0` → `v0.23.0` |
| `src/include/delta_utils.hpp` | Modified | `VisitNullLiteral` declaration gains `(uint8_t type_tag, uint8_t precision, uint8_t scale)`; new RAII typedef `KernelCommittedTransaction = TemplatedUniqueKernelPointer<ExclusiveCommittedTransaction, ffi::free_committed_transaction>` |
| `src/delta_utils.cpp` | Modified | `VisitNullLiteral` body accepts new 5-param signature (3 new params discarded; comment documents intent); still emits untyped DuckDB `Value()` |
| `src/include/storage/delta_transaction.hpp` | Modified | Removed declaration of dead `GetWriteSchema` |
| `src/storage/delta_transaction.cpp` | Modified | `Commit` adapts to new `ffi::commit` return type: unpack raw `ExclusiveCommittedTransaction*`, wrap in `KernelCommittedTransaction` RAII, call `ffi::committed_transaction_version` for the version, free on scope exit |

## sqllogic Tests Added

**None.** Test surface is unchanged — the existing suite is the test plan for this PR. The 7 directories below ran against the v0.23 kernel test data with no regressions:

- `test/sql/main/*` — 138 assertions (1 case skipped: requires httpfs)
- `test/sql/main/writing/*` — 138 assertions
- `test/sql/main/writing/ctas/*` — 38 assertions
- `test/sql/issues/*` — 23 assertions
- `test/sql/inlined/*` — 34 assertions
- `test/sql/dat/*` — 173 assertions (kernel-shipped Delta Acceptance Tests)
- `test/sql/delta_kernel_rs/*` — 196 assertions (kernel-shipped reader tests)

Total: **740 assertions pass** with the new pin. Kernel test data paths (`build/<config>/rust/src/delta_kernel/kernel/tests/data/` and `acceptance/tests/dat/`) are unchanged from v0.21, so the `Makefile`'s `DELTA_KERNEL_TESTS_PATH` / `DAT_PATH` env-var wiring still resolves correctly.

## Review Status

- **Verdict: APPROVED after 2 iterations.**
- Iter 1 (NEEDS_CHANGES, 2 High):
  - H1 (`GetWriteSchema` dead code) — turned out to already be absent; nothing to delete
  - H2 (debug build not validated due to disk exhaustion) — disk freed by the user; debug build then succeeded for the `.a` but the `unittest` link OOMs on 16 GB RAM (combines DuckDB's 3 GB debug object + extensions + DWARF). Accepted as environment limitation; release binary tests are the authoritative pass.
- Iter 2 (APPROVED, 0 blocking).
- **Outstanding Medium/Low items** (deliberately deferred — out of orchestrator scope):
  - **M1**: `VisitNullLiteral` discards `type_tag` / `precision` / `scale`. Benign now; matters when decimal-partition NULL pushdown gets exercised. Track for the next pushdown improvement.
  - **M2**: Named unused parameters lack `(void)` suppression. Style gap under DuckDB's CONTRIBUTING.md; not a build break under current flags.
  - **L1**: Redundant scope-exit comment at `delta_transaction.cpp:478` (RAII pattern is self-documenting).
  - **L2**: A duplicate scope-exit comment site on the new `KernelCommittedTransaction` block.

## Performance

- **Verdict: OPTIMIZED (0 actionable findings)**
- The bump is intrinsically thin. Perf accounting from the perf reviewer:
  - **PERF-1**: `ffi::commit` now allocates a Rust-heap `ExclusiveCommittedTransaction` per commit, requiring an extra `free_committed_transaction` FFI call at scope exit. Magnitude: ~1 Rust malloc + 1 cross-FFI free per `DeltaTransaction::Commit`. Filesystem I/O for the Delta log JSON write dominates by 3–5 orders of magnitude. Below measurement noise; no opt-out exists in v0.23 FFI; nothing to do.
  - **PERF-2**: `VisitNullLiteral`'s 3 extra `uint8_t` parameters fit in registers on both aarch64 and x86_64 (no stack spill). Callback fires at filter-translation time (query bind), not per-row. Zero measurable impact.
  - **PERF-3**: All other FFI symbols we use are ABI-stable v0.21 ↔ v0.23. No new allocations on any active path.
  - **PERF-4**: The two-step RAII pattern in `Commit` (nullptr init → unpack → wrap) is the minimum correct shape given `TryUnpackResult<T>`'s template deduction. No alternative reduces overhead.
- **Optimizations applied**: none needed
- **Optimizations skipped**: none

## Items for Human Review

1. **Step B follow-up (CTAS retirement) — actionable.** v0.22 PR #2378 added `get_create_table_builder(path, &EngineSchema, engine_info, engine)` that takes a column list via the same visitor pattern used by `scan_builder_with_schema`. This is exactly the API the CTAS commit (70a741f) lamented missing and the reason `src/storage/delta_ctas.{cpp,hpp}` exists. Retiring it would delete ~242 lines of hand-rolled JSON, remove the in-house Delta-protocol-correctness liability, and route the version-0 commit through the same kernel committer path that subsequent commits use. The architect estimated this as a clean follow-up PR: replace `delta_ctas.cpp` with a slim `BuildEngineSchema(const ColumnList &)` translator and update `delta_insert.cpp`'s CTAS sink to call `get_create_table_builder` → `create_table_builder_with_table_property` → `create_table_builder_build` → `create_table_add_files` → `create_table_commit`. Recommend filing as the next ticket.
2. **Debug unittest binary cannot be linked on the current dev machine** (16 GB RAM, zero swap). The delta extension static library compiles cleanly in debug; the unittest link OOMs combining DuckDB's full 3 GB debug object set with extensions and DWARF info. CI's hosted debug job (with more RAM) is the authoritative debug-test gate before merge. Reviewer accepted this as an environment limitation.
3. **`VisitNullLiteral` carries a quiet correctness risk** for the day someone adds decimal-partition NULL pushdown (M1). The kernel now emits `Scalar::Null(decimal_type{precision, scale})` and we discard precision/scale. Visible failure mode: a NULL value used in a partition transform could be coerced to the wrong-precision decimal at the engine. No current pushdown exercises this; flag for the next pushdown work.
4. **Two follow-up Medium fixes** (M2 unused-parameter suppression, L1/L2 redundant scope-exit comments) recommended before merge as Medium-priority cleanups — out of orchestrator's Critical/High-only scope but trivial.
5. **No further kernel bumps raised.** v0.23.0 is the latest stable release as of 2026-05-15. The architect did not raise a further bump.
