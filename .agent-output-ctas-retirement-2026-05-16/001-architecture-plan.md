# Architecture Plan: Retire hand-rolled CTAS JSON path → kernel-native `get_create_table_builder`

Step B of the v0.21 → v0.23 kernel bump. Step A landed (commit `dc387dc` chain
+ unreviewed pin bump confirmed by `005-summary.md`). This plan retires
`src/storage/delta_ctas.{cpp,hpp}` (~242 lines, 171 of which are JSON
serialization) and routes the version-0 commit through the v0.22+ kernel
`get_create_table_builder(path, &EngineSchema, engine_info, engine)` family.

The FFI surface listed below has been **verified against the
locally-generated header** at
`/workspace/build/debug/codegen/include/generated_delta_kernel_ffi.hpp`
(lines 145–163, 822–825, 2645, 2653–2745, 2783–2794, 2871). All symbol
spellings and signatures cited in this plan come from that header.

---

## 1. Domain Constraints

### Delta Lake (protocol — what we now delegate to the kernel)
- The first commit `_delta_log/00000000000000000000.json` must contain
  Protocol + Metadata + (optional) Add actions, with `engineInfo`,
  `operation = "CREATE TABLE"`, and partition columns recorded in
  Metadata. The kernel's `CreateTable` flow assembles all of this from
  the schema + table-properties we hand it. We no longer write or even
  see the JSON text.
- Atomicity for "table-must-not-exist": kernel's default
  `FileSystemCommitter` performs a put-if-absent / rename-if-absent on
  the local FS or a conditional PUT on S3. The hand-rolled
  `FILE_FLAGS_FILE_CREATE_NEW` guarantee is preserved by the kernel
  (verified by reading the upstream `FileSystemCommitter::commit_path`
  semantics — kernel returns a typed `CommitConflict` error on race).
- Empty CTAS (zero rows) is valid Delta: build → commit, no
  `create_table_add_files` call. The kernel writes Protocol+Metadata
  only.
- The kernel picks `minReaderVersion` / `minWriterVersion` from the
  features it enables. We pass **zero** opt-in features in this PR;
  the kernel selects the minimum. This removes our hand-coded
  `(minReaderVersion=1, minWriterVersion=2)` and lets the kernel
  upgrade it if a column type forces it (e.g., timestamp_ntz).
- `add.path` is relative (v0.22 #2410). Already in effect post Step A;
  no further action.

### DuckDB (engine contract — unchanged)
- `PhysicalPlanGenerator::CreatePlan(LogicalCreateTable&)` calls
  `op.schema.catalog.PlanCreateTableAs(...)`. We return a single
  `PhysicalOperator` (`DeltaInsert` in CTAS mode) that sinks the
  parquet-write child plan.
- `PhysicalCopyToFile`'s child pipeline runs before
  `DeltaInsert::GetGlobalSinkState`, so the table-root directory must
  exist by plan time. (Already true; retained.)
- `DeltaInsertGlobalState::ParallelSink() == false` — single-threaded
  sink. Retained.
- `RETURNING` and `CREATE OR REPLACE` for CTAS continue to raise
  `BinderException` at plan time (unchanged).

### FFI boundary (v0.23, verified in the generated header)
| Symbol (line in generated header) | Shape |
|---|---|
| `EngineSchema { void *schema; uintptr_t (*visitor)(void*, KernelSchemaVisitorState*) }` (822–825) | Stack-allocated, engine-owned. Caller-provided visitor pushes types into kernel's `KernelSchemaVisitorState` via the `visit_field_*` family (2286–2493) and returns the **top-level struct field ID** (`uintptr_t`). |
| `get_create_table_builder(path, &EngineSchema, engine_info, engine) -> ExternResult<Handle<ExclusiveCreateTableBuilder>>` (2744–2747) | Does NOT consume schema — caller's struct stays on the stack. Engine handle held by snapshot. |
| `create_table_builder_with_table_property(builder, key, value, engine) -> ExternResult<Handle<ExclusiveCreateTableBuilder>>` (2759–2762) | Consumes the builder unconditionally (even on error). On error the builder is gone — DO NOT free. |
| `create_table_builder_build(builder, engine) -> ExternResult<Handle<ExclusiveCreateTransaction>>` (2773–2774) | Consumes builder. Default `FileSystemCommitter`. |
| `create_table_builder_build_with_committer(builder, committer, engine) -> ExternResult<Handle<ExclusiveCreateTransaction>>` (2783–2785) | Consumes BOTH handles. CCv2 path. |
| `create_table_with_engine_info(txn, engine_info, engine) -> ExternResult<Handle<ExclusiveCreateTransaction>>` (2660–2662) | Consumes & returns the txn handle. Optional — `get_create_table_builder` already accepts `engine_info`. |
| `create_table_add_files(txn, write_metadata)` (2671–2672) | Void. Consumes `Handle<ExclusiveEngineData>`. Does NOT consume txn. |
| `create_table_set_data_change(txn, bool)` (2680) | Void. Default is `true`; not called in this PR. |
| `create_table_commit(txn, engine) -> ExternResult<Handle<ExclusiveCommittedTransaction>>` (2692–2693) | **Same shape as v0.22 `commit`.** Consumes txn. Returned handle freed with `free_committed_transaction` (already wrapped by `KernelCommittedTransaction`, added in Step A). |
| `create_table_free_transaction(txn)` (2653) | Distinct from `free_transaction`. Used by RAII deleter on the rollback path. |
| `free_create_table_builder(builder)` (2794) | Used by RAII deleter for builder. |
| `visit_field_{string,long,integer,short,byte,float,double,boolean,binary,date,timestamp,timestamp_ntz,decimal,struct,array,map,variant}(state, name, ..., AllocateErrorFn) -> ExternResult<uintptr_t>` (2286–2492) | Each pushes a typed field into the kernel's visitor state and returns its ID. Decimal takes `(precision, scale)`; struct takes `(field_ids[], field_count)`; array/map take child IDs; variant takes a struct-type ID. |

Critical FFI invariants:
- **Builder chaining consumes-on-error.** `create_table_builder_with_table_property` consumes the builder even when it returns Err. The RAII wrapper must `release()` before each call and `reset()` only with the new handle on success. On error, the old handle is gone — do NOT free.
- **String slices are borrowed.** All `KernelStringSlice` inputs to FFI calls must point at backing `string`s that outlive the call. The schema visitor in particular calls `visit_field_*` from the kernel's stack frame; the `name` slices must remain valid until the visitor returns the root struct ID.
- **Visitor returns the root struct ID, not 0.** Per docs at line 913–914, `visit_schema` returns "the id of the list of top-level columns" — but for the *write-side* engine-provided visitor used by `get_create_table_builder`, the visitor must call `visit_field_struct` last with `(name="", field_ids=top_level_ids, count=N)` and return *that* ID (the docs at 2428–2430 confirm: "This function should _also_ be used to create the final schema element, where the field IDs of the top-level fields should be passed as `field_ids`. The name for the final schema element is ignored."). This is a **load-bearing invariant** for cpp-coder.
- **AllocateErrorFn.** Every `visit_field_*` takes an `AllocateErrorFn` so kernel-side validation failures (e.g., decimal precision out of range) can surface as `ExternResult::Err`. We pass `DuckDBEngineError::AllocateError` (existing helper, lines 25–28 of `delta_utils.hpp`).

---

## 2. Affected Surfaces

```
src/storage/
  delta_ctas.cpp                        DELETE (171 lines)
  delta_catalog.cpp                     MOD: drop `#include "storage/delta_ctas.hpp"`,
                                             drop `DeltaSchemaJson::ValidateColumnTypes` call,
                                             keep directory creation + parquet COPY plumbing.
  delta_schema_entry.cpp                MOD: drop `#include "storage/delta_ctas.hpp"`,
                                             drop `DeltaSchemaJson::BuildSchemaString` call.
                                             `version0_path` existence probe stays as the
                                             "table-already-exists" guard.
  delta_insert.cpp                      MOD: CTAS arm of GetGlobalSinkState rewritten;
                                             Finalize CTAS arm now calls a new
                                             `DeltaTransaction::AppendForNewTable` /
                                             commit via existing Commit().
  delta_transaction.cpp                 MOD: add `InitializeForNewTable()`,
                                             `AppendForNewTable()`,
                                             CTAS branch of Commit() / Rollback().
                                             Keep INSERT branch untouched.

src/storage/                            NEW
  delta_create_table_schema.cpp         NEW: write-side schema visitor —
                                             owns the column list, implements
                                             the C-callback that drives
                                             `visit_field_*`, exposes
                                             EngineSchema by value.

src/include/storage/
  delta_ctas.hpp                        DELETE
  delta_create_table_schema.hpp         NEW: class skeleton (see §5).
  delta_transaction.hpp                 MOD: enum DeltaTransactionMode,
                                             new fields kernel_create_txn (etc.),
                                             new methods.

src/include/
  delta_utils.hpp                       MOD: two new RAII typedefs
                                             (KernelExclusiveCreateTableBuilder,
                                              KernelExclusiveCreateTransaction).

CMakeLists.txt                          MOD: drop `src/storage/delta_ctas.cpp`,
                                             add `src/storage/delta_create_table_schema.cpp`.

scripts/ffi/                            NO CHANGE.
build/<cfg>/codegen/include/
  generated_delta_kernel_ffi.hpp        NO CHANGE (regenerated, but unchanged content).

test/sql/main/writing/ctas/
  basic_ctas.test                       UNCHANGED (existing assertions are SQL-visible).
  ctas_attach_existing.test             UNCHANGED.
  ctas_empty_select.test                UNCHANGED.
  ctas_or_replace_unsupported.test      UNCHANGED.
  ctas_then_insert.test                 UNCHANGED.
  ctas_type_coverage.test               EXTEND: drop `TIMESTAMP_NS` exclusion if
                                             we now accept it (kernel decides);
                                             add a STRUCT column (kernel-native
                                             path supports nested types).
  ctas_kernel_native.test               NEW: kernel-emitted commit shape
                                             (engineInfo, operation, isolationLevel)
                                             — see §9.
  ctas_ccv2.test                        (DEFERRED — see §10 decision; not in this PR)
```

---

## 3. Ownership Map

```
DeltaCatalog
└── DeltaSchemaEntry              unique_ptr
    └── DeltaTableEntry           unique_ptr  (constructed at sink-init time for CTAS)
        └── DeltaMultiFileList    shared_ptr  (POPULATED only after create_table_commit;
            │                                  pre-commit, the table_entry exists with
            │                                  a borrowed extern_engine but no snapshot
            │                                  — same shape as today's CTAS branch)
            ├── extern_engine     KernelExternEngine        (read-only borrow during CTAS)
            └── snapshot          SharedKernelPointer<SharedSnapshot>  (post-commit)

DeltaTransaction
├── mode                          DeltaTransactionMode enum  NEW
├── kernel_transaction            KernelExclusiveTransaction         (INSERT path; unchanged)
├── kernel_create_txn             KernelExclusiveCreateTransaction   NEW (CTAS path)
├── write_entry                   optional_ptr<DeltaTableEntry>      (borrow)
├── outstanding_appends           vector<DeltaDataFile>              (owned, shared between modes)
├── app_versions                  unordered_map<...>                 (REGULAR mode only — see §10 Q4)
├── parent_table_entry            optional_ptr<TableCatalogEntry>    (borrow, CCv2)
└── active_error                  ErrorData                          (CCv2 escape hatch)

DeltaInsert (CTAS arm)            PhysicalOperator
├── schema                        optional_ptr<SchemaCatalogEntry>   (borrow)
├── info                          unique_ptr<BoundCreateTableInfo>   (owned, consumed at sink init)
└── children                      PhysicalCopyToFile (standard DuckDB ownership)

Transient FFI handles inside DeltaTransaction::InitializeForNewTable
├── KernelExclusiveCreateTableBuilder    NEVER stored on a field; lives in a local
│                                        unique_ptr through the chain. Each
│                                        `with_table_property` releases() and rewraps;
│                                        on error the wrapper is empty (no double-free).
└── DeltaCreateTableSchema (stack-local)  Owns the ColumnList lifetime for the visitor
                                          call. After get_create_table_builder returns,
                                          the kernel no longer touches the visitor or
                                          the schema struct; it can drop on scope exit.
```

**`shared_ptr` audit:** the only `shared_ptr` is the existing
`shared_ptr<DeltaMultiFileList>` mandated by DuckDB's
`TableCatalogEntry` model. No new `shared_ptr` introduced.

**Kernel handle audit (one RAII owner per handle):**

| Kernel handle | RAII wrapper | Deleter (verified in header) | Lifetime |
|---|---|---|---|
| `Handle<ExclusiveCreateTableBuilder>` | `KernelExclusiveCreateTableBuilder` (new) | `ffi::free_create_table_builder` (2794) | Function-local to `InitializeForNewTable`; never stored. |
| `Handle<ExclusiveCreateTransaction>` | `KernelExclusiveCreateTransaction` (new) | `ffi::create_table_free_transaction` (2653) | Field on `DeltaTransaction` while mode == `CREATING_TABLE`. Released on `Commit` (consumed by `create_table_commit`) or freed on `Rollback`. |
| `Handle<ExclusiveCommittedTransaction>` | `KernelCommittedTransaction` (EXISTS — added Step A, `delta_utils.hpp:403-404`) | `ffi::free_committed_transaction` (2700) | Local in `Commit()`; we read the version and drop. |
| `Handle<MutableCommitter>` (CCv2) | unwrapped raw, consumed by `create_table_builder_build_with_committer` | `ffi::free_uc_committer` (1734, if needed) | Built inline, immediately consumed — matches existing INSERT-side pattern at `delta_transaction.cpp:511-515`. |
| `Handle<ExclusiveEngineData>` for `add_files` | `KernelEngineData` (existing) | `ffi::free_engine_data` | Released into `create_table_add_files`. |

**No `Handle<SharedSchema>` ever crosses the boundary** for CTAS in
v0.23 — the schema is passed by *engine-side* visitor, not as a kernel
handle. This is the core simplification vs. the v1 plan from
`/workspace/.agent-output-ctas-2026-05-16/001-architecture-plan-v2.md`,
which proposed a `KernelSharedSchema` wrapper.

---

## 4. Module Layout

```
src/
├── storage/
│   ├── delta_create_table_schema.cpp    NEW
│   │     - DeltaCreateTableSchema class implementation
│   │     - DuckDB LogicalType → visit_field_* dispatcher
│   │     - C-linkage trampoline (anonymous namespace) for EngineSchema.visitor
│   ├── delta_catalog.cpp                MOD: see §2
│   ├── delta_schema_entry.cpp           MOD: see §2
│   ├── delta_insert.cpp                 MOD: see §2 (CTAS arm of
│   │                                          GetGlobalSinkState + Finalize)
│   ├── delta_transaction.cpp            MOD: InitializeForNewTable(),
│   │                                          AppendForNewTable(), CTAS branch
│   │                                          of Commit() / Rollback()
│   └── delta_ctas.cpp                   DELETE
├── include/storage/
│   ├── delta_create_table_schema.hpp    NEW
│   ├── delta_transaction.hpp            MOD: see §5
│   └── delta_ctas.hpp                   DELETE
└── include/
    └── delta_utils.hpp                  MOD: two new TemplatedUniqueKernelPointer
                                                typedefs (no new template)
```

---

## 5. Key Types (skeletons only — no method bodies)

### 5.1 RAII additions in `src/include/delta_utils.hpp`

Inserted after the existing typedef block (`delta_utils.hpp:396-404`):

```cpp
typedef TemplatedUniqueKernelPointer<ffi::ExclusiveCreateTableBuilder,
                                     ffi::free_create_table_builder>
    KernelExclusiveCreateTableBuilder;

typedef TemplatedUniqueKernelPointer<ffi::ExclusiveCreateTransaction,
                                     ffi::create_table_free_transaction>
    KernelExclusiveCreateTransaction;
```

Symbol spellings verified: `free_create_table_builder` at
header line 2794, `create_table_free_transaction` at 2653. Both take
`Handle<T>` which (per the existing wrapper pattern at
`delta_utils.hpp:391-393` and `free_transaction` precedent) cbindgen
emits as `void (*)(T *)`. If the regenerated header instead emits
`void (*)(Handle<T>)`, the cpp-coder adopts the same wrapping
convention already used for `KernelExclusiveTransaction` (line 401).

### 5.2 `src/include/storage/delta_create_table_schema.hpp` (NEW)

```cpp
//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_create_table_schema.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_utils.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {

//! Write-side kernel schema builder. Owns the DuckDB ColumnList while the
//! kernel's create-table-builder invokes the engine-supplied visitor.
//!
//! Domain invariant: the visitor must walk the columns in declaration order,
//! call `visit_field_*` for each, then call `visit_field_struct` once more
//! with the collected field IDs to produce the root struct ID — that ID is
//! the visitor's return value (see header lines 2426–2443).
//!
//! Lifetime contract: the instance must outlive the kernel call that consumes
//! the EngineSchema. Constructed stack-local in
//! DeltaTransaction::InitializeForNewTable, dropped after
//! get_create_table_builder returns.
class DeltaCreateTableSchema {
public:
	explicit DeltaCreateTableSchema(const ColumnList &columns);

	//! Build the EngineSchema by-value descriptor. The returned struct
	//! borrows `this` as opaque schema pointer and `&DispatchVisit` as the
	//! kernel-callable visitor. The struct must not outlive `*this`.
	ffi::EngineSchema GetEngineSchema();

private:
	//! Single C-linkage trampoline the kernel invokes. Recovers `*this`
	//! from the opaque pointer and calls VisitImpl().
	static uintptr_t DispatchVisit(void *schema, ffi::KernelSchemaVisitorState *state);

	//! Walks the column list. Throws BinderException on an unsupported
	//! DuckDB LogicalType; the exception unwinds out of the kernel stack
	//! frame because the visitor is C-linkage-stable but our trampoline
	//! catches and stores the error before returning a sentinel.
	uintptr_t VisitImpl(ffi::KernelSchemaVisitorState *state);

	//! Translate a single DuckDB LogicalType into a kernel field-id by
	//! dispatching to the appropriate `visit_field_*` FFI call. Returns
	//! the field id; throws BinderException on unsupported types.
	uintptr_t VisitField(ffi::KernelSchemaVisitorState *state,
	                     const string &name, const LogicalType &type,
	                     bool nullable);

private:
	const ColumnList &columns;
	//! Storage for any std::string scratch needed to back KernelStringSlice
	//! values handed to the kernel (e.g., decimal field names that exist
	//! only in this visitor's lifetime). The ColumnList itself already
	//! owns its name strings.
	vector<string> scratch_names;

	//! Captured ErrorData if VisitImpl throws. The trampoline transfers
	//! this to a member, returns 0, and the outer caller checks
	//! TakeError() after the kernel call returns Err.
	ErrorData captured_error;

public:
	bool HasError() const;
	ErrorData TakeError();
};

} // namespace duckdb
```

The exception-barrier choice in `DispatchVisit` mirrors the existing
`DeltaTransaction::CommitCallback` pattern
(`delta_transaction.cpp:388-404`): catch all exceptions, stash into
`captured_error`, return a sentinel (here `0`). The kernel will surface
this as an `ExternResult::Err` from `get_create_table_builder`, and the
outer caller throws `captured_error` if non-empty (otherwise throws the
kernel's own error).

### 5.3 `src/include/storage/delta_transaction.hpp` (MOD)

```cpp
enum class DeltaTransactionMode : uint8_t {
	REGULAR,           //! Table exists; uses kernel_transaction. Default.
	CREATING_TABLE     //! Table is being created; uses kernel_create_txn.
};

class DeltaTransaction : public Transaction {
public:
	// ... existing public API unchanged ...

	//! CTAS-only: build the kernel ExclusiveCreateTableBuilder from the
	//! BoundCreateTableInfo (schema + partition_keys + table-properties
	//! map are all sourced from `info`), then transition the
	//! transaction to CREATING_TABLE mode. Idempotent for the same
	//! info; throws InternalException on a second call.
	//!
	//! Throws InvalidInputException if access_mode == READ_ONLY.
	//! Throws BinderException if the schema contains a type the kernel
	//! rejects (verified via captured_error from DeltaCreateTableSchema).
	//! Throws IOException for other kernel failures via TryUnpackResult.
	void InitializeForNewTable(ClientContext &context,
	                           DeltaTableEntry &new_table_entry,
	                           BoundCreateTableInfo &info);

	//! Stage parquet files onto kernel_create_txn via
	//! create_table_add_files. Builds the WriteMetaData arrow batch via
	//! the existing WriteMetaData helper. No-op if append_files is empty
	//! (empty-CTAS commits Protocol+Metadata only).
	void AppendForNewTable(ClientContext &context,
	                       const vector<DeltaDataFile> &append_files);

	bool IsCreatingTable() const { return mode == DeltaTransactionMode::CREATING_TABLE; }

private:
	DeltaTransactionMode mode = DeltaTransactionMode::REGULAR;

	//! Held only when mode == CREATING_TABLE. Consumed by Commit() via
	//! ffi::create_table_commit. Freed by destructor (RAII) on rollback.
	KernelExclusiveCreateTransaction kernel_create_txn;

	// ... existing private fields unchanged ...
};
```

`Commit()` gains a CREATING_TABLE branch (skeleton; not the
implementation):

```cpp
// Inside Commit(), as a peer of the existing
// `if (!outstanding_appends.empty()) { ... }` block:
if (mode == DeltaTransactionMode::CREATING_TABLE) {
    // app_versions are NOT replayed on the create-table txn — see §10 Q4.
    ffi::ExclusiveCommittedTransaction *committed = nullptr;
    auto res = KernelUtils::TryUnpackResult(
        ffi::create_table_commit(kernel_create_txn.release(),
                                 write_entry->snapshot->extern_engine.get()),
        committed);
    KernelCommittedTransaction wrap(committed);  // RAII for free path
    if (res.HasError()) {
        if (active_error.HasError()) {
            active_error.Throw();  // CCv2 escape hatch (same as INSERT)
        }
        res.Throw();
    }
    // Read version via ffi::committed_transaction_version(wrap.get()) if
    // we want it for logging; otherwise drop wrap.
}
```

`Rollback()` extension: if `mode == CREATING_TABLE`, the existing
`CleanUpFiles()` continues to remove staged parquet, and the
`kernel_create_txn`'s RAII deleter (`create_table_free_transaction`)
fires on scope exit / next assignment. No new explicit call needed.

### 5.4 `src/storage/delta_insert.cpp` — CTAS arm of `GetGlobalSinkState` (skeleton)

Current 65-line CTAS branch (lines 78–141) collapses to roughly:

```cpp
// CTAS path
D_ASSERT(schema);
D_ASSERT(info);

auto &delta_catalog = schema->catalog.Cast<DeltaCatalog>();
auto &delta_transaction = DeltaTransaction::Get(context, delta_catalog);

// The schema entry's CreateTable was already called by DuckDB before us
// (see PhysicalPlanGenerator pathway); it returned nullptr because we
// have no snapshot to bind yet. Build the in-memory DeltaTableEntry here.
auto &delta_schema_entry = const_cast<DeltaSchemaEntry &>(schema->Cast<DeltaSchemaEntry>());
auto new_table_entry =
    delta_schema_entry.BuildEmptyTableEntryForCtas(context, *info);
// Register in transaction (BuildEmptyTableEntryForCtas is new; mirrors
// CreateTableEntry but skips the snapshot init and builds from info).

// Drive the kernel builder + commit-table-builder chain.
delta_transaction.InitializeForNewTable(context, *new_table_entry, *info);

return make_uniq<DeltaInsertGlobalState>(*new_table_entry);
```

The `BuildEmptyTableEntryForCtas` helper on `DeltaSchemaEntry` is new
(see §5.5); it produces a `DeltaTableEntry` whose `snapshot` is **not
yet populated** but whose `extern_engine` is initialized so
`AppendForNewTable` and `Commit` have a usable engine handle. The
engine itself is engine-default (built via the same `get_engine_builder`
path used at snapshot load time), parameterized only by the catalog's
ATTACH options.

CTAS `Finalize`:

```cpp
SinkFinalizeType DeltaInsert::Finalize(...) {
    auto &global_state = input.global_state.Cast<DeltaInsertGlobalState>();
    auto &transaction = DeltaTransaction::Get(context, schema->catalog);
    transaction.AppendForNewTable(context, global_state.written_files);
    return SinkFinalizeType::READY;
}
```

Note: the existing `transaction.Append(context, global_state.written_files)`
call on the CTAS branch (line 349) is replaced by `AppendForNewTable`.
The INSERT branch (line 344) remains `Append`.

### 5.5 `src/include/storage/delta_schema_entry.hpp` extension

```cpp
class DeltaSchemaEntry : public SchemaCatalogEntry {
public:
    // ... existing API unchanged ...

    //! CTAS-only: build a DeltaTableEntry that has the user-declared
    //! schema from `info` but no kernel snapshot. The extern_engine is
    //! constructed eagerly (so InitializeForNewTable can use it); the
    //! DeltaMultiFileList is constructed in a "no snapshot loaded yet"
    //! state. A post-commit re-init (kernel-driven, after
    //! create_table_commit) populates the real snapshot.
    //!
    //! No FS touch beyond what extern_engine construction implies
    //! (auth/credential pickup from the catalog).
    unique_ptr<DeltaTableEntry>
    BuildEmptyTableEntryForCtas(ClientContext &context, BoundCreateTableInfo &info);

private:
    // ... unchanged ...
};
```

### 5.6 `src/storage/delta_schema_entry.cpp::CreateTable` simplification

The body becomes shorter — the `BuildSchemaString` call goes away. The
"already exists" probe stays (file-existence check is the cheapest
fast-fail; kernel would otherwise surface the same condition as an
`IOException` from `create_table_builder_build`, but a Catalog-level
error message at probe time is the right user experience):

```cpp
optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateTable(...) {
    // 1. CREATE OR REPLACE rejection (unchanged)
    // 2. Table-name == catalog-name check (unchanged)
    // 3. Partition-key validation (unchanged — purely DuckDB-side)
    // 4. PROBE: if _delta_log/00000000000000000000.json exists, throw
    //    CatalogException (unchanged — fast user-facing error)
    //
    // 5. NO MORE BuildSchemaString call. Kernel validates types at
    //    InitializeForNewTable time and surfaces BinderException via
    //    DeltaCreateTableSchema::captured_error.
    //
    // 6. Return nullptr — DuckDB routes through PlanCreateTableAs.
}
```

---

## 6. FFI Plan

### What does NOT change
- Generated header: untouched (we read it; the build pipeline regenerates).
- `scripts/ffi/{prefix.inc,suffix.inc,generate_delta_kernel_ffi_header}`: untouched.
- `delta-kernel-rs` `GIT_TAG`: stays at `v0.23.0` (Step A is in main; no
  further bump required).

### What handles cross the FFI for CTAS

Inside `DeltaTransaction::InitializeForNewTable`:

1. **Construct the engine-side schema visitor.**
   ```cpp
   DeltaCreateTableSchema schema_visitor(info.Base().columns);
   ffi::EngineSchema engine_schema = schema_visitor.GetEngineSchema();
   ```
   `engine_schema` is by-value; the kernel reads `engine_schema.visitor`
   and `engine_schema.schema` synchronously inside
   `get_create_table_builder`. No handle, no allocation.

2. **Get the builder.** Builder handle is wrapped immediately in RAII:
   ```cpp
   ffi::ExclusiveCreateTableBuilder *builder_raw = nullptr;
   auto res = KernelUtils::TryUnpackResult(
       ffi::get_create_table_builder(KernelUtils::ToDeltaString(path),
                                     &engine_schema,
                                     KernelUtils::ToDeltaString("DuckDB"),
                                     extern_engine.get()),
       builder_raw);
   KernelExclusiveCreateTableBuilder builder(builder_raw);
   if (res.HasError()) {
       if (schema_visitor.HasError()) {
           // BinderException from DuckDB-side type rejection,
           // surfaces ahead of the kernel's generic error.
           schema_visitor.TakeError().Throw();
       }
       res.Throw();  // IOException via TryUnpackResult mapping
   }
   ```

3. **(Optional, skipped in this PR) Apply table properties.** Skeleton
   for future use; not exercised in this PR's first commit:
   ```cpp
   // for each (key, value) in info.Base().properties (if non-empty):
   ffi::ExclusiveCreateTableBuilder *next_raw = nullptr;
   auto prop_res = KernelUtils::TryUnpackResult(
       ffi::create_table_builder_with_table_property(
           builder.release(),  // CONSUMED unconditionally
           KernelUtils::ToDeltaString(key),
           KernelUtils::ToDeltaString(value),
           extern_engine.get()),
       next_raw);
   builder = KernelExclusiveCreateTableBuilder(next_raw);  // empty on err
   if (prop_res.HasError()) prop_res.Throw();
   ```

4. **Build the create-table transaction.**
   - **Default path (`parent_commit == false`):**
     ```cpp
     ffi::ExclusiveCreateTransaction *txn_raw = nullptr;
     auto build_res = KernelUtils::TryUnpackResult(
         ffi::create_table_builder_build(builder.release(),
                                          extern_engine.get()),
         txn_raw);
     kernel_create_txn = KernelExclusiveCreateTransaction(txn_raw);
     if (build_res.HasError()) build_res.Throw();
     ```
   - **CCv2 path (`parent_commit == true`):** see §10 decision (deferred);
     code shape mirrors `delta_transaction.cpp:511-515` but feeds the
     resulting `MutableCommitter` handle into
     `create_table_builder_build_with_committer`. Same `CommitCallback`
     is reused (the kernel's commit payload shape is mode-agnostic).

5. **Transition mode + record write entry.**
   ```cpp
   mode = DeltaTransactionMode::CREATING_TABLE;
   write_entry = &new_table_entry;
   transaction_state = DeltaTransactionState::TRANSACTION_STARTED;
   ```

`create_table_with_engine_info` is **not** called separately —
`engine_info` is already passed to `get_create_table_builder`
(header line 2746). The two function entry points are conveniences for
late binding; we set it once at builder creation and never again.

Inside `DeltaTransaction::AppendForNewTable`:

6. **Stage data files.** Same `WriteMetaData` arrow assembly as the
   INSERT path (delta_transaction.cpp:185-300), then:
   ```cpp
   ffi::create_table_add_files(kernel_create_txn.get(),
                                write_metadata_engine_data.release());
   ```
   Note: `create_table_add_files` is `void` (header line 2671) — it does
   not surface errors via `ExternResult`. Kernel-side errors (malformed
   stats, etc.) become commit-time errors in step 7. This matches the
   INSERT path's `ffi::add_files` (also void, line 575 of
   `delta_transaction.cpp`).

Inside `DeltaTransaction::Commit()` (CTAS branch):

7. **Commit.** See §5.3 skeleton. The `committed_transaction_version`
   call is informational; we drop the returned handle via RAII without
   reading the post-commit snapshot (it's available via
   `committed_transaction_post_commit_snapshot` if future work wants
   to skip the next `InitializeSnapshot` round-trip).

### Callback re-entrancy
- The CTAS path's only callback into our code is the
  `EngineSchema.visitor` trampoline (synchronous, runs entirely inside
  `get_create_table_builder`).
- CCv2 (deferred) reuses the existing `DeltaTransaction::CommitCallback`
  (`delta_transaction.cpp:314-405`). The kernel-side `CommitRequest`
  payload is identical for the create-table commit path — verified by
  reading the v0.23 kernel `transaction/create_table.rs` flow against
  the regular `transaction/mod.rs::commit` flow (both call into the
  same `Committer` trait).

### String lifetimes
- `path` and `engine_info` are local `string`s in `InitializeForNewTable`
  that outlive the FFI call.
- Field names handed to `visit_field_*` are slices into the
  `ColumnList`'s own strings (which the `BoundCreateTableInfo` owns for
  the duration of the bind / plan / sink-init).
- The struct's final-flatten name (always empty per docs at header line
  2430) is a `KernelStringSlice` of an empty literal — backed by
  `string` member on the visitor.

### `prefix.inc` / `suffix.inc` / generator
- No changes.

### Generator-header verification checklist for cpp-coder
After `make debug` (which forces a kernel rebuild on first compile),
spot-check the regenerated header at
`build/debug/codegen/include/generated_delta_kernel_ffi.hpp` for:
1. Line 822: `struct EngineSchema { void *schema; uintptr_t (*visitor)(...) }` — confirm shape.
2. Line 2744: `get_create_table_builder(path, &EngineSchema, engine_info, engine)`.
3. Lines 2653, 2700, 2794: deleter spellings for the three handle types.
4. Line 2692: `create_table_commit` returns `ExternResult<Handle<ExclusiveCommittedTransaction>>` (same shape as `commit` at 2645 — Step A's `KernelCommittedTransaction` wrapper works unmodified).
5. Lines 2286–2492: `visit_field_*` signatures match the dispatch table in `DeltaCreateTableSchema::VisitField`.

If anything drifts: adjust the typedef block in `delta_utils.hpp` and
the dispatcher in `delta_create_table_schema.cpp`. No other source file
needs to change.

---

## 7. Concurrency Plan

### State split (unchanged from INSERT path)
- **Bind data:** `BoundCreateTableInfo` (DuckDB-owned, immutable after
  bind). Held in `DeltaInsert::info`.
- **Global sink state:** single `DeltaInsertGlobalState`. CTAS arm
  populates `columns` from `info->Base().columns` (existing code path
  via `BuildEmptyTableEntryForCtas` → `DeltaTableEntry` → `GetColumns()`).
- **Local sink state:** none. `ParallelSink() == false`.

### Lock ordering
- `DeltaTransaction::lock` is acquired only around mutations of
  `kernel_create_txn`, `mode`, `write_entry`, and `outstanding_appends`.
- **Never hold `lock` across any `ffi::*` call.** Specifically:
  - `InitializeForNewTable` takes the lock to install
    `kernel_create_txn`/`mode`/`write_entry` AFTER all FFI calls return.
    The FFI chain (`get_create_table_builder` → `_build`) runs without
    the lock held; the local RAII wrappers ensure no leak if a peer
    thread interleaves.
  - `AppendForNewTable` mirrors `Append` (delta_transaction.cpp:539-577):
    lock-free arrow build + FFI call; lock only for
    `outstanding_appends` mutation.

### Kernel re-entry
- The visitor trampoline runs synchronously on the calling thread's
  stack inside `get_create_table_builder`. No lock is held at that
  instant. The visitor itself does not take any DuckDB lock.
- CCv2 path (deferred) re-enters via `CommitCallback` exactly as today.

### Cancellation
- `DeltaInsert::ParallelSink() == false`. No fan-out cancellation.
- Long-ish kernel calls on CTAS path:
  - `get_create_table_builder` + `create_table_builder_build` —
    cheap (no I/O for the FileSystemCommitter — directory probe only).
  - `create_table_add_files` — in-memory, no I/O.
  - `create_table_commit` — the actual I/O. Cancellation here is the
    same as the existing INSERT commit: the kernel call is
    non-interruptible; `InterruptState` is checked before/after, not
    during. Same UX as Step A; no regression.

### Rollback contract
- On `Rollback`, the destructor of `KernelExclusiveCreateTransaction`
  fires (`create_table_free_transaction`), dropping the kernel-side
  staging state. The kernel never published the commit, so no
  `_delta_log/00000000000000000000.json` exists. `CleanUpFiles()`
  removes the staged parquet (unchanged from INSERT path).
- We do **not** `rmdir` the table-root directory on rollback. The user
  may have other content there (e.g., a partially-failed second-CTAS
  retry into the same `allow_create=true` mount).

---

## 8. Error Strategy

| Failure                                                                  | Exception type                                | Where                                              |
|---|---|---|
| `RETURNING` clause on CTAS                                               | `BinderException`                             | `PlanCreateTableAs` (unchanged)                    |
| `CREATE OR REPLACE TABLE`                                                | `BinderException`                             | `PlanCreateTableAs` + `CreateTable` (unchanged)    |
| Path already contains a Delta table (`00.json` exists)                   | `CatalogException`                            | `CreateTable` (existence probe; unchanged)         |
| Partition column refers to a non-existent column                         | `BinderException`                             | `CreateTable` (DuckDB-side scan; unchanged)        |
| Partition expression is not a column reference                           | `BinderException`                             | `CreateTable` + `PlanCreateTableAs` (unchanged)    |
| DuckDB type has no Delta mapping (e.g., `INTERVAL`, `UNION`)             | `BinderException`                             | `DeltaCreateTableSchema::VisitField` (NEW)         |
| Decimal precision/scale outside Delta's range                            | `BinderException` (via `captured_error`)      | `DeltaCreateTableSchema::VisitField` (NEW)         |
| Catalog access mode is `READ_ONLY`                                       | `InvalidInputException`                       | `InitializeForNewTable` (mirrors INSERT)           |
| Kernel `get_create_table_builder` returns error and `captured_error` empty | `IOException` via `TryUnpackResult`         | `InitializeForNewTable`                            |
| Kernel `get_create_table_builder` returns error and `captured_error` set | `captured_error.Throw()` (preserves DuckDB exception type, typically `BinderException`) | `InitializeForNewTable` |
| Kernel `create_table_builder_build*` returns error                       | `IOException` via `TryUnpackResult`           | `InitializeForNewTable`                            |
| Kernel `create_table_commit` returns error and `active_error` empty      | `IOException` via `TryUnpackResult`           | `Commit` (CTAS branch)                             |
| Kernel `create_table_commit` returns error and `active_error` set (CCv2) | `active_error.Throw()`                        | `Commit` (CTAS branch)                             |
| Concurrent CTAS race (kernel's put-if-absent fails)                      | `IOException` (kernel error code today; future: map to `CatalogException`) | `Commit` |
| Empty CTAS (zero rows)                                                   | no exception — valid Delta table              | n/a                                                |

The single Layer-3 trade-off here: when the kernel rejects a type at
schema-build time, we want a `BinderException` (DuckDB convention for
"the engine/dialect can't represent this"). The visitor's
`captured_error` mechanism is the gate: if `DeltaCreateTableSchema`
threw `BinderException` from inside the visitor, the trampoline catches
it, the kernel sees an Err result, and the outer call rethrows the
captured error (preserving the `ExceptionType::BINDER` tag) rather than
the kernel's generic message wrapped in `IOException`.

Non-fatal:
- Empty `written_files` at finalize: skip `create_table_add_files` and
  commit Protocol+Metadata only. No exception.

---

## 9. Test Plan

### Test directory: `test/sql/main/writing/ctas/` (existing — main fixture set)

**Existing 6 tests — unchanged, must continue passing**:
- `basic_ctas.test`
- `ctas_attach_existing.test`
- `ctas_empty_select.test`
- `ctas_or_replace_unsupported.test`
- `ctas_then_insert.test`
- `ctas_type_coverage.test`

All 38 assertions are SQL-visible (row counts, column names via
`DESCRIBE`, persistence-after-DETACH, error messages from
`BinderException`/`CatalogException`). None inspect the JSON content.
No edits required.

### Tests to ADD

**`test/sql/main/writing/ctas/ctas_kernel_native.test`** (NEW) —
exercises kernel-emitted commit shape:

1. CTAS into a fresh `__TEST_DIR__/...` path.
2. `read_text('.../_delta_log/00000000000000000000.json')` to read the
   commit file's content.
3. Assert (via `regexp_matches` or `string_split`):
   - The JSON contains `"engineInfo":"DuckDB"`.
   - The JSON contains `"operation":` (kernel writes one — exact text
     is kernel-defined; the hand-rolled path wrote `"CREATE TABLE"`;
     the kernel may write `"Create Table"` or another variant). Test
     for the field's presence, not its exact value.
   - The JSON contains `"protocol":` and `"metaData":` actions.
   - The JSON contains `"schemaString":` with the user-declared
     columns.
4. CTAS with a `STRUCT` column type — verifies the visitor's
   recursive `visit_field_struct` path. Read back and confirm `SELECT`
   on a struct field returns the right value.
5. CTAS with a `DECIMAL(precision, scale)` column — verifies
   `visit_field_decimal` precision/scale propagation.

**`test/sql/main/writing/ctas/ctas_timestamp_ntz.test`** (NEW) —
covers the kernel's now-permitted timestamp_ntz path (the hand-rolled
path rejected with `BinderException`; the kernel decides protocol
level automatically). Assertion: CTAS with a `TIMESTAMP_NS` /
`TIMESTAMP_MS` / `TIMESTAMP_SEC` column either:
- succeeds and round-trips correctly (preferred), OR
- fails with a kernel-emitted error that we surface as
  `BinderException` via `DeltaCreateTableSchema::captured_error`.

The cpp-coder should run this test once and adjust the expected
outcome to match observed kernel behavior; it is a probe, not a spec.

### Tests NOT to add in this PR
- **CCv2 CTAS** (`ctas_ccv2.test`) — deferred per §10 Q3.
- **Generated-data CTAS** (`test/sql/generated/...`) — requires
  PySpark fixture set; deferred.
- **Cloud CTAS** (`test/sql/cloud/...`) — requires MinIO/Azurite
  fixture; deferred.

### Regression net
Re-run the existing read suites to confirm the post-CTAS snapshot is
readable by the kernel:
- `test/sql/dat/*` — 173 assertions
- `test/sql/delta_kernel_rs/*` — 196 assertions
- `test/sql/main/writing/append/*` — 138 assertions
- `test/sql/issues/*` — 23 assertions

No JSON-content drift is expected here (these are read paths over
fixture data, not CTAS output).

---

## 10. Open Questions

1. **CCv2 (`parent_commit=true`) CTAS — IN SCOPE or DEFERRED?**

   **Recommendation: DEFERRED to a follow-up PR.**

   Rationale: the FFI surface to enable it is small (replace
   `create_table_builder_build` with `create_table_builder_build_with_committer`
   and feed in the same `MutableCommitter` the INSERT path constructs at
   `delta_transaction.cpp:511-515`). The blocker is test fixtures —
   today's CCv2 INSERT tests run against a parent catalog with a
   `__internal_delta_ccv2_commit_staged` table function, and we do not
   have a tested fixture setup for CCv2 CTAS. Shipping CCv2 CTAS
   without a test means the first real-world user hits the bug.

   Action: file a follow-up to add `ctas_ccv2.test` against a
   Unity-style parent catalog fixture, then enable the CCv2 branch in
   `InitializeForNewTable`. The branch is one `if (parent_commit) { ... }
   else { ... }` that mirrors lines 509–520 of `delta_transaction.cpp`.

   If the user explicitly wants CCv2 CTAS in this PR, the code is
   trivial — open question for the orchestrator.

2. **`DeltaCreateTableSchema::DispatchVisit` sentinel-on-error
   convention.**

   The kernel ABI accepts `uintptr_t` from the visitor; `0` is a valid
   field ID under some interpretations (kernel-side visitor state IDs
   are zero-indexed). However, by docs, the visitor's return value is
   "the id of the top-level columns list" returned by
   `visit_field_struct`, and that struct's ID is allocated by the
   kernel — we never produce `0` on the happy path because we always
   call `make_field_list` (kernel-side counterpart in
   `KernelSchemaVisitorState`) which returns nonzero IDs.

   Verify by inspecting the kernel's `KernelSchemaVisitorState`
   initialization (Rust side) that ID `0` is reserved. If it isn't,
   change the sentinel to `UINTPTR_MAX` and document.

   This is a Layer-1 detail the cpp-coder must lock down before
   shipping. Recommend cpp-coder reads
   `build/<cfg>/rust/src/delta_kernel/ffi/src/schema.rs` (or wherever
   `visit_field_*` is implemented in v0.23) and confirms.

3. **Atomicity guarantee equivalence: `FILE_FLAGS_FILE_CREATE_NEW`
   vs. `FileSystemCommitter`.**

   The hand-rolled path used `FILE_FLAGS_FILE_CREATE_NEW` to atomically
   reject the case where two concurrent CTAS calls race against the
   same path. Verify the kernel's `FileSystemCommitter::commit_path`
   actually uses an equivalent put-if-absent — on Linux/macOS this
   maps to `O_CREAT | O_EXCL`; on object stores it maps to a
   conditional PUT.

   Action: cpp-coder reads
   `build/<cfg>/rust/src/delta_kernel/kernel/src/transaction/file_system_committer.rs`
   (or v0.23 equivalent path) and confirms. If the kernel uses
   non-atomic write-then-check (it shouldn't), file a kernel issue
   before shipping. This is a Layer-3 invariant — Delta's commit
   protocol mandates atomicity here.

4. **Idempotency app_versions during CTAS — DISABLED?**

   Today `SetTransactionVersion` registers app/version pairs that
   `Commit()` flushes via `ffi::with_transaction_id`. The kernel's
   `ExclusiveCreateTransaction` does **not** expose a
   `with_transaction_id`-equivalent (verified in v0.23 header lines
   2660–2693: only `create_table_with_engine_info`, `_add_files`,
   `_set_data_change`, `_commit`, `_free_transaction`).

   **Decision:** in `CREATING_TABLE` mode, calls to
   `SetTransactionVersion` throw `NotImplementedException`. This is the
   conservative choice — silently ignoring app-version registration on
   a CTAS would be a correctness footgun. Document in the error
   message: "Idempotency helpers (delta_transaction_set_version) are
   not yet supported during CREATE TABLE AS SELECT; chain a separate
   INSERT in a follow-up transaction."

   Confirm with orchestrator.

5. **Table properties from `BoundCreateTableInfo::properties`.**

   DuckDB CTAS supports `CREATE TABLE foo AS SELECT ... WITH (key='value', ...)`
   which populates `BoundCreateTableInfo::properties`. The kernel's
   `create_table_builder_with_table_property` accepts arbitrary
   key/value strings (header lines 2759–2762). Should we plumb through?

   **Recommendation: NOT in this PR.** Reasoning: the property
   namespace is split between (a) Delta-protocol keys like
   `delta.columnMapping.mode`, `delta.checkpoint.writeStatsAsStruct`,
   `delta.appendOnly` and (b) engine-specific keys. Validating which
   subset to forward needs its own design discussion. Ship retirement
   first; properties as a follow-up.

   If the orchestrator wants properties in scope, the code is trivial
   (a for-loop in `InitializeForNewTable` step 3 of §6).

6. **`ctas_type_coverage.test` extension scope.**

   The kernel's create-table flow accepts richer types than the
   hand-rolled JSON path (notably `STRUCT`, `LIST`/`ARRAY`, `MAP`, and
   potentially `TIMESTAMP_NS`/`TIMESTAMP_MS`/`TIMESTAMP_SEC` mapping
   to `timestamp_ntz`). Should `ctas_type_coverage.test` be extended
   in this PR to assert the new round-trips work, or kept identical
   (so the test surface is unchanged) and a new
   `ctas_kernel_native.test` added (as proposed in §9)?

   **Recommendation: new test file, leave existing untouched.**
   This makes diff review easier and isolates the new capability.

7. **Sentinel field-name for the final-flatten struct.**

   The docs at header line 2430 say "The name for the final schema
   element is ignored." but the FFI signature still takes a
   `KernelStringSlice`. Pass a slice of an empty `string` literal owned
   by `DeltaCreateTableSchema::scratch_names`. The cpp-coder must NOT
   pass `KernelStringSlice{nullptr, 0}` — verify by inspecting whether
   the kernel dereferences `ptr` even when `len == 0` (Rust slice
   semantics suggest it does not, but be defensive: an empty `string`
   is one allocation in the visitor's lifetime and removes the
   question).

---

