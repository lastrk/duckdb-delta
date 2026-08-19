#include "delta_kernel_ffi.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

#include "delta_functions.hpp"
#include "delta_utils.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "storage/delta_catalog.hpp"
#include "storage/delta_table_entry.hpp"
#include "storage/delta_transaction.hpp"

namespace duckdb {

struct TransactionVersionData : public GlobalTableFunctionState {
	TransactionVersionData() : finished(false) {
	}

	bool finished;
};

struct TransactionSetVersionBindData : public TableFunctionData {
	string app_id;
	int64_t new_version;
	Value expected_version;

	DeltaCatalog *delta_catalog;
};

struct TransactionGetVersionBindData : public TableFunctionData {
	string app_id;
	Value version;

	shared_ptr<DeltaMultiFileList> snapshot;
};

struct TransactionProbeBindData : public TableFunctionData {
	string claim_app_id;
	string token_app_id;
	shared_ptr<DeltaMultiFileList> snapshot;
};

struct TransactionOutcomeBindData : public TableFunctionData {
	string token_app_id;
	optional_ptr<DeltaCatalog> delta_catalog;
};

static Value GetTransactionVersion(DeltaMultiFileList &snapshot, const string &app_id) {
	auto kernel_snapshot = snapshot.snapshot->GetLockingRef();
	auto app_id_kernel_string = KernelUtils::ToDeltaString(app_id);
	auto result = ffi::get_app_id_version(kernel_snapshot.GetPtr(), app_id_kernel_string, snapshot.extern_engine.get());
	ffi::OptionalValue<int64_t> version;
	auto error = KernelUtils::TryUnpackResult(result, version);
	if (error.HasError()) {
		error.Throw();
	}
	return version.tag == ffi::OptionalValue<int64_t>::Tag::None ? Value() : Value::BIGINT(version.some._0);
}

static void DeltaGetTransactionVersionFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	const auto &bind_data = data.bind_data->Cast<TransactionGetVersionBindData>();
	auto &global_state = data.global_state->Cast<TransactionVersionData>();
	if (global_state.finished) {
		return;
	}

	auto &snapshot = bind_data.snapshot;
	auto kernel_snapshot = snapshot->snapshot->GetLockingRef();
	auto app_id_kernel_string = KernelUtils::ToDeltaString(bind_data.app_id);
	auto get_app_id_version_result =
	    ffi::get_app_id_version(kernel_snapshot.GetPtr(), app_id_kernel_string, snapshot->extern_engine.get());

	ffi::OptionalValue<int64_t> version_opt;
	auto unpacked_version_result = KernelUtils::TryUnpackResult(get_app_id_version_result, version_opt);
	if (unpacked_version_result.HasError()) {
		unpacked_version_result.Throw();
	}
	if (version_opt.tag == ffi::OptionalValue<int64_t>::Tag::None) {
		output.SetValue(0, 0, Value());
	} else {
		output.SetValue(0, 0, Value::BIGINT(version_opt.some._0));
	}
	output.SetCardinality(1);

	global_state.finished = true;
}

static void DeltaSetTransactionVersionFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	const auto &bind_data = data.bind_data->Cast<TransactionSetVersionBindData>();
	auto &global_state = data.global_state->Cast<TransactionVersionData>();
	if (global_state.finished) {
		return;
	}

	// TODO: Attach to transaction
	auto &transaction = DeltaTransaction::Get(context, *bind_data.delta_catalog);

	transaction.SetTransactionVersion(context, bind_data.app_id, bind_data.new_version, bind_data.expected_version);

	global_state.finished = true;
}

static void DeltaProbeTransactionFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	const auto &bind_data = data.bind_data->Cast<TransactionProbeBindData>();
	auto &global_state = data.global_state->Cast<TransactionVersionData>();
	if (global_state.finished) {
		return;
	}

	auto &snapshot = *bind_data.snapshot;
	output.SetValue(0, 0, GetTransactionVersion(snapshot, bind_data.claim_app_id));
	output.SetValue(1, 0, GetTransactionVersion(snapshot, bind_data.token_app_id));
	auto kernel_snapshot = snapshot.snapshot->GetLockingRef();
	output.SetValue(2, 0, Value::BOOLEAN(ffi::has_set_transaction_retention_duration(kernel_snapshot.GetPtr())));
	output.SetCardinality(1);
	global_state.finished = true;
}

static void DeltaTakeCommitOutcomeFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	const auto &bind_data = data.bind_data->Cast<TransactionOutcomeBindData>();
	auto &global_state = data.global_state->Cast<TransactionVersionData>();
	if (global_state.finished) {
		return;
	}

	auto outcome = bind_data.delta_catalog->TakeCommitOutcome(bind_data.token_app_id);
	string type;
	switch (outcome.type) {
	case DeltaCommitOutcomeType::COMMITTED:
		type = "committed";
		break;
	case DeltaCommitOutcomeType::CONFLICT:
		type = "conflict";
		break;
	case DeltaCommitOutcomeType::NO_EFFECT:
		type = "no_effect";
		break;
	case DeltaCommitOutcomeType::INDETERMINATE:
		type = "indeterminate";
		break;
	case DeltaCommitOutcomeType::NO_CHANGES:
		throw InternalException("A no-change Delta transaction has no commit outcome");
	}
	output.SetValue(0, 0, Value(type));
	output.SetValue(
	    1, 0, outcome.commit_version == DConstants::INVALID_INDEX ? Value() : Value::UBIGINT(outcome.commit_version));
	output.SetValue(2, 0, Value(outcome.message));
	output.SetCardinality(1);
	global_state.finished = true;
}

static unique_ptr<FunctionData> DeltaGetTransactionVersionBind(ClientContext &context, TableFunctionBindInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<string> &names) {
	auto res = make_uniq<TransactionGetVersionBindData>();

	auto path = input.inputs[0].GetValue<string>();
	res->app_id = input.inputs[1].GetValue<string>();

	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("version");

	// TODO: support catalog.schema.table format
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, path);
	auto lookup_result = Catalog::GetEntry(context, "", "", lookup, OnEntryNotFound::RETURN_NULL);
	if (lookup_result.get()) {
		auto &table = lookup_result->Cast<DeltaTableEntry>();
		res->snapshot = table.snapshot;
	} else {
		throw CatalogException("Table not found");
	}

	return std::move(res);
}

static unique_ptr<FunctionData> DeltaSetTransactionVersionBind(ClientContext &context, TableFunctionBindInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<string> &names) {
	auto res = make_uniq<TransactionSetVersionBindData>();

	auto path = input.inputs[0].GetValue<string>();
	res->app_id = input.inputs[1].GetValue<string>();
	res->new_version = input.inputs[2].GetValue<int64_t>();
	res->expected_version = input.inputs[3];

	// TODO: support catalog.schema.table format
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, path);
	auto lookup_result = Catalog::GetEntry(context, "", "", lookup, OnEntryNotFound::RETURN_NULL);
	if (lookup_result.get()) {
		auto &table = lookup_result->Cast<DeltaTableEntry>();
		res->delta_catalog = &table.catalog.Cast<DeltaCatalog>();
	} else {
		throw CatalogException("Table not found");
	}

	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	return std::move(res);
}

static optional_ptr<DeltaTableEntry> LookupDeltaTable(ClientContext &context, const string &path) {
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, path);
	auto result = Catalog::GetEntry(context, "", "", lookup, OnEntryNotFound::RETURN_NULL);
	if (!result) {
		throw CatalogException("Table not found");
	}
	return result->Cast<DeltaTableEntry>();
}

static unique_ptr<FunctionData> DeltaProbeTransactionBind(ClientContext &context, TableFunctionBindInput &input,
                                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<TransactionProbeBindData>();
	result->claim_app_id = input.inputs[1].GetValue<string>();
	result->token_app_id = input.inputs[2].GetValue<string>();
	result->snapshot = LookupDeltaTable(context, input.inputs[0].GetValue<string>())->snapshot;
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BOOLEAN};
	names = {"claim_version", "token_version", "has_retention"};
	return std::move(result);
}

static unique_ptr<FunctionData> DeltaTakeCommitOutcomeBind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<TransactionOutcomeBindData>();
	result->token_app_id = input.inputs[1].GetValue<string>();
	auto table = LookupDeltaTable(context, input.inputs[0].GetValue<string>());
	result->delta_catalog = table->catalog.Cast<DeltaCatalog>();
	return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::VARCHAR};
	names = {"outcome", "commit_version", "message"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> TransactionInitGlobalState(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	return make_uniq<TransactionVersionData>();
}

vector<TableFunction> DeltaFunctions::GetTransactionIdempotencyHelpers(DatabaseInstance &instance) {
	vector<TableFunction> result;
	result.push_back(TableFunction("delta_get_transaction_version", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                               DeltaGetTransactionVersionFunction, DeltaGetTransactionVersionBind,
	                               TransactionInitGlobalState));
	result.push_back(
	    TableFunction("delta_set_transaction_version",
	                  {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT},
	                  DeltaSetTransactionVersionFunction, DeltaSetTransactionVersionBind, TransactionInitGlobalState));
	result.push_back(
	    TableFunction("delta_probe_transaction", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                  DeltaProbeTransactionFunction, DeltaProbeTransactionBind, TransactionInitGlobalState));
	result.push_back(TableFunction("delta_take_commit_outcome", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                               DeltaTakeCommitOutcomeFunction, DeltaTakeCommitOutcomeBind,
	                               TransactionInitGlobalState));
	return result;
}

} // namespace duckdb
