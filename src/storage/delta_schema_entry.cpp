#include "storage/delta_schema_entry.hpp"

#include "delta_schema_builder.hpp"
#include "delta_utils.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "storage/delta_catalog.hpp"

#include "delta_extension.hpp"

#include "storage/delta_table_entry.hpp"
#include "storage/delta_transaction.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"

#include <algorithm>

namespace duckdb {

DeltaSchemaEntry::DeltaSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
}

DeltaSchemaEntry::~DeltaSchemaEntry() {
}

DeltaTransaction &GetDeltaTransaction(CatalogTransaction transaction) {
	if (!transaction.transaction) {
		throw InternalException("No transaction!?");
	}
	return transaction.transaction->Cast<DeltaTransaction>();
}

//! Canonical form for comparing two spellings of one table location. `Path::ToString` is base +
//! path + trailing separator, so dropping the separator is just leaving the last part off.
static string CanonicalTablePath(const string &path) {
	auto parsed = Path::FromString(path);
	return parsed.GetBase() + parsed.GetPath();
}

//! Resolves the destination path for a CREATE TABLE. Defaults to the attached path; `WITH (path =
//! '...')` names it explicitly. Only constants are accepted -- the binder does not evaluate table
//! options, they arrive as raw parsed expressions.
static string GetCreateTablePath(const CreateTableInfo &base, DeltaCatalog &delta_catalog) {
	auto option = base.options.find("path");
	if (option == base.options.end()) {
		return delta_catalog.GetDBPath();
	}
	if (option->second->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw BinderException("Delta CREATE TABLE option 'path' must be a constant, found '%s'",
		                      option->second->ToString());
	}
	auto path = option->second->Cast<ConstantExpression>().value.ToString();

	// A Delta catalog is a single table at a single path, so a divergent path would produce a
	// catalog entry that does not describe what was attached. Compare normalized, so a trailing
	// slash or a relative spelling of the attached path is not a spurious mismatch.
	auto attached_path = delta_catalog.GetDBPath();
	if (CanonicalTablePath(path) != CanonicalTablePath(attached_path)) {
		throw NotImplementedException(
		    "Delta CREATE TABLE can only create a table at the attached path ('%s'), not at '%s'", attached_path, path);
	}
	return path;
}

static bool DirectoryContainsFile(FileSystem &fs, const string &path, unordered_set<string> &visited) {
	auto canonical_path = fs.CanonicalizePath(path);
	if (!visited.insert(canonical_path).second) {
		return false;
	}
	bool has_file = false;
	auto listed = fs.ListFiles(path, [&](const string &name, bool is_directory) {
		if (has_file) {
			return;
		}
		if (is_directory) {
			has_file = DirectoryContainsFile(fs, fs.JoinPath(path, name), visited);
		} else {
			has_file = true;
		}
	});
	if (!listed && Path::FromString(path).IsLocal() && fs.DirectoryExists(path)) {
		throw IOException("Failed to list Delta log directory '%s'", path);
	}
	return has_file;
}

//! Whether a Delta table has been created at `path` yet. Match kernel's existence rule: a missing or
//! empty `_delta_log` is available for creation, while any file below the log means a table exists.
//! Use the client filesystem so object-store prefix listing receives the active credentials.
static bool DeltaTableExistsAt(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	unordered_set<string> visited;
	return DirectoryContainsFile(fs, Path::FromString(path).Join("_delta_log").ToString(), visited);
}

//! Applies DuckDB's CREATE conflict semantics ahead of the kernel call. Kernel remains the
//! authoritative existence check; this only decides what DuckDB should do when the table is already
//! there. Returns false when the statement should be skipped entirely.
static bool HandleCreateConflict(ClientContext &context, const CreateTableInfo &base, const string &path) {
	if (base.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		// Replacing means dropping, and Delta tables do not support dropping yet.
		throw NotImplementedException("Delta tables do not support CREATE OR REPLACE TABLE");
	}
	if (base.on_conflict != OnCreateConflict::IGNORE_ON_CONFLICT) {
		return true;
	}

	return !DeltaTableExistsAt(context, path);
}

//! Kernel rejects a table location that does not exist. Object stores conjure prefixes on write, but
//! a local filesystem needs the directory to be there first.
static void EnsureTableDirectory(ClientContext &context, const string &path) {
	if (!Path::FromString(path).IsLocal()) {
		return;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.DirectoryExists(path)) {
		fs.CreateDirectoriesRecursive(path);
	}
}

static vector<string> GetCreateTablePartitionColumns(const CreateTableInfo &base) {
	vector<string> result;
	for (auto &key : base.partition_keys) {
		if (key->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			throw BinderException("Delta CREATE TABLE only supports plain column names in PARTITIONED BY, found '%s'",
			                      key->ToString());
		}
		auto &colref = key->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			throw BinderException("Delta CREATE TABLE does not support qualified partition columns ('%s')",
			                      key->ToString());
		}
		auto column_name = colref.GetColumnName();
		if (!base.columns.ColumnExists(column_name)) {
			throw BinderException("Delta CREATE TABLE partition column '%s' does not exist", column_name);
		}
		// DuckDB resolves identifiers case-insensitively. Kernel validates schema names case-sensitively,
		// so pass the spelling from the column definition rather than the reference.
		result.push_back(base.columns.GetColumn(column_name).Name());
	}
	return result;
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	if (!transaction.HasContext()) {
		throw NotImplementedException("Can not create a Delta table without context");
	}
	auto &context = transaction.GetContext();
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();
	auto &base = info.Base();

	if (delta_catalog.access_mode == AccessMode::READ_ONLY) {
		throw InvalidInputException("Can not create a table in a read only Delta catalog");
	}

	// LookupEntry only ever resolves the single attached table, so any other name would create a
	// table that could never be read back.
	auto table_name = base.table;
	if (!StringUtil::CIEquals(table_name, catalog.GetName()) &&
	    !StringUtil::CIEquals(table_name, delta_catalog.internal_table_name)) {
		throw NotImplementedException("Delta CREATE TABLE must use the attached name ('%s'), found '%s'",
		                              catalog.GetName(), table_name);
	}
	// SORTED BY is rejected by DeltaCatalog::SupportsCreateTable, which the binder consults first.
	for (auto &constraint : base.constraints) {
		// Delta requires columns to be nullable unless the `invariants` writer feature is enabled,
		// which we do not request yet.
		if (constraint->type == ConstraintType::NOT_NULL) {
			throw NotImplementedException("Delta CREATE TABLE does not support NOT NULL constraints");
		}
		throw NotImplementedException("Delta CREATE TABLE does not support constraints");
	}
	for (auto &column : base.columns.Logical()) {
		if (column.Generated()) {
			throw NotImplementedException("Delta CREATE TABLE does not support generated column '%s'", column.Name());
		}
		if (column.HasDefaultValue()) {
			throw NotImplementedException("Delta CREATE TABLE does not support DEFAULT for column '%s'", column.Name());
		}
	}

	auto path = GetCreateTablePath(base, delta_catalog);
	if (!HandleCreateConflict(context, base, path)) {
		return nullptr;
	}
	auto partition_columns = GetCreateTablePartitionColumns(base);

	EnsureTableDirectory(context, path);
	auto engine = CreateDeltaEngine(context, path);

	// The builder holds a borrowed pointer to `schema_builder`, and kernel runs the visitor during
	// get_create_table_builder -- both must outlive that call.
	DeltaSchemaBuilder schema_builder(base.columns);
	auto engine_schema = schema_builder.CreateEngineSchema();

	ffi::ExclusiveCreateTableBuilder *create_builder;
	auto builder_res =
	    KernelUtils::TryUnpackResult(ffi::get_create_table_builder(KernelUtils::ToDeltaString(path), &engine_schema,
	                                                               KernelUtils::ToDeltaString("DuckDB"), engine.get()),
	                                 create_builder);
	if (builder_res.HasError()) {
		// A schema-lowering failure surfaces from kernel as a generic schema error; the specific
		// cause (e.g. an unsupported column type) is only on the builder.
		if (schema_builder.GetError().HasError()) {
			schema_builder.GetError().Throw();
		}
		builder_res.Throw();
	}

	if (!partition_columns.empty()) {
		vector<ffi::KernelStringSlice> slices;
		for (auto &partition_column : partition_columns) {
			slices.push_back(KernelUtils::ToDeltaString(partition_column));
		}
		// Consumes the builder handle unconditionally, including on error.
		auto partition_res =
		    KernelUtils::TryUnpackResult(ffi::create_table_builder_with_partition_columns(create_builder, slices.data(),
		                                                                                  slices.size(), engine.get()),
		                                 create_builder);
		if (partition_res.HasError()) {
			partition_res.Throw();
		}
	}

	ffi::ExclusiveCreateTransaction *create_transaction;
	auto build_res =
	    KernelUtils::TryUnpackResult(ffi::create_table_builder_build(create_builder, engine.get()), create_transaction);
	if (build_res.HasError()) {
		build_res.Throw();
	}

	ffi::ExclusiveCommittedTransaction *committed;
	auto commit_res =
	    KernelUtils::TryUnpackResult(ffi::create_table_commit(create_transaction, engine.get()), committed);
	if (commit_res.HasError()) {
		commit_res.Throw();
	}
	auto version = ffi::committed_transaction_version(&committed);
	ffi::free_committed_transaction(committed);
	DUCKDB_LOG_INTERNAL(context, "delta.CreateTable", LogLevel::LOG_DEBUG, "Created %s at version %s", path,
	                    to_string(version));

	// Serve the table we just committed through the regular lookup path, so the schema cache and the
	// transaction's entry end up in the same state as for a table that already existed. Reading it back
	// also means the catalog serves the new snapshot without a re-attach.
	EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, base.table);
	return LookupEntry(transaction, lookup_info);
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	throw BinderException("Delta tables do not support creating functions");
}

void DeltaUnqualifyColumnRef(ParsedExpression &expr) {
	if (expr.type == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		auto name = std::move(colref.column_names.back());
		colref.column_names = {std::move(name)};
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(expr, DeltaUnqualifyColumnRef);
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                         TableCatalogEntry &table) {
	throw NotImplementedException("CreateIndex");
}

string GetDeltaCreateView(CreateViewInfo &info) {
	throw NotImplementedException("GetCreateView");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	throw BinderException("Delta tables do not support creating views");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	throw BinderException("Delta databases do not support creating types");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	throw BinderException("Delta databases do not support creating sequences");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                 CreateTableFunctionInfo &info) {
	throw BinderException("Delta databases do not support creating table functions");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                CreateCopyFunctionInfo &info) {
	throw BinderException("Delta databases do not support creating copy functions");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                  CreatePragmaFunctionInfo &info) {
	throw BinderException("Delta databases do not support creating pragma functions");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                             CreateCollationInfo &info) {
	throw BinderException("Delta databases do not support creating collations");
}

void DeltaSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	if (!transaction.HasContext()) {
		throw NotImplementedException("Can not alter a Delta table without context");
	}
	if (info.type != AlterType::ALTER_TABLE) {
		throw NotImplementedException("Delta tables only support ALTER TABLE ADD COLUMN");
	}
	auto &table_info = info.Cast<AlterTableInfo>();
	if (table_info.alter_table_type != AlterTableType::ADD_COLUMN) {
		throw NotImplementedException("Delta tables only support ALTER TABLE ADD COLUMN");
	}

	auto &add_info = table_info.Cast<AddColumnInfo>();
	auto lookup_info = EntryLookupInfo(CatalogType::TABLE_ENTRY, info.name);
	auto existing = LookupEntry(transaction, lookup_info);
	if (!existing) {
		throw CatalogException("Delta table '%s' does not exist", info.name);
	}
	auto &table = existing->Cast<DeltaTableEntry>();
	auto column_mapping = table.tags.find("delta.columnMapping.mode");
	if (column_mapping != table.tags.end() && !StringUtil::CIEquals(column_mapping->second, "none")) {
		throw NotImplementedException(
		    "Delta schema evolution on column-mapped tables is not supported until writes use physical column names");
	}
	if (table.ColumnExists(add_info.new_column.Name())) {
		if (add_info.if_column_not_exists) {
			return;
		}
		throw CatalogException("Column with name %s already exists!", add_info.new_column.Name());
	}
	if (add_info.new_column.Generated() || add_info.new_column.HasDefaultValue()) {
		throw NotImplementedException("Delta schema evolution does not support generated or default columns");
	}

	ColumnList new_columns;
	new_columns.AddColumn(add_info.new_column.Copy());
	GetDeltaTransaction(transaction).AddColumns(transaction.GetContext(), new_columns);
}

static bool CatalogTypeIsSupported(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		return true;
	default:
		return false;
	}
}

unique_ptr<DeltaTableEntry> DeltaSchemaEntry::CreateTableEntry(ClientContext &context, idx_t version,
                                                               optional_ptr<const DeltaMultiFileList> old_snapshot) {
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();
	auto snapshot = make_shared_ptr<DeltaMultiFileList>(context, delta_catalog.GetDBPath(), version, old_snapshot);

	// Set log_tail and max_catalog_version for catalog-managed commits (CCV2) if available
	if (!delta_catalog.catalog_log_tail.IsNull()) {
		snapshot->delta_log_path = make_uniq<DeltaLogPathArray>(delta_catalog.catalog_log_tail);
	}
	if (delta_catalog.max_catalog_version >= 0) {
		snapshot->max_catalog_version = delta_catalog.max_catalog_version;
	}

	// Get the names and types from the delta snapshot
	vector<LogicalType> return_types;
	vector<string> names;
	snapshot->Bind(return_types, names);

	// TODO: forward nullability constraints

	CreateTableInfo table_info;
	for (idx_t i = 0; i < return_types.size(); i++) {
		table_info.columns.AddColumn(ColumnDefinition(names[i], return_types[i]));
	}
	table_info.table =
	    !delta_catalog.internal_table_name.empty() ? delta_catalog.internal_table_name : catalog.GetName();

	// Copy over constraints to table info TODO: these are incompatible currently
	// table_info.constraints = snapshot->not_null_constraints;}

	// Populate tags from domain metadata
	{
		auto snapshot_ref = snapshot->snapshot->GetLockingRef();
		vector<string> writer_features;
		ffi::visit_enabled_writer_features(
		    snapshot_ref.GetPtr(), &writer_features,
		    [](ffi::NullableCvoid engine_context, ffi::KernelStringSlice feature) {
			    auto &features = *static_cast<vector<string> *>(const_cast<void *>(engine_context));
			    features.push_back(KernelUtils::FromDeltaString(feature));
		    });
		std::sort(writer_features.begin(), writer_features.end());
		table_info.tags.insert({"delta.writerFeatures", StringUtil::Join(writer_features, ", ")});

		ffi::visit_metadata_configuration(
		    snapshot_ref.GetPtr(), &table_info.tags,
		    [](ffi::NullableCvoid engine_context, ffi::KernelStringSlice key, ffi::KernelStringSlice value) {
			    auto key_string = KernelUtils::FromDeltaString(key);
			    if (key_string != "delta.columnMapping.mode") {
				    return;
			    }
			    auto &tags = *static_cast<InsertionOrderPreservingMap<string> *>(const_cast<void *>(engine_context));
			    tags.insert({key_string, KernelUtils::FromDeltaString(value)});
		    });
		ffi::visit_domain_metadata(
		    snapshot_ref.GetPtr(), snapshot->extern_engine.get(), &table_info.tags,
		    [](ffi::NullableCvoid engine_context, ffi::KernelStringSlice domain, ffi::KernelStringSlice configuration) {
			    auto &tags = *static_cast<InsertionOrderPreservingMap<string> *>(const_cast<void *>(engine_context));
			    tags.insert({KernelUtils::FromDeltaString(domain), KernelUtils::FromDeltaString(configuration)});
		    });
	}

	auto table_entry = make_uniq<DeltaTableEntry>(delta_catalog, *this, table_info);
	table_entry->snapshot = std::move(snapshot);

	return table_entry;
}

void DeltaSchemaEntry::Scan(ClientContext &context, CatalogType type,
                            const std::function<void(CatalogEntry &)> &callback) {
	if (CatalogTypeIsSupported(type)) {
		auto transaction = catalog.GetCatalogTransaction(context);
		auto lookup_info = EntryLookupInfo(type, catalog.GetName());
		auto default_table = LookupEntry(transaction, lookup_info);
		if (default_table) {
			callback(*default_table);
		}
	}
}

void DeltaSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw NotImplementedException("Scan without context not supported");
}

void DeltaSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("Delta tables do not support dropping");
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                         const EntryLookupInfo &lookup_info) {
	if (!transaction.HasContext()) {
		throw NotImplementedException("Can not DeltaSchemaEntry::GetEntry without context");
	}
	auto &context = transaction.GetContext();

	auto type = lookup_info.GetCatalogType();
	auto &name = lookup_info.GetEntryName();
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();

	if (type == CatalogType::TABLE_ENTRY && (StringUtil::CIEquals(name, catalog.GetName()) ||
	                                         StringUtil::CIEquals(name, delta_catalog.internal_table_name))) {
		auto &delta_transaction = GetDeltaTransaction(transaction);

		idx_t version = delta_catalog.use_specific_version;

		// If there's an AT clause we are doing timetravel
		auto at_clause = lookup_info.GetAtClause();
		if (at_clause) {
			version = ParseDeltaVersionFromAtClause(*at_clause);
		}

		auto transaction_table_entry = delta_transaction.GetTableEntry(version);
		if (transaction_table_entry) {
			return *transaction_table_entry;
		}

		// With nothing cached the entry has to come from a snapshot, and kernel refuses to build one
		// where no table exists. Report that as "not found" instead of letting the IO error escape:
		// DuckDB looks the table up before CREATE TABLE, so without this no table can ever be created
		// at a fresh path, and plain catalog enumeration of an empty attach throws too.
		if (!GetCachedTable() && delta_catalog.catalog_log_tail.IsNull() &&
		    !DeltaTableExistsAt(context, delta_catalog.GetDBPath())) {
			return nullptr;
		}

		if (delta_catalog.UseCachedSnapshot()) {
			unique_lock<mutex> l(lock);

			// If the version being requested is different from the one we have cached, we
			if (delta_catalog.use_specific_version != version) {
				return delta_transaction.InitializeTableEntry(context, *this, version, nullptr);
			}

			if (!cached_table) {
				cached_table = CreateTableEntry(context, version, nullptr);
			}
			return *cached_table;
		} else {
			unique_lock<mutex> l(lock);

			if (!cached_table) {
				cached_table = CreateTableEntry(context, version, nullptr);
			}

			// Always go through InitializeTableEntry so the transaction's table_entry is set,
			// using the cached snapshot as base for fast re-initialization.
			return delta_transaction.InitializeTableEntry(context, *this, version, *cached_table->snapshot);
		}
	}
	return nullptr;
}

optional_ptr<DeltaTableEntry> DeltaSchemaEntry::GetCachedTable() {
	lock_guard<mutex> lck(lock);
	if (cached_table) {
		return *cached_table;
	}
	return nullptr;
}

} // namespace duckdb
