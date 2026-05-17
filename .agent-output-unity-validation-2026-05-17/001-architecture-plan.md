# Architecture Plan: Require `unity_table_id` for CCv2 commits + (optional) honest rename

## Scope

Two coupled fixes plus an optional rename, all in a single PR:

1. **Remove the silent path fallback** in both the INSERT-side
   (`DeltaTransaction::InitializeTransaction`) and the CTAS-side
   (`DeltaTransaction::InitializeForNewTable`) CCv2 code paths. Today
   both paths execute `unity_table_id.empty() ? path : unity_table_id`
   and pass that string to (a) the `io.unitycatalog.tableId` metadata
   property and (b) `ffi::get_uc_committer`. The kernel's
   in-process validator only checks that the metadata property and the
   committer's stored `table_id` match — which they trivially do with
   the fallback — but the real Unity Catalog REST `CommitClient`
   downstream will reject the table path with an opaque error. This is
   a footgun.

2. **Add a single ATTACH-time validator** that rejects `parent_commit
   true` (or its new alias, see decision below) without a non-empty
   `unity_table_id`. Fire as early as possible: in
   `DeltaCatalogAttach` in `src/delta_extension.cpp`.

3. **Bonus rename**: add `unity_catalog true` as an honest alias that
   implies `parent_commit true`. Keep `parent_commit true` working
   for back-compat. Defer string-valued `child_catalog_type='unity'`
   to a future PR (not needed at v0.23 since UC is the only reachable
   backend).

## Decisions

### D1: Discriminator rename — option (c) "alias with implied semantics"

Adopt option (c) from the user's brief. Concretely:

- Add a new ATTACH boolean option `unity_catalog`. When `true`, it
  enables the same code path that `parent_commit true` enables today
  (kernel UC committer, parent-catalog commit function lookup).
- Keep `parent_commit` working as a literal alias — accept it, log a
  one-line deprecation hint at `LOG_INFO`, set the same internal flag.
  (DuckDB has no first-class deprecation mechanism for ATTACH options;
  a log line is the lowest-friction signal. Do NOT throw, since
  back-compat must hold.)
- Internally, keep the field name `parent_commit` on
  `DeltaCatalog` and `DeltaTransaction` for v1 — the field is the
  *behavior gate*, not the *option name*. Renaming the field too
  would balloon the diff for no end-user benefit and would invite
  reviewers to question every touched line. A field-name cleanup can
  be a separate PR.

Rationale: option (c) gives us the honest user-facing name without
breaking existing users. It also future-proofs: if v0.24 lands a
non-UC CCv2 backend, we have headroom to introduce
`child_catalog_type` (a string) and treat `unity_catalog true` as
shorthand for `child_catalog_type 'unity'`.

### D2: Validation placement — ATTACH only

Validate in `DeltaCatalogAttach` (`src/delta_extension.cpp`) only.
Reasoning:

- ATTACH-time validation surfaces the error before any kernel handle
  is built, before any commit-function lookup, and before the user
  has invested in a CTAS or INSERT plan. That is the right UX moment.
- Validating in `InitializeTransaction` / `InitializeForNewTable` as
  a defensive check would duplicate code with no real failure mode
  (the ATTACH path is the only producer of `DeltaCatalog::parent_commit`
  and `DeltaCatalog::unity_table_id`).
- The existing precedent in the same function for missing
  `commit_function` (`InvalidInputException` at lines 87-92) shows
  ATTACH-time `InvalidInputException` is the established pattern.

The `D_ASSERT(!parent_commit || !unity_table_id.empty())` line at the
top of the two CCv2 branches in `delta_transaction.cpp` is the
secondary belt-and-suspenders guard. Use `D_ASSERT`, not an
exception — this is an invariant that the ATTACH validator already
enforces; a violation here means a programmer mistake, not user
input.

### D3: `child_catalog_mode` and `unity_table_id` are independent

Note for the reviewer: `child_catalog_mode` is a different ATTACH
option than `parent_commit`. Today they happen to be set together
in CCv2 flows, but the validation must NOT cross-couple them — only
`parent_commit` / `unity_catalog` implies the `unity_table_id`
requirement.

### D4: `max_catalog_version` re-attach path is read-only

The second test in `ctas_ccv2.test` re-attaches with only
`max_catalog_version 0` (no `parent_commit`, no `unity_table_id`).
That is a read-only re-attach against a catalogManaged table whose log
is now self-contained. The new validator must NOT touch this path.
Since the validator only fires when `parent_commit || unity_catalog`,
this is satisfied automatically.

## Domain Constraints (Layer 3)

- The Delta kernel's UC committer (`delta-kernel-unity-catalog/src/committer.rs:86-95`)
  requires the `io.unitycatalog.tableId` property in the table's
  Metadata configuration AND a matching `table_id` argument to
  `ffi::get_uc_committer`. The two values are compared at commit
  build time.
- The real UC REST commit client (downstream of the in-process
  validator) parses the `table_id` as a UUID and rejects non-UUID
  strings with an opaque error message — making the current
  fallback ("use the table path") a production-only failure.
- At kernel v0.23 the FFI only exposes UC-flavored committer
  constructors (`get_uc_commit_client`, `get_uc_committer`); there is
  no generic CCv2 committer. Therefore `parent_commit true` IS
  effectively a Unity Catalog flag today.
- DuckDB convention for ATTACH-option validation errors is
  `InvalidInputException` (see existing line 88 in
  `delta_extension.cpp` for the commit-function-missing case).

## Affected Surfaces

### `src/`

- `src/delta_extension.cpp`
  - `DeltaCatalogAttach`: parse the new `unity_catalog` ATTACH option;
    add validator that `parent_commit` (post-alias resolution) implies
    a non-empty `unity_table_id`.
- `src/storage/delta_transaction.cpp`
  - `InitializeTransaction` (line 583-590): remove the
    `unity_table_id.empty() ? path : unity_table_id` fallback;
    replace with a `D_ASSERT(!unity_table_id.empty())` and use
    `unity_table_id` directly.
  - `InitializeForNewTable` CCv2 branch (lines 705-767): same removal
    at two call sites — the `io.unitycatalog.tableId` property value
    (line 716) and the `get_uc_committer` `table_id` argument
    (line 752). Replace with the bare `unity_table_id` plus
    `D_ASSERT(!unity_table_id.empty())` at the top of the
    `if (parent_commit)` block.
  - Drop the now-stale CCv2 comments that mention the fallback.

### `src/include/`

- `src/include/storage/delta_catalog.hpp`
  - No new fields. The alias `unity_catalog` is resolved at parse time
    into the existing `parent_commit` boolean (option (c) of D1).
    Add a short comment near the `parent_commit` field noting that
    the user-facing name is `unity_catalog` and `parent_commit` is
    retained as an alias.

- No changes to `src/include/storage/delta_transaction.hpp`.

### `test/sql/`

- `test/sql/main/writing/ctas/ctas_ccv2.test`
  - Already sets `unity_table_id` explicitly in both ATTACH blocks
    (lines 36 and 88). No change required for the happy path.
  - Add a small block at the end of the file: a negative test that
    ATTACH-ing with `parent_commit true` but no `unity_table_id`
    raises the expected error.
  - Optionally, swap one of the two existing ATTACH blocks to use
    `unity_catalog true` instead of `parent_commit true` to exercise
    the alias.

- New `test/sql/main/writing/ctas/ctas_ccv2_validation.test`
  - Dedicated negative-test file. Three cases:
    1. `parent_commit true` without `unity_table_id` → error.
    2. `parent_commit true` with empty-string `unity_table_id ''`
       → error.
    3. `unity_catalog true` without `unity_table_id` → same error
       (proves the alias triggers the same validator).
  - The file is small enough that folding into `ctas_ccv2.test` is
    fine; a separate file is preferable so the negative cases live
    next to each other and are easy to extend when we add UPDATE /
    DELETE CCv2 paths.

- `CMakeLists.txt` — no change. (Test files are picked up by glob.)

### FFI / kernel

- No FFI changes. No `prefix.inc` / `suffix.inc` / generator script
  changes.
- No kernel `GIT_TAG` change.

## Ownership Map

No new kernel handles. The fix is in argument-marshalling and
input-validation only. Ownership of the existing kernel handles
remains:

```
DeltaTransaction (owner)
├── kernel_create_txn : KernelExclusiveCreateTransaction (RAII)
├── kernel_transaction : ffi::Handle<ffi::ExclusiveTransaction>
└── ctas_extern_engine : kernel engine handle (RAII via shared_ptr)

DeltaCatalog (owner, immutable post-ATTACH)
├── parent_commit : bool                  // gate
├── unity_table_id : string               // newly REQUIRED when gate==true
└── commit_function : TableFunctionCatalogEntry* (non-owning)
```

The newly-required `unity_table_id` is plain owned data on
`DeltaCatalog` (already is); we are only tightening the invariant
that it is non-empty when `parent_commit` is true. The invariant is
enforced at the catalog's construction boundary
(`DeltaCatalogAttach`).

## Validation Strategy

### Where the validator fires

`DeltaCatalogAttach` in `src/delta_extension.cpp`, after the option
loop and **before** the parent-commit-function lookup that already
runs there (lines 72-94). Order matters: if `unity_table_id` is
missing, we should not waste time on the catalog lookup.

Concrete placement:

```
parse all options into res
if (res->parent_commit && res->unity_table_id.empty())
    throw InvalidInputException(...)
// then the existing commit-function lookup block
```

### What it checks

The flag (`parent_commit` after alias resolution) is true AND
`unity_table_id` is empty (`.empty()` on the parsed `string`).
Whitespace-only strings: do NOT trim. A user who supplied
`unity_table_id '   '` deserves a clearer error than the kernel will
give them, but trimming has subtle implications for non-Latin
identifiers. Leave it raw for v1.

We also do NOT validate UUID format. Reasons:

- The test fixture uses non-UUID strings (`'test-table-id-1'`); we
  must not break that.
- The kernel + the UC REST client are the authoritative validators.
  Re-implementing UUID parsing in the extension adds a maintenance
  burden with no upside.
- The error message tells the user the value SHOULD be a UUID; that
  is sufficient guidance.

### Exception type

`InvalidInputException`. This matches:

- The existing CCv2 commit-function-missing error a few lines below.
- The "Cannot create a Delta table in read-only mode" and "Can not
  append to a read only table" errors in `delta_transaction.cpp`.
- DuckDB's general convention for ATTACH-option validation errors.

`BinderException` is wrong here (this is not SQL binding, it is
catalog attach). `CatalogException` is closer but is conventionally
reserved for "entry not found" / "duplicate entry" semantics.

### Error message

Exact wording (load-bearing — please use verbatim or near-verbatim):

> `unity_table_id` is required when `unity_catalog=true` (or the
> alias `parent_commit=true`). The Unity Catalog committer rejects
> commits whose `io.unitycatalog.tableId` does not match the
> registered table's id. Pass the table's UUID, e.g.
> `unity_table_id '01234567-89ab-cdef-0123-456789abcdef'`. You can
> retrieve it from Unity Catalog with `DESCRIBE TABLE EXTENDED` or
> the `GET /tables` REST API.

(The exact format of the error string is up to the implementer;
preserve the three load-bearing facts: option name, that it must be
a UUID matching the UC registration, and a hint about where to get
the value.)

## Migration Order

Each step compiles and existing tests pass before the next one
starts.

1. **Add the alias option** (no behavior change yet):
   - In `DeltaCatalogAttach`, recognise `unity_catalog` and treat it
     as a synonym for `parent_commit`.
   - At this point, the tree compiles. No existing test changes
     behavior.

2. **Add the validator** (this is the first behavior change):
   - After option parsing, throw `InvalidInputException` when
     `parent_commit && unity_table_id.empty()`.
   - The existing `ctas_ccv2.test` already sets `unity_table_id` on
     both ATTACH blocks, so this step does NOT break existing tests.
     This is important — it means the test-fixture update mentioned
     in the user's brief is already done by a prior PR.

3. **Remove the fallbacks** in `delta_transaction.cpp`:
   - INSERT branch line 586: `path` fallback → `unity_table_id`.
   - CTAS branch line 716: `ctas_table_path` fallback → `unity_table_id`.
   - CTAS branch line 752: `ctas_table_path` fallback → `unity_table_id`.
   - Add `D_ASSERT(!unity_table_id.empty())` at the top of each
     `if (parent_commit)` block.
   - Drop the stale comments that mention the fallback (e.g. "(falling
     back to the table path)" in the CTAS comment block).
   - Tree still compiles. `ctas_ccv2.test` still passes (it sets
     `unity_table_id`). No production user can have reached the
     fallback without an error, because step 2 already prevents it at
     ATTACH time.

4. **Add the negative tests**:
   - New `ctas_ccv2_validation.test` with the three cases listed
     above.

5. **Optional: alias-coverage test**:
   - Modify the second test in `ctas_ccv2.test` (the empty-CTAS one)
     to use `unity_catalog true` instead of `parent_commit true`.
     This proves the alias is wired through the full CTAS+read path,
     not just the validator.

The PR can be one squashed commit OR five small commits in this
order — either is reviewable. Five commits gives a clearer bisect
window if a regression appears.

## Concurrency Plan

No new shared state, no new mutex acquisitions, no new kernel
re-entry. The validator runs synchronously inside
`DeltaCatalogAttach`, on the SQL frontend thread, before any
transaction is built.

`DeltaTransaction::parent_commit` and `DeltaTransaction::unity_table_id`
are copied from `DeltaCatalog` at transaction construction (line 33-35
of `delta_transaction.cpp`); they are immutable after construction.
The validator does not change that.

## Error Strategy

| Failure surface                                                   | Exception                |
|-------------------------------------------------------------------|--------------------------|
| ATTACH: `parent_commit true` (or `unity_catalog true`) with empty `unity_table_id` | `InvalidInputException` |
| ATTACH: commit-function not found in parent catalog (existing)    | `InvalidInputException` |
| ATTACH: read-only re-attach with `max_catalog_version` (existing) | no error                 |
| INSERT: kernel UC committer rejects a real UUID at runtime        | `IOException` (existing) |
| CTAS: `unity_table_id` empty inside `InitializeForNewTable`       | `D_ASSERT` only (debug); production path can't reach this state |

Non-fatal behaviors that must NOT throw:

- A read-only ATTACH (no `parent_commit`, no `unity_catalog`) with
  no `unity_table_id` — passes the validator (gate is off).
- A read-only re-attach with `max_catalog_version` for post-CTAS
  reads — same.

## Test Plan

### Updated files

- `test/sql/main/writing/ctas/ctas_ccv2.test`
  - (Optional alias-coverage change to test 2; see step 5 above.)

### New files

- `test/sql/main/writing/ctas/ctas_ccv2_validation.test`
  - Group `[ctas]`. Requires `parquet`, `delta`, `notwindows`,
    `debug`.
  - Case 1: `ATTACH ... (TYPE delta, allow_create true,
    parent_commit true, parent_catalog 'parent',
    parent_commit_function_name '__internal_delta_test_ccv2_commit_staged');`
    expects `statement error` with message fragment
    `unity_table_id is required`.
  - Case 2: same but with `unity_table_id ''` — same error.
  - Case 3: same but with `unity_catalog true` in place of
    `parent_commit true` — same error (alias coverage).
  - For each case, do NOT detach (the ATTACH itself fails).
  - For each case, attach the parent `:memory:` catalog first so the
    parent-catalog-name lookup does not short-circuit the test.

### Tests that MUST continue to pass unmodified

- Both blocks in `ctas_ccv2.test` (happy paths; `unity_table_id` is
  already set).
- All read-only tests in `test/sql/main/writing/ctas/` that do not
  use `parent_commit` — unaffected.
- All non-CCv2 tests in `test/sql/` — unaffected.

### Tests not in scope

- No INSERT-side CCv2 test exists in the repo today (`grep -rln
  "parent_commit" test/sql` returns only `ctas_ccv2.test`). The
  user's brief asked us to look for one. There isn't one. The
  INSERT-side fix is therefore unobservable by sqllogic tests until
  someone adds a `test/sql/main/writing/insert_ccv2.test`. That is
  out of scope for this PR (the existing `ccv2_test_committer`
  fixture supports only the CTAS path today; extending it to handle
  INSERT requires non-trivial fixture work and is its own PR).
  Mention this as an "Open Question" below — the user may want this
  PR to also ship an INSERT-CCv2 test, in which case scope expands
  significantly.

## Open Questions

- **OQ-1: Test coverage for the INSERT path.** Per the user's brief,
  the INSERT fallback fix is invisible to sqllogic because no
  INSERT-CCv2 test fixture exists. Two options:
  - (a) Ship the INSERT fix without a test (the `D_ASSERT` catches
    debug regressions; the validator catches user error). Recommended.
  - (b) Build out an INSERT-side test fixture in
    `ccv2_test_committer.cpp` so we get coverage. Bigger scope.
  Recommend (a). Confirm before merging.

- **OQ-2: Deprecation log message for `parent_commit`.** If the
  rename in D1 is adopted, do we log a `LOG_INFO` line at ATTACH when
  `parent_commit` is used (and emit nothing for `unity_catalog`)? Or
  do we treat them as fully equivalent with no deprecation signal?
  Recommend: a single `LOG_INFO` line on `parent_commit true`,
  text `"parent_commit is deprecated; prefer unity_catalog true"`.
  Confirm.

- **OQ-3: Should the alias be a hard rename in a follow-up?** If
  yes, file a tracking issue now so the alias does not become
  permanent technical debt. If no, keep `parent_commit` as the
  internal field name forever and treat the user-facing name as a
  presentation concern. Recommend yes (follow-up); leave the alias
  in place for at least one release cycle so external users have
  time to migrate.

- **OQ-4: UUID validation.** Should the extension call into a UUID
  parser (DuckDB has one in `duckdb/common/types/uuid.hpp`) and reject
  obviously-malformed values at ATTACH time? Pros: better error
  earlier. Cons: breaks the test fixture's use of non-UUID strings
  like `'test-table-id-1'`, and the kernel+UC are the authoritative
  validators. Recommend: do NOT validate format. The error message
  tells the user the expected format. Confirm.

- **OQ-5: Should `parent_commit_function_name` also be required
  when `parent_commit true`?** Today it defaults to
  `__internal_delta_ccv2_commit_staged`. That default is the
  production Unity Catalog function name. The test fixture overrides
  it to `__internal_delta_test_ccv2_commit_staged`. Recommend: keep
  the default; do not require it. It is a separate concern from
  `unity_table_id` and the user's brief did not ask for it.
  Confirm.
