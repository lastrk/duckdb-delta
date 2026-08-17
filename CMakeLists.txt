cmake_minimum_required(VERSION 3.5)
include(ExternalProject)

# Core config
set(TARGET_NAME delta)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

include_directories(src/include)

set(EXTENSION_SOURCES
    src/delta_extension.cpp
    src/delta_functions.cpp
    src/delta_log_types.cpp
    src/delta_macros.cpp
    src/delta_schema_builder.cpp
    src/delta_utils.cpp
    src/functions/delta_scan/delta_scan.cpp
    src/functions/delta_scan/delta_multi_file_list.cpp
    src/functions/delta_scan/delta_multi_file_reader.cpp
    src/functions/delta_domain_metadata.cpp
    src/functions/delta_metadata_scan.cpp
    src/functions/metadata_functions.cpp
    src/functions/expression_functions.cpp
    src/functions/util_functions.cpp
    src/functions/delta_transaction_utils/idempotency_helpers.cpp
    src/functions/delta_transaction_utils/schema_evolution.cpp
    src/path_utils.cpp
    src/storage/delta_insert.cpp
    src/storage/delta_catalog.cpp
    src/storage/delta_schema_entry.cpp
    src/storage/delta_table_entry.cpp
    src/storage/delta_transaction.cpp
    src/storage/delta_transaction_manager.cpp)

# Custom config TODO: figure out if we really need this?
if(APPLE)
  set(PLATFORM_LIBS
      m
      c
      System
      resolv
      "-framework Corefoundation -framework SystemConfiguration -framework Security"
  )
elseif(UNIX)
  set(PLATFORM_LIBS m c resolv)
elseif(WIN32)
  set(PLATFORM_LIBS
      ntdll
      crypt32
      ncrypt
      secur32
      ws2_32
      userenv
      bcrypt
      msvcrt
      advapi32
      RuntimeObject)
else()
  message(STATUS "UNKNOWN OS")
endif()

# Setup delta-kernel-rs dependency
set(KERNEL_NAME delta_kernel)

set(DELTA_KERNEL_GIT_REPOSITORY
    "https://github.com/lastrk/delta-kernel-rs.git")
set(DELTA_KERNEL_GIT_COMMIT
    "7439bf8215067d925bc766bfc71dba25b3168b3d")
set(DELTA_KERNEL_LOCAL_DIR
    ""
    CACHE PATH "Use a local delta-kernel-rs checkout instead of cloning")

# Set default ExternalProject root directory
set_directory_properties(PROPERTIES EP_PREFIX ${CMAKE_BINARY_DIR}/rust)

set(RUST_ENV_VARS "")

# Propagate arch to rust build for CI
set(RUST_PLATFORM_TARGET "")
if("${OS_NAME}" STREQUAL "linux")
  if("${OS_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-unknown-linux-gnu")
  else()
    set(RUST_PLATFORM_TARGET "x86_64-unknown-linux-gnu")
  endif()

  # Point Rust openssl-sys at vcpkg OpenSSL when vcpkg supplies it. Native
  # prebuilt builds use the platform OpenSSL installation.
  if(VCPKG_BUILD)
    set(RUST_ENV_VARS
        ${RUST_ENV_VARS}
        OPENSSL_STATIC=1
        OPENSSL_LIB_DIR=${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib
        OPENSSL_INCLUDE_DIR=${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include)
  endif()
elseif("${OS_NAME}" STREQUAL "osx")
  if("${OSX_BUILD_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-apple-darwin")
  elseif("${OSX_BUILD_ARCH}" STREQUAL "x86_64")
    set(RUST_PLATFORM_TARGET "x86_64-apple-darwin")
  elseif("${OS_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-apple-darwin")
  endif()
elseif(WIN32)
  if(MINGW AND "${OS_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-pc-windows-gnu")
  elseif(MINGW AND "${OS_ARCH}" STREQUAL "amd64")
    set(RUST_PLATFORM_TARGET "x86_64-pc-windows-gnu")
  elseif(MSVC AND "${OS_ARCH}" STREQUAL "arm64")
    set(RUST_PLATFORM_TARGET "aarch64-pc-windows-msvc")
  elseif(MSVC AND "${OS_ARCH}" STREQUAL "amd64")
    set(RUST_PLATFORM_TARGET "x86_64-pc-windows-msvc")
  endif()
endif()

# We currently only support the predefined targets.
if("${RUST_PLATFORM_TARGET}" STREQUAL "")
  message(FATAL_ERROR "Failed to detect the correct platform")
endif()

set(RUST_PLATFORM_PARAM "--target=${RUST_PLATFORM_TARGET}")
message(STATUS "Building for rust target: ${RUST_PLATFORM_TARGET}")

# Remove whitespaces before and after to prevent messed up env variables
string(STRIP "${RUST_ENV_VARS}" RUST_ENV_VARS)

# Having these set will mess up cross compilation to linux arm
set(RUST_UNSET_ENV_VARS --unset=CC --unset=CXX --unset=LD)

# Define all the relevant delta-kernel-rs paths/names
if(DELTA_KERNEL_LOCAL_DIR)
  get_filename_component(DELTA_KERNEL_SOURCE_DIR "${DELTA_KERNEL_LOCAL_DIR}"
                         ABSOLUTE)
  if(NOT EXISTS "${DELTA_KERNEL_SOURCE_DIR}/Cargo.toml")
    message(
      FATAL_ERROR
        "DELTA_KERNEL_LOCAL_DIR does not contain a Cargo.toml: ${DELTA_KERNEL_SOURCE_DIR}"
    )
  endif()
  set(DELTA_KERNEL_DOWNLOAD_ARGS
      SOURCE_DIR "${DELTA_KERNEL_SOURCE_DIR}"
      DOWNLOAD_COMMAND "")
  message(STATUS "Using local delta-kernel-rs checkout: ${DELTA_KERNEL_SOURCE_DIR}")
else()
  set(DELTA_KERNEL_SOURCE_DIR
      "${CMAKE_BINARY_DIR}/rust/src/delta_kernel")
  set(DELTA_KERNEL_DOWNLOAD_ARGS
      GIT_REPOSITORY "${DELTA_KERNEL_GIT_REPOSITORY}"
      GIT_TAG "${DELTA_KERNEL_GIT_COMMIT}")
  message(STATUS "Using delta-kernel-rs commit: ${DELTA_KERNEL_GIT_COMMIT}")
endif()

set(DELTA_KERNEL_CARGO_TARGET_DIR
    "${CMAKE_BINARY_DIR}/rust/target"
    CACHE PATH "Cargo target directory for delta-kernel-rs")
set(DELTA_KERNEL_LIBNAME
    "${CMAKE_STATIC_LIBRARY_PREFIX}delta_kernel_ffi${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
set(DELTA_KERNEL_LIBPATH
    "${DELTA_KERNEL_CARGO_TARGET_DIR}/${RUST_PLATFORM_TARGET}/$<IF:$<CONFIG:Debug>,debug,release>/${DELTA_KERNEL_LIBNAME}"
)
set(DELTA_KERNEL_FFI_HEADER_PATH
    "${DELTA_KERNEL_CARGO_TARGET_DIR}")
set(DELTA_KERNEL_FFI_HEADER_C
    "${DELTA_KERNEL_CARGO_TARGET_DIR}/delta_kernel_ffi.h"
)
set(DELTA_KERNEL_FFI_HEADER_CXX
    "${DELTA_KERNEL_CARGO_TARGET_DIR}/delta_kernel_ffi.hpp"
)

set(DELTA_KERNEL_FFI_HEADER_PATCHED
    "${CMAKE_BINARY_DIR}/codegen/include/generated_delta_kernel_ffi.hpp"
)

# Cargo optimization/debug symbol flags
set(CARGO_PROFILE "$<IF:$<CONFIG:Debug>,dev,release>")

option(DELTA_KERNEL_BUILD_ACCEPTANCE
       "Build delta-kernel acceptance DATs for the SQLLogicTest suite" ON)
option(DELTA_KERNEL_BUILD_ALWAYS
       "Run the delta-kernel Cargo build whenever the extension target builds"
       OFF)
set(DELTA_KERNEL_ACCEPTANCE_COMMANDS)
if(DELTA_KERNEL_BUILD_ACCEPTANCE)
  # acceptance depends on delta_kernel_benchmarks (a regular dependency), whose
  # build.rs downloads benchmark data that the extension tests do not use.
  list(
    APPEND
    DELTA_KERNEL_ACCEPTANCE_COMMANDS
    COMMAND
    ${CMAKE_COMMAND}
    -E
    make_directory
    "${DELTA_KERNEL_SOURCE_DIR}/benchmarks/workloads"
    COMMAND
    ${CMAKE_COMMAND}
    -E
    touch
    "${DELTA_KERNEL_SOURCE_DIR}/benchmarks/workloads/.done"
    COMMAND
    ${CMAKE_COMMAND}
    -E
    env
    ${RUST_UNSET_ENV_VARS}
    ${RUST_ENV_VARS}
    "CARGO_TARGET_DIR=${DELTA_KERNEL_CARGO_TARGET_DIR}"
    cargo
    build
    "--manifest-path=${DELTA_KERNEL_SOURCE_DIR}/acceptance/Cargo.toml")
endif()

# Add rust_example as a CMake target
ExternalProject_Add(
  ${KERNEL_NAME}
  ${DELTA_KERNEL_DOWNLOAD_ARGS}
  CONFIGURE_COMMAND ""
  UPDATE_COMMAND ""
  BUILD_IN_SOURCE 1
  # Build debug build
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env ${RUST_UNSET_ENV_VARS} ${RUST_ENV_VARS}
      "CARGO_TARGET_DIR=${DELTA_KERNEL_CARGO_TARGET_DIR}" cargo build
      --package delta_kernel_ffi --profile=${CARGO_PROFILE}
      --features "default-engine-rustls,arrow,test-ffi,delta-kernel-unity-catalog,tracing"
      ${RUST_PLATFORM_PARAM}
  ${DELTA_KERNEL_ACCEPTANCE_COMMANDS}
  # Copy, patch, and generate the C++ FFI header into codegen/include/.
  # See scripts/ffi/generate_delta_kernel_ffi_header and cbindgen#402 for details.
  COMMAND
    sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/ffi/generate_delta_kernel_ffi_header
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/ffi
      ${DELTA_KERNEL_FFI_HEADER_CXX}
      ${CMAKE_BINARY_DIR}/codegen/include
  # Define the byproducts, required for building with Ninja
  BUILD_BYPRODUCTS "${DELTA_KERNEL_LIBPATH}"
  BUILD_BYPRODUCTS "${DELTA_KERNEL_FFI_HEADER_C}"
  BUILD_BYPRODUCTS "${DELTA_KERNEL_FFI_HEADER_CXX}"
  BUILD_ALWAYS ${DELTA_KERNEL_BUILD_ALWAYS}
  INSTALL_COMMAND ""
  LOG_BUILD ON)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# The generated FFI header is copied to codegen/include/ and patched at build
# time (see scripts/ffi/{prefix,suffix}.inc; inline_msvc_compat.inc is generated dynamically).
# TODO: when cbindgen C-linkage issue is resolved upstream, remove the sed patches.
include_directories("${CMAKE_BINARY_DIR}/codegen/include")

# Hides annoying linker warnings
set(CMAKE_OSX_DEPLOYMENT_TARGET
    13.3
    CACHE STRING "Minimum OS X deployment version" FORCE)

# Add the default client
add_compile_definitions(DEFINE_DEFAULT_ENGINE DEFINE_DEFAULT_ENGINE_BASE)

# Link delta-kernal-rs to static lib
target_link_libraries(${EXTENSION_NAME} ${DELTA_KERNEL_LIBPATH}
                      ${PLATFORM_LIBS})
add_dependencies(${EXTENSION_NAME} delta_kernel)

# Link delta-kernal-rs to dynamic lib
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${DELTA_KERNEL_LIBPATH}
                      ${PLATFORM_LIBS})
add_dependencies(${LOADABLE_EXTENSION_NAME} delta_kernel)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
