#include "delta_functions.hpp"
#include "delta_utils.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/common/types/value.hpp"

#ifdef DEBUG

namespace duckdb {

// ---------------------------------------------------------------------------
// __internal_delta_test_ccv2_commit_staged
//
// A test-only table function that acts as a local-filesystem UC commit
// promoter. It is registered in the delta extension so that sqllogic
// tests can exercise CCv2 CTAS / INSERT paths without a real Unity
// Catalog server.
//
// The function is invoked from DeltaTransaction::CommitCallback with a
// DataChunk whose column 0 is a STRUCT of commit metadata and column 1
// is a BOOLEAN that the function must write (true = committed, null =
// conflict / error).
//
// Struct schema (7 fields, in order):
//   staged_commit_path   VARCHAR  -- base filename inside _staged_commits/
//   staged_commit_size   BIGINT
//   staged_commit_timestamp BIGINT
//   version              BIGINT
//   table_entry_pointer  POINTER
//   file_modification_time BIGINT
//   table_uri            VARCHAR  -- table root URI (file:/// or delta:///)
//
// Behavior:
//   - Constructs the full staged-commit path from table_uri and
//     staged_commit_path.
//   - Copies the file to _delta_log/<version_padded_20>.json.
//   - Sets output column 1 to TRUE on success.
//   - Sets output column 1 to NULL to signal a commit conflict (file already
//     exists at the published location).
//   - Throws IOException if any other error occurs.
//
// NOTE: This function is ONLY for testing. It is NOT the production
// __internal_delta_ccv2_commit_staged function (which is registered by the
// Unity Catalog extension and performs catalog-side metadata updates in
// addition to the file copy). It is registered under the name
// __internal_delta_test_ccv2_commit_staged to avoid name collisions.
// ---------------------------------------------------------------------------

namespace {

// Zero-pads version to 20 digits (Delta log naming convention).
static string PadVersion(int64_t version) {
	D_ASSERT(version >= 0);
	string padded = to_string(version);
	while (padded.size() < 20) {
		padded = "0" + padded;
	}
	return padded;
}

// Strips a file:/// or file:/ prefix from a URI to get the local FS path.
// Returns the path as-is if no recognized prefix is found.
static string UriToLocalPath(const string &uri) {
	if (StringUtil::StartsWith(uri, "file:///")) {
		return uri.substr(7);
	}
	if (StringUtil::StartsWith(uri, "file:/")) {
		return uri.substr(5);
	}
	// No scheme prefix — assume it is already a plain path.
	return uri;
}

static void CcV2TestCommitterFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	try {
		// Column 0 holds the commit metadata struct; column 1 is the output BOOLEAN.
		auto struct_val = output.GetValue(0, 0);
		if (struct_val.IsNull()) {
			throw InvalidInputException(
			    "__internal_delta_test_ccv2_commit_staged: received NULL commit metadata struct");
		}

		const auto &children = StructValue::GetChildren(struct_val);
		// The struct has 7 fields (see schema above).
		if (children.size() < 7) {
			throw InvalidInputException(
			    "__internal_delta_test_ccv2_commit_staged: unexpected struct size %llu (expected >= 7)",
			    (unsigned long long)children.size());
		}

		const auto &staged_filename = StringValue::Get(children[0]);
		auto version = children[3].GetValue<int64_t>();
		const auto &raw_table_uri = StringValue::Get(children[6]);

		const string table_path = UriToLocalPath(raw_table_uri);

		// Construct paths using raw filesystem operations. The staged file was
		// written by the kernel's UCCommitter to _delta_log/_staged_commits/.
		const string staged_dir = table_path + "/_delta_log/_staged_commits";
		const string staged_full_path = staged_dir + "/" + staged_filename;
		const string published_path = table_path + "/_delta_log/" + PadVersion(version) + ".json";

		auto &fs = FileSystem::GetFileSystem(context);

		// Read the staged commit file.
		if (!fs.FileExists(staged_full_path)) {
			throw IOException("__internal_delta_test_ccv2_commit_staged: staged commit file not found: %s",
			                  staged_full_path);
		}
		auto staged_reader = fs.OpenFile(staged_full_path, FileOpenFlags::FILE_FLAGS_READ);
		int64_t file_size = staged_reader->file_system.GetFileSize(*staged_reader);
		string content(NumericCast<idx_t>(file_size), '\0');
		staged_reader->file_system.Read(*staged_reader, &content[0], file_size, 0);

		// Write to the published path. Fail gracefully on conflict (file exists).
		if (fs.FileExists(published_path)) {
			// Commit conflict: signal with NULL result.
			output.SetValue(1, 0, Value());
			output.SetCardinality(1);
			return;
		}

		auto published_writer =
		    fs.OpenFile(published_path, FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW);
		published_writer->file_system.Write(*published_writer, &content[0], NumericCast<int64_t>(content.size()), 0);

		// Success.
		output.SetValue(1, 0, Value::BOOLEAN(true));
		output.SetCardinality(1);

	} catch (std::exception &e) {
		// Re-throw as IOException so the exception barrier in CommitCallback
		// catches it and converts it to a kernel error string.
		throw IOException("__internal_delta_test_ccv2_commit_staged: %s", e.what());
	} catch (...) {
		throw IOException("__internal_delta_test_ccv2_commit_staged: unknown error");
	}
}

static unique_ptr<FunctionData> CcV2TestCommitterBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	// The function takes no explicit bind-time parameters; its input arrives
	// via the DataChunk passed to the execute function.
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("success");
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> CcV2TestCommitterInitGlobal(ClientContext &context,
                                                                        TableFunctionInitInput &input) {
	return nullptr;
}

} // anonymous namespace

TableFunction DeltaFunctions::GetCcV2TestCommitterFunction() {
	TableFunction fun("__internal_delta_test_ccv2_commit_staged", {}, CcV2TestCommitterFunction, CcV2TestCommitterBind,
	                  CcV2TestCommitterInitGlobal);
	// The function receives any DataChunk from the commit callback
	// (schema is determined by the STRUCT value in column 0).
	fun.varargs = LogicalType::ANY;
	return fun;
}

} // namespace duckdb

#endif // DEBUG
