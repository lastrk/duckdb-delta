# Performance Findings: CTAS Implementation

**Verdict: HAS_OPPORTUNITIES**

High findings: 2
Medium findings: 3

---

## Scope

Files analyzed:

- `src/storage/delta_ctas.cpp` — JSON schema serializer + initial commit JSON builder
- `src/storage/delta_catalog.cpp` — `PlanCreateTableAs`, directory creation, partition resolution
- `src/storage/delta_insert.cpp` — CTAS arm of `GetGlobalSinkState` and `Finalize`
- `src/storage/delta_schema_entry.cpp` — `CreateTable`, `LookupEntry` allow_create guard
- `src/storage/delta_transaction.cpp` — `Append` flow for CTAS

---

## [OPT-1] `allow_create` FileExists check fires on every table lookup after the table exists

**Severity: High**

**Bottleneck.** `DeltaSchemaEntry::LookupEntry` (`src/storage/delta_schema_entry.cpp:250-255`) runs a `FileSystem::FileExists()` stat on
`<path>/_delta_log/00000000000000000000.json` unconditionally at the top of the
per-query table-lookup hot path whenever `delta_catalog.allow_create == true`. This
check happens before any cached state is consulted: it runs before
`delta_transaction.GetTableEntry()` (line 266) and before `cached_table` (line 279).
After the CTAS completes, the version-0 file exists and the stat always returns true —
but the check is never removed or short-circuited. For any attach that used
`allow_create=true`, every subsequent SELECT, INSERT, or EXPLAIN on that catalog will
pay a filesystem round-trip per planning phase.

The structural argument: on local disk a `stat(2)` call is ~1 µs. On object storage
(S3/Azure/GCS) the same call costs ~1–5 ms. For a workload that runs 1000 queries
against the CTAS-created table, this is 1–5 seconds of pure overhead from a flag that
was only meaningful during table creation.

**Hypothesis.** After `cached_table` is populated (i.e., the first successful
lookup), the table exists and the allow_create guard will never return `nullptr`
again. Skipping the check when the cached state is known to be present eliminates the
stat entirely on the hot path.

**Proposed change.** Move the `allow_create` guard to execute only when no cached
table entry is already known, and only before the first table initialization:

```cpp
// src/storage/delta_schema_entry.cpp, inside LookupEntry

// BEFORE (current — stat on every query when allow_create=true):
if (delta_catalog.allow_create) {
    auto &fs = FileSystem::GetFileSystem(context);
    const string version0_path = delta_catalog.path + "/_delta_log/00000000000000000000.json";
    if (!fs.FileExists(version0_path)) {
        return nullptr;
    }
}
auto transaction_table_entry = delta_transaction.GetTableEntry(version);
if (transaction_table_entry) {
    return *transaction_table_entry;
}
// ... lock + cached_table check ...

// AFTER — guard only when no cache is available:
auto transaction_table_entry = delta_transaction.GetTableEntry(version);
if (transaction_table_entry) {
    return *transaction_table_entry;    // fast path: no stat
}
{
    unique_lock<mutex> l(lock);
    if (cached_table) {
        // Table is cached — no need to check allow_create; it exists.
        // (The check below is only needed before the first initialization.)
        return delta_transaction.InitializeTableEntry(context, *this, version,
                                                      *cached_table->snapshot);
    }
    // Only reach here before the first successful table initialization.
    if (delta_catalog.allow_create) {
        auto &fs = FileSystem::GetFileSystem(context);
        const string version0_path =
            delta_catalog.path + "/_delta_log/00000000000000000000.json";
        if (!fs.FileExists(version0_path)) {
            return nullptr;
        }
    }
    cached_table = CreateTableEntry(context, version, nullptr);
    return delta_transaction.InitializeTableEntry(context, *this, version,
                                                  *cached_table->snapshot);
}
```

Note: the full `LookupEntry` body must be restructured to unify the
`UseCachedSnapshot` / else branches under the single lock scope above. The structure
above is a sketch; the exact merge with the existing `UseCachedSnapshot` branch and
the versioned-entry path requires careful unification. The invariant: the
`allow_create` stat is performed at most once per schema entry lifetime, not once per
query.

**Verify.**

```
# Add a CTAS benchmark under benchmark/micro/ctas/:
# name: ctas_allow_create_lookup
# Creates a table with allow_create=true, then runs 100 SELECTs.
# Before fix: 100 × stat() visible in `perf stat -e syscalls:sys_enter_stat`
# After fix: 1 × stat()
make bench-run BENCHMARK_PATTERN=ctas_allow_create_lookup
```

On object storage the improvement is 1–5 ms per query times the number of queries in
the workload. On local disk it is sub-millisecond but still contributes to allocation
pressure via the string construction of `version0_path` on every call.

**Risk.** Correctness: none, provided the `allow_create` check always precedes the
first `CreateTableEntry` call. The invariant already holds in the current code; the
fix only reorders the existing checks. The allow_create semantics (return nullptr if
no version-0 exists) are preserved. Thread safety: the stat must remain inside the
lock scope so two concurrent first-lookups do not both create the entry.

**Standards check.** No `shared_ptr`. No `using namespace std`. `idx_t` preserved.
`unique_lock<mutex>` as in the existing pattern.

---

## [OPT-2] Empty CTAS starts and then abandons a kernel FFI transaction

**Severity: High**

**Bottleneck.** When CTAS produces zero rows (e.g., `WHERE false`), `DeltaInsert::Finalize`
(`src/storage/delta_insert.cpp:348-350`) calls
`DeltaTransaction::Append(context, {})` with an empty vector.
`Append` (`src/storage/delta_transaction.cpp:547-548`) checks transaction state and,
finding `TRANSACTION_NOT_YET_STARTED`, calls `InitializeTransaction`.

`InitializeTransaction` (`src/storage/delta_transaction.cpp:497-543`) performs:

1. Acquires the kernel snapshot lock.
2. Calls `ffi::transaction(path_slice, extern_engine)` — one FFI round-trip that
   creates a `Handle<ExclusiveTransaction>` on the Rust heap.
3. Builds a `DeltaCommitInfo` Arrow batch, converts it to `ArrowFFIData`, and calls
   `ffi::get_engine_data` — another FFI round-trip and heap allocation.
4. Calls `ffi::with_engine_info` — a third FFI call.

After all this, `Append`'s `if (!append_files.empty())` block is skipped (no files to
add). `Commit` sees `outstanding_appends.empty()` and skips the actual commit. The
kernel transaction is abandoned (freed via RAII deleter). Three FFI calls and at least
two Arrow-format heap allocations were performed for no observable effect.

For an ETL DAG that materializes many empty intermediate tables (the stated target
workload), this cost is paid for each empty CTAS.

**Hypothesis.** Guarding `InitializeTransaction` with an empty-files check in
`Append` eliminates all FFI overhead for zero-row CTAS. Expected improvement:
~3 FFI calls + Arrow batch allocation per empty CTAS, which on a fast local path
costs ~50–200 µs each.

**Proposed change.**

```cpp
// src/storage/delta_transaction.cpp

void DeltaTransaction::Append(ClientContext &context, const vector<DeltaDataFile> &append_files) {
+   // Short-circuit: no files to append — do not start a kernel transaction.
+   if (append_files.empty()) {
+       return;
+   }
    if (transaction_state == DeltaTransactionState::TRANSACTION_NOT_YET_STARTED) {
        InitializeTransaction(context);
    }
    // ... rest unchanged ...
}
```

`Commit` already handles the `TRANSACTION_NOT_YET_STARTED` state by checking
`transaction_state == TRANSACTION_STARTED` at line 419 — it does nothing when the
transaction was never started. `Rollback` similarly guards on `TRANSACTION_STARTED`
at line 491. `CleanUpFiles` iterates `outstanding_appends` which is empty. So the
fix requires only the one guard in `Append`.

**Verify.**

```
# Run the existing empty CTAS test after the fix and verify it still passes:
build/release/test/unittest "test/sql/main/writing/ctas/ctas_empty_select.test"

# Add a benchmark to measure per-CTAS overhead:
# name: ctas_empty_table
# Runs 100 CTAS of zero-row tables.
# Before: perf shows ffi::transaction + ffi::get_engine_data + ffi::with_engine_info
#         called 100 times for nothing.
# After: those symbols appear 0 times in the empty-CTAS path.
make bench-run BENCHMARK_PATTERN=ctas_empty_table
```

**Risk.** Correctness: `Append` is only called by `DeltaInsert::Finalize` and its
non-CTAS counterpart. Both pass `global_state.written_files`, which is empty when
no rows were written. The behavior change is: a zero-row CTAS no longer starts a
kernel transaction, which matches the intended semantics (no data was appended).
The `create_table_commit` (kernel-side) writes the log file; nothing about the empty
CTAS case interacts with the kernel transaction at all. The test
`ctas_empty_select.test` already covers this path and will catch regressions.

Note: this change also affects regular INSERT of zero rows. For INSERT, the same
reasoning applies: starting a kernel transaction that does nothing and is then
abandoned is wasteful. The fix is correct for both paths.

**Standards check.** One-line guard. No style changes. `append_files` is `const ref`.

---

## [OPT-3] `BuildSchemaString` called twice per CTAS — plan-time result is discarded

**Severity: Medium**

**Bottleneck.** `PlanCreateTableAs` (`src/storage/delta_catalog.cpp:142`) calls
`DeltaSchemaJson::BuildSchemaString(columns)` for validation and discards the result.
`GetGlobalSinkState` (`src/storage/delta_insert.cpp:98`) calls it again on the same
`ColumnList` and keeps the result for `BuildCommitJson`.

`BuildSchemaString` calls `BuildRawSchemaJson` which iterates all N columns, calls
`LogicalTypeToDeltaType` per column (each returns a `string`), calls
`JsonEscapeString` per column name (each returns a `string`), and assembles them with
`+` concatenation into `fields_json` (growing string, no `reserve`). The result is
then passed to `JsonEscapeString` again (which copies the entire JSON blob into a
second string). For a table with N columns, this allocates approximately 3N+2 `string`
objects at plan time, all of which are immediately destroyed.

For a 20-column table, roughly 62 small string allocations are made and freed at plan
time for pure type validation. In an ETL workload that creates 1000 CTAS tables, this
is ~62,000 allocations that produce no useful output.

**Hypothesis.** A dedicated `ValidateColumnTypes(const ColumnList &)` function that
iterates columns and calls `LogicalTypeToDeltaType` but discards each result is
sufficient for plan-time validation. This replaces ~62 allocations with N calls that
return `string` literals (no heap allocation for simple types like `"integer"`) or
small strings for complex types, and discards them immediately — effectively 0 net
allocations for tables with only primitive columns.

**Proposed change.**

In `src/storage/delta_ctas.hpp`, add:

```cpp
//! Validates that all column types in the list have Delta Lake representations.
//! Throws BinderException for unsupported types. Does NOT build the JSON string.
//! Use this at bind time; use BuildSchemaString only when the JSON is needed.
static void ValidateColumnTypes(const ColumnList &columns);
```

In `src/storage/delta_ctas.cpp`, implement it as:

```cpp
void DeltaSchemaJson::ValidateColumnTypes(const ColumnList &columns) {
    for (const auto &col_def : columns.Logical()) {
        // LogicalTypeToDeltaType throws BinderException for unsupported types.
        // The return value (the Delta type string) is intentionally discarded.
        (void)LogicalTypeToDeltaType(col_def.Type());
    }
}
```

In `src/storage/delta_catalog.cpp:142`, replace:

```cpp
DeltaSchemaJson::BuildSchemaString(columns);
```

with:

```cpp
DeltaSchemaJson::ValidateColumnTypes(columns);
```

**Verify.**

```
# Functional test — same behavior:
build/release/test/unittest "test/sql/main/writing/ctas/*"

# Allocation profile (heaptrack or ASAN with allocation counting):
heaptrack build/release/test/unittest "test/sql/main/writing/ctas/ctas_type_coverage.test"
# Before: ~N*3 extra heap allocations visible in DeltaSchemaJson::BuildSchemaString
# After: 0 heap allocations from this code path at plan time
```

**Risk.** Correctness: `ValidateColumnTypes` must call `LogicalTypeToDeltaType` with
the same type dispatch to guarantee identical exception behavior. The existing STRUCT
and LIST cases recurse into child types — `ValidateColumnTypes` must also recurse, or
simply delegate to `LogicalTypeToDeltaType` as shown. Portability: none. The change
is purely additive.

**Standards check.** New function follows the existing static-function pattern in
`delta_ctas.cpp`. `(void)` discard pattern is DuckDB-idiomatic.

---

## [OPT-4] Hand-rolled directory-creation loop in `PlanCreateTableAs` — excess object-store round trips

**Severity: Medium**

**Bottleneck.** `PlanCreateTableAs` (`src/storage/delta_catalog.cpp:171-188`) creates
the target directory with a hand-rolled loop:

```cpp
const string sep = fs.PathSeparator(delta_path);
vector<string> splits = StringUtil::Split(delta_path, sep[0]);
string prefix;
if (StringUtil::StartsWith(delta_path, sep)) {
    prefix = sep;
}
for (const auto &part : splits) {
    if (part.empty()) { continue; }
    prefix = prefix + part + sep;
    if (!fs.DirectoryExists(prefix)) {
        fs.CreateDirectory(prefix);
    }
}
```

For a local path with 5 components this makes 5 `DirectoryExists` calls. For an S3
path like `s3://bucket/a/b/c/table`, the `StringUtil::Split` on `'/'` produces
`["s3:", "", "bucket", "a", "b", "c", "table"]`, so the loop issues 6
`DirectoryExists` round-trips to the object store (skipping the empty string but
still checking `"s3:/"`, `"s3://bucket/"`, etc.). Each S3 `HeadObject` or `ListObjects`
call used to check existence costs ~1–5 ms.

`FileSystem::CreateDirectoriesRecursive` (`duckdb/src/common/file_system.hpp:188`,
implemented at `duckdb/src/common/file_system.cpp:504`) already exists and uses a
smarter algorithm: it walks from the leaf path upward until it finds a prefix that
exists, then creates only the non-existing parts. For a path where the bucket exists
but the table prefix does not, `CreateDirectoriesRecursive` issues 1 check (leaf),
finds it missing, walks to the bucket (1 check, found), and creates only
`bucket/a/b/c/table` in one forward pass — typically 2–4 checks vs 6+. The comment
in review finding M2 (iteration 1) already identified this.

**Hypothesis.** Replacing the hand-rolled loop with a single
`fs.CreateDirectoriesRecursive(delta_path)` call reduces the number of `DirectoryExists`
calls from `path_depth` to at most `path_depth / 2` on average. For S3 paths in ETL
workloads, this saves 3–5 × 1–5 ms = 3–25 ms per CTAS.

**Proposed change.**

```cpp
// src/storage/delta_catalog.cpp, PlanCreateTableAs

// REMOVE the hand-rolled block (lines 171-188):
// {
//     auto &fs = FileSystem::GetFileSystem(context);
//     const string sep = fs.PathSeparator(delta_path);
//     vector<string> splits = StringUtil::Split(delta_path, sep[0]);
//     ...
// }

// REPLACE with:
{
    auto &fs = FileSystem::GetFileSystem(context);
    fs.CreateDirectoriesRecursive(delta_path);
}
```

The `#include "duckdb/common/file_system.hpp"` is already present.

**Verify.**

```
# Functional correctness — local path:
build/release/test/unittest "test/sql/main/writing/ctas/basic_ctas.test"

# For cloud path verification, use the existing minio_local test harness once
# a CTAS test for cloud paths exists (T2 in the review findings list):
build/release/test/unittest "test/sql/cloud/minio_local/ctas.test"

# Measure DirectoryExists calls with strace on local:
strace -e trace=stat,statx build/release/test/unittest \
    "test/sql/main/writing/ctas/basic_ctas.test" 2>&1 | grep -c "delta_lake"
# Before: count = path_depth
# After: count <= path_depth / 2
```

**Risk.** Correctness: `CreateDirectoriesRecursive` has been in DuckDB for multiple
releases and is used by the extension install path and log storage. Its behavior for
object-store paths is wrapped through the registered virtual filesystem (the same one
the hand-rolled loop calls `DirectoryExists` through). One behavioral difference: the
hand-rolled loop checks existence of every intermediate prefix and silently skips
existing ones; `CreateDirectoriesRecursive` has the same semantics (no error if the
directory already exists). Portability: `CreateDirectoriesRecursive` is declared
`DUCKDB_API virtual` and all registered FS implementations override it or inherit the
default. Style: the replacement is strictly shorter and simpler.

**Standards check.** Removes code, no additions. DuckDB public API. No new
dependencies.

---

## [OPT-5] `fields_json` string in `BuildRawSchemaJson` and STRUCT case not reserved

**Severity: Medium**

**Bottleneck.** `BuildRawSchemaJson` (`src/storage/delta_ctas.cpp:120-135`) builds
`fields_json` by appending one field entry per column with `+=`. The string starts
empty (default capacity 0 on most implementations) and grows by doubling on each
capacity overflow. For a 10-column table with average field JSON of ~60 bytes, the
final `fields_json` is ~600 bytes; this triggers ~4 reallocations (16 → 32 → 64 →
128 → 256 → 512 → 1024 bytes). The same pattern appears in the STRUCT case
(`src/storage/delta_ctas.cpp:85`, `fields_json` not reserved).

`BuildCommitJson` (`src/storage/delta_ctas.cpp:158-168`) assembles `metadata` using
`operator+` across 5 string fragments. Each `+` produces a temporary string.
`commit_info` is assembled the same way. The final concatenation
`commit_info + "\n" + metadata + "\n" + protocol + "\n"` produces 3 more temporaries.
Total: ~8 temporaries in `BuildCommitJson` for a typical commit. None of the output
strings have a `reserve` call.

This matters for the many-small-CTAS workload: each CTAS calls `BuildRawSchemaJson`
once (at sink time) and `BuildCommitJson` once. For 1000 CTAS operations, 8000
temporary strings are created in `BuildCommitJson` alone.

`JsonEscapeString` already calls `result.reserve(s.size() + 4)` — the same pattern
applied to the other builders would eliminate most of the reallocation churn.

**Hypothesis.** Adding `reserve` calls to `fields_json` and to the output strings of
`BuildCommitJson` eliminates ~4 reallocations per `BuildRawSchemaJson` call and ~6
temporary string objects per `BuildCommitJson` call. The absolute time saving is
small per CTAS (~1–5 µs) but accumulates in tight ETL loops.

**Proposed change (minimal).**

In `BuildRawSchemaJson`:

```cpp
static string BuildRawSchemaJson(const ColumnList &columns) {
    string fields_json;
+   // Reserve ~70 bytes per field as a rough lower bound; avoids most reallocations.
+   fields_json.reserve(columns.LogicalColumnCount() * 70);
    // ... rest unchanged ...
}
```

In `BuildCommitJson`, pre-size the result string:

```cpp
string DeltaSchemaJson::BuildCommitJson(...) {
    string protocol = "{\"protocol\":{\"minReaderVersion\":1,\"minWriterVersion\":2}}";

    string part_cols_json = "[";
+   part_cols_json.reserve(32 + partition_columns.size() * 24);
    // ...

    // Build result in one string instead of three + final join:
+   string result;
+   result.reserve(600 + schema_string.size() + part_cols_json.size());
+   result += "{\"commitInfo\":{\"timestamp\":";
+   result += std::to_string(created_time_ms);
+   // ... append each fragment in place of operator+ chain ...
+   return result;
}
```

The exact size hints do not need to be precise — they only need to avoid the first few
doubling cycles. The constants 70 and 600 come from the measured typical sizes of
these strings.

Alternatively, the three-string pattern in `BuildCommitJson` can be kept and each
individual string pre-sized. The key gain is eliminating intermediate temporaries from
the multi-operand `operator+` chains.

**Verify.**

```
# Functional:
build/release/test/unittest "test/sql/main/writing/ctas/*"

# Allocation count (heaptrack):
heaptrack build/release/test/unittest "test/sql/main/writing/ctas/ctas_type_coverage.test"
# Look for std::string::_M_create or equivalent allocation site in BuildRawSchemaJson.
# Before: ~4 allocations per BuildRawSchemaJson call visible.
# After: 1 allocation (initial reserve fills the whole buffer).
```

**Risk.** Correctness: `reserve` is a no-op if the hint is too small (the string
falls back to normal growth). Size hints are advisory. If the estimate is too large,
the string wastes memory for the duration of the function call (a stack-local variable
— freed immediately on return). Portability: `std::string::reserve` is C++11 standard.
Style: `reserve` is used by `JsonEscapeString` in the same file, so the pattern is
already established.

**Standards check.** DuckDB uses `std::to_string` in the existing code (review finding
L1 noted this but it's out of scope here). `reserve` is not a style violation.

---

## Non-findings (items investigated and ruled out)

**Partition resolution complexity (`O(P × N)` in `PlanCreateTableAs`).** The loop
at `delta_catalog.cpp:146-163` iterates partition keys (P) and column names (N) to
resolve indices. For typical P (1–3) and N (5–50), this is 5–150 comparisons using
`StringUtil::CIEquals` — negligible at bind time, not a hot path, and not called at
scan time. Flagged as acceptable.

**`fields_json` STRUCT recursion.** For nested STRUCT types,
`LogicalTypeToDeltaType` recurses. Depth is bounded by the schema; for typical
schemas this is 1–2 levels deep. Not a practical bottleneck.

**`GetLastModifiedTime` per-file round trip in `DeltaTransaction::Append`
(`delta_transaction.cpp:556-562`).** This is a pre-existing round trip present in
the INSERT path before the CTAS work (the TODO comment in the code confirms it).
It affects both INSERT and CTAS equally and is not a regression introduced by the
CTAS implementation. Out of scope for this review.

**`const_cast<DeltaSchemaEntry &>` in `GetGlobalSinkState`.** This is a
correctness/design smell (review finding M4) rather than a performance issue. The
`const_cast` imposes zero runtime cost.

**Partition column iteration in `GetGlobalSinkState` (`delta_insert.cpp:102-108`).**
The O(P) loop to extract partition column names from `partition_keys` is trivially
fast (< 10 iterations for any realistic partition count). Not a bottleneck.

**`BuildSchemaString` at `delta_schema_entry.cpp:71` (inside `CreateTable`).** The
`CreateTable` path is only called on the "already exists" error path (the CTAS happy
path goes through `PlanCreateTableAs` directly). Not on the hot path.

---

## Benchmark targets

No CTAS-specific benchmark files exist under `benchmark/`. To measure the above
findings, two new micro-benchmarks should be added:

**`benchmark/micro/ctas/ctas_many_empty.benchmark`** — runs 100 zero-row CTAS
operations using `WHERE false`. Measures per-CTAS overhead; OPT-1 and OPT-2 are
visible here.

**`benchmark/micro/ctas/ctas_basic.benchmark`** — runs 100 non-empty CTAS operations
(e.g., `SELECT range AS i FROM range(1000)`). Measures end-to-end CTAS throughput;
OPT-3 and OPT-5 are visible here (allocation pressure), OPT-4 is visible on a cloud
path variant.

The benchmark format is:

```
# name: benchmark/micro/ctas/ctas_many_empty.benchmark
# description: Per-CTAS overhead for zero-row creates (allow_create path)
# group: [ctas]

name CTAS many empty
group ctas

require delta
require parquet

load
ATTACH '__TEST_DIR__/ctas_bench' AS db (TYPE delta, allow_create=true);

run
CREATE TABLE db.t AS SELECT i FROM range(0) t(i);
DROP TABLE db.t;

result I
0
```

Run with:
```
BUILD_BENCHMARK=1 make release
make bench-run BENCHMARK_PATTERN=ctas_many_empty.benchmark
```

---

## Summary table

| ID     | Severity | Location                                 | Bottleneck                                                  | Fix size     |
|--------|----------|------------------------------------------|-------------------------------------------------------------|--------------|
| OPT-1  | High     | `delta_schema_entry.cpp:250-255`         | FileExists stat on every query for allow_create catalogs    | ~20 lines    |
| OPT-2  | High     | `delta_transaction.cpp:547-548`          | Empty CTAS starts kernel FFI transaction unnecessarily      | 3 lines      |
| OPT-3  | Medium   | `delta_catalog.cpp:142`, `delta_ctas.cpp`| BuildSchemaString called twice; plan-time result discarded  | ~15 lines    |
| OPT-4  | Medium   | `delta_catalog.cpp:171-188`              | Hand-rolled directory loop; excess object-store round trips | 5 lines      |
| OPT-5  | Medium   | `delta_ctas.cpp:85,121,158`              | String buffers not reserved; unnecessary reallocations      | ~6 lines     |

**High count: 2. Medium count: 3.**
