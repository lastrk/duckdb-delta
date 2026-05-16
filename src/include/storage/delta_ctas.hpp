//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_ctas.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {

//! Utilities for CREATE TABLE AS SELECT (CTAS) on Delta tables.
//! Handles conversion of DuckDB schema types to Delta protocol JSON format
//! and construction of the initial Delta log commit JSON.
struct DeltaSchemaJson {
	//! Converts a DuckDB ColumnList to a Delta StructType JSON schemaString.
	//! The returned string is already double-escaped for embedding as a JSON string value
	//! (i.e., quotes are backslash-escaped).
	//! Throws BinderException if any column type has no Delta Lake representation.
	static string BuildSchemaString(const ColumnList &columns);

	//! Validates that all column types in the list have Delta Lake representations.
	//! Throws BinderException for unsupported types. Does NOT build the JSON string.
	//! Use this at bind time; use BuildSchemaString only when the JSON is needed.
	static void ValidateColumnTypes(const ColumnList &columns);

	//! Builds the full content of _delta_log/00000000000000000000.json.
	//! Contains commitInfo, metaData, and protocol actions (one per line).
	//! schema_string must be the output of BuildSchemaString().
	static string BuildCommitJson(const string &table_id, const string &schema_string,
	                              const vector<string> &partition_columns, int64_t created_time_ms);
};

} // namespace duckdb
