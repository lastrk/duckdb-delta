# Architecture Plan v2: CREATE TABLE AS SELECT (CTAS) for the `delta` extension

Status: design only, no implementation. Supersedes v1
(`/workspace/.agent-output/001-architecture-plan.md`), which incorrectly
assumed delta-kernel-rs v0.21 lacked a CREATE TABLE FFI.

**Premise change vs v1:** kernel v0.21.0 — our current pin in
`CMakeLists.txt:148` — *does* expose a complete table-creation builder
through `ffi/src/transaction/mod.rs`. The whole `DeltaInitialCommit`
hand-rolled JSON writer from v1 §5.1 is gone. CTAS goes through the
kernel exactly like INSERT does today.

Target seams (unchanged from v1):
- `DeltaCatalog::PlanCreateTableAs` (`src/storage/delta_catalog.cpp:102`)
- `DeltaSchemaEntry::CreateTable` (`src/storage/delta_schema_entry.cpp:36`)
- the CTAS arm of `DeltaInsert`
  (`src/include/storage/delta_insert.hpp:21-25`, GetName() switch at
  `src/storage/delta_insert.cpp:284`).

---

## 1. Domain Constraints

### Delta Lake (protocol) — what we delegate to the kernel
- The first commit (`_delta_log/00000000000000000000.json`) must contain
  a Protocol action and a Metadata action; optional initial Add actions
  follow. The kernel's `CreateTable` API computes all of this from the
  schema we hand it. We no longer hand-roll JSON.
- The kernel picks `minReaderVersion` / `minWriterVersion` based on the
  features it enables (column mapping, deletion vectors, etc.). For a
  v1 CTAS we pass no feature opt-ins, so the kernel will pick the
  minimum compatible protocol — we do not parameterize it.
- Partition columns are properties of Metadata, passed through the
  schema/builder; the kernel writes `partitionColumns` for us.
- Empty CTAS (zero rows) is a valid table: builder + commit without
  calling `create_table_add_files`.
- Atomicity: `FileSystemCommitter` (default path) does
  put-if-absent / rename-if-absent on the local FS or S3-conditional
  PUT. We do not need to think about this in C++.

### DuckDB (engine API) — unchanged from v1
- `PhysicalPlanGenerator::CreatePlan(LogicalCreateTable&)` calls
  `op.schema.catalog.PlanCreateTableAs(...)`. We must return a single
  `PhysicalOperator` that creates the table AND sinks the child plan
  (the proven pattern is `PhysicalInsert` at
  `/workspace/duckdb/src/execution/operator/persistent/physical_insert.cpp:104-119`:
  do the catalog mutation lazily inside `GetGlobalSinkState`).
- Surface conventions: `RETURNING` and `ON CONFLICT` for CTAS raise
  `BinderException`, matching the precedent in
  `DeltaCatalog::PlanInsert` (`src/storage/delta_insert.cpp:306-311`).

### FFI boundary — corrected facts (kernel v0.21.0)
The kernel exposes a full builder API in
`ffi/src/transaction/mod.rs` (Rust API landed v0.20.0 via PR #1629,
FFI exposure v0.21.0 via PR #2296):

| Rust symbol                                          | C++ binding (assumed)                                                                                       |
|------------------------------------------------------|--------------------------------------------------------------------------------------------------------------|
| `get_create_table_builder`                           | `ffi::get_create_table_builder(path_slice, Handle<SharedSchema>, engine_info_slice, engine)` → `ExternResult<Handle<ExclusiveCreateTableBuilder>>` |
| `create_table_builder_with_table_property`           | `ffi::create_table_builder_with_table_property(builder, key_slice, value_slice, engine)` → `ExternResult<Handle<ExclusiveCreateTableBuilder>>` (consumes `builder`) |
| `create_table_builder_build`                         | `ffi::create_table_builder_build(builder, engine)` → `ExternResult<Handle<ExclusiveCreateTransaction>>`     |
| `create_table_builder_build_with_committer`          | `ffi::create_table_builder_build_with_committer(builder, Handle<MutableCommitter>, engine)` → `ExternResult<Handle<ExclusiveCreateTransaction>>` (consumes both) |
| `free_create_table_builder`                          | `ffi::free_create_table_builder(builder)`                                                                   |
| `create_table_with_engine_info`                      | `ffi::create_table_with_engine_info(txn, engine_info_slice, engine)` → `ExternResult<Handle<ExclusiveCreateTransaction>>` (consumes old `txn`) |
| `create_table_add_files`                             | `ffi::create_table_add_files(txn, Handle<ExclusiveEngineData>)` (consumes `write_metadata`)                 |
| `create_table_set_data_change`                       | `ffi::create_table_set_data_change(txn, bool)`                                                              |
| `create_table_commit`                                | `ffi::create_table_commit(txn, engine)` → `ExternResult<uint64_t>` (returns new version, 0 for a fresh table) |
| `create_table_free_transaction`                      | `ffi::create_table_free_transaction(txn)`                                                                   |

Schema handle (`Handle<SharedSchema>`) ownership:
`get_create_table_builder` does **not** consume the schema; the caller
must free it (existing pattern — we already see `SharedSchema` in
`delta_utils.hpp:163`).

**C++ binding names must be verified** against the generated header
after the first `make debug` run, at
`build/debug/codegen/include/generated_delta_kernel_ffi.hpp`. The
generator is `scripts/ffi/generate_delta_kernel_ffi_header`. Cbindgen
historically names FFI free functions in lowercase Rust style under
`namespace ffi`, which is what every entry currently used in the
codebase confirms (see the `grep` audit of `ffi::*` symbols in §6).
The cpp-coder MUST cross-check exact spelling and signature pre-commit
and adjust the wrappers below if cbindgen mangled anything.

### ATTACH semantics
- v1's concern about ATTACH to an empty path is *unchanged*: kernel
  snapshot init still fails on a path with no `_delta_log`, so an
  ATTACH-then-CTAS workflow needs the schema entry to tolerate
  "snapshot does not yet exist" until CTAS creates it. The fix is the
  same — opt-in `allow_create` attach option — but the cost is purely
  in the C++ catalog plumbing; no JSON writer involved.

---

## 2. Affected Surfaces

### `src/storage/`
- `delta_catalog.cpp` — implement `PlanCreateTableAs`. Symmetric with
  `PlanInsert` (file_path comes from the catalog path, not from a
  loaded snapshot); allocates `PhysicalCopyToFile` + `DeltaInsert`
  (CTAS arm). For `parent_commit=true` we now have a real plan instead
  of `NotImplementedException`: see §6 and §10.
- `delta_schema_entry.cpp` — replace `CreateTable` body
  (`src/storage/delta_schema_entry.cpp:36`) with a real implementation
  that builds a `DeltaTableEntry` for a brand-new table and validates
  the schema/partition columns. Snapshot field is left empty; it is
  populated by the kernel after `create_table_commit` succeeds.
- `delta_insert.cpp` — extend the CTAS branch of:
  - `GetGlobalSinkState` (lazy `CreateTable` and table-entry binding,
    mirrors `PhysicalInsert::GetGlobalSinkState`),
  - `Finalize` (today line 269 hard-codes `table->catalog`; the CTAS
    branch must instead use `schema->catalog`, and must call a new
    transaction entry point — see §5.2).
- `delta_transaction.cpp` — add CTAS-aware initialization
  (`InitializeForNewTable`) and a `Commit` branch for CTAS that drives
  the `ExclusiveCreateTransaction` lifecycle. The existing INSERT
  branch (`ExclusiveTransaction`) stays intact.

### `src/include/storage/`
- `delta_catalog.hpp` — no signature change (override already
  declared).
- `delta_schema_entry.hpp` — add a private helper
  `CreateTableEntryForNewTable(...)` (no schema-changing public API
  required; existing `CreateTable` override is the public surface).
- `delta_insert.hpp` — no struct changes needed; the second
  constructor at lines 24-25 already takes `info`. Add a sink-time
  pointer to the freshly created `DeltaTableEntry` if global state
  needs it; otherwise unchanged.
- `delta_transaction.hpp` — add `InitializeForNewTable(...)`,
  `AddCreateTablePending(...)` and a `mode` enum. Hold a
  `KernelExclusiveCreateTransaction` alongside the existing
  `kernel_transaction`.

### `src/include/`
- `delta_utils.hpp` — add **two** new RAII typedefs that compose
  cleanly with the existing `TemplatedUniqueKernelPointer` pattern at
  lines 389-401:
  ```
  typedef TemplatedUniqueKernelPointer<ffi::ExclusiveCreateTableBuilder,
                                       ffi::free_create_table_builder>
      KernelExclusiveCreateTableBuilder;
  typedef TemplatedUniqueKernelPointer<ffi::ExclusiveCreateTransaction,
                                       ffi::create_table_free_transaction>
      KernelExclusiveCreateTransaction;
  ```
  (The second name follows cbindgen's free-function convention seen in
  the audited symbol set in §6.) `MutableCommitter` for CCv2 reuses the
  existing `get_uc_committer` returned handle — see §6.

### `src/functions/delta_scan/`
- No API change. After `create_table_commit` returns version 0, the
  next read of the CTAS-created table goes through the normal
  `DeltaMultiFileList::InitializeSnapshot` path. Behavior validation
  only.

### `src/delta_extension.cpp`
- `DeltaCatalogAttach` — add attach option
  `allow_create = bool` (default `false`) and store it on
  `DeltaCatalog`. Mirror of v1 §2 (semantics unchanged).
- For `parent_commit=true` + CTAS we now reuse the same
  `__internal_delta_ccv2_commit_staged` lookup that happens today at
  lines 63-74 — no new lookup required.

### `scripts/ffi/`
- No changes — the generator already exposes the FFI surface listed
  above. `prefix.inc`, `suffix.inc`, and the generator script stay
  untouched.

### `CMakeLists.txt`
- No new sources required (the v1 plan's `delta_initial_commit.cpp` is
  gone). `GIT_TAG` stays at `v0.21.0` — no kernel bump needed.

### `test/sql/`
- New directory `test/sql/main/writing/ctas/` (same layout as v1 §2).
  See §9 for the file list. The "validates 00.json has Protocol+Metadata"
  assertions become structural ("a single commit file exists,
  `count(*)` works after re-ATTACH") since we no longer own the JSON.

---

## 3. Ownership Map

```
DeltaCatalog                                                  (catalog)
└── DeltaSchemaEntry              unique_ptr                  (catalog)
    └── DeltaTableEntry           unique_ptr                  (created at PlanCreateTableAs sink-init time)
        └── DeltaMultiFileList    shared_ptr                  (initialized AFTER create_table_commit)
            ├── extern_engine     KernelExternEngine
            └── snapshot          shared_ptr<SharedKernelSnapshot>  (post-commit)

DeltaTransaction                                              (per ClientContext, per catalog)
├── mode                          DeltaTransactionMode enum   (REGULAR / CREATING_TABLE)
├── kernel_transaction            KernelExclusiveTransaction          (existing — used by INSERT)
├── kernel_create_txn             KernelExclusiveCreateTransaction    (NEW — used by CTAS)
├── pending_create_schema         KernelSharedSchema                  (NEW; freed after build, see §5)
├── write_entry                   optional_ptr<DeltaTableEntry>       (borrow)
├── outstanding_appends           vector<DeltaDataFile>               (owned)
└── parent_table_entry            optional_ptr<TableCatalogEntry>     (borrow, CCv2)

DeltaInsert (CTAS arm)            PhysicalOperator
├── schema                        optional_ptr<SchemaCatalogEntry>    (borrow)
├── info                          unique_ptr<BoundCreateTableInfo>    (owned; consumed at sink init)
├── column_index_map              (unused in CTAS arm)
└── children                      standard DuckDB ownership of physical copy
```

**`shared_ptr` audit:** the only `shared_ptr` on the path is the
existing `shared_ptr<DeltaMultiFileList>` mandated by DuckDB's
`TableCatalogEntry` model. No new `shared_ptr` added.

**Kernel handle audit (every handle has exactly one RAII owner):**
- `Handle<SharedSchema>` for CTAS: owned by `DeltaTransaction::pending_create_schema`
  (new `KernelSharedSchema` typedef, see §5). Freed via the kernel's
  existing `free_*_schema` (cbindgen-generated; spelling to verify in
  generated header).
- `Handle<ExclusiveCreateTableBuilder>`: NEVER stored. Constructed,
  chained through any `with_table_property` calls, then consumed by
  `create_table_builder_build*`. The chained consumption pattern means
  intermediate handles never escape a single function. If the chain
  must escape (it should not), an RAII typedef
  `KernelExclusiveCreateTableBuilder` exists as the safety net.
- `Handle<ExclusiveCreateTransaction>`: owned by
  `DeltaTransaction::kernel_create_txn` (new
  `KernelExclusiveCreateTransaction` typedef). The deleter is
  `ffi::create_table_free_transaction`, distinct from the INSERT path's
  `ffi::free_transaction`. The kernel's `create_table_with_engine_info`
  consumes-and-returns, so the field is reassigned via `release()` —
  same pattern as `delta_transaction.cpp:459-461` for `with_transaction_id`.
- `Handle<MutableCommitter>` (CCv2): produced by
  `ffi::get_uc_committer` (existing call in
  `delta_transaction.cpp:514`). The CCv2 CTAS path passes this into
  `create_table_builder_build_with_committer`, which consumes it.
  No RAII wrapper change required — today's code already releases the
  handle by passing it into `transaction_with_committer` immediately.
- `Handle<ExclusiveEngineData>` for the initial `add_files`: produced
  by `ffi::get_engine_data` exactly as in
  `delta_transaction.cpp:568-571`. Lifetime ends inside
  `create_table_add_files`.

---

## 4. Module Layout

```
src/
├── delta_extension.cpp                       MOD: parse `allow_create` attach option (bool).
├── storage/
│   ├── delta_catalog.cpp                     MOD: implement PlanCreateTableAs (mirror of PlanInsert,
│   │                                              uses schema/info instead of TableCatalogEntry).
│   ├── delta_schema_entry.cpp                MOD: implement CreateTable; add helper
│   │                                              CreateTableEntryForNewTable() (no FS / kernel touch
│   │                                              yet — the kernel call happens at sink init time
│   │                                              inside DeltaTransaction).
│   ├── delta_insert.cpp                      MOD: CTAS arm of GetGlobalSinkState + Finalize;
│   │                                              CTAS arm uses transaction->RegisterCreateTable()
│   │                                              instead of Append() (or uses Append() AFTER
│   │                                              RegisterCreateTable; see §5.2).
│   ├── delta_transaction.cpp                 MOD: InitializeForNewTable(), AddCreateTablePending(),
│   │                                              CTAS branch of Commit() and Rollback().
│   ├── delta_table_entry.cpp                 MOD: constructor already takes CreateTableInfo&;
│   │                                              add a path to construct without a snapshot for
│   │                                              the pre-commit CTAS state (DeltaTableEntry holds
│   │                                              the columns/types from BoundCreateTableInfo).
│   └── delta_transaction_manager.cpp         unchanged.
├── include/storage/
│   ├── delta_catalog.hpp                     unchanged.
│   ├── delta_schema_entry.hpp                MOD: private helper CreateTableEntryForNewTable().
│   ├── delta_table_entry.hpp                 MOD: optional empty-snapshot path during CTAS.
│   ├── delta_insert.hpp                      unchanged.
│   └── delta_transaction.hpp                 MOD: add mode enum + InitializeForNewTable() +
│                                                  AddCreateTablePending() + kernel_create_txn field.
├── include/
│   └── delta_utils.hpp                       MOD: add two TemplatedUniqueKernelPointer typedefs:
│                                                  KernelExclusiveCreateTableBuilder,
│                                                  KernelExclusiveCreateTransaction.
│                                                  (Optionally also a KernelSharedSchema typedef
│                                                   over the existing ffi::SharedSchema deleter.)
└── (no new files)                            v1's delta_initial_commit.{cpp,hpp} are removed.

test/sql/main/writing/ctas/
├── basic_ctas.test                                NEW
├── ctas_with_partitions.test                      NEW
├── ctas_empty_select.test                         NEW
├── ctas_attach_existing.test                      NEW
├── ctas_unsupported_types.test                    NEW
├── ctas_in_transaction.test                       NEW
└── ctas_or_replace_unsupported.test               NEW
```

---

## 5. Key Types (skeletons only — no method bodies)

### 5.1 RAII additions in `src/include/delta_utils.hpp`

```cpp
// At the existing typedef block (delta_utils.hpp:395-401), add:

typedef TemplatedUniqueKernelPointer<ffi::ExclusiveCreateTableBuilder,
                                     ffi::free_create_table_builder>
    KernelExclusiveCreateTableBuilder;

typedef TemplatedUniqueKernelPointer<ffi::ExclusiveCreateTransaction,
                                     ffi::create_table_free_transaction>
    KernelExclusiveCreateTransaction;

// Schema handle. Today the codebase passes raw ffi::Handle<ffi::SharedSchema>
// pointers (see delta_utils.hpp:163). For CTAS we need a path where the
// schema is constructed, optionally cached on the transaction across
// the bind→sink-init boundary, then explicitly freed.
// VERIFY the exact deleter name in the generated header
// (ffi::free_schema or similar). Until verified, the cpp-coder should
// pick the existing free function used elsewhere for SharedSchema and
// expose it here.
typedef TemplatedUniqueKernelPointer<ffi::SharedSchema,
                                     /* ffi::free_schema (verify) */>
    KernelSharedSchema;
```

The pattern is identical to the existing `KernelExclusiveTransaction`
(line 400) and `KernelEngineData` (line 401). No new template; no new
abstraction. The deleter function pointer is the only knob.

### 5.2 Transaction extension

```cpp
// addition to src/include/storage/delta_transaction.hpp

enum class DeltaTransactionMode : uint8_t {
    REGULAR,           // table already exists; uses kernel_transaction
    CREATING_TABLE     // table is being created in this transaction;
                       // uses kernel_create_txn
};

class DeltaTransaction : public Transaction {
public:
    // ... existing public API (Append, Commit, Rollback, ...)

    //! Allocate the create-table state on this transaction. Builds the
    //! SharedSchema from the BoundCreateTableInfo, builds the
    //! ExclusiveCreateTableBuilder via ffi::get_create_table_builder,
    //! applies optional table properties, and calls
    //! create_table_builder_build (or _with_committer when parent_commit
    //! is set). After this call, kernel_create_txn is non-null and the
    //! transaction is in CREATING_TABLE mode.
    //!
    //! Idempotent for the same DeltaTableEntry; throws if a different
    //! CREATING_TABLE state already exists.
    void InitializeForNewTable(ClientContext &context,
                               DeltaTableEntry &new_table_entry,
                               const string &path);

    //! Stages the parquet files produced by the CTAS child plan onto
    //! kernel_create_txn via ffi::create_table_add_files. Mirror of
    //! Append() for the CTAS arm.
    //! Builds the WriteMetaData arrow batch from `append_files` exactly
    //! as the existing INSERT path does (delta_transaction.cpp:226-289).
    void AppendForNewTable(ClientContext &context,
                           const vector<DeltaDataFile> &append_files);

    bool IsCreatingTable() const;

private:
    DeltaTransactionMode mode = DeltaTransactionMode::REGULAR;
    KernelExclusiveCreateTransaction kernel_create_txn;
};
```

`Commit()`'s existing branch at `delta_transaction.cpp:411-481` gets
a sibling clause:

```cpp
// inside Commit(), pseudocode (no implementation here):
if (mode == DeltaTransactionMode::CREATING_TABLE) {
    // optionally apply with_transaction_id-equivalent (kernel does not yet
    // expose an app_version path on create-table txns in v0.21 — see §10).
    uint64_t new_version = 0;
    auto res = KernelUtils::TryUnpackResult(
        ffi::create_table_commit(kernel_create_txn.release(),
                                 write_entry->snapshot->extern_engine.get()),
        new_version);
    // error handling identical to the existing branch (active_error
    // CCv2 escape hatch still applies, since
    // create_table_builder_build_with_committer uses the same callback).
}
```

### 5.3 Schema entry extension

```cpp
// addition to src/include/storage/delta_schema_entry.hpp

class DeltaSchemaEntry : public SchemaCatalogEntry {
public:
    optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction,
                                           BoundCreateTableInfo &info) override; // NOW implemented

private:
    //! Builds the in-memory DeltaTableEntry for a CTAS target.
    //! No FS / kernel touch. Validates types and partition columns,
    //! assigns a freshly generated table name (catalog-default).
    unique_ptr<DeltaTableEntry>
    CreateTableEntryForNewTable(ClientContext &context, BoundCreateTableInfo &info);
};
```

### 5.4 Table entry — pre-commit allowance

```cpp
// adjustment in src/include/storage/delta_table_entry.hpp:
//   - constructor already accepts a CreateTableInfo (line 22).
//   - `snapshot` (line 41) becomes nullable while the table is in CREATING
//     mode. Read paths that touch `snapshot` while CREATING is in flight
//     fail by design (CREATE TABLE AS does not let you SELECT from the
//     new table mid-transaction in DuckDB anyway).
// No public API change; just doc on the invariant.
```

### 5.5 No new attach-option type
The single new attach option `allow_create` is a `bool` field on
`DeltaCatalog`. No new struct/class.

---

## 6. FFI Plan

### What does NOT change
- Generated FFI header: untouched.
- `scripts/ffi/prefix.inc`, `suffix.inc`, generator script: untouched.
- `delta-kernel-rs` `GIT_TAG`: stays at `v0.21.0`. No bump required;
  the create-table FFI is already exposed at this tag (PR #2296).

### What handles cross the FFI for CTAS
Exact call sequence inside `DeltaTransaction::InitializeForNewTable`:

1. **Build the kernel schema from the bind data.** The CTAS produces a
   `BoundCreateTableInfo`; the columns (and not-null constraints) need
   to be turned into an `ffi::Handle<ffi::SharedSchema>`. This conversion
   is the one piece that the codebase does not yet have on the *write*
   path — today `SchemaVisitor` only goes kernel → DuckDB. Two options:
   - (a) reuse a kernel builder that takes JSON (verify availability
     of `ffi::*schema_from_json*` in the generated header), or
   - (b) add a small C++ helper that produces a Delta-protocol
     StructType JSON string from a `ColumnList` and feeds it into the
     schema builder.
   The cpp-coder must inspect the generated header to confirm which
   FFI entry point exists. Until verified this is the one **open
   question that gates implementation** (see §10).
2. **Get the builder.**
   ```
   builder = ffi::get_create_table_builder(
       KernelUtils::ToDeltaString(path),
       schema_handle,
       KernelUtils::ToDeltaString("DuckDB"),
       extern_engine);
   ```
   Unpacked via `KernelUtils::TryUnpackResult` exactly like
   `delta_transaction.cpp:514-516`.
3. **Optional table properties.** v1 of this feature passes none.
   When we later expose `delta.columnMapping.mode`, `delta.appendOnly`,
   etc., it is a `create_table_builder_with_table_property` chain:
   ```
   builder = ffi::create_table_builder_with_table_property(
       builder.release(), key_slice, value_slice, engine);
   ```
   Each call CONSUMES and re-yields the builder handle.
4. **Build the create-table transaction.**
   - Default path (`parent_commit == false`):
     ```
     kernel_create_txn = ffi::create_table_builder_build(
         builder.release(), engine);
     ```
   - CCv2 path (`parent_commit == true`): produce the
     `MutableCommitter` handle exactly like
     `delta_transaction.cpp:511-515` does today, then call
     `create_table_builder_build_with_committer`:
     ```
     committer = ffi::get_uc_committer(
         ffi::get_uc_commit_client(this, CommitCallback),
         KernelUtils::ToDeltaString(unity_table_id.empty() ? path : unity_table_id),
         DuckDBEngineError::AllocateError);
     kernel_create_txn = ffi::create_table_builder_build_with_committer(
         builder.release(), committer, engine);
     ```
     `CommitCallback` is the **same** static method already in
     `DeltaTransaction` (line 318) — its `commit_info` payload is
     agnostic to whether the underlying txn is an
     `ExclusiveTransaction` or `ExclusiveCreateTransaction`. CCv2 CTAS
     is therefore in scope (vs. v1, which deferred it).
5. **Engine info.** Optional but conventional. If we want
   `engineInfo: DuckDB` on the commit (matching the existing INSERT
   path):
   ```
   kernel_create_txn = ffi::create_table_with_engine_info(
       kernel_create_txn.release(),
       KernelUtils::ToDeltaString("DuckDB"), engine);
   ```
   This pattern (consume-and-replace) mirrors
   `delta_transaction.cpp:534` for `with_engine_info`.

Inside `AppendForNewTable` (called by `DeltaInsert::Finalize` CTAS arm):

6. **Stage data files.** Reuse the existing `WriteMetaData` arrow
   batch builder (defined in `delta_transaction.cpp:178-293`) verbatim.
   Convert to `Handle<ExclusiveEngineData>` via `ffi::get_engine_data`
   exactly as in `delta_transaction.cpp:567-569`, then:
   ```
   ffi::create_table_add_files(
       kernel_create_txn.get(), engine_data.release());
   ```
   The handle is consumed. Note we keep `data_change=true` (the kernel
   default for CTAS); `create_table_set_data_change` is not called in
   v1.

Inside `Commit()`:

7. **Commit.**
   ```
   uint64_t new_version = 0;
   KernelUtils::TryUnpackResult(
       ffi::create_table_commit(kernel_create_txn.release(),
                                engine),
       new_version);
   ```
   `new_version` is 0 for a fresh table.

### Callback re-entrancy
- CCv2 reuses the existing `DeltaTransaction::CommitCallback`
  (`delta_transaction.cpp:318-409`). Its exception barrier (try/catch
  with `active_error` storage and kernel-allocated error string)
  already covers the CTAS path. No new callback.
- The `EngineError` allocator (`DuckDBEngineError::AllocateError`)
  is passed in the same places. No change.

### String lifetimes
- All kernel string slices follow the existing
  `KernelUtils::ToDeltaString` discipline: the backing `string` must
  outlive the FFI call. The CTAS path constructs `path`,
  `engine_info`, and any property keys/values as local `string`
  variables in `InitializeForNewTable`, never as temporaries.

### `prefix.inc` / `suffix.inc` / generator
- No changes.

### Verification checklist for cpp-coder
After the first `make debug` the cpp-coder MUST confirm in
`build/debug/codegen/include/generated_delta_kernel_ffi.hpp`:
1. The C++ names of `ffi::ExclusiveCreateTableBuilder`,
   `ffi::ExclusiveCreateTransaction`, `ffi::MutableCommitter` —
   adjust the typedef block in §5.1 if cbindgen renamed them.
2. The exact deleter function names
   (`ffi::free_create_table_builder` and
   `ffi::create_table_free_transaction`) — adjust the typedef
   deleters in §5.1.
3. Whether a JSON-string-to-`SharedSchema` FFI exists at v0.21 (this
   is open question §10.1).

---

## 7. Concurrency Plan

### State split
- **Bind data:** `BoundCreateTableInfo` (DuckDB-owned, immutable after
  bind). Held in `DeltaInsert::info`.
- **Global sink state:** single `DeltaInsertGlobalState` instance.
  CTAS arm differs from INSERT arm only in how `columns` is populated
  (from `info->Base().columns`, not from `table->snapshot`).
  `not_null_constraints` is similarly built from `info`. Guarded by
  `ParallelSink() == false` (already the case).
- **Local sink state:** unchanged, none.

### Lock ordering
- `DeltaTransaction::lock` (existing). The CTAS path enters it in the
  same shape as the INSERT path:
  - `InitializeForNewTable` acquires `lock` to install
    `kernel_create_txn` and `write_entry`, then **releases before**
    any FFI call.
  - `AppendForNewTable` acquires `lock` only to mutate
    `outstanding_appends`. The FFI call (`create_table_add_files`)
    runs without the lock held.
- **Never** hold `lock` across any `ffi::*` call. Rule unchanged.

### Kernel re-entry
- CCv2 CTAS reuses `CommitCallback`, which today is invoked under the
  kernel's commit codepath. We must not be holding
  `DeltaTransaction::lock` when entering `create_table_commit`. The
  existing code path already obeys this (commit happens outside
  `Append`'s critical section).

### Cancellation
- `DeltaInsert::ParallelSink() == false`. No fan-out cancellation
  needed.
- Long kernel calls on the CTAS path are:
  (a) `create_table_builder_build*` (cheap, no I/O),
  (b) `create_table_add_files` (cheap, in-memory),
  (c) `create_table_commit` (does the FS / object-store write).
  Cancellation between (b) and (c) requires
  `Rollback` to drop `kernel_create_txn` (the deleter
  `create_table_free_transaction` does the kernel-side cleanup).
- The CTAS rollback contract: on `Rollback`, after dropping
  `kernel_create_txn`, call `CleanUpFiles()` (existing — removes the
  staged parquet). Because the kernel never published the commit, the
  table path has no `_delta_log`. If `allow_create` was set on a path
  that was empty before, the directory is left as we found it (no
  CTAS-side mkdir of `_delta_log`).

---

## 8. Error Strategy

| Failure                                                                  | Exception type                              | Where                                                  |
|--------------------------------------------------------------------------|---------------------------------------------|--------------------------------------------------------|
| Path already contains a valid Delta table                                | `CatalogException`                          | `DeltaSchemaEntry::CreateTable` (snapshot probe first) |
| Path exists and is not a directory                                       | `IOException`                               | `DeltaSchemaEntry::CreateTable`                        |
| ATTACH path doesn't exist and `allow_create=false`                       | `CatalogException`                          | `DeltaCatalogAttach`                                   |
| Schema contains a DuckDB type that has no Delta representation           | `BinderException`                           | schema-build helper invoked from `InitializeForNewTable` |
| Partition column refers to a nested or non-existent field                | `BinderException`                           | schema-build helper                                    |
| `RETURNING` clause on CTAS                                               | `BinderException`                           | `PlanCreateTableAs`                                    |
| `CREATE OR REPLACE TABLE`                                                | `BinderException` (v1)                      | `PlanCreateTableAs`                                    |
| Catalog access mode is `READ_ONLY`                                       | `InvalidInputException`                     | `InitializeForNewTable` (matches existing INSERT path) |
| `parent_commit=true` CTAS                                                | **supported** (no exception) — see §6 / §10 | n/a                                                    |
| Kernel `get_create_table_builder` / `_build*` fails                      | `IOException` via `KernelUtils::TryUnpackResult` | `InitializeForNewTable`                           |
| Kernel `create_table_commit` fails                                       | `IOException` via `TryUnpackResult`         | `Commit` (CCv2 escape hatch via `active_error`)        |
| Zero-row CTAS                                                            | (no exception — valid)                      | n/a                                                    |
| Concurrent CTAS race (kernel commit conflict)                            | `TransactionException` (mapped from kernel error) | `Commit`                                         |

Non-fatal:
- Zero `written_files` at finalize is **not** an error for CTAS. We
  simply skip `create_table_add_files` and go straight to
  `create_table_commit`.

### Kernel error → DuckDB exception mapping
The existing `KernelUtils::TryUnpackResult` (`delta_utils.hpp:64-78`)
unconditionally produces `ErrorData(ExceptionType::IO, ...)`. That is
acceptable for v1 — every kernel failure becomes `IOException`. If we
want finer-grained mapping (TransactionException for conflicts,
BinderException for protocol-incompat metadata) it is a follow-up that
belongs in `KernelUtils`, not in CTAS.

---

## 9. Test Plan

All tests follow the existing `test/sql/main/writing/` style and use
`__TEST_DIR__/...` paths. `require notwindows` where rename semantics
matter.

### `test/sql/main/writing/ctas/basic_ctas.test`
- `ATTACH '__TEST_DIR__/ctas_basic' AS db (TYPE delta, allow_create=true);`
- `CREATE TABLE db.foo AS SELECT range AS i FROM range(10);`
- `SELECT count(*) FROM db.foo` → 10.
- `DETACH db; ATTACH ... AS db (TYPE delta);` (no `allow_create`) →
  `SELECT count(*) FROM db.foo` still returns 10 (persistence check).
- Verify a `_delta_log/00000000000000000000.json` file exists (file
  presence only — we do NOT parse contents, the kernel owns the JSON).

### `test/sql/main/writing/ctas/ctas_with_partitions.test`
- CTAS with `PARTITION BY (p)`.
- Read the table back and confirm partition pruning works
  (existing-style `EXPLAIN` assertion using
  `delta_scan_explain_files_filtered`).

### `test/sql/main/writing/ctas/ctas_empty_select.test`
- CTAS from `SELECT ... WHERE FALSE`.
- `count(*) = 0`, no parquet files, single commit at version 0.

### `test/sql/main/writing/ctas/ctas_attach_existing.test`
- ATTACH an existing Delta table path with `allow_create=true`.
- `CREATE TABLE foo AS ...` must fail with `CatalogException`.

### `test/sql/main/writing/ctas/ctas_unsupported_types.test`
- CTAS with `RETURNING` → `BinderException`.
- CTAS with a column whose type has no Delta mapping → `BinderException`.

### `test/sql/main/writing/ctas/ctas_in_transaction.test`
- `BEGIN; CTAS; ROLLBACK;` — no `_delta_log` created, no parquet
  files left behind.
- `BEGIN; CTAS; INSERT INTO new_table VALUES (...); COMMIT;` —
  decide v1 behavior: CTAS commit and INSERT commit are separate
  (versions 0 and 1) OR fail with `NotImplementedException`. The
  kernel's `ExclusiveCreateTransaction` and `ExclusiveTransaction`
  are distinct types, so bundling into one commit is non-trivial. v1
  proposes: CTAS commit is its own transaction; subsequent INSERTs in
  the same SQL transaction land in commit `01.json` via the normal
  INSERT path. See §10.

### `test/sql/main/writing/ctas/ctas_or_replace_unsupported.test`
- `CREATE OR REPLACE TABLE` → `BinderException`.

### `test/sql/main/writing/ctas/ctas_ccv2.test` (NEW vs v1)
- ATTACH with `parent_commit=true` and a valid parent catalog
  configured for `__internal_delta_ccv2_commit_staged`.
- CTAS goes through `create_table_builder_build_with_committer` →
  parent catalog records the staged commit → read works.
- If CCv2 fixture is unavailable in the default test setup, gate this
  test on the same fixture set as existing CCv2 tests (see
  `test/sql/...` for current Unity Catalog test gates) or move it
  under `test/sql/issues/` until fixtures land.

Future (not in v1):
- `test/sql/generated/writing/ctas/` — cross-engine round-trip with
  delta-spark / delta-rs, gated on `GENERATED_DATA_AVAILABLE`.
- `test/sql/cloud/{minio_local,azurite}/ctas.test` — CTAS against
  object storage.

---

## 10. Open Questions

(v1 question #1 about the kernel bump is **closed**: no bump needed.)

1. **Write-side schema construction.** How exactly does the cpp-coder
   produce a `Handle<SharedSchema>` from a DuckDB `ColumnList` for
   CTAS? The codebase has read-side `SchemaVisitor` only.
   - Confirm whether v0.21 exposes a JSON-to-schema FFI (likely
     `ffi::schema_from_json` or `ffi::parse_schema` — verify in the
     generated header after `make debug`).
   - If yes, the cpp-coder writes a small `DuckDB ColumnList -> Delta
     StructType JSON` serializer (pure data formatting, no I/O, no
     kernel) and feeds that into the FFI. This belongs in
     `delta_utils.cpp` next to `KernelUtils`.
   - If no JSON path exists at v0.21, the cpp-coder uses whatever
     visitor-based schema-build entry point the FFI exposes (analogous
     to the read-side `SchemaVisitor` but inverted).
   This is the one verification gate before implementation starts.

2. **`allow_create` attach-option naming and default.** Default `false`
   is recommended (no silent typo-creates-a-table). Confirm name and
   default.

3. **`CREATE OR REPLACE TABLE` semantics.** v1 proposes
   `BinderException`. The "correct" Delta semantic is uncertain (write
   a metadata-change commit on the existing log vs. drop the log and
   re-create — they have very different time-travel implications).
   Defer to a follow-up.

4. **Multi-statement transaction: CTAS + INSERT.** v1 proposes:
   CTAS commits its own kernel transaction at `Finalize`; subsequent
   INSERTs in the same SQL transaction go through the standard INSERT
   path and produce commit `01.json`. The alternative — bundle the
   CTAS adds and subsequent inserts into a single first commit —
   requires inventing a switchover from `ExclusiveCreateTransaction`
   to `ExclusiveTransaction` mid-transaction, which the kernel does
   not appear to support. Confirm v1 splits the commits.

5. **CCv2 CTAS scope for v1.** v1 plan supports CCv2 CTAS by
   re-binding `create_table_builder_build_with_committer` to the same
   `CommitCallback` mechanism the INSERT path uses today. Confirm
   whether to ship CCv2 CTAS support in the first PR or to gate it
   behind a follow-up (the diff is small but the test fixture is
   non-trivial).

6. **Table properties for v1.** v1 ships with zero
   `create_table_builder_with_table_property` calls — kernel-default
   protocol version, no column mapping, no deletion vectors. Confirm.

7. **Idempotency app_versions on CTAS.** Today
   `DeltaTransaction::SetTransactionVersion` registers app/version
   pairs that are flushed via `ffi::with_transaction_id` in
   `Commit()`. We need to confirm whether v0.21's
   `ExclusiveCreateTransaction` has an analogous
   `with_transaction_id`-equivalent. If not, v1 disables idempotency
   helpers when the active transaction is in CREATING_TABLE mode and
   throws `NotImplementedException` if a user calls them in that
   state. The cpp-coder verifies in the generated header.

8. **Engine-info string.** v1 uses `"DuckDB"` for both the
   `get_create_table_builder` engine_info argument and the
   `create_table_with_engine_info` call (if invoked). Matches existing
   INSERT path. Confirm we want this on the first commit too.

9. **CTAS into a non-default schema.** DuckDB's CTAS can route through
   `op.schema`, but the Delta extension only has one schema. CTAS into
   any non-main schema → `BinderException`. Confirm.

10. **Rollback file cleanup ordering.** The CTAS path stages parquet
    via `PhysicalCopyToFile` *before* calling
    `create_table_add_files`. On rollback we must remove those parquet
    files (existing `CleanUpFiles()` does this). Confirm that we do
    NOT attempt to remove the parent directory itself, because the
    user may have other content there (think: `ATTACH 'workdir'`,
    `allow_create=true`, CTAS, rollback). Files only; never `rmdir`.
