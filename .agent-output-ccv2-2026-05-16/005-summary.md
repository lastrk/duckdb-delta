# Feature Summary: CCv2 CTAS wiring + new test fixtures + 4 pre-existing bug fixes

## What Was Built

Three logical clusters in one PR:

1. **CCv2 CTAS wiring** — replaced the `NotImplementedException` short-circuit (left over from the prior CTAS retirement) with a real `parent_commit=true` branch that routes through `ffi::create_table_builder_build_with_committer`. Mirrors the existing INSERT-side CCv2 path; uses a `MutableCommitter` built from `ffi::get_uc_commit_client` + `ffi::get_uc_committer`. The `CommitCallback` non-null assertion on `parent_table_entry` is relaxed for version-0 commits (CTAS): a comment documents this contract change so the real Unity Catalog committer knows the convention.
2. **New test fixtures** — 3 new sqllogic tests under `test/sql/main/writing/ctas/` covering LIST / MAP / nested complex types (`ctas_complex_types.test`), partitioned CTAS (`ctas_partitioned.test`), and CCv2 CTAS itself (`ctas_ccv2.test`). Total 175 new assertions.
3. **Bug fixes uncovered by the new tests** — the user expanded scope to fix four pre-existing CTAS bugs that the test fixtures exposed: MAP CTAS, STRUCT-with-LIST CTAS, MAP-with-STRUCT CTAS (all three shared one root cause in stats-collection skip logic), and multi-partition CTAS (a wrong "expects a single row" assertion in `DeltaInsert::Sink`).

Plus some justified scope creep to make CCv2 CTAS observable in the same session: a new `max_catalog_version` ATTACH option and an `std::atomic<idx_t> ccv2_committed_version` field on `DeltaCatalog` that's populated from `ffi::committed_transaction_version` after CCv2 CTAS commit and consumed by subsequent snapshot reads.

## Architecture Decisions

- **CCv2 CTAS test fixture lives inside the extension as `__internal_delta_test_ccv2_commit_staged`**, gated behind `#ifdef DEBUG`. The real `__internal_delta_ccv2_commit_staged` is provided by the Unity Catalog extension. The test fixture promotes the staged commit file to the published `_delta_log/00000000000000000000.json` path, mimicking what UC would do. The DEBUG guard ensures release builds don't expose this test-only function (verified: `nm` shows no `CcV2TestCommitter` symbols in the release extension binary). A new `parent_commit_function_name` ATTACH option lets the test override the function name to invoke the test fixture; non-DEBUG users use the default name which only resolves in the parent catalog (no system-catalog fallback unless the function name is non-default).
- **`parent_table_entry=nullptr` is the contract for version-0 (CTAS) commits.** The CommitCallback assertion at `delta_transaction.cpp:359` was relaxed: it requires the pointer only when `commit_info.version > 0`. CTAS passes `nullptr` because the parent-catalog table entry doesn't exist yet (the version-0 commit IS the registration). A comment documents this convention.
- **`MAP`, `STRUCT-with-LIST`, and `MAP-with-STRUCT-value` stats-collection bug** shared a single root cause: `AddWrittenFiles` skipped only LIST and VARIANT columns when collecting per-row min/max stats. MAP and STRUCT-with-nested-collection columns fell through, causing a cast failure when the parquet writer's min/max strings were re-typed. Fixed with a new `HasNonStatsType` helper that recursively classifies LIST/MAP/VARIANT/STRUCT-with-LIST-or-MAP as non-stats-collectable.
- **Multi-partition CTAS bug** was a wrong `InternalException("DeltaInsert::Sink expects a single row")` assertion. `PhysicalCopyToFile`'s `partition_output` mode emits one row per partition file; `AddWrittenFiles` already iterated correctly — only the assertion was wrong. Removed.
- **Scope-creep additions** (reviewer-scrutinized per user's request, kept after review):
  - `max_catalog_version` ATTACH option: required for read-only re-attach of catalogManaged tables per the Delta protocol; the reviewer's proposed validation (requires `parent_commit=true`) was rejected because read-only attaches legitimately set this without `parent_commit`.
  - `std::atomic<idx_t> ccv2_committed_version`: stored after CCv2 CTAS so the same session's subsequent read can see the new table. Atomic for thread-safety across sessions sharing a catalog.
  - System-catalog fallback for commit function lookup narrowed to the case where `parent_commit_function_name` is explicitly set (i.e., test override only).

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `src/functions/delta_transaction_utils/ccv2_test_committer.cpp` | Created (DEBUG-only) | Test fixture for `__internal_delta_test_ccv2_commit_staged`; promotes staged commits to published log path |
| `test/sql/main/writing/ctas/ctas_ccv2.test` | Created | 26 assertions; basic CCv2 CTAS + empty CCv2 CTAS + same-session read + re-attach with `max_catalog_version`. `require debug`. |
| `test/sql/main/writing/ctas/ctas_complex_types.test` | Created | 86 assertions; LIST, MAP, LIST-of-STRUCT, STRUCT-with-LIST, MAP-with-STRUCT, empty LIST, empty MAP, BLOB, ENUM, TIMESTAMP_TZ. All round-trip values asserted. |
| `test/sql/main/writing/ctas/ctas_partitioned.test` | Created | 63 assertions; single-value partition, multi-partition (3 rows, 2 files), multi-column partition (region,year), NULL partition, empty CTAS, missing-partition-column error |
| `CMakeLists.txt` | Modified | Add `ccv2_test_committer.cpp` to sources |
| `src/delta_extension.cpp` | Modified | New `max_catalog_version` and `parent_commit_function_name` ATTACH options; narrowed system-catalog fallback for commit function lookup; `InternalException` → `InvalidInputException` for missing function |
| `src/delta_functions.{cpp,hpp}` | Modified | Register `CcV2TestCommitterFunction` (DEBUG-only) alongside idempotency helpers |
| `src/include/storage/delta_catalog.hpp` | Modified | New `std::atomic<idx_t> ccv2_committed_version`, `idx_t max_catalog_version`, `string parent_commit_function_name` |
| `src/include/storage/delta_transaction.hpp` | Modified | New `DeltaCatalog &delta_catalog` ref for post-commit version storage |
| `src/storage/delta_transaction.cpp` | Modified | CCv2 CTAS branch in `InitializeForNewTable` (`get_uc_committer` + `create_table_builder_build_with_committer`); `CommitCallback` version-conditional assertion; post-commit `ccv2_committed_version` store; `io.unitycatalog.tableId` table property in CCv2 CTAS |
| `src/storage/delta_insert.cpp` | Modified | New `HasNonStatsType` recursive helper (bug fix 1+2+3); removed wrong "single row" assertion (bug fix 4); CTAS arm passes `nullptr` parent entry under CCv2 |
| `src/storage/delta_schema_entry.cpp` | Modified | Propagate `max_catalog_version` to `DeltaMultiFileList` (explicit ATTACH > `ccv2_committed_version`) |
| `src/functions/delta_scan/delta_multi_file_list.{cpp,hpp}` | Modified | New `mutable idx_t max_catalog_version` field; `snapshot_builder_set_max_catalog_version` call in `InitializeSnapshot` |

Net: **11 source files modified, 1 created, 3 new test files, ~200 insertions / 30 deletions.**

## sqllogic Tests Added

- `test/sql/main/writing/ctas/ctas_ccv2.test` — 26 assertions; covers basic CCv2 CTAS, empty CCv2 CTAS, same-session reads, re-attach with explicit `max_catalog_version`. Uses test-only `__internal_delta_test_ccv2_commit_staged`. `require debug`.
- `test/sql/main/writing/ctas/ctas_complex_types.test` — 86 assertions; LIST of INTEGER, LIST of INTEGER with NULL, LIST of VARCHAR, MAP varchar→integer, LIST of STRUCT, STRUCT containing LIST, MAP varchar→STRUCT, empty LIST, empty MAP, BLOB, ENUM, TIMESTAMP_TZ. Round-trip values asserted.
- `test/sql/main/writing/ctas/ctas_partitioned.test` — 63 assertions; single-value partition (one file), multi-partition (3 rows producing 2 files), multi-column partition (PARTITIONED BY (region, year)), NULL partition value, empty CTAS with partitions, missing partition column error.

**Final test status (release build, except ctas_ccv2.test which `require debug`):**
| Suite | Result |
|---|---|
| `test/sql/main/writing/ctas/*` | 258 assertions, 11 cases (10 pass + 1 skipped) — ALL PASS |
| `test/sql/main/writing/*` | 358 assertions, 15 cases (14 pass + 1 skipped) — ALL PASS |
| `test/sql/main/*` | 193+ assertions — ALL PASS |
| `test/sql/issues/*` | 23 assertions — ALL PASS |
| `test/sql/dat/*`, `test/sql/delta_kernel_rs/*` | Unchanged from prior pipelines (pass) |

## Review Status

- **Verdict: APPROVED after 2 iterations + 1 trailing trivial fix.**
- Iter 1 (NEEDS_CHANGES: 1 Critical + 4 High):
  - **C1** (inverted test comment in `ctas_ccv2.test`) — fixed inline comment.
  - **H1** (test committer shipped in production without guard) — fixed with `#ifdef DEBUG` around the function definition + declaration + registration; `require debug` added to the test. Verified `nm` shows no `CcV2TestCommitter` symbols in release binary.
  - **H2** (validation that `max_catalog_version` requires `parent_commit=true`) — **NOT APPLIED, with documented justification**. The proposed validation would break the legitimate read-only re-attach pattern that catalogManaged tables require per the Delta protocol. The reviewer accepted this pushback in iter 2.
  - **H3** (`std::atomic<uint64_t>` → `std::atomic<idx_t>`) — fixed.
  - **H4** (system-catalog fallback) — fixed; now only fires when `parent_commit_function_name` is non-empty (test-override path).
- Iter 2 (NEEDS_CHANGES: 1 Critical carryover):
  - **C1 part 2** — the iter-1 inline comment fix didn't update the file-header description, leaving two contradicting comments in the same file. Fixed in iter 2 (file-header now matches inline).
- **Outstanding Medium/Low items** (deliberately deferred — out of orchestrator scope):
  - **M1**: `DeltaCatalog &delta_catalog` back-reference in `DeltaTransaction` adds an implicit lifetime coupling. Single use; could be avoided by passing version through return value of `InitializeForNewTable`.
  - **M2**: `mutable idx_t max_catalog_version` on `DeltaMultiFileList` — write-once-before-first-use contract not enforced by type system. The class already has a `mutable` pattern.
  - **M3**: `unity_table_id` should arguably be required when `parent_commit=true`; today silently falls back to the table path which a real UC server would reject.
  - **M4**: `HasNonStatsType` comment doesn't mention that `LogicalTypeId::ARRAY` is rejected earlier (at `DeltaCreateTableSchema::ValidateTypes`).
  - **M5**: `PadVersion` O(n²) string prepend in `ccv2_test_committer.cpp`; immaterial at the loop bound (20 chars).
  - **L1**: `parent_commit_function_name` field placement in header.
  - **L2**: Asymmetry comment between CTAS and INSERT version storage paths.
  - **L3**: In-out DataChunk pattern in test committer documentation.

## Performance

- **Verdict: HAS_OPPORTUNITIES (0 High, 0 Medium, 3 Low — none applied)**
- All 5 perf observations from the perf reviewer:
  - **OPT-1 (Low-Medium)**: `HasNonStatsType` is now called per-column-per-file; was O(1), is now O(depth × fan-out) for STRUCTs. Result is stable per column — precomputing `vector<bool> column_skip_stats` in `DeltaInsertGlobalState` reduces repeat traversal. Perf reviewer ranked Low (real but small), so out of Stage-5 scope, but **recommend filing as follow-up before the visitor gets richer type coverage**.
  - **OPT-2 (Low)**: `std::atomic<idx_t>` load under existing mutex emits a redundant `LDAR` on aarch64. Architectural risk of changing it outweighs the negligible saving.
  - **OPT-3 (Zero)**: `max_catalog_version` propagation on non-CCv2 path costs 2 integer compares; no FFI hop reached when unset.
  - **OPT-4 (Low)**: `create_table_builder_with_table_property` chains O(properties) FFI hops. Currently just 1 (`io.unitycatalog.tableId`); kernel has no batch API at v0.23. Negligible vs. surrounding I/O.
  - **OPT-5 (Zero)**: `Sink` loop after assertion removal has no new cost.

## Items for Human Review

1. **OPT-1 perf follow-up** — `HasNonStatsType` precomputation in `DeltaInsertGlobalState`. Cheap, low-risk, but currently Low priority. Worth filing as a small follow-up ticket so it lands before the visitor adds more nested type coverage.
2. **`unity_table_id` should likely be required for `parent_commit=true`** (review M3). Current behavior silently falls back to the table path, which a real Unity Catalog server would reject with an opaque error. Recommend adding `InvalidInputException` in a separate small PR (the test fixture currently uses the fallback, so this needs the test to set the option explicitly).
3. **`DeltaCatalog` back-reference architectural smell** (review M1). The `DeltaTransaction → DeltaCatalog&` reference is one of two scope-creep additions. Single use today (post-CTAS version store). Could be removed by passing the committed version back through `InitializeForNewTable`'s return value and letting `Finalize` push it. Defer to a refactoring PR if you want to keep this PR focused.
4. **`parent_table_entry` nullptr-on-version-0 contract** — this is a new convention the real Unity Catalog committer must accept. The comment documents it but it's worth a direct conversation with the UC maintainers if they don't already accept NULL on version 0.
5. **Test-fixture-in-production-extension** (review H1) — addressed by `#ifdef DEBUG`. But this is the first piece of test-only C++ shipped in this repo; if there are more in the future, consider establishing a `test/` subdirectory pattern in `EXTENSION_SOURCES` so the guard becomes structural rather than ad-hoc.
6. **No kernel `GIT_TAG` bump.** Kernel pin remains at v0.23.0.
