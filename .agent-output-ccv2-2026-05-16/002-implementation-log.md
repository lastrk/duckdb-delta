## Bug Fixes (Pre-Existing CTAS Bugs)

### Bug 1+2+3: MAP / STRUCT-with-LIST / MAP-with-STRUCT CTAS fails

**Location**: `src/storage/delta_insert.cpp`, `AddWrittenFiles` function, line ~250 (skip condition).

**Root cause confirmed**: Yes, matches prediction. The skip condition `coltype.id() == LogicalTypeId::VARIANT || coltype.id() == LogicalTypeId::LIST` did not include MAP. When parquet writes stats for a MAP column (or a STRUCT containing a LIST child), the stats entries fall through to `stats.root_type = coltype` and then get collected into `data_file.column_stats`. Later in `WriteMetaData::CreateStatsValue` -> `ParseStatsType` -> `ParseInnerType`, the code tries to navigate the MAP/LIST type tree, either crashing with an `InternalException` (for nested LIST) or attempting to cast string min/max values to MAP type (for MAP), producing "Invalid Input Error: Failed to cast value".

**Fix applied**: Added a `HasNonStatsType(const LogicalType &type)` helper function that returns `true` for:
- `LIST`, `MAP`, `VARIANT` at the top level
- Any `STRUCT` that transitively contains a `LIST` or `MAP` child

Replaced the old two-condition skip (`VARIANT || LIST`) with `HasNonStatsType(coltype)`. This skips stats collection for all three bug cases while keeping stats for purely scalar STRUCT columns.

**Test now asserts success**: Parts 4 (MAP varchar->int), 6 (STRUCT-with-LIST), and 7 (MAP-with-STRUCT) in `test/sql/main/writing/ctas/ctas_complex_types.test` are now `statement ok` with round-trip value checks. The file header comment was updated to remove the "known bugs" caveat.

---

### Bug 4: Multi-partition CTAS fails with InternalException

**Location**: `src/storage/delta_insert.cpp`, `DeltaInsert::Sink`, lines ~283-286.

**Root cause confirmed**: Yes, matches prediction exactly. The code had:
```cpp
if (chunk.size() != 1) {
    throw InternalException("DeltaInsert::Sink expects a single row...");
}
```
`PhysicalCopyToFile` in `partition_output` mode emits one row per distinct partition value (i.e., one row per output file). When CTAS produces two or more distinct partition values, the chunk has `size() > 1`, triggering the exception. The `AddWrittenFiles` function that follows already loops `for (idx_t r = 0; r < chunk.size(); r++)` — it was always capable of handling multiple rows.

**Fix applied**: Removed the `chunk.size() != 1` assertion and replaced it with a comment explaining the correct invariant (PhysicalCopyToFile emits one row per written file; AddWrittenFiles handles any cardinality).

**Test now asserts success**: `test/sql/main/writing/ctas/ctas_partitioned.test` was expanded with:
- Part 2: multi-partition CTAS (2 distinct values → 2 files → chunk.size()==2)
- Part 3: multi-column partition CTAS (partitioned by region, year)
- Part 4: NULL partition value (Delta uses empty string serialization)
- Existing Parts 2+3 renumbered to Parts 5+6

---

### Per-directory test counts after fixes

- `test/sql/main/writing/ctas/*`: 11 tests, 284 assertions — all pass
- `test/sql/main/writing/*`: 15 tests, 384 assertions — all pass
- `test/sql/main/*`: 16 tests, 384 assertions + 1 skipped (httpfs) — all pass
- `test/sql/issues/*`: 2 tests, 23 assertions — all pass
- `test/sql/dat/*`: 4 tests — all skipped (require-env DAT_PATH not set)
- `test/sql/delta_kernel_rs/*`: 5 tests — all skipped (require-env DELTA_KERNEL_TESTS_PATH not set)

---

### Nothing escalated

All 4 bugs were simpler than or exactly as predicted. No deeper issues discovered.

---

## Review Fix Iteration 1

### C1 — Inverted comment in `ctas_ccv2.test`

**File**: `test/sql/main/writing/ctas/ctas_ccv2.test:22-24`
**Change**: Replaced the comment "is NOT called during CTAS (version 0)" with "IS called during CTAS: it receives the staged commit file and promotes it to the published log path. That callback is what creates `_delta_log/00000000000000000000.json`."
**Note**: Also added `require debug` to the test (see H1 rationale below).

### H1 — Test committer gated behind `#ifdef DEBUG`

**Files**: `src/functions/delta_transaction_utils/ccv2_test_committer.cpp`, `src/include/delta_functions.hpp`, `src/delta_functions.cpp`, `test/sql/main/writing/ctas/ctas_ccv2.test`
**Change**: Wrapped the entire `namespace duckdb { ... }` block in `ccv2_test_committer.cpp` with `#ifdef DEBUG / #endif`. Added matching `#ifdef DEBUG` guard around the `GetCcV2TestCommitterFunction()` declaration in `delta_functions.hpp` and around the registration call in `delta_functions.cpp`. Added `require debug` to `ctas_ccv2.test` so it is skipped in release builds where the function is not registered.
**Verification**: `nm` on release extension shows no `GetCcV2TestCommitterFunction` or `CcV2TestCommitter*` symbols. Debug static library (`libdelta_extension.a`) does contain the symbols. Release test run: `ctas_ccv2.test` is skipped with "require debug: 1".

### H2 — `max_catalog_version` ATTACH validation (NOT APPLIED)

**Reason**: The reviewer's proposed validation `if (res->max_catalog_version != DConstants::INVALID_INDEX && !res->parent_commit)` would break the existing re-attach pattern used in `ctas_ccv2.test` (`ATTACH '...' AS t1r (TYPE delta, max_catalog_version 0)` without `parent_commit=true`). This pattern is legitimate per the Delta protocol: `catalogManaged` tables always require `max_catalog_version` when reading, even for read-only attaches without the CCv2 write machinery. Adding the validation would contradict the protocol requirement. The fix was not applied to preserve test correctness.

### H3 — `std::atomic<uint64_t>` → `std::atomic<idx_t>`

**File**: `src/include/storage/delta_catalog.hpp:62`
**Change**: Changed `std::atomic<uint64_t> ccv2_committed_version` to `std::atomic<idx_t> ccv2_committed_version`. Since `idx_t` is `typedef uint64_t idx_t` on 64-bit platforms, no casts were needed at the two call sites (`store` in `delta_transaction.cpp` and `load` in `delta_schema_entry.cpp`); `ffi::committed_transaction_version` returns `uint64_t` which is the same underlying type.

### H4 — Narrowed system-catalog fallback for commit function

**File**: `src/delta_extension.cpp:83-86`
**Change**: The unconditional `SYSTEM_CATALOG` fallback was replaced with a conditional one: fallback only when `res->parent_commit_function_name` is non-empty (i.e., the user explicitly specified a non-default function name). The production path (function name empty → uses `__internal_delta_ccv2_commit_staged` by default) will now fail early if the function is not in the parent catalog, preventing accidental resolution to a same-named function in an unrelated system catalog.

### Test status

- Debug: extension (`libdelta_extension.a`, `delta.duckdb_extension`) builds cleanly, no new warnings. Debug `unittest` binary could not be linked (OOM in linker — pre-existing environment constraint unrelated to these changes).
- Release: all tests pass. `test/sql/main/writing/ctas/*` — 10 tests, 258 assertions pass; `ctas_ccv2.test` skipped (require debug). `test/sql/main/writing/*` — 14 tests, 358 assertions pass; 1 skipped.
- Test committer NOT registered in release: confirmed by `nm` showing no `GetCcV2TestCommitterFunction` or `CcV2TestCommitter*` symbols in the release extension; `ctas_ccv2.test` skipped via `require debug`.

## Review Fix Iteration 2

Updated the `# description:` block header of `/workspace/test/sql/main/writing/ctas/ctas_ccv2.test` (lines 4–5): replaced "the kernel's UCCommitter writes the published commit JSON directly (no callback invoked)" with "The test committer IS called during CTAS: it receives the staged commit file and promotes it to the published log path," eliminating the contradiction with the corrected inline comment added in iteration 1. All 10 tests still pass (1 skipped, require debug).
