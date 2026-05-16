# Feature Summary: CREATE TABLE … AS SELECT (CTAS) for the delta extension

## What Was Built

CTAS support for Delta tables in DuckDB. Users can now write `CREATE TABLE t AS SELECT …` against a delta-attached database (`ATTACH 'path' AS db (TYPE delta, allow_create=true)`) and the extension materializes both the on-disk Delta table (initial `_delta_log/00000000000000000000.json` commit + Parquet data files) and the in-memory catalog entry. The implementation reuses DuckDB's `PhysicalCopyToFile` for partition-aware parquet output and the existing `DeltaInsert` operator for the row sink, mirroring the established blind-append path. A new `allow_create` ATTACH option gates table creation so missing paths can't be auto-created from typos.

## Architecture Decisions

- **Hand-roll the initial-commit JSON instead of using the kernel FFI.** The architect's v2 plan proposed using kernel v0.21.0's `get_create_table_builder` → `create_table_commit` FFI. During implementation, the coder verified against `generated_delta_kernel_ffi.hpp` and discovered that while the kernel exposes the *builder*, it exposes **no** way to construct a `Handle<SharedSchema>` from scratch — every schema-handle producer (`logical_schema`, `scan_logical_schema`, etc.) needs an existing snapshot. Chicken-and-egg. Fallback: synthesize the version-0 JSON in C++ via a new `DeltaSchemaJson` module, then let the kernel parse it during the read-back. Trade-off: we now own the protocol-action JSON for the *initial* commit only; all subsequent commits go through the kernel.
- **Bind-time validation, not runtime.** `PlanCreateTableAs` calls `DeltaSchemaJson::ValidateColumnTypes` and validates partition keys at plan time so users get errors during query planning, not mid-pipeline at sink init. (Fixed in review iter 1; OPT-3 later replaced the throw-away `BuildSchemaString` call with a pure validation pass.)
- **No `pending_create_table` machinery.** Originally the architect proposed staging the new `DeltaTableEntry` on the transaction and reading it back via a `HasPendingCreateTable` guard. Review found this was dead code on the happy path (DuckDB routes around it) and it was removed. Existing-table protection works correctly via DuckDB's planner routing `existing_entry` to `PhysicalCreateTable` → `CreateTable`, which throws `CatalogException`.
- **Atomic file create with `FILE_FLAGS_FILE_CREATE_NEW`.** Prevents two concurrent CTAS to the same path from racing — the second throws `IOException` rather than overwriting.
- **Try/catch cleanup of the version-0 file.** If anything between writing the JSON and the kernel re-reading the snapshot throws, the orphaned `00000000000000000000.json` is removed so the path is reusable.

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `src/include/storage/delta_ctas.hpp` | Created | `DeltaSchemaJson` declarations (`BuildSchemaString`, `BuildCommitJson`, `ValidateColumnTypes`) |
| `src/storage/delta_ctas.cpp` | Created | DuckDB-type → Delta JSON schema serializer + initial-commit JSON builder; JSON-escape helper; `LogicalTypeToDeltaType` |
| `CMakeLists.txt` | Modified | Added `src/storage/delta_ctas.cpp` to `EXTENSION_SOURCES` |
| `src/delta_extension.cpp` | Modified | Wired the new `allow_create` boolean ATTACH option |
| `src/include/storage/delta_catalog.hpp` | Modified | Added `bool allow_create = false` field on `DeltaCatalog` |
| `src/storage/delta_catalog.cpp` | Modified | Implemented `PlanCreateTableAs` (mirror of `PlanInsert` with CTAS-specific validation, directory creation, and `DeltaInsert` construction); replaced `D_ASSERT` partition-expression guards with `BinderException` |
| `src/storage/delta_insert.cpp` | Modified | CTAS arm in `GetGlobalSinkState`: writes version-0 JSON exclusively, re-initializes snapshot, cleanup on failure; `int64_t→idx_t` loop counters |
| `src/storage/delta_schema_entry.cpp` | Modified | `CreateTable` validates inputs + throws `CatalogException` on path collision; `LookupEntry` returns null on first-CTAS lookup when `allow_create=true`; `allow_create` stat moved past the cached-entry fast path |
| `src/storage/delta_transaction.cpp` | Modified | Empty-append guard in `Append`; `StatNode` self-referential `unique_ptr<StatNodeMap>` fix |

## sqllogic Tests Added

- `test/sql/main/writing/ctas/basic_ctas.test` — happy path: round-trip `CREATE TABLE … AS SELECT …`
- `test/sql/main/writing/ctas/ctas_attach_existing.test` — `CREATE TABLE` against a path that already contains a Delta table fails with the right error
- `test/sql/main/writing/ctas/ctas_empty_select.test` — zero-row select still produces a valid Delta table (and after OPT-2, doesn't spin up a kernel transaction)
- `test/sql/main/writing/ctas/ctas_or_replace_unsupported.test` — `CREATE OR REPLACE TABLE` throws `BinderException`
- `test/sql/main/writing/ctas/ctas_then_insert.test` — subsequent `INSERT INTO` after CTAS works (validates the kernel's snapshot read-back)
- `test/sql/main/writing/ctas/ctas_type_coverage.test` — type-by-type CTAS coverage (numeric, string, bool, date, timestamp; deliberately excludes `timestamp_ntz` which is rejected at bind time per C2)

**Final test status:** 138 assertions in 10 test cases pass (`build/debug/test/unittest "test/sql/main/writing/*"`).

## Review Status

- **Verdict: APPROVED after 2 iterations.**
- Iter 1 fixed 5 blocking issues:
  - C1 (user-triggerable `D_ASSERT` on partition expression type → `BinderException`)
  - C2 (timestamp_ntz emitted with wrong protocol version → `BinderException` directing users to alternative types)
  - H1 (validation moved from runtime to plan time)
  - H2 (dead `pending_create_table` machinery removed)
  - H3 (orphaned version-0 file cleanup on init failure)
- Iter 2 surfaced one new High issue (`CreateTableEntryForNewTable` left dead-code after H2's cleanup) which was immediately fixed.
- **Outstanding Medium/Low items (deliberately deferred — out of orchestrator scope):**
  - M1: `_delta_log/00000000000000000000.json` path assembled by hand in three places — could extract a helper
  - M3: `JsonEscapeString` does not escape control characters U+0001–U+0008, U+000B–U+001F (only newline/tab/CR/quote/backslash handled). Spec violation only triggered by column names with embedded control chars.
  - M5: Column `nullable` field always `true`; NOT NULL constraints not propagated to the Delta schema (runtime enforcement unaffected; downstream-reader optimisations potentially missed)
  - M6: `const_cast<DeltaSchemaEntry &>` in `GetGlobalSinkState` (acknowledged design smell; protected by `schema.lock`)
  - L1: `std::to_string` vs DuckDB-idiomatic `StringUtil::Format`
  - L3: em-dash in an exception message
- **Outstanding test gaps (out of orchestrator scope; would be useful):**
  - T1: explicit `timestamp_ntz` rejection test (would lock in C2 behavior)
  - T2: partitioned CTAS happy-path test (partition code is reached but not exercised)
  - T3: CTAS without `allow_create=true` (currently produces a confusing kernel error rather than a guided message)
  - T4: STRUCT / LIST / MAP column types in CTAS
  - T5: explicit unsupported-type rejection (UBIGINT, INTERVAL, etc.)

## Performance

- **Verdict: HAS_OPPORTUNITIES (5 findings, all applied)**
- **Optimizations applied (5/5; 0 skipped, 0 reverted):**
  - OPT-1 (High): `allow_create` `FileExists` stat moved past the `transaction_table_entry` and `cached_table` fast paths — eliminates 1–5 ms object-store stat per query after first lookup
  - OPT-2 (High): `DeltaTransaction::Append` early-returns when `append_files` is empty — saves ~50–200 µs FFI overhead per zero-row CTAS
  - OPT-3 (Medium): new `ValidateColumnTypes` avoids building (and discarding) a JSON string at plan time — ~62 fewer allocations for a 20-column CTAS
  - OPT-4 (Medium): replaced hand-rolled `DirectoryExists`/`CreateDirectory` loop with `FileSystem::CreateDirectoriesRecursive` — 6–30 ms fewer round trips on deep cloud paths
  - OPT-5 (Medium): `string::reserve` in `BuildRawSchemaJson` and `BuildCommitJson`, plus rewrote `BuildCommitJson` to single-buffer `+=` pattern — eliminates 4–8 reallocations per CTAS

## Items for Human Review

1. **The architect's plan was wrong twice in a row.** v1 hand-rolled the JSON because the architect didn't know the kernel FFI exposed `create_table` builders. v2 used the FFI but didn't notice the FFI has no `SharedSchema` constructor. The implementer caught it the third time, against the actual generated header. The final design (hand-rolled JSON for the version-0 commit, kernel for everything after) is correct, but the path to get there suggests the architect should verify FFI claims against the generated header (`build/<config>/codegen/include/generated_delta_kernel_ffi.hpp`) before designing around a hypothetical kernel surface.
2. **Hand-rolled JSON for the initial commit is a protocol-correctness liability.** We now own the version-0 JSON shape forever (or until the kernel FFI grows a `SharedSchema` constructor and we revisit). Future Delta-protocol additions (column mapping, deletion vectors, V2 checkpoints, anything requiring writer-feature flags or a newer min reader/writer version) will require us to update `delta_ctas.cpp` rather than picking them up "for free" by bumping the kernel pin. Worth tracking as a known liability.
3. **`timestamp_ntz` is now explicitly rejected.** Users with `TIMESTAMP_NS` / `TIMESTAMP_MS` / `TIMESTAMP_SEC` columns will hit `BinderException` at CTAS time directing them to `TIMESTAMP` / `TIMESTAMP WITH TIME ZONE`. This is correct (the alternative is shipping protocol-noncompliant tables), but it is a usability regression vs. silently emitting a wrong-protocol table — flag for release notes.
4. **`allow_create=true` is required at ATTACH time** for CTAS to succeed. Without it, users get a kernel-side error message that isn't tuned for "you forgot the flag." Test gap T3 captures this; consider adding a friendlier diagnostic before release.
5. **No kernel `GIT_TAG` bump was made.** The architect raised v0.23.0 as a possible upgrade in an earlier `/new-feature` invocation that was not run; that work is still open. Bumping the pin might unlock a future kernel-native CTAS path (worth checking when v0.23 / v0.24 / etc. ship `Handle<SharedSchema>` construction from JSON / column list).
6. **Outstanding test gaps and Medium-priority items** (listed under "Review Status" above) were deliberately not addressed under the orchestrator's "Critical and High only" scope. Recommend filing them as follow-ups before this feature ships.
