# Known issues

## `VisitNullLiteral` discards type information

The kernel FFI callback `VisitNullLiteral` (in `src/delta_utils.cpp`) accepts `type_tag`, `precision`, and `scale` parameters introduced in delta-kernel-rs v0.22 and silently discards all three. Today this is benign — current filter-pushdown paths use `visit_is_null` for IS-NULL predicates, and integer/string NULL constants don't care about precision/scale.

The risk surfaces the day someone adds decimal-partition NULL pushdown: the kernel will emit `Scalar::Null(decimal_type{precision, scale})` and our visitor will translate it to an untyped DuckDB `Value()`, which could be coerced to the wrong-precision decimal at the partition-value comparison and produce incorrect pruning results.

**Trigger condition:** decimal columns used as partition keys, with NULL values present, with a filter expression that the kernel translates into a NULL literal in its pushdown form.

**Fix shape:** thread `type_tag` / `precision` / `scale` into a typed `Value::DECIMAL(NULL, precision, scale)` (or equivalent for other typed nulls) in `VisitNullLiteral`. Verify against the v0.23+ kernel's expression-visitor expectations.

**Filed by:** kernel v0.21.0 → v0.23.0 bump pipeline (see `.agent-output-ctas-2026-05-16/005-summary.md` follow-ups for the CTAS run, and the bump pipeline's perf review accounting for context).
