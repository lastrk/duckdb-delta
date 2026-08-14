PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=deltatable
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

PYTHON_PIP=python3 -m pip
PYTHON_BIN=python3

ifeq ($(SANITIZER_MODE), thread)
	EXT_DEBUG_FLAGS:=-DENABLE_THREAD_SANITIZER=1
endif

ifneq ("${CUSTOM_LINKER}", "")
	EXT_DEBUG_FLAGS:=${EXT_DEBUG_FLAGS} -DCUSTOM_LINKER=${CUSTOM_LINKER}
endif

# Build against the official DuckDB static archive without compiling DuckDB
# core. The DuckDB source submodule still supplies headers and build metadata.
ifneq ("${DUCKDB_PREBUILT_LIBRARY}", "")
ifeq ("$(wildcard ${DUCKDB_PREBUILT_LIBRARY})", "")
$(error DUCKDB_PREBUILT_LIBRARY does not exist: ${DUCKDB_PREBUILT_LIBRARY})
endif
EXT_FLAGS:=${EXT_FLAGS} -DPREBUILT_BINARY='$(abspath ${DUCKDB_PREBUILT_LIBRARY})' -DBUILD_EXTENSIONS_ONLY=1
BUILD_EXTENSION_TEST_DEPS?=none
endif

ifneq ("${DELTA_KERNEL_LOCAL_DIR}", "")
ifeq ("$(wildcard ${DELTA_KERNEL_LOCAL_DIR}/Cargo.toml)", "")
$(error DELTA_KERNEL_LOCAL_DIR does not contain Cargo.toml: ${DELTA_KERNEL_LOCAL_DIR})
endif
DELTA_KERNEL_SOURCE_DIR:=$(abspath ${DELTA_KERNEL_LOCAL_DIR})
EXT_FLAGS:=${EXT_FLAGS} -DDELTA_KERNEL_LOCAL_DIR='${DELTA_KERNEL_SOURCE_DIR}'
endif

# Set test paths
test_release: export DELTA_KERNEL_TESTS_PATH=$(if ${DELTA_KERNEL_SOURCE_DIR},${DELTA_KERNEL_SOURCE_DIR},./build/release/rust/src/delta_kernel)/kernel/tests/data
test_release: export DAT_PATH=$(if ${DELTA_KERNEL_SOURCE_DIR},${DELTA_KERNEL_SOURCE_DIR},./build/release/rust/src/delta_kernel)/acceptance/tests/dat

test_debug: export DELTA_KERNEL_TESTS_PATH=$(if ${DELTA_KERNEL_SOURCE_DIR},${DELTA_KERNEL_SOURCE_DIR},./build/debug/rust/src/delta_kernel)/kernel/tests/data
test_debug: export DAT_PATH=$(if ${DELTA_KERNEL_SOURCE_DIR},${DELTA_KERNEL_SOURCE_DIR},./build/debug/rust/src/delta_kernel)/acceptance/tests/dat

# Core extensions that we need for crucial testing
DEFAULT_TEST_EXTENSION_DEPS=tpcds;tpch;json;
# For cloud testing we also need these extensions
FULL_TEST_EXTENSION_DEPS=azure;httpfs;aws

# Aws and Azure have vcpkg dependencies and therefore need vcpkg merging
ifeq (${BUILD_EXTENSION_TEST_DEPS}, full)
	USE_MERGED_VCPKG_MANIFEST:=1
endif

# Set this flag during building to enable the benchmark runner
ifeq (${BUILD_BENCHMARK}, 1)
	TOOLCHAIN_FLAGS:=${TOOLCHAIN_FLAGS} -DBUILD_BENCHMARKS=1
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Include the Makefile from the benchmark directory
include benchmark/benchmark.Makefile

LOADABLE_EXTENSION_TARGET=delta_loadable_extension
PREBUILT_BUILD_DIR?=build/prebuilt-release

ifeq ($(strip ${PREBUILT_BUILD_DIR}),)
$(error PREBUILT_BUILD_DIR must not be empty)
endif

# Build only the loadable Delta artifact against the supplied official DuckDB
# archive. This avoids both a DuckDB engine source build and unrelated targets.
.PHONY: prebuilt-release
prebuilt-release: ${EXTENSION_CONFIG_STEP}
	@test -f "${DUCKDB_PREBUILT_LIBRARY}" || \
		(echo "DUCKDB_PREBUILT_LIBRARY is missing: ${DUCKDB_PREBUILT_LIBRARY}" >&2; exit 1)
	mkdir -p -- "${PREBUILT_BUILD_DIR}"
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) \
		-DPREBUILT_BINARY='$(abspath ${DUCKDB_PREBUILT_LIBRARY})' \
		-DBUILD_EXTENSIONS_ONLY=1 -DDELTA_KERNEL_BUILD_ACCEPTANCE=OFF \
		-DCMAKE_BUILD_TYPE=Release \
		-S $(DUCKDB_SRCDIR) -B "${PREBUILT_BUILD_DIR}"
	cmake --build "${PREBUILT_BUILD_DIR}" --config Release \
		--target ${LOADABLE_EXTENSION_TARGET}

# Generate some test data to test with
# Note: make sure the JAVA_HOME var is set correctly and a venv is configured, e.g:
#   python3 -m venv venv
#	. ./venv/bin/activate
#   export JAVA_HOME=/opt/homebrew/Cellar/openjdk@11/11.0.27/libexec/openjdk.jdk/Contents/Home
generate-data:
	# NOTE: @benfleis - for now pin versions that work, since unversioned/HEAD caused a big JVM stack trace that I couldn't trivially track down;
	${PYTHON_PIP} install delta-spark==4.0.0 deltalake==1.2.1 duckdb==1.4.4 pandas==2.3.3 pyarrow==22.0.0 pyspark==4.0.1 typing-extensions==4.15.0
	${PYTHON_BIN} scripts/data_generator/generate_test_data.py

unpack-golden-tables-release:
	./scripts/unwrap_golden_tables.sh

# shortcuts for FFI targets
kernel_debug:
	cd build/debug && cmake --build . --config Debug --target extension/delta/CMakeFiles/delta_kernel

kernel_release:
	cd build/release && cmake --build . --config Release --target extension/delta/CMakeFiles/delta_kernel
