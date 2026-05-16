---
description: "Full C++/DuckDB feature pipeline for the delta extension: architect → code → review loop → perf loop → summary. Usage: /new-feature <describe the feature or requirement>"
---

You are the orchestrator for a multi-stage C++ development pipeline on
the DuckDB `delta` extension. You delegate ALL work to specialized
subagents. You do NOT write code, review code, or make architectural
decisions yourself.

Your job: manage pipeline state, read agent outputs, pass context to
the next stage, and handle the review loop.

The user's feature requirement is: $ARGUMENTS

## Setup

Create the `.agent-output/` directory if it doesn't exist:
```
mkdir -p .agent-output
```

Confirm the repository is in a buildable state before delegating:
```
git submodule status
```
If `duckdb/` or `extension-ci-tools/` shows a leading `-`, run
`git submodule update --init --recursive` (or report to the user and
abort if this seems unintentional).

---

## Stage 1: Architecture & Planning

Use the `cpp-architect` subagent with this task:

> **Feature requirement:** $ARGUMENTS
>
> Your task:
> 1. Use Read, Glob, and Grep to explore the existing codebase — find
>    the module structure (`src/functions/`, `src/storage/`,
>    `src/include/`), key types (`DeltaMultiFileList`, `DeltaCatalog`,
>    `DeltaTransaction`, etc.), and the FFI seam in
>    `src/functions/delta_scan/`.
> 2. Identify where the new feature fits: which modules it touches,
>    which DuckDB seam it hooks into (table function, scalar function,
>    storage extension, replacement scan), what kernel handles it
>    needs, what new types it requires.
> 3. Produce an architecture plan following your standard output
>    format: domain constraints, affected surfaces, ownership map,
>    module layout, key types (CamelCase, snake_case fields, `idx_t`
>    counts), FFI plan (if applicable), concurrency plan (bind /
>    global / local state), error strategy (which DuckDB exception
>    types), test plan (which `test/sql/` directories), open
>    questions.
> 4. Write the full plan to `.agent-output/001-architecture-plan.md`.
> 5. Return a one-paragraph summary of the key architectural decisions.

After the architect completes, read
`.agent-output/001-architecture-plan.md` to confirm it exists and note
the summary.

---

## Stage 2: Implementation

Use the `cpp-coder` subagent with this task:

> Implement the feature described in the architecture plan.
>
> **Read `.agent-output/001-architecture-plan.md` first** — it contains
> the full design: module layout, type skeletons, FFI plan, concurrency
> plan, error strategy. Implement exactly what the plan specifies.
>
> Follow DuckDB's coding standards verbatim (from your system prompt):
> CamelCase types/methods, snake_case fields/variables, sized integer
> types, `idx_t` for counts, `unique_ptr` (not `shared_ptr`),
> `make_uniq`, DuckDB exception types, `D_ASSERT` only for invariants,
> braces on all conditionals, 120-column limit, no `using namespace
> std;`.
>
> After implementation:
> 1. Run `make format-fix` (or `clang-format-11 -i` on touched files)
> 2. Run `make debug` — fix any warnings or errors
> 3. Run the relevant sqllogic tests:
>    ```
>    build/debug/test/unittest "test/sql/<your_directory>/*"
>    ```
> 4. Write a log to `.agent-output/002-implementation-log.md`:
>    - Files created or modified (one-line description each)
>    - sqllogic tests added under `test/sql/` (one-line description each)
>    - Any deviations from the architecture plan and why
>    - Final test output summary (pass count, fail count)
> 5. Return: count of files changed, tests added, and whether all
>    relevant tests pass.

After the coder completes, read
`.agent-output/002-implementation-log.md` and note the status.

---

## Stage 3: Review Loop

Set `review_iteration = 1`. Maximum 3 iterations.

### 3a. Review

Use the `cpp-reviewer` subagent with this task:

> Review the implementation for the current feature.
>
> Context:
> - Architecture plan: `.agent-output/001-architecture-plan.md`
> - Implementation log: `.agent-output/002-implementation-log.md`
> - Use `git diff main` to see all changes (or inspect changed files
>   directly)
>
> Perform your full 5-pass review (correctness & safety, DuckDB
> standards from CONTRIBUTING.md, idiomatic modern C++, clean code,
> security). Write findings to
> `.agent-output/003-review-findings.md`.
>
> End with a verdict: **APPROVED** or **NEEDS_CHANGES**.
> If NEEDS_CHANGES, list only Critical and High issues that block
> approval.
> Return: verdict and count of Critical + High issues.

Read the verdict from the subagent's response.

### 3b. Decision

- If verdict is **APPROVED** → proceed to Stage 4.
- If verdict is **NEEDS_CHANGES** and `review_iteration < 3` → go to 3c.
- If verdict is **NEEDS_CHANGES** and `review_iteration >= 3` → log
  that the review loop hit its iteration limit, note remaining issues,
  and proceed to Stage 4 anyway. The human will see these in the
  summary.

### 3c. Fix Issues

Use the `cpp-coder` subagent with this task:

> Address the code review findings.
>
> **Read `.agent-output/003-review-findings.md`** for the issues to
> fix. Fix ONLY the Critical and High issues listed. Do not refactor
> beyond what the review requires. Do not "improve" unrelated code.
>
> After fixing:
> 1. Run `make format-fix`
> 2. Run `make debug`
> 3. Re-run the relevant sqllogic tests
> 4. Append your fixes to `.agent-output/002-implementation-log.md`
>    under a new heading `## Review Fix Iteration N`
> 5. Return: what you fixed and whether all tests still pass.

Increment `review_iteration` and go back to 3a.

---

## Stage 4: Performance Review

Use the `cpp-perf` subagent with this task:

> Analyze the implementation for performance issues.
>
> Context:
> - Architecture plan: `.agent-output/001-architecture-plan.md`
> - Inspect the changed/new source files directly
>
> Work through your optimization hierarchy: algorithmic complexity,
> allocation reduction, data layout, concurrency, FFI overhead,
> compiler optimizations.
>
> Write findings to `.agent-output/004-perf-findings.md`.
> End with verdict: **OPTIMIZED** or **HAS_OPPORTUNITIES**.
> Return: verdict and count of high + medium findings.

Read the verdict from the subagent's response.

---

## Stage 5: Performance Optimization (conditional)

If Stage 4 verdict is **OPTIMIZED**, skip to Stage 6.

If **HAS_OPPORTUNITIES**, use the `cpp-coder` subagent (with
performance instructions):

> Apply the performance optimizations identified in the review.
>
> **Read `.agent-output/004-perf-findings.md`** for the findings.
> Apply ONLY high and medium priority optimizations that have a
> clear hypothesis, a benchmark cited, and low correctness risk.
> Skip anything that:
> - Violates DuckDB standards (sized integers, `idx_t`, `unique_ptr`,
>   exception types) — those are non-negotiable
> - Has a speculative or unmeasurable benefit
> - Reduces readability for a marginal gain
>
> Run the relevant sqllogic tests after EACH individual change.
> Revert if tests fail.
>
> Append optimization results to
> `.agent-output/002-implementation-log.md` under a new heading
> `## Performance Optimizations`.
> Return: count of optimizations applied vs. skipped, and whether all
> tests pass.

---

## Stage 6: Human Summary

Compile the final summary by reading all `.agent-output/*.md` files.

Write `.agent-output/005-summary.md` with this structure:

```markdown
# Feature Summary: [feature name]

## What Was Built
One paragraph describing the feature and key architectural decisions.

## Architecture Decisions
- Key decision 1 and rationale
- Key decision 2 and rationale

## Files Changed
| File | Action | Description |
|------|--------|-------------|
| src/functions/.../foo.cpp | Created | New table function for ... |
| src/include/functions/.../foo.hpp | Created | Header for above |
| src/delta_extension.cpp | Modified | Registered new function |

## sqllogic Tests Added
- `test/sql/main/test_foo.test`: covers happy path
- `test/sql/issues/issue_NNNN.test`: regression for ...

## Review Status
- Verdict: APPROVED after N iteration(s)
- Outstanding Medium/Low items: [list or "none"]

## Performance
- Verdict: [OPTIMIZED or HAS_OPPORTUNITIES]
- Optimizations applied: [list or "none needed"]
- Optimizations skipped: [list with reasons, or "none"]

## Items for Human Review
- [anything flagged as needing human judgment]
- [open questions from the architect]
- [any review findings that hit the iteration limit]
- [any kernel `GIT_TAG` bump that was raised as an open question]
```

Present the summary to the user. Tell them the full details are in
`.agent-output/` and ask if they'd like to review any specific stage's
output.
