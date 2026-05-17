# CTAS Kernel-Native Path — Performance Findings

**Verdict: OPTIMIZED**
**High findings: 0 | Medium findings: 1**

---

## Summary

The kernel-native CTAS path replaces a hand-rolled JSON writer with per-column FFI
visitor callbacks. CTAS is a planning-time operation executed once per table creation —
not a hot scan or append path. No finding in this report warrants a code change before
shipping. One medium finding documents a latent allocation pattern that may matter at
very high column counts.

---

## [PERF-1] Visitor overhead vs. JSON construction (ANALYSIS ONLY — no action needed)

**Bottleneck:** The old path (`DeltaSchemaJson::BuildSchemaString`) did O(N_columns)
string concatenations inside a single in-process loop, with a `reserve(N * 70)` on
`fields_json`. The new path does O(N_columns) FFI callbacks, each of which crosses the
Rust ABI boundary, allocates a kernel-side `StructField` in the `ReferenceSet<T>`,
and returns a `uintptr_t` field ID.

**Old path allocation profile (from `delta_ctas.cpp`, git history):**

- `fields_json`: one `string` with `reserve(N * 70)`, grows in-place.
- `result` in `BuildCommitJson`: one `string` with `reserve(420 + schema_size)`.
- Per-column: zero heap allocations for scalar types; one additional `string`
  per STRUCT/LIST/MAP nesting level (recursive `LogicalTypeToDeltaType` call returns
  a `string` by value, not a view).

Total schema-build allocations (flat schema, N columns): 2 + 0 + N zero = 2 strings
visible from C++, plus whatever the JSON string concatenation produces internally.

**New path allocation profile:**

- `DeltaCreateTableSchema` object: stack-local; `captured_error` is default-initialized
  (`ErrorData()` is empty, no heap).
- `top_level_ids` vector in `VisitImpl`: one `vector<uintptr_t>` with `reserve(N)`.
- Per column: `KernelUtils::ToDeltaString(name)` produces a `KernelStringSlice`
  borrowing the `ColumnList`'s own `string` — zero copy. The
  `ffi::visit_field_*` call crosses the FFI and performs a Rust-side allocation into
  `ReferenceSet<StructField>`.
- The final `visit_field_struct` for the root struct crosses the FFI once more.

Total visible-C++ allocations: 1 `vector<uintptr_t>`. Per-column cost moves from
C++ string construction (in-process, cache-hot) to N Rust-side allocations (cross-ABI,
likely cache-cold on first CTAS).

**Magnitude estimate:**

At N=10 columns (typical):
- Old: ~2 small heap allocs + 10 string-append micro-ops (~50 ns total on L1-warm cache).
- New: 1 vector alloc + 11 FFI hops with Rust allocator calls. An FFI hop at this
  boundary costs roughly 10–50 ns per call on modern hardware (no syscall, no lock —
  it is a pure C function call + Rust allocator). Total: ~110–550 ns.

At N=100 columns:
- Old: ~202 small heap allocs (N * 2 from recursive LogicalTypeToDeltaType for complex
  types) + string concatenation: ~200–500 ns.
- New: 101 FFI hops + 101 Rust allocs: ~1–5 µs.

**Conclusion:** The new path is measurably slower for schema building in isolation, but
CTAS is a once-per-table operation. At 100 columns the difference is ≤5 µs — dominated
by the directory probe, `builder_build` (Tokio runtime init), and parquet write which
take milliseconds. This difference does not appear in any benchmark and is not worth
optimizing. Flag only if profiler shows `visit_field_*` in a hot path (it cannot be;
CTAS is never in a tight loop).

**Verdict: No action. Magnitude is irrelevant relative to surrounding I/O.**

---

## [PERF-2] Engine construction in `BuildEngine` — duplication analysis (NO REGRESSION)

**Bottleneck candidate:** `BuildEngine` (added to `delta_multi_file_list.cpp:527–538`)
calls `ToDeltaPath` + `CreateBuilder` + `builder_build`. The normal read path calls
`CreateBuilder` from inside `DeltaMultiFileList`'s constructor (line 713–714). The
question is whether there is hidden duplication or extra overhead per CTAS.

**Analysis:**

`BuildEngine` is a thin wrapper:
```
delta_path = ToDeltaPath(path)          // string normalization, no I/O
builder = CreateBuilder(context, delta_path)  // calls ffi::get_engine_builder + secret lookup
engine  = ffi::builder_build(builder)  // builds the Tokio runtime + object-store client
```

`CreateBuilder` is shared code — not duplicated. The same function body is called from
both the read path and the CTAS path. There is no config rebuild, no mutex, no second
secret lookup: `CreateBuilder` holds no state between calls.

`builder_build` initialises the Tokio multi-threaded runtime. The comment at line 82
says `set_builder_with_multithreaded_executor` is "required for checkpoint support".
This is a one-time cost per CTAS, not per row or per commit. The Tokio runtime is cheap
to initialize for local-FS paths (no network stack); for S3 paths it spins up a thread
pool, adding ~1–10 ms.

**Hidden mutex / config-rebuild overhead:** none visible. `SecretManager::LookupSecret`
acquires a read lock on the secret manager, which is correct and shared with the read
path. No new contention is introduced.

**Verdict: No regression. `BuildEngine` correctly reuses `CreateBuilder` without
duplication. The one-time Tokio init cost is identical to the read path.**

---

## [PERF-3] Allocations on the CTAS path — new vs. old footprint (MEDIUM)

**Bottleneck:** `DeltaTransaction` now carries three additional fields when
`mode == CREATING_TABLE`:

| Field | Type | Size | Heap? |
|---|---|---|---|
| `kernel_create_txn` | `KernelExclusiveCreateTransaction` (RAII ptr) | 8 bytes | Yes: the Rust object is on the Rust heap |
| `ctas_extern_engine` | `KernelExternEngine` (RAII ptr) | 8 bytes | Yes: the Tokio runtime + credentials |
| `ctas_table_path` | `string` | 24 bytes inline, heap if path > 15 chars | Yes for any real path |
| `ctas_partition_columns` | `vector<string>` | 24 bytes + heap per column | Yes for partitioned tables |

**Compared to the old path:**

The old path did not hold the engine in `DeltaTransaction` — it wrote the
`_delta_log/00000000000000000000.json` synchronously inside the now-deleted CTAS
arm and then discarded all transient state. The engine object now lives on the
`DeltaTransaction` until `Commit()` or destructor, which is typically for the
duration of the parquet write (~milliseconds to seconds). This is not a leak; RAII
handles it correctly.

**Hot-path impact (INSERT path):** On REGULAR mode transactions (the common INSERT
path), the three new fields are zero-initialized at `DeltaTransaction` construction:
- `kernel_create_txn.get() == nullptr` — one pointer field, no heap.
- `ctas_extern_engine.get() == nullptr` — one pointer field, no heap.
- `ctas_table_path` — empty `string`, no heap (SSO).
- `ctas_partition_columns` — empty `vector`, no heap.

The `DeltaTransactionMode mode` field is 1 byte (declared `uint8_t`). The enum check
in `Commit()` (`mode == DeltaTransactionMode::CREATING_TABLE`) is a single integer
compare — branch predictor will predict REGULAR on INSERT paths with high accuracy
since it is the dominant case. No misprediction concern in practice.

**CTAS path:** `ctas_table_path` stores the Delta path. For local FS paths like
`/tmp/foo/bar/`, this fits in SSO (≤15 chars on most STL implementations for the
path segment after normalization). For S3 paths (`s3://bucket/prefix/table/`), the
string will heap-allocate. This is one allocation per CTAS — negligible.

`ctas_partition_columns` heap-allocates once for the `vector` backing, then once per
partition column name. For tables with 0–5 partition columns (the typical case), this
is ≤6 small allocations. The old path allocated the same partition column name strings
inside `delta_ctas.cpp::BuildCommitJson` to serialize them as JSON; the new path just
stores them in the vector instead of serializing them immediately.

**One non-obvious allocation: the CTAS `WriteMetaData` constructor.**

`WriteMetaData` (the CTAS overload at `delta_transaction.cpp:266–290`) constructs a
`DataChunk` via `make_uniq<DataChunk>()` and calls `buffer->Initialize(...)` for the
metadata Arrow batch. This occurs once per CTAS (in `AppendForNewTable`). The old path
did not use Arrow for the schema commit — it wrote raw JSON to disk. The new path
marshals file metadata through Arrow to hand to `create_table_add_files`. This is the
**largest new allocation** on the CTAS path: an Arrow `DataChunk` with N_files rows
(typically 1 for small CTAS). The allocation is correct and unavoidable given the FFI
contract.

**Verdict (MEDIUM):** The new path introduces one `make_uniq<DataChunk>()` per CTAS
plus the kernel engine handle — both unavoidable given the FFI contract. This is not
a regression on the INSERT path. The total added allocation per CTAS (all modes) is
less than the old path's JSON `reserve` + file I/O cost. No action required; document
here for completeness.

---

## [PERF-4] `DeltaTransactionMode` enum overhead on INSERT hot path (NO ISSUE)

**Bottleneck candidate:** Reviewer M3 flagged the `DeltaTransactionMode` enum as
redundant (equivalent to `kernel_create_txn.get() != nullptr`). From a performance
perspective the question is whether the enum check introduces branch misprediction on
the INSERT path.

**Analysis:**

The enum is checked in exactly three places:
1. `Commit()` line 444: `if (mode == DeltaTransactionMode::CREATING_TABLE)`
2. `InitializeForNewTable()` line 639: `D_ASSERT(mode == ...)` (debug-only, compiled
   away in release).
3. `AppendForNewTable()` line 699: `D_ASSERT(mode == ...)` (debug-only).

The `Commit()` check runs once per transaction — not in the per-row Sink path. It is
not a hot branch. The branch predictor will correctly predict REGULAR for all INSERT
transactions and CREATING_TABLE for all CTAS transactions, since transactions do not
switch modes. There is zero misprediction cost in practice.

The `D_ASSERT` checks in debug builds catch programmer error; in release builds
(`-O3`) they are elided. No runtime overhead.

**Verdict: No performance issue. The enum vs. null-pointer-check distinction is
a correctness/style question (M3 in the code review), not a performance question.**

---

## [PERF-5] TIMESTAMP_NS rejection — short-circuit verification (NO ISSUE)

**Bottleneck candidate:** H1 fix added a `BinderException` for `TIMESTAMP_NS` in
`DeltaCreateTableSchema::VisitField`. The question is whether this short-circuits
before any expensive work.

**Code path for TIMESTAMP_NS rejection:**

1. `DeltaCatalog::PlanCreateTableAs` — plan time, no kernel calls yet.
2. `DeltaInsert::GetGlobalSinkState` — sink init time:
   a. Creates `_delta_log` directory (one `DirectoryExists` + possibly `CreateDirectory`
      syscall — cheap, ~1 µs).
   b. Calls `DeltaTransaction::InitializeForNewTable`:
      - `BuildEngine` — **this is expensive**: `ffi::get_engine_builder` +
        `ffi::builder_build` (Tokio runtime init, ~1–50 ms for network paths).
      - `DeltaCreateTableSchema schema_visitor(create_info.columns)` — stack-local.
      - `ffi::get_create_table_builder(...)` — invokes `DispatchVisit`, which calls
        `VisitField` per column. When a `TIMESTAMP_NS` column is encountered,
        `VisitField` throws `BinderException`, `DispatchVisit` catches it, stores it in
        `captured_error`, returns sentinel `0`. The kernel sees an `Err` result from
        `get_create_table_builder`. The caller checks `schema_visitor.HasError()` and
        rethrows the `BinderException`.
      - At this point the engine (`ctas_extern_engine`) has already been constructed
        and is destroyed by RAII (`KernelExternEngine` destructor).

**Finding:** The `TIMESTAMP_NS` rejection does NOT short-circuit before `BuildEngine`.
The engine (Tokio runtime + optional S3/Azure credentials) is always constructed before
the schema visitor runs. For a table with only `TIMESTAMP_NS` columns on an S3-backed
catalog, the user waits ~10–50 ms for the engine to spin up before receiving the
`BinderException`.

**Hypothesis and proposed change:** Move the type validation earlier — before
`BuildEngine` — in `InitializeForNewTable`. Since `DeltaCreateTableSchema` does not
need an engine handle to validate types (the visitor only needs the `ColumnList`), we
can run validation in a dry-run phase with no kernel calls:

```cpp
void DeltaTransaction::InitializeForNewTable(ClientContext &context,
                                             const string &table_path,
                                             BoundCreateTableInfo &info) {
    // ... access_mode and parent_commit guards unchanged ...

    auto &create_info = info.Base();

    // Fast type validation before any expensive kernel or file-system call.
    // DeltaCreateTableSchema::ValidateTypes walks the ColumnList and throws
    // BinderException for any column type that has no Delta representation.
    // This avoids building the Tokio runtime for an invalid schema.
    DeltaCreateTableSchema::ValidateTypes(create_info.columns);  // NEW

    // Engine construction (expensive for cloud paths: Tokio runtime + creds).
    ctas_extern_engine = DeltaMultiFileList::BuildEngine(context, table_path);
    // ... rest unchanged ...
}
```

`ValidateTypes` is a static method that runs `VisitField`'s switch (without FFI calls)
and throws on unsupported types:

```cpp
// In DeltaCreateTableSchema:
static void ValidateTypes(const ColumnList &columns) {
    for (const auto &col : columns.Logical()) {
        ValidateType(col.Name(), col.Type());  // throws BinderException on TIMESTAMP_NS etc.
    }
}
```

**Expected improvement:** For TIMESTAMP_NS rejection (and any other unsupported type),
eliminates the `BuildEngine` call (1–50 ms on cloud paths, ~1 ms on local FS).

**Correctness risk:** Zero. Type validation is purely deterministic from the `ColumnList`;
no kernel state is involved. The existing `VisitField` path validates the same types in
the same order — moving the check earlier cannot change which types are accepted or
rejected.

**DuckDB standards check:** The new `static void ValidateTypes(const ColumnList &)` is
a pure utility that belongs alongside `DeltaCreateTableSchema`. No `using namespace std`,
no raw pointers, `ColumnList` is passed by const reference. `idx_t` not needed (loop
uses range-for). Consistent with `DeltaSchemaJson::ValidateColumnTypes` from the old
path, which did the same thing.

**Benchmark:** CTAS-with-TIMESTAMP_NS on an S3 path would show the savings. Locally,
the savings is ~1 ms (one `ffi::get_engine_builder` + `builder_build` call skipped).
The existing `make test_debug` plus adding a TIMESTAMP_NS-rejection test that runs
against a minio_local fixture would demonstrate the improvement, but this is a quality-
of-life change, not a throughput change for valid CTAS.

**Magnitude:** Medium. On local FS: ~0.5–2 ms saved per invalid CTAS. On cloud: ~10–50
ms saved. Relevant only for the error path.

---

## [PERF-6] `static const string` locals in `VisitField` — thread safety (ANALYSIS ONLY)

**Observation:** `VisitField` uses function-local `static const string` for placeholder
names passed to the kernel in array/map/list branches:

```cpp
static const string array_elem_name = "";
static const string map_key_name = "";
static const string map_val_name = "";
```

And `VisitImpl` uses:
```cpp
static const string root_name = "";
```

**Performance implication:** C++11 guarantees that static-local initialization is
thread-safe (the first call initialises, subsequent calls are no-ops with a low-overhead
guard check). Since these are `const string ""` (empty, SSO, no heap allocation), the
initialization is trivially cheap. The guard check on subsequent calls is a branch on a
flag byte — dominated by the FFI call that follows it.

**Verdict: No issue.** These could be replaced with `const char * const` literals
(`KernelUtils::ToDeltaString` accepts `string` by const-ref, so `""` won't work
directly, but a local `string` with an empty literal would be equally efficient without
the static guard). However, the current pattern is correct and the overhead is
immeasurable. No change needed.

---

## Findings Summary Table

| ID | Area | Magnitude | Action |
|---|---|---|---|
| PERF-1 | Visitor FFI overhead vs. JSON string build | <5 µs per CTAS, irrelevant | None |
| PERF-2 | `BuildEngine` duplication / mutex | Zero regression | None |
| PERF-3 | New allocations on CTAS path | Correct, unavoidable | None |
| PERF-4 | `DeltaTransactionMode` enum on INSERT hot path | Zero overhead in release | None |
| PERF-5 | TIMESTAMP_NS rejection after `BuildEngine` | 1–50 ms on error path (cloud) | Medium: add `ValidateTypes` pre-check |
| PERF-6 | `static const string` in `VisitField` | Immeasurable | None |

---

**Verdict: OPTIMIZED**

The implementation is performance-sound for all valid CTAS operations. One medium
opportunity (PERF-5) exists on the error path — type-rejected CTAS on cloud paths
wastes 10–50 ms building a Tokio runtime before the `BinderException` is thrown. This
is a quality-of-life fix for an error path, not a throughput regression. It warrants
a follow-up one-liner rather than a blocking change.
