#include "delta_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/column_list.hpp"
#include "storage/delta_table_entry.hpp"
#include "storage/delta_transaction.hpp"

namespace duckdb {

struct DeltaAddColumnsState : public GlobalTableFunctionState {
	bool finished = false;
};

struct DeltaAddColumnsBindData : public TableFunctionData {
	string catalog_name;
	string schema_name;
	string table_name;
	LogicalDependency dependency;
	vector<pair<string, LogicalType>> columns;
	string metadata_schema_json;
};

static unique_ptr<FunctionData> DeltaAddColumnsBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 5) {
		throw BinderException("delta_add_columns requires catalog, schema, table, columns, and Delta schema JSON");
	}

	auto result = make_uniq<DeltaAddColumnsBindData>();
	auto catalog_name = input.inputs[0].GetValue<string>();
	auto schema_name = input.inputs[1].GetValue<string>();
	auto table_name = input.inputs[2].GetValue<string>();
	auto columns_type = input.inputs[3].type();
	if (columns_type.id() != LogicalTypeId::STRUCT) {
		throw BinderException("delta_add_columns columns argument must be a typed STRUCT");
	}
	for (const auto &column : StructType::GetChildTypes(columns_type)) {
		result->columns.emplace_back(column.first, column.second);
	}
	if (result->columns.empty()) {
		throw BinderException("delta_add_columns requires at least one column");
	}
	result->metadata_schema_json = input.inputs[4].GetValue<string>();
	auto &table = Catalog::GetEntry<DeltaTableEntry>(context, catalog_name, schema_name, table_name);
	result->catalog_name = std::move(catalog_name);
	result->schema_name = std::move(schema_name);
	result->table_name = std::move(table_name);
	result->dependency = LogicalDependency(table);
	auto column_mapping = table.tags.find("delta.columnMapping.mode");
	if (column_mapping != table.tags.end() && !StringUtil::CIEquals(column_mapping->second, "none")) {
		throw NotImplementedException(
		    "Delta schema evolution on column-mapped tables is not supported until writes use physical column names");
	}
	for (const auto &column : result->columns) {
		if (table.ColumnExists(column.first)) {
			throw CatalogException("Column with name %s already exists!", column.first);
		}
	}

	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("success");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> DeltaAddColumnsInit(ClientContext &context,
                                                                TableFunctionInitInput &input) {
	return make_uniq<DeltaAddColumnsState>();
}

static void DeltaAddColumnsDependency(LogicalDependencyList &entries, const FunctionData *bind_data_ptr) {
	auto &bind_data = bind_data_ptr->Cast<DeltaAddColumnsBindData>();
	entries.AddDependency(bind_data.dependency);
}

static void DeltaAddColumnsExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<DeltaAddColumnsState>();
	if (state.finished) {
		return;
	}
	auto &bind_data = input.bind_data->Cast<DeltaAddColumnsBindData>();
	auto &table = Catalog::GetEntry<DeltaTableEntry>(context, bind_data.catalog_name, bind_data.schema_name,
	                                               bind_data.table_name);
	ColumnList columns;
	for (const auto &column : bind_data.columns) {
		columns.AddColumn(ColumnDefinition(column.first, column.second));
	}

	DeltaTransaction::Get(context, table.catalog).AddColumns(context, columns, bind_data.metadata_schema_json);
	output.SetValue(0, 0, Value::BOOLEAN(true));
	output.SetCardinality(1);
	state.finished = true;
}

TableFunction DeltaFunctions::GetSchemaEvolutionFunction() {
	TableFunction function("delta_add_columns",
	                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY,
	                        LogicalType::VARCHAR},
	                       DeltaAddColumnsExecute, DeltaAddColumnsBind, DeltaAddColumnsInit);
	function.dependency = DeltaAddColumnsDependency;
	return function;
}

} // namespace duckdb
