# Feature Summary: require `unity_table_id` when `parent_commit=true` (+ `unity_catalog` alias)

## What Was Built

Closed the M3 silent-footgun raised in the prior CCv2 CTAS pipeline: both the INSERT and CTAS paths used to silently fall back to the table filesystem path when `unity_table_id` was empty under `parent_commit=true`. That fallback satisfied the kernel's UC committer validator (the property and the committer field would both be set to the same path string, so the "must match" check passed) but produced an opaque error from the real Unity Catalog REST API, which requires `tableId` to be a UUID. This PR replaces both fallbacks with an ATTACH-time `InvalidInputException` that fires once, immediately, with a clear message — plus a new `unity_catalog` ATTACH option that's an honest user-facing alias for `parent_commit` (the latter remains accepted, with a `LOG_INFO` deprecation hint).

## Architecture Decisions

- **ATTACH-time validation, single site.** The check `parent_commit && unity_table_id.empty()` runs once in `DeltaCatalogAttach`, after the option-parse loop and before the commit-function lookup. `InvalidInputException` matches the established convention for ATTACH-time validation. Validation at ATTACH means the error surfaces at the earliest possible moment (the user gets it as soon as they try to attach with the bad config, not when they later try to INSERT or CTAS).
- **`D_ASSERT(!unity_table_id.empty())` guards at use sites.** Once ATTACH validates, the non-empty invariant is programmer-checked at the three CCv2 use sites (INSERT line ~586, CTAS lines ~716 + ~752). `D_ASSERT` is correct here — these are programmer invariants because user input is already validated upstream, not user-triggerable conditions.
- **`unity_catalog=true` alias, not a rename.** The architect's option (c) — additive: setting `unity_catalog=true` implies the same internal `parent_commit` flag. Both options work; the new name is honest about what the feature does today (Unity Catalog, not generic CCv2). The legacy `parent_commit=true` continues to work; the C++ field name (`DeltaCatalog::parent_commit`) is intentionally not renamed because that would balloon the diff for no behavioral benefit. A field-rename can be a future cosmetic cleanup.
- **Investigation-driven scope justification.** Before this PR I ran a focused investigation that established three load-bearing facts: (a) INSERT and CTAS had the same fallback (so M3 was a both-paths bug, not a CTAS follow-up); (b) the kernel's UC committer requires `io.unitycatalog.tableId` to match its stored `table_id`, both of which we were setting to the path string under fallback (silently passing the kernel check but failing the REST call); (c) the FFI exposes only `get_uc_committer`, no generic CCv2 committer, so `parent_commit=true` is effectively a Unity Catalog flag — and the "block future non-UC CCv2 backends" concern that would have argued against requiring `unity_table_id` does not apply at v0.23.

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `src/delta_extension.cpp` | Modified | New `unity_catalog` ATTACH option (boolean, alias for `parent_commit`); `LOG_INFO` deprecation hint when the legacy `parent_commit` name is used directly; new `InvalidInputException` validator for missing `unity_table_id` |
| `src/storage/delta_transaction.cpp` | Modified | Removed three `unity_table_id.empty() ? path : unity_table_id` fallbacks (one INSERT, two CTAS); added matching `D_ASSERT(!unity_table_id.empty())` guards; updated comments |
| `src/include/storage/delta_catalog.hpp` | Modified | Doc-comment on `parent_commit` field noting `unity_catalog` is the preferred user-facing name |
| `test/sql/main/writing/ctas/ctas_ccv2.test` | Modified | Test 2's ATTACH switched from `parent_commit true` to `unity_catalog true` for end-to-end alias coverage |
| `test/sql/main/writing/ctas/ctas_ccv2_validation.test` | Created | 3 negative cases covering the new validator |

## sqllogic Tests Added

- `test/sql/main/writing/ctas/ctas_ccv2_validation.test` — 3 negative cases:
  - `parent_commit true` with no `unity_table_id` → expects `InvalidInputException`
  - `parent_commit true` with explicit empty `unity_table_id ''` → same error
  - `unity_catalog true` with no `unity_table_id` → same error (proves the alias triggers the same validator)
- `ctas_ccv2.test` Test 2 — flipped to `unity_catalog true` for alias end-to-end coverage

**Test status (release build):** 261 assertions in 11 CTAS test cases. 1 test (`ctas_ccv2.test` itself) is skipped under release because it calls `__internal_delta_test_ccv2_commit_staged` which is DEBUG-only; the validation test correctly does NOT use `require debug` (H1 fix from review).

## Review Status

- **Verdict: APPROVED after 1 iteration.**
- Iter 1 (NEEDS_CHANGES: 1 High):
  - **H1** — `ctas_ccv2_validation.test` had `require debug` but the validator it tests is unconditional in release. Fixed by removing the `require debug` line; 3 negative cases now run in release CI.
- **Outstanding Medium/Low items** (out of orchestrator scope):
  - **M1**: `unity_catalog=false` cannot override `parent_commit=true` (additive-only). Surprising precedence; add a comment beside the handlers (or in a follow-up).
  - **M2**: Deprecation hint fires when user passes BOTH `parent_commit=true` AND `unity_catalog=true`. Suppress when `new_unity_catalog_used` is also true (one-line fix).
  - **L1**: `LOG_INFO` is gated on `delta_kernel_logging`; production users won't see the deprecation hint. `LOG_WARNING` is the established level for messages that should reach users by default.

## Performance

- **Verdict: OPTIMIZED (0 findings)**
- The diff is performance-positive: removed three runtime `string::empty() ? path : unity_table_id` ternaries on the CCv2 commit path (saved 1 branch + 1 string-empty test + a potential string copy per commit attempt). ATTACH-time validation is a single string-empty check on a cold path. `D_ASSERT` is a no-op in release.

## Items for Human Review

1. **M1, M2, L1 follow-ups** — each is a 1-2 line fix; trivial. Can ship as a small cleanup PR after this one (or roll in before merge if you want).
2. **No INSERT-side sqllogic test for CCv2** (architect's OQ-1, confirmed). The INSERT-side fix lands without a sqllogic regression test because there's no CCv2 INSERT test infrastructure today. Building one would be scope creep — same shape as the CTAS test fixture but for `transaction_with_committer`. Worth a future follow-up.
3. **C++ field name `DeltaCatalog::parent_commit` was deliberately not renamed.** Behavior gate vs. option name is a deliberate split — the field name is internal. A field rename can be cosmetic cleanup later if you want.
4. **Deprecation strategy for `parent_commit`** — current plan is `LOG_INFO` hint, never hard-error or remove. If you want to eventually hard-deprecate, file a tracking issue with a target kernel version or major release for removal.
5. **No kernel `GIT_TAG` bump.** Pin still v0.23.0.
