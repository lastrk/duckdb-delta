//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_table_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <duckdb/parser/constraints/not_null_constraint.hpp>

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {
class DeltaMultiFileList;
class NestedNotNullConstraint;

class DeltaTableEntry : public TableCatalogEntry {
public:
	DeltaTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);
	~DeltaTableEntry();

public:
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
	                              const EntryLookupInfo &lookup_info) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

	case_insensitive_map_t<vector<NestedNotNullConstraint>> GetNotNullConstraints() const;
	void ThrowOnUnsupportedFieldForInserting() const;

	//! Return an empty row-id column list: Delta v1 DELETE uses static file-level proofs
	//! and does not need per-row identifiers. Returning empty prevents DuckDB's
	//! BindRowIdColumns from projecting COLUMN_IDENTIFIER_ROW_ID into the scan (which
	//! would fail since the parquet reader doesn't handle that virtual column id).
	vector<column_t> GetRowIdColumns() const override {
		return {};
	}

public:
	shared_ptr<DeltaMultiFileList> snapshot;

protected:
	TableFunction GetScanFunctionInternal(ClientContext &context, unique_ptr<FunctionData> &bind_data,
	                                      optional_ptr<const EntryLookupInfo> lookup_info);
};

} // namespace duckdb
