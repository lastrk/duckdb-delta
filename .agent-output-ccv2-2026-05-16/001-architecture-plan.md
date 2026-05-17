# Architecture Plan: CCv2 CTAS wiring + LIST/MAP/STRUCT CTAS test coverage

Two related deliverables in one PR:

1. **Wire up CCv2 CTAS** (`parent_commit=true`): replace the
   `NotImplementedException` short-circuit in
   `DeltaTransaction::InitializeForNewTable` with a branch that builds a
   `MutableCommitter` the same way INSERT does (`get_uc_commit_client`
   --> `get_uc_committer`) and passes it to
   `ffi::create_table_builder_build_with_committer` instead of the
   default `ffi::create_table_builder_build`.
2. **Add CTAS test fixtures for currently-untested visitor paths**:
   `LIST` (`[1,2,3]`), `MAP` (`MAP {'a':1}`), nested combinations
   (`LIST` of `STRUCT`, `MAP` with `STRUCT` value), `BLOB`, `ENUM`,
   `TIMESTAMP_TZ`, and partitioned CTAS. All lock in current
   behavior without changing code.

Authoritative context: the prior CTAS retirement plan and summary in
`/workspace/.agent-output-ctas-retirement-2026-05-16/{001-architecture-plan.md,005-summary.md}`.
Open Question 1 of that plan is the CCv2 deferral we now close.

The FFI surface has been **re-verified against the locally-generated
header** at
`/workspace/build/debug/codegen/include/generated_delta_kernel_ffi.hpp`
(lines 1708-1734, 2773-2785) and the kernel Rust source at
`/workspace/build/debug/rust/src/delta_kernel/ffi/src/transaction/mod.rs`
(lines 582-602). The plan-relevant fact is:
`create_table_builder_build` and `create_table_builder_build_with_committer`
**share the same `create_table_builder_build_impl`** in Rust, which calls
`builder.build(engine, committer)`. The committer's `commit()` method is
invoked the same way for create-table transactions as for regular
transactions; **our existing `DeltaTransaction::CommitCallback` works
unchanged for CCv2 CTAS**.

---

## 1. Domain Constraints

### Delta Lake (protocol)
- A catalog-managed (CCv2) Delta table requires that the version-0
  commit JSON be **staged** in `_delta_log/_staged_commits/` instead of
  written directly to `_delta_log/00000000000000000000.json`. The
  parent catalog (Unity Catalog or equivalent) is responsible for
  promoting the staged commit to the published log entry as part of an
  atomic catalog commit. This is the entire reason the
  `__internal_delta_ccv2_commit_staged` table function exists.
- For CTAS, the staged commit contains Protocol + Metadata + (optional)
  Add actions for any parquet files written by the CTAS sink. The
  commit shape is identical to INSERT; only the protocol+metadata bit
  differs.
- The kernel's `UCCommitter` (the Rust impl behind `get_uc_committer`)
  produces this staged commit and surfaces a `CommitRequest` to our
  `CommitCallback`. The C++ side has zero knowledge of the staged
  commit content — we just route the request through to
  `commit_function`.

### DuckDB
- CTAS routes through `DeltaCatalog::PlanCreateTableAs` -->
  `DeltaInsert` --> `DeltaInsert::GetGlobalSinkState` -->
  `DeltaTransaction::InitializeForNewTable`. The bind data
  (`BoundCreateTableInfo`) carries the schema and partition keys.
- INSERT-side CCv2 wiring sets `parent_table_entry` via
  `DeltaTransaction::SetParentTableEntry` from
  `PlanInsert` (`delta_insert.cpp:412-415`) using `op.table` — the
  parent-catalog `TableCatalogEntry` for the existing table.
- **CTAS has no pre-existing parent-catalog `TableCatalogEntry`**: the
  table is being created. This is the central design tension and is
  resolved in section 7 (Test Plan / Open Questions).

### FFI boundary
- `ffi::get_uc_commit_client(context, CommitCallback) -> Handle<SharedFfiUCCommitClient>`
  (header line 1708). The `context` pointer is stored verbatim and
  passed back into the callback; it must be a stable pointer for the
  client's lifetime. Today's INSERT path uses `this` (the
  `DeltaTransaction *`).
- `ffi::get_uc_committer(commit_client, table_id_slice, error_fn)
  -> ExternResult<Handle<MutableCommitter>>` (header line 1723).
  Consumes the client handle implicitly (the kernel doc on line 1727
  says the client is consumed when used to build a committer via
  `transaction_with_committer`, but the contract is symmetric for the
  create-table builder).
- `ffi::create_table_builder_build_with_committer(builder, committer, engine)`
  (header line 2783) **consumes both handles** unconditionally — on
  error path the kernel has freed them; the engine-side RAII wrappers
  must NOT free again.

---

## 2. Affected Surfaces

```
src/storage/
  delta_transaction.cpp          MOD  Wire CCv2 branch in InitializeForNewTable;
                                       set parent_table_entry where needed.
  delta_insert.cpp               MOD  CTAS arm of GetGlobalSinkState: in CCv2 mode,
                                       call SetParentTableEntry (see Section 7 for the
                                       exact value passed; see Open Question 1).

src/include/storage/             NO CHANGE.
src/include/                     NO CHANGE.
src/delta_extension.cpp          NO CHANGE.

scripts/ffi/                     NO CHANGE.
CMakeLists.txt                   NO CHANGE.
build/<cfg>/codegen/include/
  generated_delta_kernel_ffi.hpp NO CHANGE (regenerated, content unchanged).

test/sql/main/writing/ctas/
  ctas_complex_types.test        NEW   LIST, MAP, nested combinations,
                                       BLOB, ENUM, TIMESTAMP_TZ.
  ctas_partitioned.test          NEW   PARTITIONED BY single col,
                                       PARTITIONED BY two cols, NULL partition value,
                                       partition value with special chars.
  ctas_ccv2.test                 NEW   Mock parent-catalog with a stub
                                       __internal_delta_ccv2_commit_staged
                                       table function defined via DuckDB's
                                       SQL macro / scalar UDF path
                                       (see Section 7 for the strategy).
                                       MAY be omitted if Section 7's
                                       Strategy (a) proves infeasible.
```

**Net code change**: ~30 lines in `delta_transaction.cpp`, ~5 lines in
`delta_insert.cpp`. No header changes. No new files in `src/`.

**Net test change**: 2 new tests for sure (LIST/MAP and partitioned),
1 new test conditional on CCv2 fixture feasibility.

---

## 3. CCv2 Lookup Wiring — what's reusable vs. new

The existing INSERT-side CCv2 wiring at `delta_transaction.cpp:564-575`:

```cpp
if (parent_commit) {
    auto commit_client = ffi::get_uc_commit_client(this, CommitCallback);
    auto table_id = KernelUtils::ToDeltaString(unity_table_id.empty() ? path : unity_table_id);
    auto uc_committer = table_entry->snapshot->TryUnpackKernelResult(
        ffi::get_uc_committer(commit_client, table_id, DuckDBEngineError::AllocateError));
    new_kernel_transaction = table_entry->snapshot->TryUnpackKernelResult(ffi::transaction_with_committer(
        snapshot_ref.GetPtr(), table_entry->snapshot->extern_engine.get(), uc_committer));
}
```

**What's reusable verbatim**:
- `ffi::get_uc_commit_client(this, CommitCallback)` — `this` is still
  the `DeltaTransaction *`. `CommitCallback` is the static class
  method, called by the kernel for both INSERT and CTAS commits.
- The `unity_table_id` fallback to the table path is the same.
- The `DuckDBEngineError::AllocateError` function pointer is the same.

**What's NOT reusable directly**:
- `table_entry->snapshot->TryUnpackKernelResult(...)` — in CTAS we
  have no `table_entry` (no snapshot yet). The CCv2 CTAS path must
  unpack via a free-standing call to `KernelUtils::TryUnpackResult`
  (the same pattern the existing CTAS path uses at
  `delta_transaction.cpp:674` and `:690`).
- `ffi::transaction_with_committer(snapshot, engine, committer)` is
  swapped for
  `ffi::create_table_builder_build_with_committer(builder, committer, engine)`.

**Helper extraction decision**: do NOT extract a shared helper.
Reasoning: the call sites differ in three ways (snapshot vs.
builder-driven, return type, error-unpacking dependency on
`table_entry`). A shared helper would have a fan-in of 2 and would
need 3 parameters that vary independently. The duplication is a
4-line cluster (`get_uc_commit_client`, `ToDeltaString` for table_id,
`get_uc_committer`, unpack). Inline both call sites.

**Required additional wiring**:
- `parent_table_entry` must be set on the `DeltaTransaction` before
  `CommitCallback` runs. Today's INSERT path sets it in `PlanInsert`
  (`delta_insert.cpp:412-415`) from `op.table`. For CTAS, the
  parent-catalog `TableCatalogEntry` does not yet exist — see
  Open Question 1 for resolution.
- The `current_context` weak_ptr must be set before
  `create_table_commit` runs. The existing CTAS code already does this
  on line 700 (`current_context = context.shared_from_this()`); it's
  correctly set before the commit invocation in `Commit()`. No change.

---

## 4. Ownership Map (delta from prior plan)

Only one new transient handle crosses the boundary on the CCv2 CTAS
path. The mapping mirrors INSERT-side exactly:

| Kernel handle | RAII wrapper | Deleter | Lifetime |
|---|---|---|---|
| `Handle<SharedFfiUCCommitClient>` | unwrapped raw (consumed by `get_uc_committer` on success path) | `ffi::free_uc_commit_client` (line 1714) if `get_uc_committer` fails before consuming | Function-local to CCv2 branch of `InitializeForNewTable`. |
| `Handle<MutableCommitter>` | unwrapped raw (consumed by `create_table_builder_build_with_committer`) | `ffi::free_uc_committer` (line 1734) on the error path BETWEEN `get_uc_committer` returning Ok and `create_table_builder_build_with_committer` returning | Function-local to CCv2 branch of `InitializeForNewTable`. |
| `Handle<ExclusiveCreateTableBuilder>` | `KernelExclusiveCreateTableBuilder` (exists) | `ffi::free_create_table_builder` | Same as today's CTAS path. |
| `Handle<ExclusiveCreateTransaction>` | `KernelExclusiveCreateTransaction` (exists, field on `DeltaTransaction`) | `ffi::create_table_free_transaction` | Same as today's CTAS path. |

**Consume-on-error subtlety** (mirrors INSERT pattern):
- `ffi::get_uc_commit_client` is infallible (returns a `Handle`, not
  `ExternResult<Handle>`). The handle is consumed by `get_uc_committer`
  on the success path. If `get_uc_committer` returns Err, the kernel's
  contract (read from `ffi/src/delta_kernel_unity_catalog.rs:206-211`
  and the deferred-free comment at line 1727) is that the client is
  NOT consumed on error and the caller must free it. The INSERT path
  at `delta_transaction.cpp:566-569` does NOT explicitly free it —
  **this is an existing latent leak on the INSERT-side error path**.
  We mirror the existing pattern (do not fix in this PR; flag as Open
  Question 4) so the CTAS code is identical-shaped to INSERT.
- `ffi::create_table_builder_build_with_committer` consumes BOTH
  handles on success AND error. The committer raw pointer must NOT be
  freed by us regardless of the result. The builder is handled the
  same way as the existing CTAS path (release from RAII wrapper
  before the call, then check the result).

---

## 5. Module Layout

No new files. The two changed files keep their current responsibilities:

```
src/storage/
  delta_transaction.cpp     MOD  CCv2 CTAS branch in InitializeForNewTable.
  delta_insert.cpp          MOD  Set parent_table_entry in CTAS arm when
                                  parent_commit == true.
```

---

## 6. Key Types — no changes

No struct / class / enum changes. The `DeltaTransactionMode::CREATING_TABLE`
enum already covers CCv2 because the kernel commit-path dispatch is
identical for default and UC-committer flows. The `parent_commit` bool on
`DeltaTransaction` is already populated from `DeltaCatalog::parent_commit`
in the constructor (`delta_transaction.cpp:33`).

**One Layer-3 invariant to note in code**: the CCv2 CTAS branch must
behave as "CREATING_TABLE mode AND parent_commit". The two are
orthogonal — CREATING_TABLE means "we hold a kernel_create_txn"; the
`parent_commit` field is unchanged from its attach-time value. Do not
introduce a `CREATING_TABLE_CCV2` enum variant — `bool` + enum is
sufficient and matches the INSERT-side pattern.

---

## 7. FFI Plan

### Modify `DeltaTransaction::InitializeForNewTable` in `src/storage/delta_transaction.cpp`

Today's lines 641-646:

```cpp
if (parent_commit) {
    throw NotImplementedException(
        "CREATE TABLE AS SELECT with Unity Catalog managed commits (parent_commit=true) is not yet "
        "supported. Use a non-managed catalog for CTAS, or insert into the table in a separate step.");
}
```

Replace with **nothing** at this site (the `parent_commit` branch is
moved into Step 2 below — the builder construction and the committer
attachment are both at the same scope level).

Then at the **Step 2** location (today's line 688-695), today's code is:

```cpp
// Step 2: build the exclusive-create-transaction (default FileSystemCommitter path).
ffi::ExclusiveCreateTransaction *txn_raw = nullptr;
auto build_res = KernelUtils::TryUnpackResult(
    ffi::create_table_builder_build(builder.release(), ctas_extern_engine.get()), txn_raw);
kernel_create_txn = KernelExclusiveCreateTransaction(txn_raw);
if (build_res.HasError()) {
    build_res.Throw();
}
```

Replace with the branched form:

```cpp
// Step 2: build the exclusive-create-transaction. CCv2 (parent_commit=true) takes the
//         UC committer path so the kernel routes the staged commit through our
//         CommitCallback --> commit_function; the default path uses FileSystemCommitter.
ffi::ExclusiveCreateTransaction *txn_raw = nullptr;
ffi::ExternResult<ffi::Handle<ffi::ExclusiveCreateTransaction>> build_result;
if (parent_commit) {
    // Mirror the INSERT-side CCv2 wiring at line 564-575: construct a UC commit client
    // bound to this transaction's CommitCallback, then a committer for the unity_table_id
    // (falling back to the table path), then consume both handles in the build call.
    auto commit_client = ffi::get_uc_commit_client(this, CommitCallback);
    auto table_id = KernelUtils::ToDeltaString(unity_table_id.empty() ? ctas_table_path : unity_table_id);
    ffi::MutableCommitter *committer_raw = nullptr;
    auto committer_res = KernelUtils::TryUnpackResult(
        ffi::get_uc_committer(commit_client, table_id, DuckDBEngineError::AllocateError), committer_raw);
    if (committer_res.HasError()) {
        // The kernel did NOT consume `commit_client` on the error path. The INSERT-side
        // CCv2 code does not free it either (existing latent leak; see Open Question 4).
        // Mirror that behavior here so the two CCv2 paths stay symmetric.
        committer_res.Throw();
    }
    // Consumes BOTH the builder and the committer unconditionally (header line 2783).
    build_result = ffi::create_table_builder_build_with_committer(
        builder.release(),
        ffi::Handle<ffi::MutableCommitter>{committer_raw},
        ctas_extern_engine.get());
} else {
    build_result = ffi::create_table_builder_build(builder.release(), ctas_extern_engine.get());
}

auto build_res = KernelUtils::TryUnpackResult(build_result, txn_raw);
kernel_create_txn = KernelExclusiveCreateTransaction(txn_raw);
if (build_res.HasError()) {
    build_res.Throw();
}
```

(Skeleton only; cpp-coder fills in any signature wrinkles around
`ffi::Handle<MutableCommitter>{raw}` brace-init based on the generated
header's actual struct shape.)

### Modify `DeltaInsert::GetGlobalSinkState` (CTAS arm) in `src/storage/delta_insert.cpp`

Today (lines 78-107): no `SetParentTableEntry` call on the CTAS arm.
Add a CCv2 hook that mirrors the INSERT-side pattern at
`delta_insert.cpp:412-415`. **The value passed is the open question** —
see Open Question 1. Skeleton:

```cpp
if (delta_catalog.parent_commit) {
    // CCv2 CTAS: the CommitCallback requires a parent_table_entry pointer that the
    // parent commit function will read out via the `table_entry_pointer` struct field.
    // For CTAS there is no pre-existing TableCatalogEntry on the parent catalog --
    // see Architect Open Question 1 for the resolution. As a first cut, pass the
    // schema entry's catalog entry; the parent commit function MUST accept either a
    // TableCatalogEntry pointer (INSERT) or a SchemaCatalogEntry pointer (CTAS)
    // distinguishable from the version field (version == 0 means CTAS).
    delta_transaction.SetParentTableEntry(<TBD per Open Question 1>);
}
```

If Open Question 1's resolution is "pass nullptr is allowed for CTAS",
the `CommitCallback` body at `delta_transaction.cpp:359-361` must
loosen its non-null check to only require it when `commit_info.version > 0`,
and the staged-commit struct should still carry the pointer field
populated as `Value::POINTER(0)`. Either way, the change is localized
to those two sites (`DeltaInsert::GetGlobalSinkState` and
`DeltaTransaction::CommitCallback`).

### What does NOT change
- Generated header — never edited.
- `scripts/ffi/prefix.inc`, `suffix.inc`, generator — untouched.
- Kernel `GIT_TAG` — stays at `v0.23.0`.
- `CommitCallback` body — verbatim. Its `commit_info.version` will
  read as 0 on CTAS (kernel sets it on the `Commit` struct it
  produces from the staged-commit payload). Today's callback already
  uses `commit_info.version`; only the conditional null-check on
  `parent_table_entry` may need adjusting (see Open Question 1).

### Callback re-entrancy
- The kernel calls `CommitCallback` synchronously from within
  `ffi::create_table_commit` (NOT from
  `create_table_builder_build_with_committer` — the build step only
  registers the committer; the commit step invokes it). This is the
  same shape as INSERT. The callback transfers control into
  `commit_function->functions.functions[0].function(...)` (line 403 of
  `delta_transaction.cpp`), which executes the parent-catalog table
  function inline. **Lock contract**: `DeltaTransaction::lock` is NOT
  held across the `ffi::create_table_commit` call (it's held only
  during state mutation, not FFI calls — same as today's INSERT
  Commit).

### String lifetimes
- `ctas_table_path` is a `std::string` field on `DeltaTransaction`
  (set in `InitializeForNewTable`) — outlives the FFI call.
- `unity_table_id` is a `std::string` field set at attach time —
  outlives the FFI call.
- The fallback ternary creates a `KernelStringSlice` from whichever
  `std::string` is non-empty; both are on `DeltaTransaction` so the
  slice is valid for the duration of `get_uc_committer`.

---

## 8. Concurrency Plan — no changes

The existing CTAS concurrency model already covers CCv2 CTAS:
- `DeltaInsert::ParallelSink() == false` — single-threaded sink. No
  new locks introduced.
- `CommitCallback` runs synchronously on the calling thread during
  `ffi::create_table_commit`; the kernel does not spawn a thread.
- `DeltaTransaction::lock` is taken only by `SetParentTableEntry` and
  by `HasOutstandingAppends` / `GetTableEntry` / `InitializeTableEntry`
  — none of these are entered during the FFI commit.

The `current_context` weak_ptr is set in `InitializeForNewTable`
(`delta_transaction.cpp:700`) before the kernel commit runs in
`Commit()`, so `CommitCallback`'s `current_context.lock()` returns a
valid context on the CTAS path. No change.

---

## 9. Error Strategy

| Failure                                                                              | Exception type                                | Where                                                       |
|---|---|---|
| `parent_commit=true` and `commit_function` not registered on parent catalog          | `InternalException`                           | `DeltaCatalogAttach` (existing, line 74)                    |
| `get_uc_committer` returns Err (e.g., bad `table_id`)                                | `IOException` via `TryUnpackResult`           | `InitializeForNewTable` CCv2 branch (NEW)                   |
| `create_table_builder_build_with_committer` returns Err                              | `IOException` via `TryUnpackResult`           | `InitializeForNewTable` CCv2 branch (NEW)                   |
| `create_table_commit` returns Err AND `active_error` set (from `CommitCallback`)     | `active_error.Throw()` (preserves caller-side `DuckDBException` typing) | `Commit` CTAS branch (EXISTING — line 456-460) |
| `create_table_commit` returns Err AND `active_error` empty (kernel-internal failure) | `IOException` via `TryUnpackResult`           | `Commit` CTAS branch (EXISTING)                             |
| Parent commit function returns NULL (commit conflict)                                | propagated to kernel as Err string from `CommitCallback`; resurfaces in `Commit` as `IOException` | `Commit` CTAS branch (EXISTING flow)                        |
| Parent commit function throws an exception                                           | caught by `CommitCallback` exception barrier; stashed in `active_error`; resurfaces in `Commit` as the original `DuckDBException` type | `Commit` CTAS branch (EXISTING flow) |

All exceptions are at "query-terminating" level — there's no
non-fatal failure mode introduced by CCv2 CTAS that the existing
catalog-managed INSERT path doesn't already have.

---

## 10. Test Plan

### 10.1 LIST / MAP / nested-type tests — `ctas_complex_types.test` (NEW)

**Fixture set**: `test/sql/main/writing/ctas/` (main fixture set; no
external data required).

```
require parquet
require delta
require notwindows
```

**Cases** (each asserts count + round-trip after re-attach):

1. **LIST of INTEGER** — explicit user ask. CTAS with
   `[1, 2, 3] AS xs`. Assert `xs[1] == 1`, `xs[3] == 3` after
   re-attach. Also assert NULL inside list: `[1, NULL, 3]`.
2. **LIST of VARCHAR** — `['a','b'] AS tags`. Assert join with the
   list, the element type, and re-attach round-trip.
3. **MAP varchar to integer** — explicit user ask.
   `MAP {'a': 1, 'b': 2} AS counts`. Assert `counts['a'] == 1` after
   re-attach.
4. **LIST of STRUCT** — exercises the visitor's recursion through
   array-element-into-struct. `[{'k':1}, {'k':2}] AS items`.
5. **STRUCT containing LIST** — `{'tags': ['x','y']} AS payload`.
6. **MAP varchar to STRUCT** —
   `MAP {'k': {'n': 1, 's': 'one'}} AS lookup`.
7. **Empty LIST** — `CAST([] AS INTEGER[]) AS xs`. Verifies the
   visitor's array path with no element data.
8. **Empty MAP** — `MAP() AS m` (DuckDB syntax) or
   `CAST(MAP {} AS MAP(VARCHAR, INTEGER)) AS m`. Verifies the
   visitor's map path with zero entries.

**Why these cover the visitor gaps**: per
`/workspace/src/storage/delta_create_table_schema.cpp` lines
133-141 (LIST), 142-153 (MAP), 121-132 (STRUCT) — none of these are
exercised by any existing CTAS test. The existing `ctas_kernel_native.test`
covers a single-level STRUCT; this file adds LIST, MAP, and nested
combinations.

### 10.2 Partitioned CTAS — `ctas_partitioned.test` (NEW)

`PlanCreateTableAs` accepts partition keys (`delta_catalog.cpp:140-158`)
and `DeltaInsert::AppendForNewTable` builds `WriteMetaData` with
`ctas_partition_columns` (`delta_transaction.cpp:721-725`). No CTAS test
exercises this path today.

**Cases**:

1. **Single-column partition** —
   ```sql
   CREATE TABLE t.t AS SELECT * FROM (VALUES (1,'a'),(2,'b'),(3,'a'))
     v(id, part) PARTITIONED BY (part);
   ```
   Assert: file layout on disk includes `part=a/` and `part=b/` dirs;
   row counts per partition match; re-attach round-trips.
2. **Two-column partition** — `(year, month)`. Assert nested
   directory layout (`year=2024/month=01/...`).
3. **NULL partition value** — one row with `part = NULL`. Assert the
   kernel-emitted partition representation (Delta uses
   `__HIVE_DEFAULT_PARTITION__` for NULL) round-trips correctly: the
   re-attached table reports the NULL row.
4. **Empty CTAS with partitions** —
   `CREATE TABLE t.t AS SELECT 1 id, 'x' part WHERE false PARTITIONED BY (part);`.
   Assert: table is created (Protocol+Metadata commit) with the
   partition spec but no Add actions, and `SELECT COUNT(*) == 0`.
5. **Partition column not in select list — error** —
   `... PARTITIONED BY (missing);`. Already an existing branch in
   `delta_catalog.cpp:155-157` throwing `BinderException`. Assert error
   message.
6. **Partition column with special chars in value** — value containing
   `=`, space, slash. Assert URL-encoding of partition dir, round-trip.

### 10.3 Other reachable-but-untested visitor paths — folded into `ctas_complex_types.test`

A second batch of cases in the same file:

7. **BLOB column** — `CAST('hello' AS BLOB) AS data`. Exercises
   `visit_field_binary` (`delta_create_table_schema.cpp:111-113`).
8. **ENUM column** — DuckDB ENUMs map to `visit_field_string`
   (`delta_create_table_schema.cpp:82-85`). CTAS with
   `CAST('mood1' AS my_enum_t) AS mood`. Assert: the column reads back
   as VARCHAR after re-attach (Delta has no ENUM concept).
9. **TIMESTAMP_TZ column** — exercises the
   `LogicalTypeId::TIMESTAMP_TZ` arm of `VisitField`
   (`delta_create_table_schema.cpp:90-92`). Round-trip a known UTC
   timestamp value.

### 10.4 CCv2 CTAS — `ctas_ccv2.test` (NEW, conditional)

**Test fixture design — the central open question** (Section 11 OQ1).
DuckDB-side sqllogic tests can register table functions ONLY through
loaded extensions (no in-test `CREATE TABLE FUNCTION` for native
table functions returning the special chunk shape that
`CommitCallback` invokes). Three strategies considered:

**Strategy (a): self-contained sqllogic with a DuckDB SQL macro**

Use a DuckDB **table macro** (`CREATE MACRO`) to define
`__internal_delta_ccv2_commit_staged` in the test, then attach the
delta catalog with `parent_catalog` pointing at the macro's host
database. DuckDB macros can return a table; we just need it to accept
a struct row and emit a row whose first column is the original input
and whose second column is BOOLEAN (success). The body would
manually copy `staged_commit_path` to the published location.

**Verdict: INFEASIBLE.** The `CommitCallback` body
(`delta_transaction.cpp:402-403`) invokes
`commit_function->functions.functions[0].function(*current_context,
data, output)` directly — a raw C++ function pointer call into the
table function's bound execution function. A SQL `MACRO` resolves to a
view-style expression substitution, not a `TableFunction`-shaped
executor. So a macro cannot be discovered through
`EntryLookupInfo(TABLE_FUNCTION_ENTRY, ...)` at attach time
(`delta_extension.cpp:68-76`). Discarded.

**Strategy (b): test-only C++ table function registered by the extension itself**

Register a hidden `__internal_delta_ccv2_commit_staged` table function
**inside the delta extension** but only when an environment variable
(`DELTA_TEST_CCV2_FIXTURE=1`) is set, or unconditionally with a leading
underscore that signals "internal." The function's body just:
(i) reads `staged_commit_path` from the input row, (ii) reads the
file's content, (iii) writes it to the published path
(`_delta_log/00000000000000000000.json` for version 0, etc.),
(iv) returns a row with the input plus `TRUE`. This is exactly what
a real catalog would do, minus the catalog-side metadata update.

**Verdict: FEASIBLE BUT SCOPE CREEP.** This adds ~80-120 LOC of
production-shipped C++ for a test fixture. The function would have to
be hidden from user-facing surface — a leading-underscore convention
plus a comment is the canonical DuckDB pattern (cf.
`__internal_delta_ccv2_commit_staged` itself). The function lands in
`src/functions/delta_transaction_utils/ccv2_test_committer.cpp`.

**Recommendation**: implement Strategy (b) as a separate file ring-fenced
from production paths. Name it `__internal_delta_test_ccv2_commit_staged`
(different name than the real one — opt-in via `parent_commit=true,
parent_catalog='<memory db>', use a different table function name`).
Actually, simpler: register `__internal_delta_ccv2_commit_staged` itself
under a parent-catalog DuckDB database; attach to the parent first,
attach delta with `parent_catalog='<parent>'`. The function executes a
local FS copy from staged to published. Add a `KNOWN_ISSUES.md` note
that the function is a test-only commit promoter.

**Strategy (c): skip with documented rationale + leave path as
`statement error: NotImplementedException` to assert the CCv2 wiring
intent without exercising it**

Discarded: the entire point of this PR is to land the wiring, so
shipping it with no test surface contradicts the goal.

**Decision**: pursue Strategy (b). cpp-coder implements
`__internal_delta_ccv2_commit_staged` as a test-fixture table function
registered by the delta extension. Its body is a file-copy from
staged path to the published log path. Sqllogic test:

```
require parquet
require delta
require notwindows

# Set up a parent-catalog database with the test commit-staged function.
statement ok
ATTACH ':memory:' AS parent;

# Attach delta with parent_commit=true and the parent catalog pointing at the in-memory db.
statement ok
ATTACH '__TEST_DIR__/ccv2/t1' AS t (TYPE delta, allow_create true, parent_commit true,
                                    parent_catalog 'parent', unity_table_id 'test-table-id');

statement ok
CREATE TABLE t.t AS SELECT 1 AS id, 'a' AS name UNION ALL SELECT 2, 'b';

# After commit, the published log entry must exist (the test stub commit fn copies it there).
query I
SELECT count(*) > 0 FROM glob('__TEST_DIR__/ccv2/t1/_delta_log/00000000000000000000.json');
----
true

query II rowsort
SELECT id, name FROM t;
----
1	a
2	b

statement ok
DETACH t;
statement ok
DETACH parent;

# Re-attach and confirm the table reads back correctly.
statement ok
ATTACH '__TEST_DIR__/ccv2/t1' AS tr (TYPE delta);

query II rowsort
SELECT id, name FROM tr;
----
1	a
2	b

statement ok
DETACH tr;
```

Plus an empty-CTAS case and a case where the parent commit function
returns NULL (commit conflict simulation) — assert
`statement error` on the CTAS with a kernel-emitted commit-conflict
message.

If the orchestrator considers Strategy (b)'s extra production-shipped
LOC unacceptable, fall back to **NO test for CCv2 CTAS in this PR**;
the wiring still ships, and a follow-up adds the fixture. Mark this as
a test gap in `KNOWN_ISSUES.md`. This is acceptable because:
- The CCv2 INSERT path also lacks sqllogic tests today (verified by
  `grep -rl parent_commit /workspace/test/sql/` returning empty); we're
  not regressing test coverage, just not improving it.
- The CCv2 CTAS branch is structurally identical to the CCv2 INSERT
  branch (both go through `CommitCallback`); landing the wiring without
  a test is no worse than the status quo.

### 10.5 Regression net

Re-run after the changes:
- `test/sql/main/writing/ctas/*` — 8 existing tests (109 assertions),
  PLUS the 2 new ones, PLUS the CCv2 one (if Strategy b ships).
- `test/sql/main/writing/*` — confirm INSERT-side CCv2 stays unchanged
  (the only INSERT-touched line is `delta_insert.cpp:412-415`'s
  `SetParentTableEntry`, which we extend not modify).
- `test/sql/dat/*`, `test/sql/delta_kernel_rs/*`, `test/sql/issues/*`
  — confirm no regressions to read paths.

No CCv2 INSERT test exists today to regress against — that's a
broader test-coverage gap orthogonal to this PR.

---

## 11. Migration Order (for cpp-coder)

Land in this order so each step is independently testable and a fault
in step 3 doesn't block steps 1-2:

1. **Step 1 — Add LIST/MAP/STRUCT/partitioned tests** (NO CODE CHANGE).
   Run against the current `main` build. These lock in current
   behavior. If any of them surface a latent bug in the visitor, file
   it as a follow-up and either fix in this PR or punt to a separate
   PR depending on severity.

2. **Step 2 — Wire CCv2 CTAS code change** (the ~30 LOC edit to
   `delta_transaction.cpp` + ~5 LOC to `delta_insert.cpp`). Verify the
   non-CCv2 path is unchanged by re-running all existing CTAS tests.
   At this point the CCv2 branch is present but has no test exercising
   it.

3. **Step 3 — Land the CCv2 test fixture** (Strategy b — register
   `__internal_delta_ccv2_commit_staged` in the delta extension). Add
   `ctas_ccv2.test`. The fixture function lives in a new file
   `src/functions/delta_transaction_utils/ccv2_test_committer.cpp`.
   Update `CMakeLists.txt` source list.

If Step 3 is rejected by the orchestrator, ship Steps 1-2 and document
the CCv2 CTAS test gap in `KNOWN_ISSUES.md`.

---

## 12. Open Questions

1. **CCv2 CTAS `parent_table_entry`: what pointer goes into
   `Value::POINTER` in the staged-commit struct?**

   The INSERT-side passes the parent-catalog `TableCatalogEntry`
   (`delta_insert.cpp:414`). For CTAS, no such entry exists yet on the
   parent catalog. Three sub-options:

   (i) Pass a pointer to the DELTA-side `DeltaTableEntry` once we
   construct it post-commit. Problem: the post-commit table entry
   doesn't exist during `CommitCallback` — it's constructed AFTER the
   commit lands. The staged-commit struct must be filled in BEFORE
   the commit lands. (ii) Pass `nullptr` and loosen
   `CommitCallback`'s non-null assertion to "non-null required only
   when version > 0". The parent commit function must accept a NULL
   table_entry_pointer for CTAS and infer it from the
   `staged_commit_path`. (iii) Pass a pointer to the
   `DeltaSchemaEntry` (which always exists) or the `DeltaCatalog`
   itself, and the parent commit function distinguishes by the
   version field.

   **Recommendation: (ii) — pass nullptr.** Rationale: the
   `table_entry_pointer` is opaque to the kernel; only the parent
   commit function reads it. For version-0 (CTAS) commits, the parent
   has no existing table to look up — the commit IS the table
   registration. NULL is the right signal for "this commit creates the
   table; you don't have a prior entry to reference." Loosen
   `CommitCallback`'s assertion at
   `delta_transaction.cpp:359-361` to:
   `if (commit_info.version > 0 && !transaction->parent_table_entry) throw ...`.
   The parent commit function in production (Unity Catalog,
   etc.) needs to handle this convention; it's a one-line spec
   addition.

   **Confirm with orchestrator before cpp-coder edits the
   CommitCallback assertion.** This is the highest-stakes question;
   alternative resolutions:
   - If the orchestrator confirms a real parent catalog already
     uses pattern (ii), proceed.
   - If the orchestrator wants pattern (iii), the
     `SetParentTableEntry` call in
     `DeltaInsert::GetGlobalSinkState` CTAS arm passes the schema entry
     or catalog pointer; `CommitCallback` is unchanged. Trade-off: the
     parent commit function must polymorphically interpret the
     pointer based on `version`, which is slightly more error-prone
     than NULL-on-CTAS.

2. **CCv2 CTAS test fixture: Strategy (b) okay to ship?**

   Strategy (b) adds ~80-120 LOC of test-only C++ permanently to the
   delta extension binary. Acceptable trade-off, or prefer to ship
   the wiring without a test (Strategy (c)) and add the fixture in a
   follow-up?

   **Recommendation: Strategy (b).** Reasoning: shipping CCv2 CTAS
   wiring without ANY test surface means we discover bugs in the
   field. The test stub is small and clearly labeled
   ("__internal_delta_..." prefix matches the production convention).

3. **Strategy (b) test fixture name collision.**

   If we register `__internal_delta_ccv2_commit_staged` in our own
   extension AND the user has the real Unity Catalog extension
   loaded, both will define a table function of the same name. DuckDB
   will throw a `CatalogException` on the second registration.

   **Mitigation**: register the test stub under a DIFFERENT name
   (e.g., `__internal_delta_test_ccv2_commit_staged`) and configure
   `delta_extension.cpp:68` to use whichever name is requested via a
   new ATTACH option `parent_commit_function_name` (defaults to
   `__internal_delta_ccv2_commit_staged`). The test attaches with
   `parent_commit_function_name='__internal_delta_test_ccv2_commit_staged'`.
   This costs one new ATTACH option (small) but avoids the collision.

   **Recommendation: add `parent_commit_function_name` ATTACH option
   in this PR**. Wiring change is one line in `delta_extension.cpp`
   plus one field on `DeltaCatalog`.

4. **Pre-existing latent leak on INSERT-side
   `get_uc_commit_client` error path** (carried over from §4).

   When `get_uc_committer` returns Err, `get_uc_commit_client`'s
   returned handle is not freed by the INSERT path
   (`delta_transaction.cpp:566-575`). The same hole exists in the new
   CCv2 CTAS branch by symmetry.

   **Recommendation**: defer fixing to a separate PR; flag in
   `KNOWN_ISSUES.md`. The leak is one client handle per failed
   transaction (rare error path), and fixing it requires confirming
   the kernel-side ownership contract via Rust source inspection.

5. **`unity_table_id` fallback for CTAS — table path or attach name?**

   For INSERT (`delta_transaction.cpp:567`) the fallback is
   `path` (snapshot path, which is the delta-protocol URI). For CTAS
   we have `ctas_table_path` which is the same shape (delta-protocol
   URI of the new table). The plan uses `ctas_table_path` —
   symmetric with INSERT. **No open question if confirmed.**

6. **`get_uc_committer` consumes-on-success-only contract** (carried
   over from §4).

   The kernel docs on `free_uc_commit_client`
   (`generated_delta_kernel_ffi.hpp:1712-1714`) imply the client is
   freed only by explicit `free_uc_commit_client` OR when consumed by
   a build call. The INSERT path lets `get_uc_committer` consume on
   success and otherwise leaks (Open Question 4). Same behavior
   here. Verify in `ffi/src/delta_kernel_unity_catalog.rs:206-225`
   that the consumption is at the success-return path.

7. **Should `ctas_complex_types.test` and `ctas_partitioned.test` be
   added in a SEPARATE prior PR, not bundled with CCv2 wiring?**

   These two tests are pure test additions; they could land in a
   minimal PR while CCv2 CTAS goes through more careful review.

   **Recommendation: keep bundled — they're small, complementary,
   and the user requested both in one ticket.** If the CCv2 piece
   blocks on Open Question 1 or 3, ship the test additions first by
   splitting the PR.

---

## Appendix A: Cross-references

- Existing CTAS retirement plan:
  `/workspace/.agent-output-ctas-retirement-2026-05-16/001-architecture-plan.md`
  Open Question 1 (lines 828-848) is the deferral this plan closes.
- Existing CTAS retirement summary:
  `/workspace/.agent-output-ctas-retirement-2026-05-16/005-summary.md`
  "Items for Human Review" #1 and "Outstanding Medium/Low" L2 are the
  exact gaps this plan addresses.
- INSERT-side CCv2 reference:
  `/workspace/src/storage/delta_transaction.cpp:564-575`.
- CCv2 ATTACH wiring:
  `/workspace/src/delta_extension.cpp:65-77`.
- `CommitCallback`:
  `/workspace/src/storage/delta_transaction.cpp:347-438`.
- Existing CTAS `InitializeForNewTable`:
  `/workspace/src/storage/delta_transaction.cpp:634-701`.
- CTAS sink-state init (where `SetParentTableEntry` would be called):
  `/workspace/src/storage/delta_insert.cpp:78-107`.
- INSERT-side `SetParentTableEntry` call site:
  `/workspace/src/storage/delta_insert.cpp:412-415`.
- Visitor entry points (uncovered): LIST at
  `/workspace/src/storage/delta_create_table_schema.cpp:133-141`; MAP at
  lines 142-153.
- Partition CTAS pipeline (uncovered):
  `/workspace/src/storage/delta_catalog.cpp:140-158` (validation) and
  `/workspace/src/storage/delta_transaction.cpp:721-725` (write-metadata
  partition column names).
- Kernel FFI source for the `_with_committer` dispatch:
  `/workspace/build/debug/rust/src/delta_kernel/ffi/src/transaction/mod.rs:581-602`.
- Kernel UC committer impl:
  `/workspace/build/debug/rust/src/delta_kernel/ffi/src/delta_kernel_unity_catalog.rs:206-234`.
