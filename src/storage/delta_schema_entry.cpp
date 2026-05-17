#include "storage/delta_schema_entry.hpp"

#include "delta_utils.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "storage/delta_catalog.hpp"

#include "delta_extension.hpp"

#include "storage/delta_table_entry.hpp"
#include "storage/delta_transaction.hpp"

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"

namespace duckdb {

DeltaSchemaEntry::DeltaSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
}

DeltaSchemaEntry::~DeltaSchemaEntry() {
}

optional_ptr<CatalogEntry> DeltaSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	auto &create_info = info.Base();
	auto &delta_catalog = catalog.Cast<DeltaCatalog>();

	//! CREATE OR REPLACE TABLE is not supported in v1.
	if (create_info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		throw BinderException("Delta tables do not support CREATE OR REPLACE TABLE");
	}

	//! Verify the table name matches the catalog name (Delta catalogs are single-table).
	const string &table_name = create_info.table;
	if (!table_name.empty() && table_name != delta_catalog.GetName() &&
	    (delta_catalog.internal_table_name.empty() || table_name != delta_catalog.internal_table_name)) {
		throw BinderException("Delta catalog '%s' only supports a single table named '%s'. Cannot create table '%s'.",
		                      delta_catalog.GetName(), delta_catalog.GetName(), table_name);
	}

	//! Validate partition columns: each referenced column must exist in the column list.
	const auto &columns = create_info.columns;
	for (const auto &pk : create_info.partition_keys) {
		if (pk->type != ExpressionType::COLUMN_REF) {
			throw BinderException("Delta CTAS partition key must be a simple column reference, not an expression");
		}
		auto &colref = pk->Cast<ColumnRefExpression>();
		const string &pk_name = colref.GetColumnName();
		bool found = false;
		for (const auto &col_def : columns.Logical()) {
			if (StringUtil::CIEquals(col_def.Name(), pk_name)) {
				found = true;
				break;
			}
		}
		if (!found) {
			throw BinderException("Partition column '%s' does not exist in the table schema", pk_name);
		}
	}

	//! Check whether the path already contains a valid Delta table.
	const string &delta_path = delta_catalog.path;
	const string delta_log_dir = delta_path + "/_delta_log";
	const string version0_path = delta_log_dir + "/00000000000000000000.json";

	if (!transaction.HasContext()) {
		throw InternalException("CreateTable requires a client context");
	}
	auto &context = transaction.GetContext();
	auto &fs = FileSystem::GetFileSystem(context);

	if (fs.FileExists(version0_path)) {
		//! A Delta table already exists at this path — reject.
		throw CatalogException("Cannot create Delta table at path '%s': a Delta table already exists there "
		                       "(found _delta_log/00000000000000000000.json). "
		                       "Use a different path or detach and re-attach with a fresh path.",
		                       delta_path);
	}

	//! Validation passed and no existing table found. For Delta catalogs, CTAS is the only
	//! supported table-creation path — bare CREATE TABLE (without AS SELECT) is not meaningful
	//! because the kernel log is written by DeltaInsert::GetGlobalSinkState, not here.
	//! Return nullptr; the planner will route CTAS through PlanCreateTableAs instead.
	return nullptr;
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
	throw NotImplementedException("Delta tables do not support altering");
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

	// Set log_tail for catalog-managed commits (CCV2) if available
	if (!delta_catalog.catalog_log_tail.IsNull()) {
		snapshot->delta_log_path = make_uniq<DeltaLogPathArray>(delta_catalog.catalog_log_tail);
	}

	// Set max_catalog_version for CCv2 tables.
	// An explicit ATTACH option takes highest priority (supports both CCv2 and plain re-attach
	// to catalogManaged tables). For CCv2 catalogs (parent_commit=true) without an explicit
	// option, fall back to the version stored after a CTAS commit in this session.
	if (delta_catalog.max_catalog_version != DConstants::INVALID_INDEX) {
		snapshot->max_catalog_version = delta_catalog.max_catalog_version;
	} else if (delta_catalog.parent_commit) {
		auto committed = delta_catalog.ccv2_committed_version.load(std::memory_order_acquire);
		if (committed != DConstants::INVALID_INDEX) {
			snapshot->max_catalog_version = committed;
		}
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

	if (type == CatalogType::TABLE_ENTRY && (name == catalog.GetName() || name == delta_catalog.internal_table_name)) {
		if (!transaction.transaction) {
			throw InternalException("No transaction in DeltaSchemaEntry::LookupEntry");
		}
		auto &delta_transaction = transaction.transaction->Cast<DeltaTransaction>();

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

		if (delta_catalog.UseCachedSnapshot()) {
			unique_lock<mutex> l(lock);

			// If the version being requested is different from the one we have cached, we
			if (delta_catalog.use_specific_version != version) {
				return delta_transaction.InitializeTableEntry(context, *this, version, nullptr);
			}

			if (!cached_table) {
				//! If allow_create is enabled and the Delta log doesn't exist yet, return nullptr
				//! (table not found). This check runs only before the first successful table
				//! initialization; once cached_table is set the table is known to exist.
				if (delta_catalog.allow_create) {
					auto &fs = FileSystem::GetFileSystem(context);
					const string version0_path = delta_catalog.path + "/_delta_log/00000000000000000000.json";
					if (!fs.FileExists(version0_path)) {
						return nullptr;
					}
				}
				cached_table = CreateTableEntry(context, version, nullptr);
			}
			return *cached_table;
		} else {
			unique_lock<mutex> l(lock);

			if (!cached_table) {
				//! If allow_create is enabled and the Delta log doesn't exist yet, return nullptr
				//! (table not found). This check runs only before the first successful table
				//! initialization; once cached_table is set the table is known to exist.
				if (delta_catalog.allow_create) {
					auto &fs = FileSystem::GetFileSystem(context);
					const string version0_path = delta_catalog.path + "/_delta_log/00000000000000000000.json";
					if (!fs.FileExists(version0_path)) {
						return nullptr;
					}
				}
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
