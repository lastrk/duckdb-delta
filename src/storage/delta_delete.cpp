#include "storage/delta_delete.hpp"
#include "storage/delta_catalog.hpp"
#include "storage/delta_transaction.hpp"
#include "storage/delta_table_entry.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"

namespace duckdb {

DeltaDelete::DeltaDelete(PhysicalPlan &plan, LogicalOperator &op, TableCatalogEntry &table,
                         FileLevelDeletePlan delete_plan_p)
    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(&table),
      delete_plan(std::move(delete_plan_p)) {
}

//===--------------------------------------------------------------------===//
// Global Sink State
//===--------------------------------------------------------------------===//
class DeltaDeleteGlobalState : public GlobalSinkState {
public:
	DeltaDeleteGlobalState() {
	}
	//! Running total of rows removed; reported by GetDataInternal.
	idx_t delete_count = 0;
};

unique_ptr<GlobalSinkState> DeltaDelete::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DeltaDeleteGlobalState>();
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType DeltaDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	// v1: the file set is computed statically at plan time; we do not need to inspect
	// per-row ids here. Just drain the child pipeline to satisfy the sink protocol.
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType DeltaDelete::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                       OperatorSinkFinalizeInput &input) const {
	auto &global_state = input.global_state.Cast<DeltaDeleteGlobalState>();

	if (!delete_plan.files_to_remove.empty()) {
		auto &delta_table = table->Cast<DeltaTableEntry>();
		auto &transaction = DeltaTransaction::Get(context, table->catalog);
		transaction.RemoveFiles(context, *delta_table.snapshot, delete_plan.files_to_remove);
	}

	global_state.delete_count = delete_plan.total_rows_removed;
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType DeltaDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                              OperatorSourceInput &input) const {
	auto &global_state = sink_state->Cast<DeltaDeleteGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(global_state.delete_count)));
	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DeltaDelete::GetName() const {
	return "DELTA_DELETE";
}

InsertionOrderPreservingMap<string> DeltaDelete::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table ? table->name : string();
	return result;
}

//===--------------------------------------------------------------------===//
// PlanDelete  (DeltaCatalog entry points)
//===--------------------------------------------------------------------===//

// BuildDeletePlan computes the FileLevelDeletePlan from the logical tree.
//
// DuckDB's FilterPushdown optimizer pass moves WHERE predicates out of a
// LogicalFilter node and into LogicalGet::table_filters BEFORE PlanDelete is
// ever called. The LogicalFilter node is removed from the tree entirely.
// Therefore we must detect the WHERE clause by inspecting
// LogicalGet::table_filters, not by searching for a LogicalFilter child.
static FileLevelDeletePlan BuildDeletePlan(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) {
	auto &delta_table = op.table.Cast<DeltaTableEntry>();
	auto &snapshot = *delta_table.snapshot;

	// Ensure the snapshot is initialized so we can inspect partition columns
	// and enumerate files.  GetTotalFileCount() drives this internally.
	const idx_t total_files = snapshot.GetTotalFileCount();

	// Determine effective partition columns.  Prefer the kernel-reported list
	// (populated from the Delta metaData action's partitionColumns field).  When
	// that is empty — which happens for tables written by DuckDB's CTAS path because
	// the kernel's create_table_builder API does not yet support setting partitionColumns
	// (delta-kernel-rs TODO #2355) — fall back to the per-file partition_map keys,
	// which are populated by VisitCallback from the per-add-action partitionValues.
	vector<string> partition_columns = snapshot.GetPartitionColumns();
	if (partition_columns.empty() && total_files > 0) {
		const auto &first_meta = snapshot.GetMetaData(0);
		for (const auto &kv : first_meta.partition_map) {
			partition_columns.push_back(kv.first);
		}
	}
	const case_insensitive_set_t partition_set(partition_columns.begin(), partition_columns.end());

	// Locate the LogicalGet node (direct child or underneath Projection nodes).
	// The optimizer has already pushed all WHERE predicates into
	// LogicalGet::table_filters, so there is no LogicalFilter in the tree.
	const LogicalGet *get_node = nullptr;
	{
		const LogicalOperator *node = op.children.empty() ? nullptr : op.children[0].get();
		while (node) {
			if (node->type == LogicalOperatorType::LOGICAL_GET) {
				get_node = &node->Cast<LogicalGet>();
				break;
			}
			if (node->type == LogicalOperatorType::LOGICAL_PROJECTION) {
				node = node->children.empty() ? nullptr : node->children[0].get();
				continue;
			}
			break;
		}
	}

	if (!get_node) {
		throw InternalException("DeltaCatalog::PlanDelete: could not locate LogicalGet in child plan");
	}

	// A DELETE has a WHERE clause iff the optimizer pushed at least one filter
	// into the LogicalGet's table_filters map.
	const bool predicate_was_true = get_node->table_filters.filters.empty();

	// If there IS a WHERE clause, every filtered column must be a partition column.
	// Row-level rewrites (needed for non-partition column filters) are not supported.
	if (!predicate_was_true) {
		for (const auto &kv : get_node->table_filters.filters) {
			const idx_t col_idx = kv.first;
			if (col_idx >= get_node->names.size()) {
				throw InternalException("DeltaCatalog::PlanDelete: table_filters key %llu out of range for names "
				                        "vector (size=%llu)",
				                        col_idx, get_node->names.size());
			}
			const string &col_name = get_node->names[col_idx];
			if (partition_set.count(col_name) == 0) {
				throw NotImplementedException(
				    "Row-level DELETE is not yet supported for Delta tables. This DELETE would require rewriting "
				    "individual files, which is not implemented in this version. "
				    "Workaround: use CTAS to write the surviving rows into a new Delta table, e.g.\n"
				    "  CREATE TABLE t_new AS SELECT * FROM t WHERE NOT (<predicate>);\n"
				    "File-level DELETE (e.g. WHERE partition_col = 'x') is supported.");
			}
		}
	}

	FileLevelDeletePlan delete_plan_value;
	delete_plan_value.predicate_was_true = predicate_was_true;

	if (predicate_was_true) {
		// --- No WHERE: remove all files ---
		// Read all cardinalities under a single lock acquisition instead of N separate
		// GetMetaData() calls (each of which acquires and releases the snapshot mutex).
		// The snapshot is fully materialized by GetTotalFileCount() above so metadata is stable.
		vector<idx_t> cardinalities;
		snapshot.GetAllCardinalities(cardinalities);
		D_ASSERT(cardinalities.size() == total_files);

		delete_plan_value.files_to_remove.reserve(total_files);
		idx_t total_rows = 0;
		for (idx_t file_idx = 0; file_idx < total_files; file_idx++) {
			delete_plan_value.files_to_remove.push_back(file_idx);
			if (cardinalities[file_idx] != DConstants::INVALID_INDEX) {
				total_rows += cardinalities[file_idx];
			}
		}
		delete_plan_value.total_rows_removed = total_rows;
	} else {
		// --- Partition-only WHERE: use ComplexFilterPushdown to identify matching files ---
		// We re-derive filter expressions from the LogicalGet's table_filters for the
		// pushdown API.  ToExpression() reconstructs the original expression from the
		// table filter so ComplexFilterPushdown can evaluate partition-column predicates.
		// MultiFilePushdownInfo requires non-const LogicalGet even though it does not mutate it.
		auto &logical_get = const_cast<LogicalGet &>(*get_node);

		// Build a filter expression for each pushed-down table filter.
		// table_filters.filters keys are absolute column indices (positions in LogicalGet::names).
		// ColumnBinding.column_index must be the position within the PROJECTED column list
		// (LogicalGet::GetColumnIds()), which GenerateTableScanFilters uses as its
		// column_ids[] array.  Map from absolute index to projected position here.
		const auto &projected_col_ids = logical_get.GetColumnIds();
		vector<unique_ptr<Expression>> filter_exprs;
		filter_exprs.reserve(get_node->table_filters.filters.size());
		for (const auto &kv : get_node->table_filters.filters) {
			const idx_t col_idx = kv.first; // absolute column index
			// Find the position of col_idx in the projected column list.
			idx_t proj_pos = projected_col_ids.size(); // sentinel: not found
			for (idx_t j = 0; j < projected_col_ids.size(); j++) {
				if (projected_col_ids[j].GetPrimaryIndex() == col_idx) {
					proj_pos = j;
					break;
				}
			}
			if (proj_pos >= projected_col_ids.size()) {
				// Filter column not projected; GenerateTableScanFilters cannot push it down.
				// Skip — if the predicate cannot be expressed as a scan filter, the column
				// cannot be a file-level partition predicate either (caught below by the
				// empty filter_set check in ComplexFilterPushdown).
				continue;
			}
			// Reconstruct a column reference expression with the PROJECTED position.
			auto col_ref =
			    make_uniq<BoundColumnRefExpression>(get_node->names[col_idx], get_node->returned_types[col_idx],
			                                        ColumnBinding(logical_get.table_index, proj_pos));
			col_ref->alias = get_node->names[col_idx];
			// Build the full filter expression (comparison / conjunction etc.).
			filter_exprs.push_back(kv.second->ToExpression(*col_ref));
		}

		MultiFilePushdownInfo pushdown_info(logical_get);
		MultiFileOptions mf_options;
		auto pruned = snapshot.ComplexFilterPushdown(context, mf_options, pushdown_info, filter_exprs);

		// Collect the candidate files (those NOT pruned away, i.e., the filtered list).
		// ComplexFilterPushdown returns nullptr when partition pushdown is disabled
		// (pushdown_mode == NONE) or when the filter cannot be mapped to partition predicates.
		// In that case all files are candidates — the partition-column validation above
		// already confirmed the predicate refers only to partition columns, so every
		// file satisfies it and is safe to remove.
		DeltaMultiFileList *candidate_list_ptr;
		unique_ptr<MultiFileList> pruned_owned;
		if (pruned) {
			pruned_owned = std::move(pruned);
			candidate_list_ptr = &pruned_owned->Cast<DeltaMultiFileList>();
		} else {
			candidate_list_ptr = &snapshot;
		}

		// Build a path → original-index map from the UNFILTERED snapshot under a single lock
		// acquisition, avoiding the full vector<OpenFileInfo> copy that GetAllFiles() returns.
		// The snapshot is fully materialized by GetTotalFileCount() above.
		unordered_map<string, idx_t> path_to_orig_idx;
		snapshot.BuildPathIndexMap(path_to_orig_idx);

		// Enumerate candidate files using GetFilePath(idx_t) per entry to avoid a second full
		// GetAllFiles() copy.  The candidate count is bounded by M <= N (files surviving the
		// partition pushdown), typically much smaller than N for partition-aligned DELETEs.
		const idx_t candidate_count = candidate_list_ptr->GetTotalFileCount();
		idx_t total_rows = 0;
		for (idx_t file_idx = 0; file_idx < candidate_count; file_idx++) {
			const string candidate_path = candidate_list_ptr->GetFilePath(file_idx);
			auto it = path_to_orig_idx.find(candidate_path);
			if (it == path_to_orig_idx.end()) {
				throw InternalException("DeltaCatalog::PlanDelete: candidate file '%s' not found in original snapshot",
				                        candidate_path);
			}
			const idx_t original_idx = it->second;
			auto &meta = candidate_list_ptr->GetMetaData(file_idx);
			delete_plan_value.files_to_remove.push_back(original_idx);
			if (meta.cardinality != DConstants::INVALID_INDEX) {
				total_rows += meta.cardinality;
			}
		}
		delete_plan_value.total_rows_removed = total_rows;
	}

	return delete_plan_value;
}

PhysicalOperator &DeltaCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) {
	// --- Guard 1: RETURNING not supported ---
	if (op.return_chunk) {
		throw BinderException("RETURNING clause is not yet supported for DELETE on Delta tables");
	}

	// --- Guard 2: READ_ONLY attach ---
	if (access_mode == AccessMode::READ_ONLY) {
		throw InvalidInputException("Cannot delete from a read-only Delta table");
	}

	// --- Guard 3: USING / cross-product child ---
	// bind_delete.cpp wraps USING tables in a LogicalCrossProduct above the get.
	// Walk the child tree looking for any node that is not in
	// {LogicalDelete, LogicalProjection, LogicalGet}.
	// (LogicalFilter nodes may already have been eliminated by the FilterPushdown
	// optimizer; they are harmless to allow here for safety.)
	{
		const LogicalOperator *node = &op;
		while (node != nullptr) {
			auto type = node->type;
			if (type != LogicalOperatorType::LOGICAL_DELETE && type != LogicalOperatorType::LOGICAL_FILTER &&
			    type != LogicalOperatorType::LOGICAL_PROJECTION && type != LogicalOperatorType::LOGICAL_GET) {
				throw NotImplementedException(
				    "DELETE ... USING / join is not yet supported for Delta tables. "
				    "Workaround: use CTAS to write the surviving rows into a new Delta table.");
			}
			node = node->children.empty() ? nullptr : node->children[0].get();
		}
	}

	auto delete_plan_value = BuildDeletePlan(context, planner, op);

	// For catalog-managed commits (CCv2), the commit callback needs a pointer to the
	// parent catalog's table entry so the UC committer can resolve metadata.
	// Register it at plan time (same approach as PlanInsert) before Finalize runs.
	if (parent_commit) {
		auto &delta_transaction = DeltaTransaction::Get(context, op.table.catalog);
		delta_transaction.SetParentTableEntry(op.table);
	}

	// Build the physical plan for the child (filter + scan pipeline).
	// This call moves filter expressions into PhysicalFilter; delete_plan_value
	// is already computed above so this is safe.
	auto &child_plan = planner.CreatePlan(*op.children[0]);

	// Create the DeltaDelete operator and attach the child pipeline.
	auto &del = planner.Make<DeltaDelete>(op, op.table, std::move(delete_plan_value));
	del.children.push_back(child_plan);
	return del;
}

PhysicalOperator &DeltaCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                           PhysicalOperator &plan) {
	// This overload is called by Catalog::PlanDelete(2-arg) which already called
	// CreatePlan(*op.children[0]), so filter expressions are already moved.
	// DeltaCatalog overrides the 2-arg version above to avoid that issue.
	// This 3-arg overload should never be invoked for DeltaCatalog — the vtable
	// routes CreatePlan(LogicalDelete) directly to our 2-arg override.
	throw InternalException("DeltaCatalog::PlanDelete(3-arg) should not be called directly");
}

} // namespace duckdb
