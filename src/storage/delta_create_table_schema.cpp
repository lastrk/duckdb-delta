#include "storage/delta_create_table_schema.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/column_definition.hpp"

namespace duckdb {

//! Unpack a kernel ExternResult<uintptr_t> or throw IOException on failure.
static uintptr_t UnpackFieldId(ffi::ExternResult<uintptr_t> result, const char *context) {
	uintptr_t out = 0;
	auto err = KernelUtils::TryUnpackResult(result, out);
	if (err.HasError()) {
		throw IOException("Delta kernel schema visitor error in %s: %s", context, err.Message());
	}
	return out;
}

DeltaCreateTableSchema::DeltaCreateTableSchema(const ColumnList &columns_p) : columns(columns_p) {
}

ffi::EngineSchema DeltaCreateTableSchema::GetEngineSchema() {
	ffi::EngineSchema result;
	result.schema = this;
	result.visitor = &DeltaCreateTableSchema::DispatchVisit;
	return result;
}

bool DeltaCreateTableSchema::HasError() const {
	return captured_error.HasError();
}

ErrorData DeltaCreateTableSchema::TakeError() {
	ErrorData err = std::move(captured_error);
	captured_error = ErrorData();
	return err;
}

// C-linkage trampoline: the kernel calls this from inside get_create_table_builder.
// Must not let any C++ exception propagate across the FFI boundary.
uintptr_t DeltaCreateTableSchema::DispatchVisit(void *schema, ffi::KernelSchemaVisitorState *state) {
	auto *self = static_cast<DeltaCreateTableSchema *>(schema);
	try {
		return self->VisitImpl(state);
	} catch (std::exception &e) {
		self->captured_error = ErrorData(e);
		// Sentinel: ReferenceSet IDs start at 1 (ffi/src/lib.rs line 1282-1283:
		// "next_id: 1"), so 0 is never a valid field ID on the happy path.
		return 0;
	} catch (...) {
		self->captured_error = ErrorData(ExceptionType::BINDER, "Unknown error building Delta table schema");
		return 0;
	}
}

uintptr_t DeltaCreateTableSchema::VisitField(ffi::KernelSchemaVisitorState *state, const string &name,
                                             const LogicalType &type, bool nullable) {
	auto name_slice = KernelUtils::ToDeltaString(name);

	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return UnpackFieldId(ffi::visit_field_boolean(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_boolean");
	case LogicalTypeId::TINYINT:
		return UnpackFieldId(ffi::visit_field_byte(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_byte");
	case LogicalTypeId::SMALLINT:
		return UnpackFieldId(ffi::visit_field_short(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_short");
	case LogicalTypeId::INTEGER:
		return UnpackFieldId(ffi::visit_field_integer(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_integer");
	case LogicalTypeId::BIGINT:
		return UnpackFieldId(ffi::visit_field_long(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_long");
	case LogicalTypeId::FLOAT:
		return UnpackFieldId(ffi::visit_field_float(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_float");
	case LogicalTypeId::DOUBLE:
		return UnpackFieldId(ffi::visit_field_double(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_double");
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::ENUM:
		return UnpackFieldId(ffi::visit_field_string(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_string");
	case LogicalTypeId::DATE:
		return UnpackFieldId(ffi::visit_field_date(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_date");
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return UnpackFieldId(ffi::visit_field_timestamp(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_timestamp");
	case LogicalTypeId::TIMESTAMP_NS:
		// Delta's timestamp_ntz type stores microsecond precision. DuckDB's TIMESTAMP_NS
		// carries nanosecond precision (int64 nanoseconds since epoch). Writing TIMESTAMP_NS
		// into a Delta timestamp_ntz column would silently drop the two sub-microsecond digits.
		throw BinderException(
		    "Cannot create Delta table: column '%s' has type TIMESTAMP_NS (nanosecond precision). "
		    "Delta's timestamp_ntz type stores microseconds; nanosecond precision cannot be preserved. "
		    "Cast to TIMESTAMP or TIMESTAMP_MS if microsecond precision is acceptable.",
		    name);
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		// TIMESTAMP_MS and TIMESTAMP_SEC are lossless mappings to Delta timestamp_ntz
		// (millisecond and second precision fit within microsecond storage). The kernel
		// selects the correct protocol version (minReaderVersion=3, minWriterVersion=7,
		// timestampNtz feature) automatically based on the schema.
		return UnpackFieldId(
		    ffi::visit_field_timestamp_ntz(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		    "visit_field_timestamp_ntz");
	case LogicalTypeId::BLOB:
		return UnpackFieldId(ffi::visit_field_binary(state, name_slice, nullable, DuckDBEngineError::AllocateError),
		                     "visit_field_binary");
	case LogicalTypeId::DECIMAL: {
		auto precision = NumericCast<uint8_t>(DecimalType::GetWidth(type));
		auto scale = NumericCast<uint8_t>(DecimalType::GetScale(type));
		return UnpackFieldId(
		    ffi::visit_field_decimal(state, name_slice, precision, scale, nullable, DuckDBEngineError::AllocateError),
		    "visit_field_decimal");
	}
	case LogicalTypeId::STRUCT: {
		const auto &children = StructType::GetChildTypes(type);
		vector<uintptr_t> field_ids;
		field_ids.reserve(children.size());
		for (const auto &child : children) {
			field_ids.push_back(VisitField(state, child.first, child.second, /*nullable=*/true));
		}
		return UnpackFieldId(ffi::visit_field_struct(state, name_slice, field_ids.data(),
		                                             NumericCast<uintptr_t>(field_ids.size()), nullable,
		                                             DuckDBEngineError::AllocateError),
		                     "visit_field_struct");
	}
	case LogicalTypeId::LIST: {
		const LogicalType &elem_type = ListType::GetChildType(type);
		// The element placeholder name is ignored by the kernel; empty string is safe.
		static const string array_elem_name = "";
		uintptr_t elem_id = VisitField(state, array_elem_name, elem_type, /*nullable=*/true);
		return UnpackFieldId(
		    ffi::visit_field_array(state, name_slice, elem_id, nullable, DuckDBEngineError::AllocateError),
		    "visit_field_array");
	}
	case LogicalTypeId::MAP: {
		const LogicalType &key_type = MapType::KeyType(type);
		const LogicalType &val_type = MapType::ValueType(type);
		// Key/value placeholder names are ignored by the kernel; empty strings are safe.
		static const string map_key_name = "";
		static const string map_val_name = "";
		uintptr_t key_id = VisitField(state, map_key_name, key_type, /*nullable=*/false);
		uintptr_t val_id = VisitField(state, map_val_name, val_type, /*nullable=*/true);
		return UnpackFieldId(
		    ffi::visit_field_map(state, name_slice, key_id, val_id, nullable, DuckDBEngineError::AllocateError),
		    "visit_field_map");
	}
	default:
		throw BinderException(
		    "Cannot create Delta table: column '%s' has type '%s' which has no Delta Lake representation. "
		    "Supported types: boolean, byte, short, integer, long, float, double, string, date, "
		    "timestamp, timestamp_ntz, binary, decimal, struct, array, map.",
		    name, type.ToString());
	}
}

//! Validate a single column type (and recurse into nested types) without touching the
//! kernel. Throws the same BinderException that VisitField would throw for any
//! unsupported type, with the same message text. `name` is used only in the error message.
static void ValidateType(const string &name, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		// All supported leaf types — no further checking needed.
		return;
	case LogicalTypeId::TIMESTAMP_NS:
		// Identical message to VisitField — must stay in sync.
		throw BinderException(
		    "Cannot create Delta table: column '%s' has type TIMESTAMP_NS (nanosecond precision). "
		    "Delta's timestamp_ntz type stores microseconds; nanosecond precision cannot be preserved. "
		    "Cast to TIMESTAMP or TIMESTAMP_MS if microsecond precision is acceptable.",
		    name);
	case LogicalTypeId::STRUCT: {
		for (const auto &child : StructType::GetChildTypes(type)) {
			ValidateType(child.first, child.second);
		}
		return;
	}
	case LogicalTypeId::LIST:
		ValidateType(name, ListType::GetChildType(type));
		return;
	case LogicalTypeId::MAP:
		ValidateType(name, MapType::KeyType(type));
		ValidateType(name, MapType::ValueType(type));
		return;
	default:
		// Identical message to VisitField — must stay in sync.
		throw BinderException(
		    "Cannot create Delta table: column '%s' has type '%s' which has no Delta Lake representation. "
		    "Supported types: boolean, byte, short, integer, long, float, double, string, date, "
		    "timestamp, timestamp_ntz, binary, decimal, struct, array, map.",
		    name, type.ToString());
	}
}

void DeltaCreateTableSchema::ValidateTypes(const ColumnList &columns) {
	for (const auto &col : columns.Logical()) {
		ValidateType(col.Name(), col.Type());
	}
}

uintptr_t DeltaCreateTableSchema::VisitImpl(ffi::KernelSchemaVisitorState *state) {
	// Collect the field ID for each top-level column.
	vector<uintptr_t> top_level_ids;
	top_level_ids.reserve(NumericCast<idx_t>(columns.LogicalColumnCount()));

	for (const auto &col : columns.Logical()) {
		uintptr_t field_id = VisitField(state, col.Name(), col.Type(), /*nullable=*/true);
		top_level_ids.push_back(field_id);
	}

	// Build the root struct element. Per header docs (lines 2428-2430): the name of the
	// final schema element is ignored; pass an empty string to be safe.
	static const string root_name = "";
	auto root_name_slice = KernelUtils::ToDeltaString(root_name);
	return UnpackFieldId(ffi::visit_field_struct(state, root_name_slice, top_level_ids.data(),
	                                             NumericCast<uintptr_t>(top_level_ids.size()),
	                                             /*nullable=*/false, DuckDBEngineError::AllocateError),
	                     "visit_field_struct (root schema)");
}

} // namespace duckdb
