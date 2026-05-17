# Review Findings: unity_table_id Requirement + unity_catalog Alias PR

## Review Summary

The change is well-structured and follows the architecture plan closely. The validator placement, exception type, and D_ASSERT classifications are correct. The `unity_catalog` alias wires cleanly into the internal `parent_commit` gate without touching unrelated code. The D_ASSERT invariant arguments are valid (parent_commit=true on a DeltaCatalog can only originate from DeltaCatalogAttach, which now enforces the constraint). There are no correctness bugs, no lifetime issues, no FFI handle leaks, and no user-triggerable asserts. The critical gap is that the new validation test file carries `require debug` without needing it, meaning the negative validation coverage is silently skipped in release CI.

---

## Critical (must fix before merge)

None.

---

## High (should fix)

### [H1] `ctas_ccv2_validation.test` carries `require debug` without needing it

- **Location**: `test/sql/main/writing/ctas/ctas_ccv2_validation.test:14`
- **Standard / Issue**: The three negative cases in this file test only the ATTACH-time `InvalidInputException` thrown by the validator in `DeltaCatalogAttach`. That code path contains no `#ifdef DEBUG` guard, no `D_ASSERT`, and no debug-only function. The validator fires unconditionally in both debug and release builds. By marking the file `require debug`, these negative tests are silently skipped in release CI, which is exactly the build configuration most likely to encounter a regression in this code path. The `require debug` constraint is valid for `ctas_ccv2.test` because that file exercises `__internal_delta_test_ccv2_commit_staged` (registered only in DEBUG builds); it is not valid here.
- **Fix**: Remove `require debug` from `ctas_ccv2_validation.test`. All three ATTACH statements are expected to fail with the validator error before the commit-function lookup runs, so the absence of `__internal_delta_test_ccv2_commit_staged` in the system catalog is irrelevant.

```diff
-require debug
-
 # ---------------------------------------------------------------------------
 # Case 1: parent_commit true with no unity_table_id -- must raise an error.
```

---

## Medium (nice to fix)

### [M1] Contradicting option pair `unity_catalog=false` + `parent_commit=true` silently enables CCv2 mode

- **Location**: `src/delta_extension.cpp:52-62`
- **Standard / Issue**: The `unity_catalog` and `parent_commit` option handlers both use `if (option.second.GetValue<bool>())` guards that only fire when the value is `true`; neither can clear the `parent_commit` flag. If a user passes `ATTACH ... (parent_commit true, unity_catalog false, ...)`, the deprecation warning fires, the flag stays `true`, and CCv2 mode is active despite the explicit `unity_catalog false`. While this combination is impractical, the behavior is surprising and undocumented. The precedence (last-writer-wins for `true`, but `false` is a no-op) is not stated anywhere.
- **Fix**: One of two options:
  1. Document the behavior with a comment beside the option handlers.
  2. Change the `unity_catalog` handler to also handle `false` explicitly — if `unity_catalog false` is the only option, keep existing behavior (no change), but if `parent_commit` was already set, clear it. However, this second approach touches established semantics and should be a separate issue. The comment-only fix is sufficient for this PR.

```cpp
// Note: neither unity_catalog=false nor parent_commit=false can *disable* a CCv2
// mode that was enabled by the other option. They are additive-only. If both are
// specified with contradicting values, the 'true' value wins.
```

### [M2] Deprecation hint fires when user passes both `parent_commit=true` AND `unity_catalog=true`

- **Location**: `src/delta_extension.cpp:52-62`, 80-83
- **Standard / Issue**: `legacy_parent_commit_used` is set whenever the `parent_commit` key appears with value `true`, regardless of whether `unity_catalog true` was also passed. A user who writes `(parent_commit true, unity_catalog true, ...)` receives the deprecation hint even though they have already adopted the new name. This is a false positive for the deprecation signal, though it causes no functional harm.
- **Fix**: Track whether `unity_catalog=true` was also set, and suppress the deprecation when it was.

```cpp
bool legacy_parent_commit_used = false;
bool new_unity_catalog_used = false;
// ...
if (StringUtil::Lower(option.first) == "parent_commit") {
    if (option.second.GetValue<bool>()) {
        res->parent_commit = true;
        legacy_parent_commit_used = true;
    }
}
if (StringUtil::Lower(option.first) == "unity_catalog") {
    if (option.second.GetValue<bool>()) {
        res->parent_commit = true;
        new_unity_catalog_used = true;
    }
}
// ...
if (legacy_parent_commit_used && !new_unity_catalog_used) {
    DUCKDB_LOG_INTERNAL(context, "delta.Attach", LogLevel::LOG_INFO,
                        "parent_commit is deprecated; prefer unity_catalog true");
}
```

---

## Low (style suggestions)

### [L1] Deprecation notice uses `LOG_INFO`, which is not user-visible by default

- **Location**: `src/delta_extension.cpp:81-83`
- **Standard / Issue**: DuckDB's logging system is off by default; `LOG_INFO` messages are only visible when the user has explicitly enabled logging via `SET delta_kernel_logging=true`. A deprecation notice aimed at production users who have not enabled debug logging will never be seen. `LOG_WARNING` is the conventional level for notices the user should act on (as seen in `delta_utils.cpp:1216` which maps the kernel's `WARN` tier to `LOG_WARNING`).
- **Fix**: Use `LogLevel::LOG_WARNING` instead of `LogLevel::LOG_INFO`.

```cpp
DUCKDB_LOG_INTERNAL(context, "delta.Attach", LogLevel::LOG_WARNING,
                    "parent_commit is deprecated; prefer unity_catalog true");
```

### [L2] `return std::move(res)` inhibits copy elision

- **Location**: `src/delta_extension.cpp:121`
- **Standard / Issue**: Returning a named `unique_ptr` local with an explicit `std::move` is equivalent to a move construction; it is not wrong, but it prevents NRVO in C++17. This pattern appears throughout the project (it is a pre-existing style choice), so this is a note rather than a blocking concern for this PR.
- **Fix**: `return res;` — NRVO applies automatically for named local unique_ptrs in C++17. No behavior change, but consistent with the C++17 guarantee. Given this is project-wide, leave as-is or fix project-wide; do not mix styles within a file.

---

## What's Done Well

1. **Validator placement is exactly right.** The `InvalidInputException` fires after option parsing and before the commit-function lookup (`res->parent_commit` block at line 95). This gives the most specific error first and avoids wasted work — a `parent_catalog 'noparent'` that doesn't exist would produce a confusing "Cannot find commit function" error without the validator; with it the user gets "unity_table_id is required" immediately.

2. **D_ASSERT invariant classification is correct.** Both assert sites (`delta_transaction.cpp:586` and `:711`) are inside `if (parent_commit)` branches. `DeltaTransaction::parent_commit` is copied from `DeltaCatalog::parent_commit` at construction (line 34), and `DeltaCatalog::parent_commit` is only set to `true` in `DeltaCatalogAttach` at lines 54 and 60 — after the validator that prevents an empty `unity_table_id`. There is no code path from C++ (test or production) that sets `parent_commit=true` and bypasses the validator. The asserts are correctly classified as programmer invariants, not user-triggerable conditions. The accompanying comments explaining the invariant also satisfy DuckDB's contributor guide requirement.

3. **Alias semantics are clean and non-breaking.** `unity_catalog=true` sets the same internal flag as `parent_commit=true`. Existing tests that use `parent_commit true` with a valid `unity_table_id` continue to pass unchanged. The new `unity_catalog true` alias is exercised end-to-end in Test 2 of `ctas_ccv2.test` (the full CTAS+commit+read-back path), not just at the validator level.

---

## Verdict: NEEDS_CHANGES

- Critical issues: 0
- High issues: 1 (H1 — `require debug` on the validation test file incorrectly prevents release CI coverage of the new validator)
