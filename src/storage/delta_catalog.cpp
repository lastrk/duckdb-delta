#include "storage/delta_catalog.hpp"
#include "storage/delta_ctas.hpp"
#include "storage/delta_insert.hpp"
#include "storage/delta_schema_entry.hpp"
#include "storage/delta_transaction.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"

#include "functions/delta_scan/delta_multi_file_list.hpp"

namespace duckdb {

idx_t ParseDeltaVersionFromAtClause(const BoundAtClause &at_clause) {
	if (StringUtil::Lower(at_clause.Unit()) != "version") {
		throw InvalidConfigurationException("Delta tables only support at_clause with unit 'version'");
	}
	Value version_value = at_clause.GetValue();
	if (!version_value.DefaultTryCastAs(LogicalType::UBIGINT, false)) {
		throw InvalidInputException("Failed to parse version number '%s' into a valid version",
		                            at_clause.GetValue().ToString().c_str());
	}
	return version_value.GetValue<idx_t>();
}

DeltaCatalog::DeltaCatalog(AttachedDatabase &db_p, const string &path, AccessMode access_mode)
    : Catalog(db_p), path(path), access_mode(access_mode), use_cache(false),
      use_specific_version(DConstants::INVALID_INDEX), pushdown_partition_info(true),
      filter_pushdown_mode(DEFAULT_PUSHDOWN_MODE) {
}

DeltaCatalog::~DeltaCatalog() = default;

void DeltaCatalog::Initialize(bool load_builtin) {
	CreateSchemaInfo info;
	main_schema = make_uniq<DeltaSchemaEntry>(*this, info);
}

optional_ptr<CatalogEntry> DeltaCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw BinderException("Delta tables do not support creating new schemas");
}

void DeltaCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw BinderException("Delta tables do not support dropping schemas");
}

void DeltaCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_ptr<SchemaCatalogEntry> DeltaCatalog::LookupSchema(CatalogTransaction transaction,
                                                            const EntryLookupInfo &schema_lookup,
                                                            OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	return nullptr;
}

bool DeltaCatalog::InMemory() {
	return false;
}

string DeltaCatalog::GetDBPath() {
	return path;
}

bool DeltaCatalog::UseCachedSnapshot() {
	return use_cache;
}

optional_idx DeltaCatalog::GetCatalogVersion(ClientContext &context) {
	auto &delta_transaction = DeltaTransaction::Get(context, *this);

	// Option 1: snapshot is cached table-wide
	auto cached_snapshot = main_schema->GetCachedTable();
	if (cached_snapshot) {
		return cached_snapshot->snapshot->GetVersion();
	}

	// Option 2: snapshot is cached in transaction
	auto transaction_table_entry = delta_transaction.GetTableEntry(use_specific_version);
	if (transaction_table_entry) {
		return transaction_table_entry->snapshot->GetVersion();
	}

	return use_specific_version == DConstants::INVALID_INDEX ? optional_idx::Invalid() : use_specific_version;
}

DatabaseSize DeltaCatalog::GetDatabaseSize(ClientContext &context) {
	if (default_schema.empty()) {
		throw InvalidInputException("Attempting to fetch the database size - but no database was provided "
		                            "in the connection string");
	}
	DatabaseSize size;
	return size;
}

PhysicalOperator &DeltaCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                  LogicalCreateTable &op, PhysicalOperator &plan) {
	auto &create_info = op.info->Base();

	if (create_info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		throw BinderException("CREATE OR REPLACE TABLE is not supported for Delta tables");
	}

	//! Look up the parquet copy function (required to write data files).
	auto &db_instance = *context.db;
	auto &system_catalog = Catalog::GetSystemCatalog(db_instance);
	auto data = CatalogTransaction::GetSystemTransaction(db_instance);
	auto &sys_schema = system_catalog.GetSchema(data, DEFAULT_SCHEMA);
	auto copy_fun_entry = sys_schema.GetEntry(data, CatalogType::COPY_FUNCTION_ENTRY, "parquet");
	if (!copy_fun_entry) {
		throw MissingExtensionException("Parquet copy function not found — parquet extension must be loaded for "
		                                "Delta table CTAS");
	}
	auto &copy_fun = copy_fun_entry->Cast<CopyFunctionCatalogEntry>();

	//! Derive column names and types from the create info.
	auto &columns = create_info.columns;
	auto names_to_write = columns.GetColumnNames();
	auto types_to_write = columns.GetColumnTypes();

	//! Validate column types have Delta representations (throws BinderException on unsupported types).
	//! This runs at bind time so the error surfaces before any I/O is performed.
	//! ValidateColumnTypes is used here (not BuildSchemaString) to avoid allocating the JSON string
	//! at plan time — GetGlobalSinkState builds it when actually needed.
	DeltaSchemaJson::ValidateColumnTypes(columns);

	//! Validate partition keys and resolve them as indices into the column list.
	vector<idx_t> partition_columns;
	for (const auto &pk : create_info.partition_keys) {
		if (pk->type != ExpressionType::COLUMN_REF) {
			throw BinderException("Delta CTAS PARTITIONED BY only supports simple column references");
		}
		auto &colref = pk->Cast<ColumnRefExpression>();
		const string &pk_name = colref.GetColumnName();
		bool found = false;
		for (idx_t col_idx = 0; col_idx < names_to_write.size(); col_idx++) {
			if (StringUtil::CIEquals(names_to_write[col_idx], pk_name)) {
				partition_columns.push_back(col_idx);
				found = true;
				break;
			}
		}
		if (!found) {
			throw BinderException("Partition column '%s' does not exist in the table schema", pk_name);
		}
	}

	//! The table path is the catalog path.
	string delta_path = Path::Normalize(path);

	//! Ensure the table root directory exists before the parquet pipeline writes into it.
	//! PhysicalCopyToFile's child pipeline runs before DeltaInsert::GetGlobalSinkState, so the
	//! directory must exist at plan time rather than at sink-init time.
	{
		auto &fs = FileSystem::GetFileSystem(context);
		fs.CreateDirectoriesRecursive(delta_path);
	}

	//! Bind the copy function.
	auto info = make_uniq<CopyInfo>();
	info->file_path = delta_path;
	info->format = "parquet";
	info->is_from = false;

	CopyFunctionBindInput bind_input(*info);
	auto function_data = copy_fun.function.copy_to_bind(context, bind_input, names_to_write, types_to_write);

	//! Build the CTAS DeltaInsert operator.
	auto &insert = planner.Make<DeltaInsert>(op, op.schema, std::move(op.info));

	//! Generate a unique write UUID for this batch of files.
	auto current_write_uuid = UUID::ToString(UUID::GenerateRandomUUID());

	auto &physical_copy = planner.Make<PhysicalCopyToFile>(
	    GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType::WRITTEN_FILE_STATISTICS), copy_fun.function,
	    std::move(function_data), op.estimated_cardinality);
	auto &physical_copy_ref = physical_copy.Cast<PhysicalCopyToFile>();

	physical_copy_ref.use_tmp_file = false;
	if (!partition_columns.empty()) {
		physical_copy_ref.filename_pattern.SetFilenamePattern("duckdb_" + current_write_uuid + "_{i}");
		physical_copy_ref.file_path = delta_path;
		physical_copy_ref.partition_output = true;
		physical_copy_ref.partition_columns = partition_columns;
		physical_copy_ref.write_empty_file = true;
	} else {
		physical_copy_ref.file_path =
		    Path::FromString(delta_path).Join("duckdb-" + current_write_uuid + ".parquet").ToString();
		physical_copy_ref.partition_output = false;
		physical_copy_ref.write_empty_file = false;
	}

	physical_copy_ref.file_extension = "parquet";
	physical_copy_ref.overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
	physical_copy_ref.per_thread_output = false;
	physical_copy_ref.rotate = false;
	physical_copy_ref.return_type = CopyFunctionReturnType::WRITTEN_FILE_STATISTICS;
	physical_copy_ref.write_partition_columns = true;
	physical_copy_ref.children.push_back(plan);
	physical_copy_ref.names = names_to_write;
	physical_copy_ref.expected_types = types_to_write;
	physical_copy_ref.hive_file_pattern = true;

	insert.children.push_back(physical_copy);

	return insert;
}
PhysicalOperator &DeltaCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                           PhysicalOperator &plan) {
	throw NotImplementedException("DeltaCatalog PlanDelete");
}
PhysicalOperator &DeltaCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                           PhysicalOperator &plan) {
	throw NotImplementedException("DeltaCatalog PlanUpdate");
}
unique_ptr<LogicalOperator> DeltaCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                          TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	throw NotImplementedException("DeltaCatalog BindCreateIndex");
}

} // namespace duckdb
