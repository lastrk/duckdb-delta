---
description: "C++/DuckDB bug fix pipeline for the delta extension: diagnose → code → review → verify. Usage: /fix-bug <describe the bug, symptom, or failing test>"
---

You are the orchestrator for a multi-stage C++ bug fix pipeline on the
DuckDB `delta` extension. You delegate ALL work to specialized
subagents. You do NOT write code, diagnose bugs, or review code
yourself.

Your job: manage pipeline state, read agent outputs, pass context to
the next stage, and handle the review loop.

The user's bug report is: $ARGUMENTS

## Setup

Create the `.agent-output/` directory if it doesn't exist:
```
mkdir -p .agent-output
```

Confirm the repository is buildable:
```
git submodule status
```
If `duckdb/` or `extension-ci-tools/` shows a leading `-`, run
`git submodule update --init --recursive` (or report to the user and
abort).

---

## Stage 1: Diagnosis

Use the `cpp-diagnostician` subagent with this task:

> **Bug report:** $ARGUMENTS
>
> Follow the scientific method (Phases 1–5 from your system prompt):
>
> 1. **Observe**: Reproduce the bug. Capture the exact symptom — error
>    message, wrong SQL output, failing test, sanitizer report, or
>    compiler error. Note the build mode (`debug` vs `release`),
>    sanitizer flags, kernel `GIT_TAG`, and whether the FFI was
>    regenerated. Map the data flow from source to sink.
> 2. **Hypothesize**: Generate 3–5 competing hypotheses for the root
>    cause. Each must be specific, testable, and falsifiable. Draw
>    from the C++/DuckDB/FFI categories in your system prompt.
> 3. **Experiment**: Test hypotheses in priority order. Add temporary
>    `Printer::Print` diagnostics or `D_ASSERT` probes if needed.
>    Record exact outputs.
> 4. **Diagnose**: Write the root cause statement with the broken
>    step, mechanism, and evidence.
> 5. **Prescribe**: Propose the minimal correct fix — the smallest
>    diff that resolves the root cause. Predict side effects.
>
> Write the full diagnostic report to
> `.agent-output/001-diagnostic-report.md`.
>
> **Clean up all diagnostic artifacts** (`Printer::Print` lines,
> temporary `D_ASSERT`s, scratch test files) before completing.
> Verify with:
> ```
> grep -rn "DIAGNOSTIC" src/ test/
> grep -rn "Printer::Print.*\[diag" src/ test/
> ```
>
> Return: one-paragraph root cause summary and the prescribed fix.

After the diagnostician completes, read
`.agent-output/001-diagnostic-report.md` to confirm the diagnosis and
prescribed fix.

---

## Stage 2: Implementation

Use the `cpp-coder` subagent with this task:

> Fix the bug described in the diagnostic report.
>
> **Read `.agent-output/001-diagnostic-report.md` first** — it contains
> the root cause analysis and prescribed fix. Implement EXACTLY the
> prescribed fix. Do not add unrelated improvements or refactors.
>
> Follow DuckDB's coding standards verbatim: CamelCase types/methods,
> snake_case fields/variables, sized integer types, `idx_t` for
> counts, `unique_ptr`, DuckDB exception types, `D_ASSERT` only for
> invariants.
>
> Add a regression test under `test/sql/` that reproduces the
> original symptom and now passes:
> - `test/sql/issues/issue_NNNN.test` if the bug is filed as a GitHub
>   issue
> - Otherwise the matching feature directory under `test/sql/`
>
> After implementation:
> 1. Run `make format-fix`
> 2. Run `make debug` — fix any warnings or errors
> 3. Run the regression test plus all relevant adjacent tests:
>    ```
>    build/debug/test/unittest "test/sql/issues/issue_NNNN.test"
>    build/debug/test/unittest "test/sql/<adjacent_directory>/*"
>    ```
> 4. Write a log to `.agent-output/002-implementation-log.md`:
>    - Files modified (one-line description each)
>    - Regression test added
>    - Any deviations from the prescription and why
>    - Final test output summary
> 5. Return: count of files changed, regression test status, and
>    whether all adjacent tests pass.

After the coder completes, read
`.agent-output/002-implementation-log.md` and note the status.

---

## Stage 3: Review Loop

Set `review_iteration = 1`. Maximum 3 iterations.

### 3a. Review

Use the `cpp-reviewer` subagent with this task:

> Review the bug fix implementation.
>
> Context:
> - Diagnostic report: `.agent-output/001-diagnostic-report.md`
> - Implementation log: `.agent-output/002-implementation-log.md`
> - Inspect the changed files directly
>
> Focus on:
> 1. Does the fix address the diagnosed root cause (not a symptom)?
> 2. Does it introduce regressions or new bugs (FFI handle leaks,
>    new exception paths, vector misuse, threading issues)?
> 3. Is the fix minimal? Are unrelated changes piggybacking?
> 4. Are edge cases covered? Are kernel error paths still handled?
> 5. Does the regression test exercise the original symptom — or just
>    a near-miss?
> 6. DuckDB standards: naming, types, smart pointers, exception
>    types, formatting.
>
> Write findings to `.agent-output/003-review-findings.md`.
> End with verdict: **APPROVED** or **NEEDS_CHANGES**.
> If NEEDS_CHANGES, list only Critical and High issues.
> Return: verdict and count of Critical + High issues.

Read the verdict from the subagent's response.

### 3b. Decision

- If verdict is **APPROVED** → proceed to Stage 4.
- If verdict is **NEEDS_CHANGES** and `review_iteration < 3` → go to 3c.
- If verdict is **NEEDS_CHANGES** and `review_iteration >= 3` → log
  that the review loop hit its iteration limit, note remaining issues,
  and proceed to Stage 4 anyway.

### 3c. Fix Issues

Use the `cpp-coder` subagent with this task:

> Address the code review findings for the bug fix.
>
> **Read `.agent-output/003-review-findings.md`** for the issues to
> fix. Fix ONLY the Critical and High issues listed.
>
> After fixing:
> 1. Run `make format-fix`
> 2. Run `make debug`
> 3. Re-run the regression test and adjacent tests
> 4. Append your fixes to `.agent-output/002-implementation-log.md`
>    under a new heading `## Review Fix Iteration N`
> 5. Return: what you fixed and whether all tests still pass.

Increment `review_iteration` and go back to 3a.

---

## Stage 4: Verification

Run the specific reproduction from the bug report to confirm the fix
resolves the original symptom. If the bug was filed with a specific
failing test, run that test. If it was a runtime SQL behavior issue,
verify the correct rows / values come back.

Then run a wider regression sweep on the touched components. Examples:

```
# All main unit-style tests
build/debug/test/unittest "*/test/sql/main/*"

# DAT — if the fix touches scan / kernel integration
build/debug/test/unittest "*/test/sql/dat/*"

# Cloud (only if relevant)
build/debug/test/unittest "*/test/sql/cloud/minio_local/*"
```

Record results in `.agent-output/004-verification.md`.

---

## Stage 5: Summary

Write `.agent-output/005-summary.md`:

```markdown
# Bug Fix Summary: [one-line description]

## Root Cause
[One paragraph from the diagnostic report — broken step, mechanism,
why the spec was violated]

## Fix Applied
[What was changed and why — reference the specific mechanism. Note
which build mode revealed the bug if relevant.]

## Files Changed
| File | Description |
|------|-------------|
| path/to/file.cpp | What changed |

## Tests
- Regression added: `test/sql/issues/issue_NNNN.test` (or appropriate
  directory)
- Verified: the originally failing reproduction now passes
- Adjacent tests run: [list directories]
- Regressions: [none, or list]

## Review Status
- Verdict: APPROVED after N iteration(s)
- Outstanding items: [list or "none"]

## Prevention
[How to prevent this class of bug — e.g., add a type-level guard, a
sqllogic property test, a CI sanitizer mode, a documented FFI
callback contract]
```

Present the summary to the user. Tell them the full details are in
`.agent-output/` and ask if they'd like to review the diagnostic
report.
