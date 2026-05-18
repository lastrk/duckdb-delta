//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_delete.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class DeltaTableEntry;

//! Compile-time proof that a DELETE's predicate aligns to file boundaries.
//! Computed at PlanDelete time and embedded into the operator. The invariant
//! is: every file_index in `files_to_remove` has every active row matched by
//! the predicate; no file outside the set has any matched row.
struct FileLevelDeletePlan {
	//! file_index values (as enumerated by DeltaMultiFileList::GetFile)
	//! to mark as Remove in the kernel transaction.
	vector<idx_t> files_to_remove;
	//! Sum of cardinalities (per DeltaFileMetaData::cardinality) of the files
	//! in `files_to_remove`. Used as the BIGINT return of DELETE.
	idx_t total_rows_removed;
	//! True iff the original predicate was the boolean literal TRUE
	//! (DELETE FROM t without WHERE). The Sink path ignores incoming chunks.
	bool predicate_was_true;
};

//! Physical operator for file-level DELETE on a Delta table.
//!
//! Lifecycle mirrors DeltaInsert:
//!   GetGlobalSinkState → Sink(chunks of rowids; ignored in v1) →
//!   Finalize(stages Remove actions on kernel transaction) →
//!   GetDataInternal(emits BIGINT delete count).
//!
//! Sink is NOT parallel: ParallelSink() returns false, matching DeltaInsert.
class DeltaDelete : public PhysicalOperator {
public:
	DeltaDelete(PhysicalPlan &plan, LogicalOperator &op, TableCatalogEntry &table, FileLevelDeletePlan delete_plan);

	optional_ptr<TableCatalogEntry> table;
	FileLevelDeletePlan delete_plan;

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
