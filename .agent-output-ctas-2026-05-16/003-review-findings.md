# Code Review Findings: CTAS Implementation for the `delta` Extension

**Verdict: NEEDS_CHANGES**

---

## Review Summary

The CTAS implementation is architecturally sound: the `allow_create` attach option, the hand-rolled version-0 JSON (accepted deviation), the `DeltaInsert` CTAS arm, and the `PhysicalCopyToFile` integration all follow the existing `PlanInsert` pattern correctly. The `StatNode::children` fix is correct and necessary. The `int64_t` → `idx_t` fixes are correct.

However, there are two correctness bugs that block approval: (1) `D_ASSERT` guards in plan-time and sink-init code fire on user-supplied input (non-column-ref partition expressions) instead of throwing `BinderException`; (2) the `timestamp_ntz` types (TIMESTAMP_NS, TIMESTAMP_MS, TIMESTAMP_SEC) are mapped to the Delta `timestamp_ntz` type but the emitted protocol action uses `minReaderVersion=1` and `minWriterVersion=2`, which violates the Delta protocol — readers that see `timestamp_ntz` without `minReaderVersion=3` and the `timestampNtz` reader feature will reject the table. Beyond those, there are three High issues and several Medium/Low findings.

---

## Critical (must fix before merge)

### [C1] D_ASSERT on user-controlled partition-key expression type

**Location:** `src/storage/delta_catalog.cpp:142` and `src/storage/delta_insert.cpp:103`

**Issue:** `D_ASSERT(pk->type == ExpressionType::COLUMN_REF)` fires if a user writes `CREATE TABLE t.t PARTITIONED BY (UPPER(col)) AS SELECT …`. In release builds the assert is a no-op; `Cast<ColumnRefExpression>()` then throws `InternalException` — wrong exception type. In debug builds it aborts the process. Both are user-triggerable and violate the DuckDB standard that `D_ASSERT` is for programmer invariants only (CONTRIBUTING.md).

The comments read "validated in CreateTable already" but `DeltaSchemaEntry::CreateTable` is not called for the CTAS path (see CTAS flow analysis below). The validation is entirely absent from the happy path.

**Fix:**
```cpp
// In PlanCreateTableAs (delta_catalog.cpp) and GetGlobalSinkState (delta_insert.cpp):
if (pk->type != ExpressionType::COLUMN_REF) {
    throw BinderException("Delta CTAS PARTITIONED BY only supports simple column references");
}
```

### [C2] Delta protocol version is incorrect for `timestamp_ntz` columns

**Location:** `src/storage/delta_ctas.cpp:141` (the `protocol` action string) and `src/storage/delta_ctas.cpp:65-71` (`LogicalTypeToDeltaType`)

**Issue:** `TIMESTAMP_NS`, `TIMESTAMP_MS`, and `TIMESTAMP_SEC` are serialised as `"timestamp_ntz"` in the Delta schema. The Delta protocol specification requires tables using `timestamp_ntz` to declare `minReaderVersion=3`, `minWriterVersion=7`, and include `"readerFeatures":["timestampNtz"]` / `"writerFeatures":["timestampNtz"]` in the protocol action. The code emits `minReaderVersion=1`, `minWriterVersion=2` unconditionally. Any compliant Delta reader (Spark, delta-rs, etc.) will either reject the table or misinterpret the column type.

**Fix (minimal):** Either (a) detect timestamp_ntz columns and emit the correct protocol version/features, or (b) throw `BinderException` for TIMESTAMP_NS/MS/SEC at CTAS time with a message explaining the limitation until the protocol version bumping is implemented.

The test `ctas_type_coverage.test` does not include any timestamp variant, so this bug is undetected by the test suite.

---

## High (should fix)

### [H1] `DeltaSchemaEntry::CreateTable` validation is bypassed for happy-path CTAS

**Location:** `src/storage/delta_schema_entry.cpp:40-106`; `src/storage/delta_insert.cpp:71-130`

**Issue:** The DuckDB planner calls `Catalog::PlanCreateTableAs` when the table does not yet exist, which builds a `DeltaInsert` operator and returns immediately — **without ever calling `DeltaSchemaEntry::CreateTable`**. The column-type validation (`DeltaSchemaJson::BuildSchemaString(columns)`) and partition-column validation in `CreateTable` are therefore dead code for the happy CTAS path.

`CreateTable` *is* called when the table already exists (DuckDB routes to `PhysicalCreateTable` which calls `CreateTable` for the "already exists" error path). The existing-table guard works correctly by accident of this routing. However, the validation (type check, partition-column existence) only runs on the error path, not on the path that writes data.

In `GetGlobalSinkState`, `DeltaSchemaJson::BuildSchemaString` is called at runtime (during sink initialisation) rather than at bind time, giving a poor user experience — the error occurs after resources have been allocated rather than during query planning.

**Fix:** Add the type-validity check and partition-column existence check as early as possible in `PlanCreateTableAs` at plan time (before building any operators), not in `GetGlobalSinkState`.

### [H2] `TakePendingCreateTable()` is dead code; `SetPendingCreateTable` / `HasPendingCreateTable` serve an impossible code path

**Location:** `src/include/storage/delta_transaction.hpp:43` and `src/storage/delta_transaction.cpp:592-594`

**Issue:** `TakePendingCreateTable()` is declared, implemented, but never called in the entire codebase. `SetPendingCreateTable` is only reached when `CreateTable` is called with a non-existent table (see above, this is not the happy CTAS path). `HasPendingCreateTable()` is checked in `LookupEntry`, but its condition is never true during a normal CTAS execution.

Leaving dead API in a transaction object pollutes the interface and misleads future developers about the flow.

**Fix:** Remove `TakePendingCreateTable` (never called), `SetPendingCreateTable`, `HasPendingCreateTable`, and `pending_create_table`. Remove the corresponding guard in `LookupEntry` (the guard has no observable effect). The existing-table protection already works via the `PhysicalCreateTable` → `CreateTable` routing.

### [H3] Failed `GetGlobalSinkState` leaves a partially-created table directory on disk

**Location:** `src/storage/delta_insert.cpp:92-128`

**Issue:** `GetGlobalSinkState` writes `_delta_log/00000000000000000000.json` (step 1), then calls `InitializeTableEntry` (step 2). If step 2 throws (e.g., the kernel rejects the JSON, or there is an I/O error), the JSON file is left on disk. `Rollback()` → `CleanUpFiles()` only removes outstanding parquet files; it does not remove the log directory or the JSON file.

The result: a subsequent CTAS attempt to the same path fails with "Delta table already exists" even though no data was ever committed. The user must manually delete the directory to recover.

**Fix:** Wrap steps 1–2 in a try/catch inside `GetGlobalSinkState`; on any exception, attempt to remove the version-0 file and re-throw. Alternatively, track the created-file path in the transaction and clean it up in `Rollback()`.

---

## Medium (nice to fix)

### [M1] `_delta_log/00000000000000000000.json` path literal duplicated in three locations

**Location:** `src/storage/delta_schema_entry.cpp:83`, `src/storage/delta_schema_entry.cpp:282`, `src/storage/delta_insert.cpp:115`

**Issue:** The string `"/_delta_log/00000000000000000000.json"` is assembled by hand-concatenating the catalog path in three separate places. Any change to this path (e.g., moving to a helper that uses `Path::Join`) requires updating all three.

**Fix:** Extract a `constexpr` or `static` helper function, e.g. `GetVersion0Path(const string &base)`, in `delta_ctas.hpp`/`.cpp` or a shared utility header.

### [M2] Manual recursive-directory creation reinvents `CreateDirectoriesRecursive`

**Location:** `src/storage/delta_catalog.cpp:163-180`

**Issue:** The manual `PathSeparator` / `StringUtil::Split` / `fs.DirectoryExists` / `fs.CreateDirectory` loop reimplements `FileSystem::CreateDirectoriesRecursive`, which already exists in DuckDB's `FileSystem` API (see `duckdb/src/include/duckdb/common/file_system.hpp:188`). The hand-rolled version also has fragile behaviour for cloud URLs (the separator check on `"s3://"` paths produces incorrect intermediate paths).

**Fix:**
```cpp
auto &fs = FileSystem::GetFileSystem(context);
fs.CreateDirectoriesRecursive(delta_path);
```

### [M3] `JsonEscapeString` does not escape control characters U+0000–U+001F (except the three it handles)

**Location:** `src/storage/delta_ctas.cpp:13-39`

**Issue:** RFC 8259 §7 requires that all characters in the range U+0000–U+001F be escaped in a JSON string. `JsonEscapeString` only handles `\"`, `\\`, `\n`, `\r`, and `\t`. Characters like `\b` (0x08), form-feed (0x0C), NUL (0x00), and all others in 0x00–0x1F except the five handled ones are emitted literally. Column names containing such characters (unusual but possible) would produce malformed JSON that the kernel parser may reject or misparse.

**Fix:** Add a `default` branch in the switch that emits `\uXXXX` for characters in `0x00–0x1F`:
```cpp
default:
    if (static_cast<unsigned char>(c) < 0x20) {
        // JSON requires escaping all control characters
        char buf[7];
        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
        result += buf;
    } else {
        result += c;
    }
    break;
```

### [M4] `const_cast<DeltaSchemaEntry &>` in `GetGlobalSinkState` indicates a design smell

**Location:** `src/storage/delta_insert.cpp:125`

**Issue:** `GetGlobalSinkState` is declared `const` (standard DuckDB physical-operator API), but the CTAS path mutates the schema entry indirectly via `InitializeTableEntry` → `CreateTableEntry` (which may populate `cached_table`). The `const_cast` works at runtime but hides the mutation from the type system, making it harder to reason about thread safety.

**Fix:** Accept the `const_cast` for now with an expanded comment explaining why the mutation is safe (the schema entry's `lock` protects `cached_table`). Longer-term, `GetGlobalSinkState` could take a non-const overload for CTAS, matching the DuckDB `PhysicalInsert` pattern.

### [M5] Empty CTAS starts then abandons a kernel exclusive transaction

**Location:** `src/storage/delta_transaction.cpp:546-580`

**Issue:** For a CTAS that produces zero rows (`WHERE false`), `Finalize` calls `Append({})` with an empty vector. `Append` calls `InitializeTransaction` (starts a kernel `ExclusiveTransaction`) even when no files were written. `Commit` then sees `outstanding_appends.empty()` and skips `ffi::commit`. The kernel transaction is abandoned (only `ffi::free_transaction` is called via RAII). Although `free_transaction` is an implicit rollback of a no-op transaction, this is an unclean path.

**Fix:** Skip `InitializeTransaction` when `append_files.empty()` in `Append`:
```cpp
if (append_files.empty()) {
    return;
}
if (transaction_state == DeltaTransactionState::TRANSACTION_NOT_YET_STARTED) {
    InitializeTransaction(context);
}
```

### [M6] Column schema: `nullable` is always `true`, ignoring NOT NULL constraints

**Location:** `src/storage/delta_ctas.cpp:128` (`BuildRawSchemaJson`)

**Issue:** Every column in the emitted Delta schema is marked `"nullable":true` regardless of NOT NULL constraints in the DuckDB schema. The Delta protocol uses `nullable` for informational purposes; runtime enforcement is still handled by DuckDB. However, downstream tools (e.g., Spark readers, delta-rs) may use this field for optimizations. The Delta table created by CTAS loses NOT NULL metadata.

**Fix:** Check for NOT NULL constraints and emit `"nullable":false` when appropriate. Pass constraint information from `BoundCreateTableInfo` through to `BuildRawSchemaJson`.

---

## Low (style suggestions)

### [L1] `std::to_string` used in JSON builder — prefer `StringUtil::Format`

**Location:** `src/storage/delta_ctas.cpp:77, 158, 162`

**Issue:** DuckDB idiomatic style uses `StringUtil::Format` for mixed string/integer construction rather than `std::to_string`. Three uses in `BuildCommitJson` and `LogicalTypeToDeltaType`.

**Fix:** Replace `"..." + std::to_string(x) + "..."` with `StringUtil::Format("...%d...", x)`.

### [L2] `D_ASSERT(found); (void)found;` pattern without a comment explaining the invariant

**Location:** `src/storage/delta_catalog.cpp:153-154`

**Issue:** DuckDB style requires `D_ASSERT` statements to have a comment explaining the invariant. The existing comment says "validated in CreateTable already" which is factually incorrect (see H1). The `(void)found;` idiom is correct but the `D_ASSERT` comment is misleading.

**Fix after [C1] is fixed:** Either promote to a proper error check or write a correct invariant comment. Example: `// partition_key existence was verified at bind time above`.

### [L3] Em-dash (—) in exception message strings

**Location:** `src/storage/delta_catalog.cpp:130`

**Issue:** `"Parquet copy function not found — parquet extension must be loaded..."` uses a Unicode em-dash (3-byte UTF-8). DuckDB exception messages conventionally use plain ASCII. Minor style inconsistency.

**Fix:** Replace with ` - ` or `: `.

### [L4] Missing space before opening brace of `PlanCreateTableAs`/`PlanDelete`

**Location:** `src/storage/delta_catalog.cpp:231`

**Issue:** `PlanCreateTableAs`'s closing brace is followed immediately by `PhysicalOperator &DeltaCatalog::PlanDelete(...)` with no blank line separator. `clang-format` would reformat this.

---

## Test Coverage Gaps (High priority)

### [T1] No test for `timestamp_ntz` (TIMESTAMP_NS / TIMESTAMP_MS / TIMESTAMP_SEC) types

The `ctas_type_coverage.test` tests DATE and DECIMAL but not TIMESTAMP variants. Add a test — this would likely expose C2 immediately.

### [T2] No test for CTAS with PARTITIONED BY

Partitioned CTAS is implemented (partition_columns vector, hive output), but there is no sqllogic test exercising it. Add a test with at least one partition column, verify hive directory structure and row counts.

### [T3] No test for CTAS without `allow_create=true`

A user who writes `CREATE TABLE t.t AS SELECT …` without `allow_create=true` gets a confusing "cannot find _delta_log" error from the kernel rather than a helpful message. Add a test that verifies a useful error message is produced.

### [T4] No test for STRUCT / LIST / MAP types

`LogicalTypeToDeltaType` supports STRUCT, LIST, MAP. Add entries to `ctas_type_coverage.test`.

### [T5] No test for CTAS with an unsupported type (e.g., UBIGINT, INTERVAL)

The BinderException from `LogicalTypeToDeltaType` for unsupported types should be covered by a `statement error` test.

---

## What's Done Well

1. **`StatNode` self-referential map fix** (`src/storage/delta_transaction.cpp:97-108`): Introducing the `StatNodeMap` typedef and changing `children` to `unique_ptr<StatNodeMap>` is the correct solution to an incomplete-type UB in the original `unordered_map<string, StatNode>`. The `StatNode()` constructor properly initialises the children pointer.

2. **`FILE_FLAGS_FILE_CREATE_NEW` for the version-0 write** (`src/storage/delta_insert.cpp:117`): Using the exclusive-create flag correctly prevents a race where two concurrent CTAS to the same path both succeed. The second opener gets an IOException rather than silently overwriting the first table's log.

3. **`idx_t` corrections in loop variables** (`src/storage/delta_insert.cpp:412-413`): Changing `int64_t i` / `int64_t j` to `idx_t` in the partition-column search loops is correct (DuckDB CONTRIBUTING.md mandates `idx_t` for indices). The pattern `(void)found` after `D_ASSERT(found)` is the correct DuckDB idiom for a variable only referenced in an assert.

---

## Blocking Issues Summary (for NEEDS_CHANGES verdict)

| ID | Location | Description |
|----|----------|-------------|
| C1 | `delta_catalog.cpp:142`, `delta_insert.cpp:103` | User-triggerable `D_ASSERT` on partition expression type (should be `BinderException`) |
| C2 | `delta_ctas.cpp:141` | `timestamp_ntz` schema requires `minReaderVersion=3` + features in protocol action |
| H1 | `delta_schema_entry.cpp:40-106` | CreateTable validation bypassed for happy-path CTAS |
| H2 | `delta_transaction.hpp:43`, `delta_transaction.cpp:592` | `TakePendingCreateTable` is dead code |
| H3 | `delta_insert.cpp:92-128` | Failed GetGlobalSinkState leaves orphaned `00000000000000000000.json` |

