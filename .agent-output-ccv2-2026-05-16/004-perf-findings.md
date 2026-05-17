# Performance Findings: CCv2 CTAS Wiring + Bug Fixes

## Executive Summary

Five areas analyzed. Two have zero perf cost on the dominant (non-CCv2) path.
One has a negligible cost but with an avoidable structural issue. Two findings
warrant attention. Overall verdict: **HAS_OPPORTUNITIES** — but only one
qualifies as Medium; the others are Low or zero.

---

## [OPT-1] `HasNonStatsType` recursive traversal — per-column per-file call

**Bottleneck**

File: `/workspace/src/storage/delta_insert.cpp:195-209` (`HasNonStatsType`)
and line 269 (call site inside `AddWrittenFiles`).

The old check was `O(1)`: `type.id() == LIST || type.id() == VARIANT`.

The new check is recursive over the STRUCT child tree. For a STRUCT column
with `k` transitively nested children, `HasNonStatsType` visits `O(k)` nodes.
The function is called once per stats entry emitted by parquet per column per
file. For a wide table written across `F` files and containing `C` STRUCT
columns each with `D` levels of nesting:

- Old cost: `O(C * F)` comparisons.
- New cost: `O(C * F * D * branching_factor)` comparisons.

For typical OLAP schemas: `C` is 1–5, `F` is 1–hundreds, `D` is 1–3, and
each STRUCT rarely has more than 10 direct children. Worst-case plausible:
20 columns, 500 files, average 3-level STRUCT with 5 children per level.
Old cost ≈ 10,000 iterations. New cost ≈ 10,000 * (1 + 5 + 25) = 310,000
iterations — a 31x increase in iterations for this check alone. Each iteration
is a cheap `id()` comparison so the absolute cost is still in the microsecond
range, but for CTAS workloads writing many partitioned files the accumulation
matters.

**Hypothesis**

The result of `HasNonStatsType` is **stable per column** — a column's logical
type does not change within a single CTAS sink invocation. Computing it once
per column (not once per column per file) eliminates the redundant traversal.

The existing `DeltaInsertGlobalState::columns` vector is already iterated once
per `AddWrittenFiles` call to resolve the column type. A parallel
`vector<bool> column_has_non_stats_type` computed during `GetGlobalSinkState`
(where `columns` is built) would pay the traversal cost exactly once.

**Change**

```cpp
// In DeltaInsertGlobalState (delta_insert.cpp ~ line 67):
vector<bool> column_skip_stats;  // parallel to `columns`; true == skip stats

// In both DeltaInsertGlobalState constructors, after columns is populated:
column_skip_stats.reserve(columns.size());
for (const auto &col : columns) {
    column_skip_stats.push_back(HasNonStatsType(col.type));
}

// In AddWrittenFiles, replace the column lookup + HasNonStatsType call:
// OLD (lines 233-244):
//   bool found = false; LogicalType coltype;
//   for (auto &col : global_state.columns) { if col.name == ... { found=true; coltype=col.type; break; } }
//   ...
//   if (HasNonStatsType(coltype)) { continue; }
//
// NEW: look up index instead of type:
idx_t col_idx_found = DConstants::INVALID_INDEX;
for (idx_t ci = 0; ci < global_state.columns.size(); ci++) {
    if (global_state.columns[ci].name == column_names[0]) {
        col_idx_found = ci;
        break;
    }
}
if (col_idx_found == DConstants::INVALID_INDEX) {
    throw InternalException("Column %s not found in table %s", ...);
}
if (global_state.column_skip_stats[col_idx_found]) {
    continue;
}
stats.root_type = global_state.columns[col_idx_found].type;
```

The recursive walk executes exactly once per column at sink-state construction
rather than `O(num_files)` times at sink time.

**Expected magnitude**: For typical schemas (scalar columns dominate, few
deeply nested STRUCTs, O(1) files), cost difference is negligible. For
partitioned CTAS writing hundreds of files with STRUCT-of-STRUCT columns, this
turns O(F * D * branching) back into O(1) amortized per file. The change also
converts the per-file column linear scan from "find type + check HasNonStats"
into "find index + check precomputed bool", which is branch-predictor friendly.

**Verify**

Benchmark: add a CTAS writing a 10-column schema (3 STRUCT columns, each
2 levels deep with 5 children) partitioned into 200 files. Measure
`AddWrittenFiles` call wall time before/after with `samply` or `perf stat`.
Expected: the `HasNonStatsType` stack frames should disappear from the profile.

```
BUILD_BENCHMARK=1 make release
make bench-run-tpch-sf1 BENCHMARK_PATTERN=ctas_wide_struct.benchmark
```

(New benchmark file to be added under `benchmark/` for a before/after
comparison.)

**Risk**: None for correctness — the precomputed result is derived from the
same `LogicalType` values as the inline call. The `column_skip_stats` vector is
local to the non-shared global sink state (CTAS is single-threaded:
`ParallelSink() == false`). No concurrency concern.

**Standards check**: Uses `idx_t`, `vector<bool>`, `DConstants::INVALID_INDEX`.
No `using namespace std`. Follows DuckDB style.

**Severity**: Low-Medium. The absolute execution time is small because
`HasNonStatsType` body is cheap. The O-complexity improvement is real but
only matters for wide-STRUCT schemas with many output files.

---

## [OPT-2] `std::atomic<idx_t> ccv2_committed_version` — wrong primitive for the access pattern

**Bottleneck**

File: `/workspace/src/include/storage/delta_catalog.hpp:62`
Store site: `/workspace/src/storage/delta_transaction.cpp:479`
Load site: `/workspace/src/storage/delta_schema_entry.cpp:180`

The field uses `std::atomic<idx_t>` with `memory_order_release` on the store
and `memory_order_acquire` on the load. This is correct for cross-thread
communication but introduces unnecessary memory barriers on every snapshot
lookup.

The critical access-pattern analysis:

1. **Store** (`delta_transaction.cpp:479`): called once, inside
   `DeltaTransaction::Commit`, which runs on the committing thread when
   `parent_commit == true`. This is the CTAS commit path — extremely rare
   (once per `CREATE TABLE AS SELECT` statement).

2. **Load** (`delta_schema_entry.cpp:180`): called inside
   `CreateTableEntry`, which is called from `LookupEntry`. `LookupEntry` is
   called on **every snapshot lookup** — every `SELECT` from a Delta table
   goes through `LookupEntry` → `InitializeTableEntry` → eventually
   `CreateTableEntry`. The `CreateTableEntry` call itself is guarded by a
   `unique_lock<mutex>` on `DeltaSchemaEntry::lock` (line 293 / 305 in
   `delta_schema_entry.cpp`).

The `mutex` on `DeltaSchemaEntry` is held every time `CreateTableEntry` is
called. The `acquire` load of the atomic is therefore always executed under
an exclusive lock. An `acquire` fence while holding a mutex is redundant: the
mutex acquire itself provides the acquire fence for all prior writes (including
the `release` store to `ccv2_committed_version`). The atomic with explicit
ordering buys nothing over a plain `idx_t` field protected by the same mutex.

On aarch64 (this host: `aarch64`), `std::atomic<idx_t>::load(acquire)` emits
a `LDAR` instruction — a load-acquire that prevents load/store reordering
across the instruction. When the mutex is already providing that ordering,
this is a redundant hardware barrier. On x86 this would be `MOV` + compiler
fence (cheaper), but the intent is cross-platform correctness.

**Hypothesis**

For the non-CCv2 path (the `else if (delta_catalog.parent_commit)` branch is
not taken): the load is never reached because the guard `parent_commit` is
`false`. Cost on the dominant path is zero (the branch is not taken, so the
atomic load is never issued).

For the CCv2 path (where `parent_commit == true`): the load IS reached on
every `CreateTableEntry` call. However, `CreateTableEntry` itself spins up a
DeltaMultiFileList (which calls `BuildEngine`, which calls `CreateBuilder`,
which calls `ffi::get_engine_builder` — an FFI call). The atomic load cost
is completely dominated by the FFI cost.

**Conclusion**: the atomic adds no observable overhead on the common path
(non-CCv2, `parent_commit == false`, branch not taken). On the CCv2 path it is
dominated by FFI. The fix is valuable for correctness documentation — the
`memory_order_acquire/release` pair misleads readers into thinking cross-thread
synchronization is required, when in fact the mutex already provides it.

**Proposed change** (documentation clarity, not perf-critical):

Convert `std::atomic<idx_t>` to a plain `idx_t` field guarded by
`DeltaSchemaEntry::lock`. The store in `DeltaTransaction::Commit` must then
take the schema entry's lock before writing. Alternatively, accept the current
shape as correct-but-overcautious and add a comment explaining the redundancy.

The simpler path: add a comment.

```cpp
// In delta_catalog.hpp:
// Note: this atomic is accessed only from CreateTableEntry, which is called
// under DeltaSchemaEntry::lock. The acquire/release ordering is therefore
// redundant but retained for correctness under hypothetical future callers
// that bypass the lock.
std::atomic<idx_t> ccv2_committed_version {DConstants::INVALID_INDEX};
```

**Verify**: Not performance-measurable at current call frequency. The
correctness argument is the main concern. No benchmark target applies.

**Risk**: Converting to a plain field protected by the schema lock would
require either adding a back-pointer from `DeltaCatalog` to its
`DeltaSchemaEntry` lock (awkward coupling) or a separate `mutex` on
`DeltaCatalog` just for this field. The atomic is a reasonable pragmatic choice.
Changing it introduces coupling risk that outweighs the negligible perf gain.

**Standards check**: Current form already uses `idx_t`. Consistent with
DuckDB style (H3 fix from review iteration 1 corrected `uint64_t` → `idx_t`).

**Severity**: Low (zero measurable impact; correctness-documentation concern
only).

---

## [OPT-3] `max_catalog_version` propagation — non-CCv2 path cost

**Bottleneck**

File: `/workspace/src/storage/delta_schema_entry.cpp:177-184`
(inside `CreateTableEntry`).
File: `/workspace/src/functions/delta_scan/delta_multi_file_list.cpp:746-748`
(inside `InitializeSnapshot`).

The propagation path is:

1. `CreateTableEntry` (called under `DeltaSchemaEntry::lock`):
   - Compare `delta_catalog.max_catalog_version != DConstants::INVALID_INDEX` — one integer comparison.
   - If false: compare `delta_catalog.parent_commit` — one bool read.
   - If false: neither branch taken; `snapshot->max_catalog_version` stays at `INVALID_INDEX`.

2. `InitializeSnapshot` (called lazily, guarded by `DeltaMultiFileList::lock`):
   - Compare `max_catalog_version != DConstants::INVALID_INDEX` — one integer comparison.
   - If false: `ffi::snapshot_builder_set_max_catalog_version` is NOT called.

For the dominant non-CCv2 path (`parent_commit == false`,
`max_catalog_version == INVALID_INDEX`):
- `CreateTableEntry` pays: 2 integer comparisons + no atomic load (branch not
  taken, see OPT-2).
- `InitializeSnapshot` pays: 1 integer comparison.

Total: 3 integer comparisons and 3 memory reads (two in `DeltaCatalog`, one
field on `DeltaMultiFileList`). These are already in hot cache lines
(all fields accessed within the same `CreateTableEntry` and
`InitializeSnapshot` calls respectively). No allocation, no FFI hop, no branch
misprediction risk (the predictor will quickly learn the `INVALID_INDEX`
pattern is "branch not taken").

**Conclusion**: The non-CCv2 path pays exactly 3 integer comparisons.
This is zero measurable overhead. The guard pattern is correct and minimal.

**Verify**: No benchmark required. Static analysis of the branch structure is
sufficient.

**Risk**: None.
**Severity**: Zero (no finding to address).

---

## [OPT-4] `create_table_builder_with_table_property` chain — O(properties) FFI hops

**Bottleneck**

File: `/workspace/src/storage/delta_transaction.cpp:708-745`
(inside `InitializeForNewTable`, CCv2 branch).

Three sequential calls to `ffi::create_table_builder_with_table_property`,
each consuming the prior builder handle and returning a new one:

```
builder --[prop1: catalogManaged]--> builder_raw_prop
                                          ↓
                                [prop2: vacuumProtocolCheck]--> builder_raw_prop
                                                                      ↓
                                                     [prop3: io.unitycatalog.tableId]--> builder_raw_prop
```

Each call is an FFI round-trip into Rust: it crosses the C ABI boundary,
enters the delta-kernel-rs runtime, and returns a new
`Handle<ExclusiveCreateTableBuilder>`. The pattern is O(num_properties) FFI
hops. Currently num_properties = 3 (constants defined at lines 711-716).

Each `KernelUtils::TryUnpackResult` call on the `ExternResult` also performs
the `ErrorData` constructor path on the success branch. The builder is
replaced in-place via `builder = KernelExclusiveCreateTableBuilder(builder_raw_prop)`
on every iteration — this means 3 allocations for 3 RAII wrapper constructions
(the underlying pointer is raw, but the wrapper itself is stack-local, so no
heap allocation — `KernelExclusiveCreateTableBuilder` holds a raw pointer and
calls `ffi::free_create_table_builder` on destruction).

**Magnitude analysis**: This runs only in the CCv2 CTAS path, which is:
- Already under a Tokio async runtime startup (from `BuildEngine` via
  `ffi::builder_build` several lines above).
- Already doing one `ffi::get_create_table_builder` call.
- The 3 extra property calls add ~3× the per-FFI-call overhead on top of the
  already-expensive table-creation setup.

Per-FFI call on a local path is ~50–500 ns (JIT warm, no I/O). For 3 extra
calls: 150–1500 ns total. The `ffi::builder_build` call that opens the Tokio
runtime is measured in milliseconds. The property calls are noise.

**Hypothesis**: There is no kernel API to batch multiple table properties into
a single FFI call in the current `v0.21.0` (or `v0.23.0` per the implementation
log) kernel. The chain pattern is what the FFI surface forces. If future
kernel versions expose a batch-property call, this pattern should be revisited.
For now it is correct and the absolute overhead is unmeasurable against the
surrounding I/O.

**Proposed change**: None — document the pattern for future evolution.
If the kernel adds `create_table_builder_with_table_properties(builder, keys[], values[], n)`,
switch to it. For now the 3-call chain is optimal given the available FFI.

**Verify**: Benchmark cannot distinguish 3× ~100 ns from noise in a multi-ms
commit. No benchmark target applicable.

**Risk**: None (no change proposed).
**Severity**: Low (acceptable given FFI surface; flag for kernel API improvement).

---

## [OPT-5] `Sink` loop after assertion removal — correctness, no perf delta

**Bottleneck**

File: `/workspace/src/storage/delta_insert.cpp:299-306`
(the `DeltaInsert::Sink` body after the `InternalException` was removed).

The removed assertion (`if (chunk.size() != 1) throw`) was a no-op for the
`AddWrittenFiles` loop that immediately follows — `AddWrittenFiles` has always
iterated `for (idx_t r = 0; r < chunk.size(); r++)`. Removing the guard:

1. Does not change the loop bounds — the loop always runs `chunk.size()`
   iterations.
2. Does not introduce or remove any allocation, branch, or FFI call.
3. Does not affect branch prediction: `chunk.size()` is typically 1 for
   non-partitioned CTAS (unchanged from before the fix); for partitioned CTAS
   it is now correctly > 1 instead of throwing.

The only observable behavior change is that partitioned CTAS with N distinct
partition values now processes N rows per `Sink` call instead of throwing.
`AddWrittenFiles` is already O(chunk.size()) — this is the correct behavior.

**Conclusion**: Zero new performance cost. The removed throw was cold-path
dead code for any working partitioned write. The `Sink` body is now a single
`AddWrittenFiles(global_state, chunk)` call, which is the minimal possible
form.

**Severity**: Zero (no perf finding; correctness-only).

---

## Summary Table

| Finding | Area | Severity | Proposed Change |
|---------|------|----------|-----------------|
| OPT-1 | `HasNonStatsType` per-column per-file | Low-Medium | Precompute per-column in global sink state constructor |
| OPT-2 | `atomic<idx_t>` under mutex | Low | Add clarifying comment; atomic is harmless but misleading |
| OPT-3 | `max_catalog_version` on non-CCv2 path | Zero | No change needed |
| OPT-4 | `with_table_property` chain FFI | Low | Document; revisit if kernel adds batch API |
| OPT-5 | `Sink` loop after assert removal | Zero | No change needed |

---

## Verdict

**HAS_OPPORTUNITIES**

High findings: 0
Medium findings: 0
Low findings: 3 (OPT-1, OPT-2, OPT-4)

The only actionable optimization is OPT-1 (`HasNonStatsType` memoization
in global sink state). It has zero correctness risk, follows DuckDB style,
and eliminates a real O(files × schema_depth) redundancy — even though the
absolute cost is tiny for current workloads, the pattern is correct to fix
before the schema-visitor traversal gets deeper with richer type support.
OPT-2 and OPT-4 are documentation-level notes with no code changes required
at this time.
