# DELETE FROM — Performance Findings

Analyst: perf agent
Date: 2026-05-18
Base: main (post-review-iteration-2, all blockers resolved)

Files analysed: `src/storage/delta_delete.cpp`,
`src/functions/delta_scan/delta_multi_file_list.cpp`,
`src/storage/delta_transaction.cpp`,
`src/include/functions/delta_scan/delta_multi_file_list.hpp`

---

## Summary table

| ID   | Level | Priority | Hot path               | Finding |
|------|-------|----------|------------------------|---------|
| P1   | 2     | Medium   | `predicate_was_true`   | N mutex round-trips for cardinality accumulation |
| P2   | 2     | Medium   | `WHERE` branch         | Two full `GetAllFiles()` copies of `resolved_files` |
| P3   | 2     | Low      | `StageRemoveFiles`     | Per-batch `vector<uint8_t>` allocation without `reserve` |
| P4   | 1     | Low      | `StageRemoveFiles`     | `unordered_set` for a sorted index lookup that could be binary-searched |
| P5   | 6     | Low      | `DeltaDeleteGlobalState` | Missing `= default` constructor (style; zero perf impact) |

---

## [P1] `predicate_was_true` branch acquires the snapshot mutex N times

**Bottleneck:**
`BuildDeletePlan` (`src/storage/delta_delete.cpp` lines 173–183) loops
over `total_files` indices and calls `snapshot.GetMetaData(file_idx)`
inside the loop body. `GetMetaData` acquires `unique_lock<mutex>(lock)`
on every call (line 1117 of `delta_multi_file_list.cpp`). For a table
with N files that means N mutex lock/unlock round-trips — one per file.

This is the path exercised by `DELETE FROM t` with no WHERE clause
(full-table delete) and is the single hottest plan-time path for that
case. At N = 10,000 files the cost is on the order of tens of
microseconds of pure synchronisation overhead (modern mutex lock:
~20–50 ns uncontended × 10,000 = 0.2–0.5 ms). At N = 100,000 files
it is 2–5 ms of plan overhead on top of whatever the kernel commit
takes. This is not in measurement noise on realistic workloads with
many small partitioned files.

**Root cause:** `GetMetaData` is a public API designed for
single-file lookups from the scan path. The `predicate_was_true` loop
reuses it in bulk without a lock-free internal variant.

**Hypothesis:** Replace the N per-call locks with a single lock scope
that reads all cardinalities from the already-materialized `metadata`
vector. The snapshot is guaranteed fully materialized before this loop
executes (the `GetTotalFileCount()` call at line 104 drives
`GetFileInternal` to completion). After that point `metadata` is
stable and a single locked bulk-read is correct.

**Proposed change sketch:**

```cpp
if (predicate_was_true) {
    // --- No WHERE: remove all files ---
    // Reserve upfront to avoid N reallocations.
    delete_plan_value.files_to_remove.reserve(total_files);

    // Single lock scope: read all cardinalities in one shot.
    // The snapshot is fully materialized by GetTotalFileCount() above.
    idx_t total_rows = 0;
    {
        unique_lock<mutex> lck(snapshot.lock);
        for (idx_t file_idx = 0; file_idx < total_files; file_idx++) {
            delete_plan_value.files_to_remove.push_back(file_idx);
            const idx_t card = snapshot.metadata[file_idx]->cardinality;
            if (card != DConstants::INVALID_INDEX) {
                total_rows += card;
            }
        }
    }
    delete_plan_value.total_rows_removed = total_rows;
```

This requires making `lock` and `metadata` accessible to the free
function `BuildDeletePlan`. Two approaches:

Option A: declare `BuildDeletePlan` as a `friend` of
`DeltaMultiFileList` (minimal change, matches existing `ScanDataCallBack`
pattern).

Option B: add a lock-free internal helper
`GetAllCardinalitiesInternal(vector<idx_t> &out) const` that the caller
invokes while holding the lock, mirroring `GetTotalFileCountInternal`.
This is cleaner from an API boundary perspective and does not require
friending a free function.

Option B is preferred: it keeps the internal/public distinction clean
and can also benefit the `GetCardinality()` method which already holds
the lock and iterates `metadata` directly (lines 1094–1100).

**Benchmark to measure:**
Add `benchmark/delete/full_table_delete.benchmark` targeting a
partitioned table with 1,000–10,000 files. Before/after comparison
via `make bench-run-delete BENCHMARK_PATTERN=full_table_delete.benchmark`.
The improvement is N-proportional: at N=10,000 expect ~0.3 ms plan-time
reduction (lock uncontended baseline). This is below noise for a single
run but measurable across 100 iterations in the benchmark harness.

**Risk:** Low. The `metadata` vector is stable after full materialization.
The correctness argument is identical to `GetCardinality()`, which already
reads `metadata` directly under one lock (lines 1088–1101). No semantic
change, just lock scope widening.

**Standards check:** `idx_t` throughout. `unique_lock<mutex>`. No raw
pointers introduced. No `using namespace std`. Matches the pattern
already used by `GetCardinality()`.

---

## [P2] Two full `GetAllFiles()` copies of `resolved_files` in the WHERE branch

**Bottleneck:**
`BuildDeletePlan`'s WHERE branch (lines 249–274 of `delta_delete.cpp`)
calls `GetAllFiles()` twice:
1. `snapshot.GetAllFiles()` (line 251) — builds the `path_to_orig_idx`
   map.
2. `candidate_list_ptr->GetAllFiles()` (line 258) — iterates candidate
   files to look up original indices.

`GetAllFiles()` returns `vector<OpenFileInfo>` **by value** (line 1067
of `delta_multi_file_list.cpp`: `return resolved_files;`). Each call
copies the entire `resolved_files` vector — one element per active Add
file in the snapshot. An `OpenFileInfo` contains at least a `string`
(the path, typically 100–300 bytes for a cloud path). For N=10,000 files
this is two O(N) copies of the path strings: on the order of
2–6 MB of data copied and heap-allocated for every DELETE planning call,
solely to extract the path.

The second `GetAllFiles()` is even more expensive when the pushdown
produced a different `DeltaMultiFileList` (the `pruned_owned` branch):
that list has its own `resolved_files` vector, also returned by copy.

This is a Level-2 (allocation reduction) bottleneck. The cardinality
information from the `metadata` vector is already accessible per-index;
only the path strings require the `GetAllFiles` detour.

**Hypothesis:** Replace the two `GetAllFiles()` copies with a single
helper that builds the `unordered_map<string, idx_t>` under one lock
from `resolved_files` without materializing a `vector<OpenFileInfo>`
copy, or — better — use `GetMetaData`/the path from the candidate list
directly since each candidate file's `DeltaFileMetaData` already carries
the path indirectly through `resolved_files`. The cleanest fix is to
add an internal helper `GetPathForIndex(idx_t) const` that reads from
`resolved_files[i].path` under one lock instead of returning the full
vector.

Alternatively: replace `snapshot.GetAllFiles()` (which copies all N
paths) with a direct `unordered_map`-building function that iterates
`resolved_files` under the lock without copying strings by key:

```cpp
// New internal helper (or friend-accessed):
//   fills out: path -> original_index
void BuildPathIndexMap(unordered_map<string, idx_t> &out) const {
    unique_lock<mutex> lck(lock);
    out.reserve(resolved_files.size());
    for (idx_t i = 0; i < resolved_files.size(); i++) {
        out.emplace(resolved_files[i].path, i);
    }
}
```

This avoids the full `vector<OpenFileInfo>` copy entirely. The
`candidate_list_ptr->GetAllFiles()` call for the pruned list is harder
to avoid but is bounded by the smaller candidate count (not N, but M
where M <= N). Still, for correctness the pruned path currently
copies M paths too; `GetFile(idx_t)` could be used per-candidate
instead, avoiding the second copy.

**Benchmark to measure:** Same as P1: partition-aligned delete on a
large table (10,000 files, 500 partitions). The allocator pressure
shows as reduced peak RSS and fewer GC events in `heaptrack` output.
Expected: ~2N string copies eliminated per DELETE plan call.

**Risk:** Low for the `BuildPathIndexMap` approach. The `GetAllFiles()`
return-by-value is an existing API contract (DuckDB's `MultiFileList`)
so the public API cannot be changed, but the internal usage in
`BuildDeletePlan` is free-function scope and can use a narrower API.
Changing the return type of `GetAllFiles()` to `const
vector<OpenFileInfo> &` (with a warning about lock required) would be
more invasive and risk a DuckDB API contract break.

**Standards check:** `idx_t`, `unique_lock<mutex>`. No raw pointers.
Named function instead of lambda. Consistent with the `GetAllFiles`
pattern in the existing codebase.

---

## [P3] Per-batch `vector<uint8_t>` in `StageRemoveFiles` allocated without `reserve`

**Bottleneck:**
`StageRemoveFiles` (lines 1231) constructs a fresh `vector<uint8_t>
remove_sv(batch_rows, 0)` on every loop iteration. For small N (few
files) this is one or two allocations and is negligible. For large
tables where the kernel streams many batches, each batch triggers a
fresh heap allocation for `remove_sv`.

The typical kernel batch size is a few hundred to a few thousand rows.
For a 10,000-file table with batches of 1,000, there are ~10 allocations.
This is genuinely low magnitude — each allocation is fast — but it is an
easy win.

**Hypothesis:** Hoist `remove_sv` out of the loop and reuse the buffer
by calling `assign(batch_rows, 0)` at the top of each iteration (which
will reallocate only if `batch_rows` grows, and reuses the allocation
otherwise).

**Proposed change:**

```cpp
vector<uint8_t> remove_sv;   // hoisted outside the loop
while (true) {
    // ... fetch raw_result ...

    const idx_t batch_rows = NumericCast<idx_t>(raw_result->arrow_data.array.length);
    // Reuse allocation; zero-fill for this batch.
    remove_sv.assign(batch_rows, 0);

    // rest of loop body unchanged
```

**Benchmark to measure:** The benefit is only visible at high batch
counts (many small batches). At the scale of typical DELETE workloads
(tens to hundreds of files removed), this is below measurement noise.
Include in `full_table_delete.benchmark` as a `heaptrack` comparison.

**Risk:** Minimal. `assign()` is equivalent to construction but reuses
capacity. No semantic change. The `any_selected` guard means `remove_sv`
is only passed to the kernel when it contains at least one `1` entry;
hoisting does not affect that.

**Standards check:** Standard C++. No DuckDB-specific concerns.

---

## [P4] `unordered_set<idx_t>` in `StageRemoveFiles` — acceptable but could be sorted+binary-search

**Bottleneck:**
`StageRemoveFiles` builds `unordered_set<idx_t> remove_set` from
`file_indices` (line 1171). For each active ADD row in each batch, it
calls `remove_set.count(global_file_idx + active_in_batch)` (line 1238).

For small `file_indices.size()` (typical: 1–10 files per DELETE),
`unordered_set` is actually slower than a sorted `vector<idx_t>` +
`std::binary_search` due to hash-table overhead (load factor, cache
misses on the bucket array). For large N (hundreds of files to remove)
`unordered_set` is better.

The crossover point is around 10–20 elements. Since DELETE is most
commonly partition-aligned with a small number of candidate files, a
sorted-vector approach may be faster in the common case.

**Hypothesis:** For typical DELETE workloads (1–20 files), replace
`unordered_set` with a sorted `vector<idx_t>` and use `std::binary_search`.

```cpp
// Sort once upfront (file_indices may not be sorted).
vector<idx_t> sorted_indices = file_indices;
std::sort(sorted_indices.begin(), sorted_indices.end());

// In the loop:
if (std::binary_search(sorted_indices.begin(), sorted_indices.end(),
                       global_file_idx + active_in_batch)) {
```

**Benchmark to measure:** Micro-benchmark: `StageRemoveFiles` with N=1,
N=5, N=20, N=100 files. The `unordered_set` construction also saves
one heap allocation (the bucket array) for the sorted-vector variant.

**Risk:** Low. `std::sort` and `std::binary_search` are standard and
correct. The sorted-vector path adds an O(k log k) upfront sort
(k = `file_indices.size()`) which is negligible. For k > ~50 the hash
set is faster; a simple heuristic threshold (e.g. sort if k < 64) keeps
both cases fast. Complexity argument only; no profiler data in hand, so
this is a hypothesis, not a measurement.

**Standards check:** Standard C++ algorithms. No `using namespace std`.
No DuckDB style violations.

---

## [P5] `DeltaDeleteGlobalState()` default constructor (style-only, zero perf impact)

**Bottleneck:** Not a performance concern. `DeltaDeleteGlobalState() { }`
at line 27 of `delta_delete.cpp` is an empty body where `= default` is
conventional in DuckDB. Flagged in review as [L2]. Zero perf delta.

**Verdict:** Skip. Already tracked in review findings [L2].

---

## Assessment by optimization level

### Level 1 — Algorithmic complexity: NO NEW FINDINGS

The `predicate_was_true` loop is O(N) — correct and unavoidable.
The `path_to_orig_idx` map build is O(N) — correct.
The `StageRemoveFiles` iteration is O(total_snapshot_files) — unavoidable
given the kernel's streaming API.
No accidental quadratic patterns found. The root-bug fix (path-based
lookup rather than `file_number` from pruned scan) correctly eliminated
what would have been a correctness bug, not a perf issue.

### Level 2 — Allocation reduction: P1, P2, P3

P1 eliminates N mutex acquisitions in the full-delete path.
P2 eliminates two O(N)-string-copy calls in the WHERE path.
P3 is a minor allocation-reuse fix in the staging loop.

### Level 3 — Data layout: NOT APPLICABLE

`DeltaFileMetaData` is accessed once per file at plan time. The struct
is already fine for this access pattern; no SoA opportunity exists at
this scale.

### Level 4 — Concurrency: COVERED BY P1

P1 is also a concurrency concern: the N round-trips to acquire the
snapshot lock from `BuildDeletePlan` are unnecessary lock traffic.
No new races identified. `outstanding_removes` is written once
per `RemoveFiles` call and read once in `Commit`, all within the
single DuckDB pipeline executor for a non-parallel sink — logically
race-free, consistent with the review finding [M3].

### Level 5 — FFI overhead: NO FINDING

`StageRemoveFiles` already minimizes FFI calls to one
`scan_metadata_next_arrow` + one `get_engine_data` + (conditional)
one `remove_files` per batch. There is no coalescing opportunity
given the kernel's per-batch streaming model. The `remove_files` call
is gated on `any_selected`, avoiding FFI calls for batches with no
removals — this is already correct.

### Level 6 — Compiler / move semantics: MINOR

`FileLevelDeletePlan` is already moved into `DeltaDelete` via
`std::move(delete_plan_value)` at the call site (line 328). No missed
moves on hot paths found. The `remove_sv` fix in P3 is the closest
compiler-friendly change.

### Level 7 — SIMD: NOT APPLICABLE

The selection-vector construction loop in `StageRemoveFiles`
(lines 1234–1244) is small and branch-dominated; SIMD offers no gain
for typical file counts.

---

## Magnitude reality check

The kernel `ffi::commit` call itself involves a Rust async runtime,
likely an S3 or local-file write, and delta-log JSON serialization.
On a cold path that takes, say, 50–500 ms for commit, a 0.3 ms
plan-time improvement (P1 at N=10,000) is a 0.06–0.6% gain — below
the target of "measurable benefit" for most workloads. At N=100,000
files (a large table) P1 becomes 2–5 ms, which is ~0.4–10% of commit
latency depending on storage speed. P2 (string copies) adds 2–10 ms
allocator pressure at N=10,000 which is similarly borderline.

**Conclusion:** P1 and P2 are Medium priority because:
1. They are trivially correct changes (single lock scope, remove a
   copy).
2. They matter at the high-file-count tail where DELETE is most likely
   to be used in bulk partition management.
3. They are not premature optimisations — they fix structural
   inefficiencies (unnecessary repeated locking, unnecessary full
   vector copy) that will worsen proportionally as table size grows.

P3 and P4 are Low because their benefit is below measurement noise
on typical DELETE workloads (tens to hundreds of files).

---

**HAS_OPPORTUNITIES**
