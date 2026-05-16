#include "storage/delta_ctas.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

//! Escape a C++ string value for embedding as a JSON string value.
//! Does NOT add surrounding quotes.
static string JsonEscapeString(const string &s) {
	string result;
	result.reserve(s.size() + 4);
	for (char c : s) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			result += c;
			break;
		}
	}
	return result;
}

//! Serialize a DuckDB LogicalType to a Delta protocol type representation (unquoted JSON value).
//! Returns either a JSON string literal (with quotes), or a JSON object for complex types.
//! Throws BinderException for types with no Delta representation.
static string LogicalTypeToDeltaType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "\"boolean\"";
	case LogicalTypeId::TINYINT:
		return "\"byte\"";
	case LogicalTypeId::SMALLINT:
		return "\"short\"";
	case LogicalTypeId::INTEGER:
		return "\"integer\"";
	case LogicalTypeId::BIGINT:
		return "\"long\"";
	case LogicalTypeId::FLOAT:
		return "\"float\"";
	case LogicalTypeId::DOUBLE:
		return "\"double\"";
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::ENUM:
		return "\"string\"";
	case LogicalTypeId::DATE:
		return "\"date\"";
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return "\"timestamp\"";
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		throw BinderException(
		    "Cannot create Delta table: column type '%s' maps to Delta 'timestamp_ntz', which requires "
		    "minReaderVersion=3 and minWriterVersion=7 (protocol features not yet supported). "
		    "Use TIMESTAMP or TIMESTAMP WITH TIME ZONE instead.",
		    type.ToString());
	case LogicalTypeId::BLOB:
		return "\"binary\"";
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		return "\"decimal(" + std::to_string(width) + "," + std::to_string(scale) + ")\"";
	}
	case LogicalTypeId::STRUCT: {
		const auto &children = StructType::GetChildTypes(type);
		string fields_json;
		// Reserve ~70 bytes per field to avoid most reallocations.
		fields_json.reserve(children.size() * 70);
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				fields_json += ",";
			}
			const string &field_name = children[i].first;
			const LogicalType &field_type = children[i].second;
			string type_str = LogicalTypeToDeltaType(field_type);
			fields_json += "{\"name\":\"" + JsonEscapeString(field_name) + "\",\"type\":" + type_str +
			               ",\"nullable\":true,\"metadata\":{}}";
		}
		return "{\"type\":\"struct\",\"fields\":[" + fields_json + "]}";
	}
	case LogicalTypeId::LIST: {
		const LogicalType &elem_type = ListType::GetChildType(type);
		string elem_str = LogicalTypeToDeltaType(elem_type);
		return "{\"type\":\"array\",\"elementType\":" + elem_str + ",\"containsNull\":true}";
	}
	case LogicalTypeId::MAP: {
		const LogicalType &key_type = MapType::KeyType(type);
		const LogicalType &val_type = MapType::ValueType(type);
		string key_str = LogicalTypeToDeltaType(key_type);
		string val_str = LogicalTypeToDeltaType(val_type);
		return "{\"type\":\"map\",\"keyType\":" + key_str + ",\"valueType\":" + val_str +
		       ",\"valueContainsNull\":true}";
	}
	default:
		throw BinderException("Cannot create Delta table: column type '%s' has no Delta Lake representation. "
		                      "Supported types: boolean, byte, short, integer, long, float, double, string, date, "
		                      "timestamp, binary, decimal, struct, array, map.",
		                      type.ToString());
	}
}

//! Build the raw Delta StructType JSON (not escaped for embedding).
static string BuildRawSchemaJson(const ColumnList &columns) {
	string fields_json;
	// Reserve ~70 bytes per column as a rough lower bound; avoids most reallocations.
	fields_json.reserve(columns.LogicalColumnCount() * 70);
	bool first = true;
	for (const auto &col_def : columns.Logical()) {
		if (!first) {
			fields_json += ",";
		}
		first = false;
		const string &name = col_def.Name();
		const LogicalType &col_type = col_def.Type();
		string type_str = LogicalTypeToDeltaType(col_type);
		fields_json +=
		    "{\"name\":\"" + JsonEscapeString(name) + "\",\"type\":" + type_str + ",\"nullable\":true,\"metadata\":{}}";
	}
	return "{\"type\":\"struct\",\"fields\":[" + fields_json + "]}";
}

string DeltaSchemaJson::BuildSchemaString(const ColumnList &columns) {
	//! Build the raw schema JSON then escape it for embedding as a JSON string value.
	return JsonEscapeString(BuildRawSchemaJson(columns));
}

void DeltaSchemaJson::ValidateColumnTypes(const ColumnList &columns) {
	for (const auto &col_def : columns.Logical()) {
		//! LogicalTypeToDeltaType throws BinderException for unsupported types.
		//! The return value (the Delta type string) is intentionally discarded.
		(void)LogicalTypeToDeltaType(col_def.Type());
	}
}

string DeltaSchemaJson::BuildCommitJson(const string &table_id, const string &schema_string,
                                        const vector<string> &partition_columns, int64_t created_time_ms) {
	//! partitionColumns JSON array
	string part_cols_json = "[";
	// Reserve ~24 bytes per partition column name plus brackets to avoid reallocations.
	part_cols_json.reserve(2 + partition_columns.size() * 24);
	for (idx_t i = 0; i < partition_columns.size(); i++) {
		if (i > 0) {
			part_cols_json += ",";
		}
		part_cols_json += "\"";
		part_cols_json += JsonEscapeString(partition_columns[i]);
		part_cols_json += "\"";
	}
	part_cols_json += "]";

	//! Assemble the full commit JSON directly into a pre-sized result buffer to avoid
	//! temporary string allocations from operator+ chains.
	//! Fixed overhead: ~200 bytes for commitInfo + ~160 bytes for metaData frame + ~52 for protocol.
	//! Variable: schema_string, table_id, part_cols_json, and two timestamp digits (~20 bytes each).
	string result;
	result.reserve(420 + table_id.size() + schema_string.size() + part_cols_json.size());

	//! commitInfo action
	result += "{\"commitInfo\":{\"timestamp\":";
	result += std::to_string(created_time_ms);
	result += ",\"operation\":\"CREATE TABLE\",\"operationParameters\":{},\"isolationLevel\":\"Serializable\","
	          "\"isBlindAppend\":true,\"operationMetrics\":{}}}";
	result += "\n";

	//! metaData action: schema_string is already JSON-escaped for embedding as a string value
	result += "{\"metaData\":{\"id\":\"";
	result += table_id;
	result += "\",\"format\":{\"provider\":\"parquet\",\"options\":{}},\"schemaString\":\"";
	result += schema_string;
	result += "\",\"partitionColumns\":";
	result += part_cols_json;
	result += ",\"configuration\":{},\"createdTime\":";
	result += std::to_string(created_time_ms);
	result += "}}";
	result += "\n";

	//! protocol action
	result += "{\"protocol\":{\"minReaderVersion\":1,\"minWriterVersion\":2}}";
	result += "\n";

	return result;
}

} // namespace duckdb
