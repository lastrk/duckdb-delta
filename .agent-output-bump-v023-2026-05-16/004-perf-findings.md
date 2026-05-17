# Performance Analysis: delta-kernel-rs v0.21.0 → v0.23.0 bump

## Scope

This analysis covers the four areas identified in the task brief:
1. Per-transaction overhead from the new `ffi::commit` return type
2. `VisitNullLiteral` callback overhead from the 3 extra parameters
3. Other ABI shape changes affecting hot paths
4. `KernelCommittedTransaction` RAII lifetime tightness

---

## [PERF-1] Per-transaction commit overhead: extra heap allocation + free per commit

- **Bottleneck**: In v0.21, `ffi::commit()` returned `ExternResult<uint64_t>` — a 16-byte struct
  on the call stack, no heap involvement. In v0.23 it returns
  `ExternResult<Handle<ExclusiveCommittedTransaction>>`, where `Handle<T> = T*`. The `Ok` variant
  now carries a raw pointer to a Rust-heap-allocated `ExclusiveCommittedTransaction` object. The
  C++ side must call `ffi::free_committed_transaction()` to drop it. The new code path is:
  `ffi::commit()` → `TryUnpackResult()` → `KernelCommittedTransaction(ptr)` constructor →
  scope-exit destructor calls `ffi::free_committed_transaction(ptr)` (a Rust FFI call that runs
  the Rust `Drop` impl).

  Evidence:
  - Old path (`HEAD~3`): `uint64_t commit_result` local on the C++ stack; no extra FFI call after
    `ffi::commit`. `commit_result` is assigned but never read (version was already discarded in
    v0.21 too).
  - New path (`delta_transaction.cpp:465-478`): `ffi::ExclusiveCommittedTransaction *committed_txn_ptr`
    unpacked from `ExternResult`; wrapped in `KernelCommittedTransaction committed_txn`; destructor
    calls `ffi::free_committed_transaction` at scope exit.
  - `ExclusiveCommittedTransaction` is an opaque forward-declared struct (line 149 of generated
    header). Its Rust-side allocation is unconditional on the success path.

- **Hypothesis**: The extra overhead per successful commit is:
  1. One Rust heap allocation inside `ffi::commit` to produce the `ExclusiveCommittedTransaction`.
  2. One additional cross-FFI call (`free_committed_transaction`) to release it.
  Neither `committed_transaction_version` nor `committed_transaction_post_commit_snapshot` is
  called anywhere in `src/` — the handle is created, wrapped in RAII, and immediately freed
  without reading it. This makes the allocation and free a pure overhead with no offsetting
  benefit at present.

- **Expected magnitude**: Commit is a per-transaction operation that runs I/O (writing the Delta
  log file) plus a Parquet flush. The allocation + free cycle is measured in hundreds of
  nanoseconds; the I/O dominates by many orders of magnitude. For a single large INSERT, this is
  completely noise.

  For workloads that issue many small rapid commits — specifically:
  - Idempotent ETL using `delta_set_transaction_version` (each row or small batch triggers
    `InitializeTransaction` → `ffi::commit` once per DuckDB transaction commit).
  - CCv2/Unity Catalog commit chains where `parent_commit=true` and the committer callback
    does network I/O on every `ffi::commit`.

  Even in these cases, the additional allocation/free cost is bounded by the network or file-I/O
  latency of the commit itself. Estimated regression: well under 1% of total commit time.

- **Proposed change**: None. The overhead is not actionable without a profiler measurement showing
  it as a hot spot. If a future workload does show commit-rate bottleneck, the correct optimization
  is to read and cache `committed_transaction_version` to avoid a follow-up `ffi::version()` call
  (which the old code already required separately), not to avoid the allocation. The kernel API
  change is intentional — `ExclusiveCommittedTransaction` exists to expose the post-commit snapshot
  without a second round-trip.

- **Verify**: `make bench-run-tpch-sf1` does not exercise write paths. A targeted benchmark
  would be a new `benchmark/write/insert_small_batches.benchmark` that runs N×1-row INSERTs and
  compares before/after. Absent that benchmark, this finding cannot be quantified.

- **Risk of current code**: Correctness is clean. RAII wrap-before-check ordering is correct
  (confirmed by iter-2 review). The `nullptr` init ensures no double-free if the kernel returns
  `Err`. No opportunity for optimization without measurement.

---

## [PERF-2] `VisitNullLiteral` 3-parameter overhead: negligible, on-stack only

- **Bottleneck**: The `visit_literal_null` callback changed from 2 parameters to 5:
  - Old: `void(void *data, uintptr_t sibling_list_id)`
  - New: `void(void *data, uintptr_t sibling_list_id, uint8_t type_tag, uint8_t precision, uint8_t scale)`
  The kernel materializes 3 extra `uint8_t` values per call. On both aarch64 and x86_64, all 5
  arguments fit in registers (x0–x4 / rdi, rsi, rdx, rcx, r8). No stack spill occurs.

- **Call frequency analysis**: `VisitNullLiteral` fires once per typed-NULL literal in a kernel
  expression tree returned during filter pushdown. This happens at query-bind time, not per-row.
  The number of NULL literals in a typical filter predicate is O(number of partition columns with
  NULL defaults) — practically zero to single digits per query.

- **Expected magnitude**: Zero measurable impact. The extra 3 register assignments per call occur
  at filter-translation time (bind), not in the scan hot path. This is not a per-row or
  per-STANDARD_VECTOR_SIZE cost.

- **Proposed change**: None.

- **Verify**: Not benchmarkable in isolation; no benchmark target is relevant.

- **Carry-forward from review (M2)**: `type_tag`, `precision`, and `scale` are named but
  suppressed with neither `(void)` casts nor unnamed-parameter syntax. Under `-Wunused-parameter`
  this produces warnings. This is a correctness/hygiene issue, not a performance issue. Fix is:

  ```cpp
  // src/delta_utils.cpp:251-256
  void ExpressionVisitor::VisitNullLiteral(void *state, uintptr_t sibling_list_id,
                                           uint8_t type_tag, uint8_t precision, uint8_t scale) {
      (void)type_tag;
      (void)precision;
      (void)scale;
      auto expression = make_uniq<ConstantExpression>(Value());
      static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
  }
  ```

  This is DuckDB coding standards compliance, not a performance optimization.

---

## [PERF-3] Other ABI shape changes in the generated FFI header: no new hot-path allocations

Checked all `ffi::` calls made in the extension's write and scan hot paths:

| Call site | v0.21 return type | v0.23 return type | Change |
|---|---|---|---|
| `ffi::commit` | `ExternResult<uint64_t>` | `ExternResult<Handle<ExclusiveCommittedTransaction>>` | Covered by PERF-1 |
| `ffi::transaction` | `ExternResult<Handle<ExclusiveTransaction>>` | same | No change |
| `ffi::transaction_with_committer` | same | same | No change |
| `ffi::with_engine_info` | same | same | No change |
| `ffi::with_transaction_id` | same | same | No change |
| `ffi::add_files` | `void` | `void` | No change |
| `ffi::get_app_id_version` | same | same | No change |
| `ffi::get_engine_data` | same | same | No change |
| All scan-path calls (`ffi::scan`, `ffi::scan_metadata_next`, etc.) | stable | stable | No change |

One new function signature appears in the generated header that is relevant to future work but not
called today:
- `ffi::get_unpartitioned_write_context(Handle<ExclusiveTransaction>, Handle<SharedExternEngine>)`
  returns `ExternResult<Handle<SharedWriteContext>>` — this DOES consume the transaction handle
  per the FFI doc. This is NOT called anywhere in `src/` currently (confirmed by grep returning no
  matches). `VisitWriteContextSchema` in `src/delta_utils.cpp` is defined and compiles but has no
  callers. This is a latent dead-code situation, not a performance issue.

- **Verdict on PERF-3**: No new allocations or FFI costs were introduced in any currently-active
  hot path by the bump.

---

## [PERF-4] `KernelCommittedTransaction` RAII lifetime: tight and correct, intermediate raw pointer is unavoidable

- **Pattern under review** (`delta_transaction.cpp:465-470`):
  ```cpp
  ffi::ExclusiveCommittedTransaction *committed_txn_ptr = nullptr;
  auto res = KernelUtils::TryUnpackResult(
      ffi::commit(kernel_transaction.release(), table_entry->snapshot->extern_engine.get()),
      committed_txn_ptr);
  KernelCommittedTransaction committed_txn(committed_txn_ptr);
  ```

- **Could the raw pointer be eliminated?** No. `TryUnpackResult<T>` deduces `T` from
  `ExternResult<T>` via template argument deduction. `ExternResult<T>` carries `T` in the `Ok`
  body. If the out-parameter were typed as `KernelCommittedTransaction`, template deduction would
  fail because `KernelCommittedTransaction` (a `TemplatedUniqueKernelPointer`) is not
  `ffi::ExclusiveCommittedTransaction *`. The two-step idiom is the minimal correct workaround and
  is well-documented in the implementation log (section 4.4).

- **Is there an exception window?** No. `TryUnpackResult` returns `ErrorData` (value, never
  throws). The RAII wrap on line 470 follows immediately with no throwing code between lines 468
  and 470. The `nullptr` init on line 465 ensures the `KernelCommittedTransaction` destructor is a
  safe no-op if `commit` returns `Err` and never writes the `Ok` field.

- **Proposed change**: None. The lifetime is tight. An alternative using a local lambda or
  `std::unique_ptr` with a custom deleter would not reduce overhead and would reduce readability
  relative to the consistent `TemplatedUniqueKernelPointer` pattern used by all other RAII handles
  in this codebase.

- **One genuine micro-opportunity** (not actionable without a benchmark): since
  `committed_transaction_version` is never called, the allocation+free cycle is pure waste. If we
  could signal to the kernel "don't populate the post-commit snapshot" the kernel could potentially
  skip building the `ExclusiveCommittedTransaction` altogether. No such opt-out exists in v0.23's
  FFI surface, so this is a kernel-side enhancement request for a future kernel bump, not something
  actionable on our side now.

---

## Summary

| Finding | Severity | Actionable now? | Proposed change |
|---|---|---|---|
| PERF-1: extra heap alloc+free per commit | Low | No — I/O-dominated; no benchmark showing impact | None |
| PERF-2: 3 extra register args to `VisitNullLiteral` | Negligible | No — bind-time, not per-row | `(void)` casts (style hygiene, not perf) |
| PERF-3: other ABI changes in hot paths | None | N/A | N/A |
| PERF-4: intermediate raw pointer in RAII pattern | None — design-forced | No | None |

The v0.23 bump introduces **one structural overhead** (the `ExclusiveCommittedTransaction`
allocation+free per commit) that did not exist in v0.21. This overhead is bounded by the I/O cost
of the commit operation itself and is below measurement noise for any realistic workload. The
`VisitNullLiteral` ABI change adds 3 register values per rare bind-time callback invocation and is
not measurable.

No optimization changes are proposed. The bump is intrinsically thin on the C++ side, and every
observable overhead is dominated by I/O or Rust-side kernel logic that is outside this extension's
control.

---

## Verdict: OPTIMIZED

**High findings: 0**
**Medium findings: 0**

The diff is performance-neutral for all currently-active code paths. The single structural
overhead (PERF-1) is non-actionable at this time: it is I/O-dominated, no benchmark target
exists to measure it, and no opt-out exists in the v0.23 kernel FFI surface. The correct response
if commit-rate performance becomes a concern in a future workload is to add a targeted write
benchmark and file a kernel feature request for a "no post-commit snapshot" flag.
