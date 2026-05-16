# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

DuckDB extension (`delta`, binary name `deltatable`) for Delta Lake tables. The long-term goal is full Delta read and write support; today read is broadly supported and write is currently limited to blind inserts. The extension wraps the Rust-based [`delta-kernel-rs`](https://github.com/delta-io/delta-kernel-rs) via its C++ FFI and plugs delta scans into DuckDB's regular parquet scan pipeline.

The `duckdb/` and `extension-ci-tools/` directories are git submodules. The build system pulls `delta-kernel-rs` itself via `ExternalProject_Add` in `CMakeLists.txt` — pinned by `GIT_TAG` (currently `v0.21.0`). Bumping the kernel version means changing that tag and running `make clean <debug|release>`.

## Build & test commands

Build uses the makefile from `extension-ci-tools/makefiles/duckdb_extension.Makefile` (included from the root `Makefile`).

- `make debug` / `make release` — build the extension (statically and as a loadable `.duckdb_extension`).
- `make test_debug` / `make test` — run sqllogic tests (debug / release). Sets `DELTA_KERNEL_TESTS_PATH` and `DAT_PATH` env vars pointing into the fetched `delta-kernel-rs` build tree.
- `make kernel_debug` / `make kernel_release` — rebuild just the Rust FFI library.
- `BUILD_BENCHMARK=1 make` — build with the benchmark runner enabled.
- `SANITIZER_MODE=thread make debug` — enable TSAN.

Running a single sqllogic test (after a release build):
```
build/release/test/unittest "test/sql/main/test_expression.test"
build/release/test/unittest "*/test/sql/cloud/minio_local/*"   # glob form, used in CI
```

Generated data tests (require Python venv + Java for PySpark; `JAVA_HOME` must be set):
```
make generate-data                          # installs pinned pip deps and runs scripts/data_generator/
GENERATED_DATA_AVAILABLE=1 make test        # includes test/sql/generated/* and test/sql/golden_tests/*
```

Golden tables (used by `test/sql/golden_tests`): `make unpack-golden-tables-release` then run tests with `GOLDEN_TABLES_PATH=data/unpacked_golden_tables`. CI runs `make generate-data release unpack-golden-tables-release` together and then `GENERATED_DATA_AVAILABLE=1 GOLDEN_TABLES_PATH=... make test_release`.

Cloud tests live under `test/sql/cloud/` (azure, azurite, gcs, minio_local). Azurite/MinIO tests need their respective servers and use `scripts/upload_test_files_to_*.sh` + `scripts/env_minio` to set up env. Cloud build sets `BUILD_EXTENSION_TEST_DEPS=full` which triggers `USE_MERGED_VCPKG_MANIFEST=1` to merge in azure/httpfs/aws extension deps.

Benchmarks: `make bench-run-tpch-sf1`, `make bench-run-tpcds-sf1`, etc. Filter with `BENCHMARK_PATTERN=q01.benchmark`. Plot with `make plot`. See `benchmark/benchmark.Makefile` for the full target list.

## Architecture

### How the extension plugs into DuckDB

`src/delta_extension.cpp` (`LoadInternal`) is the entry point. It:
1. Registers DuckDB table functions (`delta_scan`, `delta_file_list`, `delta_domain_metadata`, transaction idempotency helpers) and scalar functions (delta expression evaluation, `write_file`) via `DeltaFunctions::GetTableFunctions` / `GetScalarFunctions` (see `src/delta_functions.cpp`).
2. Registers a `StorageExtension` keyed by `"delta"` so users can `ATTACH 'path' AS x (TYPE delta, …)`. The `DeltaCatalogAttach` callback reads attach options (`pin_snapshot`, `version`, `pushdown_partition_info`, `pushdown_filters`, `child_catalog_mode`, `parent_catalog`, `parent_commit`, `log_tail`, `unity_table_id`, `internal_table_name`) into the `DeltaCatalog`.
3. Registers macros (`delta_macros.cpp`), the LogType for kernel→DuckDB log forwarding (`delta_log_types.cpp`), and the `delta_kernel_logging` / `delta_scan_explain_files_filtered` config options.
4. Sets `variant_legacy_encoding = true` globally — this is an internal signal the extension uses for parquet variant encoding; not really optional.

### Two ways to query a Delta table

- **Function form**: `FROM delta_scan('s3://…/table')`. Direct table function, no ATTACH needed.
- **Catalog form**: `ATTACH 'path' AS t (TYPE delta, …);` then `FROM t`. This goes through `DeltaCatalog` → `DeltaSchemaEntry` → `DeltaTableEntry` and supports more options (snapshot pinning, version pinning for time travel, child-catalog mode for catalogs like Unity, etc.).

Both paths converge on `DeltaMultiFileList`, which is the integration seam with DuckDB's parquet scanner.

### `DeltaMultiFileList` — the integration seam (`src/functions/delta_scan/`)

`DeltaMultiFileList` implements DuckDB's `MultiFileList` interface so that delta scans reuse the parquet reader, parallel scheduler, projection pushdown, row-group skipping on parquet metadata, etc. Roughly:

- The list is materialized lazily from the kernel snapshot. `InitializeSnapshot` / `InitializeScan` call into `delta-kernel-rs` via the FFI.
- `ComplexFilterPushdown` and `DynamicFilterPushdown` translate DuckDB filters into kernel filter expressions to prune files by partition values (controlled by attach option `pushdown_filters` → `DeltaFilterPushdownMode` enum: `NONE`, `ALL`, `CONSTANT_ONLY`, `DYNAMIC_ONLY`).
- Per-file metadata is held in `DeltaFileMetaData` (partition map, deletion-vector selection vector, transform expression).
- `delta_multi_file_reader.cpp` wraps the multi-file reader so deletion vectors and partition transforms are applied as parquet rows are read.

When investigating "why was this file not pruned" / "why didn't a filter push down" / "wrong row count", this is the layer to look at, alongside the kernel scan state in `delta-kernel-rs`.

### Writes & transactions (`src/storage/`)

Write support is intentionally incremental — today only blind inserts are wired up through SQL; update/delete and richer write paths are on the roadmap as the kernel and this extension mature. The relevant pieces:
- `DeltaCatalog` / `DeltaSchemaEntry` / `DeltaTableEntry` — catalog model for a single delta table mounted as a database. `DeltaCatalog::PlanInsert` / `PlanCreateTableAs` / `PlanDelete` / `PlanUpdate` are the planner entry points — extending write support generally means filling these in (and adding the matching physical operators alongside `DeltaInsert`).
- `DeltaTransaction` / `DeltaTransactionManager` — wraps kernel commit/abort.
- `DeltaInsert` — physical operator for blind inserts.
- `parent_commit` mode: when set, the catalog delegates the actual commit to a `__internal_delta_ccv2_commit_staged` table function on a parent catalog (used for Catalog-managed Commits v2 / Unity Catalog integration). The function is looked up at ATTACH time.
- `src/functions/delta_transaction_utils/idempotency_helpers.cpp` exposes helper table functions for idempotent transaction writes.

### FFI to `delta-kernel-rs`

- `delta-kernel-rs` is built by `ExternalProject_Add` in `CMakeLists.txt` with features `default-engine-rustls,arrow,test-ffi,delta-kernel-unity-catalog,tracing`.
- The C++ FFI header is generated by `cargo` (via `cbindgen`), then post-processed by `scripts/ffi/generate_delta_kernel_ffi_header` (which prepends `scripts/ffi/prefix.inc` and appends `scripts/ffi/suffix.inc`) into `${build}/codegen/include/generated_delta_kernel_ffi.hpp`. **Don't edit the generated header — edit `prefix.inc` / `suffix.inc` or the generator script.** The `TODO` in `CMakeLists.txt` notes this is a workaround for a `cbindgen` C-linkage issue.
- DAT (Delta Acceptance Test) data and kernel test data both live inside the kernel's source tree under `build/<config>/rust/src/delta_kernel/`; the makefile exports `DELTA_KERNEL_TESTS_PATH` and `DAT_PATH` so the sqllogic tests find them.

## Test layout (`test/sql/`)

- `dat/` — runs the upstream Delta Acceptance Tests against the extension.
- `delta_kernel_rs/` — uses test tables that ship with `delta-kernel-rs`.
- `generated/` — relies on data produced by `make generate-data` (PySpark + delta-spark + delta-rs); gated on `GENERATED_DATA_AVAILABLE=1`.
- `golden_tests/` — uses unpacked golden tables (`make unpack-golden-tables-release`, env `GOLDEN_TABLES_PATH`).
- `cloud/{azure,azurite,gcs,minio_local}/` — cloud backend tests; need their respective servers.
- `inlined/` — small SQL tests that ship test data inline (e.g., variant type).
- `issues/` — regression tests pinned to a specific GitHub issue number.
- `main/` — assorted unit-style tests (error messages, expressions, writing).

Files ending in `.test_slow` are skipped by default; the runner picks them up only when slow tests are explicitly enabled.

## Conventions specific to this repo

- C++ formatting is governed by `.clang-format` (symlinked to `duckdb/.clang-format`) — same style as upstream DuckDB.
- Includes use `delta_…` flat names plus `functions/…` and `storage/…` subdirectories under `src/include/`.
- The header `generated_delta_kernel_ffi.hpp` is build-time generated and **not** in the source tree.
- The Rust submodule is fetched by CMake, not by git — `git submodule update` won't touch it. To force a re-fetch, `make clean_<debug|release>` (or delete `build/<config>/rust/`).
