//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_create_table_schema.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_utils.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {

//! Write-side kernel schema builder. Owns the DuckDB ColumnList reference while the
//! kernel's create-table-builder invokes the engine-supplied visitor.
//!
//! Domain invariant: the visitor walks the columns in declaration order, calls
//! visit_field_* for each, then calls visit_field_struct once more with the collected
//! field IDs to produce the root struct ID — that ID is the visitor's return value.
//!
//! Lifetime contract: the instance must outlive the kernel call that consumes the
//! EngineSchema. Constructed stack-local in DeltaTransaction::InitializeForNewTable,
//! dropped after get_create_table_builder returns.
class DeltaCreateTableSchema {
public:
	explicit DeltaCreateTableSchema(const ColumnList &columns);

	//! Build the EngineSchema by-value descriptor. The returned struct borrows
	//! `this` as opaque schema pointer and DispatchVisit as the kernel-callable
	//! visitor. The struct must not outlive `*this`.
	ffi::EngineSchema GetEngineSchema();

	//! Pure-C++ pre-validator: iterates every column in the list and throws the same
	//! BinderException that VisitField would throw for any column type that has no
	//! Delta Lake representation. No FFI calls, no engine, no kernel handles.
	//! Call this before BuildEngine to avoid spinning up the Tokio runtime for
	//! an invalid schema.
	static void ValidateTypes(const ColumnList &columns);

	//! True if the visitor captured an error during the last kernel call.
	bool HasError() const;

	//! Transfer the captured error out of this instance. Must only be called when
	//! HasError() is true; leaves the instance's captured_error in a cleared state.
	ErrorData TakeError();

private:
	//! Single C-linkage trampoline the kernel invokes. Recovers *this from the
	//! opaque pointer and calls VisitImpl(). Catches all C++ exceptions so none
	//! cross the FFI boundary.
	static uintptr_t DispatchVisit(void *schema, ffi::KernelSchemaVisitorState *state);

	//! Walks the column list and emits visit_field_* calls. Returns the root struct
	//! field ID. Throws BinderException on an unsupported DuckDB LogicalType.
	uintptr_t VisitImpl(ffi::KernelSchemaVisitorState *state);

	//! Translate a single DuckDB LogicalType into a kernel field ID by dispatching
	//! to the appropriate visit_field_* FFI call. Returns the field ID on success;
	//! throws BinderException on unsupported or out-of-range types.
	uintptr_t VisitField(ffi::KernelSchemaVisitorState *state, const string &name, const LogicalType &type,
	                     bool nullable);

private:
	const ColumnList &columns;

	//! Captured ErrorData if VisitImpl threw. The trampoline transfers any thrown
	//! exception here, stores sentinel 0 as the return, and lets the outer caller
	//! rethrow via TakeError() after the kernel call surfaces an Err result.
	ErrorData captured_error;
};

} // namespace duckdb
