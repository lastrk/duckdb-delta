//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_transaction.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_utils.hpp"
#include "duckdb/transaction/transaction.hpp"

namespace duckdb {
class BoundCreateTableInfo;
class DeltaCatalog;
class DeltaSchemaEntry;
class DeltaTableEntry;
class DeltaMultiFileList;
struct DeltaDataFile;
struct DeltaMultiFileColumnDefinition;

enum class DeltaTransactionState { TRANSACTION_NOT_YET_STARTED, TRANSACTION_STARTED, TRANSACTION_FINISHED };

enum class DeltaTransactionMode : uint8_t {
	REGULAR,       //! Table exists; uses kernel_transaction. Default.
	CREATING_TABLE //! Table is being created via CTAS; uses kernel_create_txn.
};

class DeltaTransaction : public Transaction {
public:
	DeltaTransaction(DeltaCatalog &delta_catalog, TransactionManager &manager, ClientContext &context);
	~DeltaTransaction() override;

	void Start();
	void Commit(ClientContext &context);
	void Rollback();

	void Append(ClientContext &context, const vector<DeltaDataFile> &append_files);

	//! CTAS-only: drive the kernel CreateTableBuilder → ExclusiveCreateTransaction chain and
	//! transition the transaction to CREATING_TABLE mode. The schema is built from
	//! info.Base().columns; partition column names are extracted from info.Base().partition_keys.
	//! Throws InvalidInputException if access_mode == READ_ONLY.
	//! Throws BinderException if the schema contains a type the kernel rejects.
	//! Throws IOException for other kernel failures.
	void InitializeForNewTable(ClientContext &context, const string &table_path, BoundCreateTableInfo &info);

	//! Stage parquet files onto kernel_create_txn via create_table_add_files.
	//! No-op if append_files is empty (empty-CTAS commits Protocol+Metadata only).
	//! Must only be called after InitializeForNewTable() succeeds.
	void AppendForNewTable(ClientContext &context, const vector<DeltaDataFile> &append_files);

	//! True if this transaction is in CREATING_TABLE mode.
	bool IsCreatingTable() const {
		return mode == DeltaTransactionMode::CREATING_TABLE;
	}

	void SetTransactionVersion(const string &app_id, idx_t new_version, Value expected_value);

	static DeltaTransaction &Get(ClientContext &context, Catalog &catalog);
	AccessMode GetAccessMode() const;

	bool HasOutstandingAppends() const;

	optional_ptr<DeltaTableEntry> GetTableEntry(idx_t version);

	DeltaTableEntry &InitializeTableEntry(ClientContext &context, DeltaSchemaEntry &schema_entry, idx_t version,
	                                      optional_ptr<const DeltaMultiFileList> old_snapshot);
	//! Removes all outstanding appends and removes the files if possible
	void CleanUpFiles();

	//! CGetCommits callback for Unity Catalog managed commits
	//! CCommit callback for Unity Catalog managed commits - returns None on success, Some(error) on failure
	static ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>> CommitCallback(ffi::NullableCvoid context,
	                                                                                ffi::CommitRequest request);

	void SetParentTableEntry(TableCatalogEntry &entry) {
		lock_guard<mutex> guard(lock);
		parent_table_entry = &entry;
	}

protected:
	void InitializeTransaction(ClientContext &context);

private:
	mutable mutex lock;

	//! Back-reference to the owning catalog. Used to propagate committed version after CTAS.
	DeltaCatalog &delta_catalog;

	//! Cached table entry (without a specified version)
	//! Note: this should be the latest version of the table, pinned at the version of first reading it during this
	//! transaction
	unique_ptr<DeltaTableEntry> table_entry;

	//! Cached table entries at specific versions
	unordered_map<idx_t, unique_ptr<DeltaTableEntry>> versioned_table_entries;

	//	DeltaConnection connection;
	DeltaTransactionState transaction_state;

	const AccessMode access_mode;

	vector<DeltaDataFile> outstanding_appends;

	DeltaTransactionMode mode = DeltaTransactionMode::REGULAR;

	KernelExclusiveTransaction kernel_transaction;

	//! Held only when mode == CREATING_TABLE. Consumed by Commit() via
	//! create_table_commit. Freed by RAII on rollback.
	KernelExclusiveCreateTransaction kernel_create_txn;

	//! CTAS-only: engine handle built for the new table (mode == CREATING_TABLE).
	//! Owned here because the DeltaTableEntry does not exist until after commit.
	KernelExternEngine ctas_extern_engine;

	//! CTAS-only: table root path (mode == CREATING_TABLE).
	string ctas_table_path;

	//! CTAS-only: ordered partition column names (mode == CREATING_TABLE).
	vector<string> ctas_partition_columns;

	//! stores a ptr to the table entry that this transaction is writing to
	optional_ptr<DeltaTableEntry> write_entry;

	// Versions registered to this transaction
	struct TransactionVersion {
		idx_t new_version;
		Value expected_version;
	};
	unordered_map<string, TransactionVersion> app_versions;

	//! Whether we should invoke our parent catalog to do the commit or this catalog can do the commit itself
	bool parent_commit = false;
	string parent_catalog_name;
	// string parent_catalog_schema;
	optional_ptr<TableFunctionCatalogEntry> commit_function;
	string unity_table_id;
	weak_ptr<ClientContext> current_context;
	optional_ptr<TableCatalogEntry> parent_table_entry;

	ErrorData active_error;
};

} // namespace duckdb
