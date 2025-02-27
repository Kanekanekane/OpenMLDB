/*
 * Copyright 2021 4Paradigm
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vm/engine.h"
#include <string>
#include <utility>
#include <vector>
#include "base/fe_strings.h"
#include "boost/none.hpp"
#include "boost/optional.hpp"
#include "codec/fe_row_codec.h"
#include "codec/fe_schema_codec.h"
#include "codec/list_iterator_codec.h"
#include "codegen/buf_ir_builder.h"
#include "gflags/gflags.h"
#include "llvm-c/Target.h"
#include "vm/local_tablet_handler.h"
#include "vm/mem_catalog.h"
#include "vm/sql_compiler.h"

DECLARE_bool(logtostderr);
DECLARE_string(log_dir);
DECLARE_bool(enable_spark_unsaferow_format);

namespace hybridse {
namespace vm {

static bool LLVM_IS_INITIALIZED = false;

EngineOptions::EngineOptions()
    : keep_ir_(false),
      compile_only_(false),
      plan_only_(false),
      cluster_optimized_(false),
      batch_request_optimized_(true),
      enable_expr_optimize_(true),
      enable_batch_window_parallelization_(false),
      max_sql_cache_size_(50),
      enable_spark_unsaferow_format_(false) {
    // TODO(chendihao): Pass the parameter to avoid global gflag
    FLAGS_enable_spark_unsaferow_format = enable_spark_unsaferow_format_;
}

EngineOptions* EngineOptions::set_enable_spark_unsaferow_format(bool flag) {
    enable_spark_unsaferow_format_ = flag;
    FLAGS_enable_spark_unsaferow_format = flag;
    return this;
}

Engine::Engine(const std::shared_ptr<Catalog>& catalog) : cl_(catalog), options_(), mu_(), lru_cache_() {}
Engine::Engine(const std::shared_ptr<Catalog>& catalog, const EngineOptions& options)
    : cl_(catalog), options_(options), mu_(), lru_cache_() {}
Engine::~Engine() {}
void Engine::InitializeGlobalLLVM() {
    if (LLVM_IS_INITIALIZED) return;
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVM_IS_INITIALIZED = true;
}

bool Engine::GetDependentTables(const std::string& sql, const std::string& db, EngineMode engine_mode,
                                std::set<std::pair<std::string, std::string>>* db_tables, base::Status& status) {
    auto info = std::make_shared<hybridse::vm::SqlCompileInfo>();
    info->get_sql_context().sql = sql;
    info->get_sql_context().db = db;
    info->get_sql_context().engine_mode = engine_mode;
    SqlCompiler compiler(std::atomic_load_explicit(&cl_, std::memory_order_acquire), options_.is_keep_ir(), false,
                         options_.is_plan_only());
    bool ok = compiler.Parse(info->get_sql_context(), status);
    if (!ok || 0 != status.code) {
        // TODO(chenjing): do clean
        status.msg = "fail to get depend tables:" + status.str();
        return false;
    }

    auto& logical_plan = info->get_sql_context().logical_plan;

    if (logical_plan.empty()) {
        status.msg = "fail to get depend tables: logical plan is empty";
        return false;
    }

    for (auto iter = logical_plan.cbegin(); iter != logical_plan.cend(); iter++) {
        if (!GetDependentTables(*iter, db, db_tables, status)) {
            return false;
        }
    }
    return true;
}

/**
 * Get Dependent tables for given logical node.
 *
 * @param node
 * @param tables
 * @param status
 * @return
 */
bool Engine::GetDependentTables(const node::PlanNode* node, const std::string& default_db,
                                std::set<std::pair<std::string, std::string>>* db_tables,
                                base::Status& status) {  // NOLINT
    if (nullptr == db_tables) {
        status.code = common::kNullPointer;
        status.msg = "fail to get sql depend tables, output tables vector is null";
        return false;
    }

    if (nullptr != node) {
        switch (node->GetType()) {
            case node::kPlanTypeTable: {
                const node::TablePlanNode* table_node = dynamic_cast<const node::TablePlanNode*>(node);
                db_tables->insert(std::make_pair(table_node->db_.empty() ? default_db : table_node->db_,
                                                 table_node->table_));
                return true;
            }
            case node::kPlanTypeProject: {
                const node::ProjectPlanNode* project_plan = dynamic_cast<const node::ProjectPlanNode*>(node);
                if (!project_plan->project_list_vec_.empty()) {
                    for (node::PlanNode* item : project_plan->project_list_vec_) {
                        node::ProjectListNode* project_list = dynamic_cast<node::ProjectListNode*>(item);
                        if (nullptr != project_list->GetW()) {
                            if (!project_list->GetW()->union_tables().empty()) {
                                for (node::PlanNode* union_table : project_list->GetW()->union_tables()) {
                                    if (!GetDependentTables(union_table, default_db, db_tables, status)) {
                                        return false;
                                    }
                                }
                            }
                        }
                    }
                }
                if (node->GetChildrenSize() > 0) {
                    for (auto child : node->GetChildren()) {
                        if (!GetDependentTables(child, default_db, db_tables, status)) {
                            return false;
                        }
                    }
                }
                return true;
            }
            default: {
                if (node->GetChildrenSize() > 0) {
                    for (auto child : node->GetChildren()) {
                        if (!GetDependentTables(child, default_db, db_tables, status)) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool Engine::IsCompatibleCache(RunSession& session,  // NOLINT
                               std::shared_ptr<CompileInfo> info,
                               base::Status& status) {  // NOLINT
    if (info->GetEngineMode() != session.engine_mode()) {
        status = Status(common::kEngineCacheError, "Inconsistent cache, mode expect " +
                                                       EngineModeName(session.engine_mode()) + " but get " +
                                                       EngineModeName(info->GetEngineMode()));
        return false;
    }
    auto& cache_ctx = std::dynamic_pointer_cast<SqlCompileInfo>(info)->get_sql_context();

    if (session.engine_mode() == kBatchMode) {
        auto batch_sess = dynamic_cast<BatchRunSession*>(&session);
        if (cache_ctx.parameter_types.size() != batch_sess->GetParameterSchema().size()) {
            status = Status(common::kEngineCacheError, "Inconsistent cache parameter schema size");
            return false;
        }
        for (int i = 0; i < batch_sess->GetParameterSchema().size(); i++) {
            if (cache_ctx.parameter_types.Get(i).type() != batch_sess->GetParameterSchema().Get(i).type()) {
                status = Status(common::kEngineCacheError, "Inconsistent cache parameter type, expect " +
                                                       batch_sess->GetParameterSchema().Get(i).DebugString() +
                                                       " but get " + cache_ctx.parameter_types.Get(i).DebugString());
                return false;
            }
        }
    } else if (session.engine_mode() == kBatchRequestMode) {
        auto batch_req_sess = dynamic_cast<BatchRequestRunSession*>(&session);
        if (batch_req_sess == nullptr) {
            return false;
        }
        auto& cache_indices = cache_ctx.batch_request_info.common_column_indices;
        auto& sess_indices = batch_req_sess->common_column_indices();
        if (cache_indices != sess_indices) {
            status = Status(common::kEngineCacheError, "Inconsistent common column config");
            return false;
        }
    }
    return true;
}

bool Engine::Get(const std::string& sql, const std::string& db, RunSession& session,
                 base::Status& status) {  // NOLINT (runtime/references)
    std::shared_ptr<CompileInfo> cached_info = GetCacheLocked(db, sql, session.engine_mode(),
        session.GetPerformanceSensitive());
    if (cached_info && IsCompatibleCache(session, cached_info, status)) {
        session.SetCompileInfo(cached_info);
        return true;
    }
    // TODO(baoxinqi): IsCompatibleCache fail, return false, or reset status.
    if (!status.isOK()) {
        LOG(WARNING) << status;
        status = base::Status::OK();
    }
    DLOG(INFO) << "Compile Engine ...";
    status = base::Status::OK();
    std::shared_ptr<SqlCompileInfo> info = std::make_shared<SqlCompileInfo>();
    auto& sql_context = std::dynamic_pointer_cast<SqlCompileInfo>(info)->get_sql_context();
    sql_context.sql = sql;
    sql_context.db = db;
    sql_context.engine_mode = session.engine_mode();
    sql_context.is_performance_sensitive = session.GetPerformanceSensitive();
    sql_context.is_cluster_optimized = options_.is_cluster_optimzied();
    sql_context.is_batch_request_optimized = options_.is_batch_request_optimized();
    sql_context.enable_batch_window_parallelization = options_.is_enable_batch_window_parallelization();
    sql_context.enable_expr_optimize = options_.is_enable_expr_optimize();
    sql_context.jit_options = options_.jit_options();
    if (session.engine_mode() == kBatchMode) {
        sql_context.parameter_types = dynamic_cast<BatchRunSession*>(&session)->GetParameterSchema();
    } else if (session.engine_mode() == kBatchRequestMode) {
        auto batch_req_sess = dynamic_cast<BatchRequestRunSession*>(&session);
        sql_context.batch_request_info.common_column_indices = batch_req_sess->common_column_indices();
    }

    SqlCompiler compiler(std::atomic_load_explicit(&cl_, std::memory_order_acquire), options_.is_keep_ir(), false,
                         options_.is_plan_only());
    bool ok = compiler.Compile(info->get_sql_context(), status);
    if (!ok || 0 != status.code) {
        return false;
    }
    if (!options_.is_compile_only()) {
        ok = compiler.BuildClusterJob(info->get_sql_context(), status);
        if (!ok || 0 != status.code) {
            LOG(WARNING) << "fail to build cluster job: " << status.msg;
            return false;
        }
    }

    SetCacheLocked(db, sql, session.engine_mode(), session.GetPerformanceSensitive(), info);
    session.SetCompileInfo(info);
    if (session.is_debug_) {
        std::ostringstream plan_oss;
        if (nullptr != sql_context.physical_plan) {
            sql_context.physical_plan->Print(plan_oss, "");
            LOG(INFO) << "physical plan:\n" << plan_oss.str() << std::endl;
        }
        std::ostringstream runner_oss;
        sql_context.cluster_job.Print(runner_oss, "");
        LOG(INFO) << "cluster job:\n" << runner_oss.str() << std::endl;
    }
    return true;
}

bool Engine::Explain(const std::string& sql, const std::string& db, EngineMode engine_mode,
                     const codec::Schema& parameter_schema,
                     const std::set<size_t>& common_column_indices, ExplainOutput* explain_output,
                     base::Status* status, bool performance_sensitive) {
    if (explain_output == NULL || status == NULL) {
        LOG(WARNING) << "input args is invalid";
        return false;
    }
    if (!common_column_indices.empty() && engine_mode != kBatchRequestMode) {
        LOG(WARNING) << "common column config can only be valid in batch request mode";
        return false;
    }
    if (!parameter_schema.empty() && engine_mode != kBatchMode) {
        LOG(WARNING) << "parameterized query can only be valid in batch mode";
        return false;
    }
    SqlContext ctx;
    ctx.engine_mode = engine_mode;
    ctx.sql = sql;
    ctx.db = db;
    ctx.parameter_types = parameter_schema;
    ctx.is_performance_sensitive = performance_sensitive;
    ctx.is_cluster_optimized = options_.is_cluster_optimzied();
    ctx.is_batch_request_optimized = !common_column_indices.empty();
    ctx.batch_request_info.common_column_indices = common_column_indices;
    SqlCompiler compiler(std::atomic_load_explicit(&cl_, std::memory_order_acquire), true, true, true);
    bool ok = compiler.Compile(ctx, *status);
    if (!ok || 0 != status->code) {
        return false;
    }
    explain_output->input_schema.CopyFrom(ctx.request_schema);
    explain_output->output_schema.CopyFrom(ctx.schema);
    explain_output->logical_plan = ctx.logical_plan_str;
    explain_output->physical_plan = ctx.physical_plan_str;
    explain_output->ir = ctx.ir;
    explain_output->request_name = ctx.request_name;
    explain_output->request_db_name = ctx.request_db_name;
    if (engine_mode == ::hybridse::vm::kBatchMode) {
        std::set<std::pair<std::string, std::string>> tables;
        base::Status status;
        for (auto iter = ctx.logical_plan.cbegin(); iter != ctx.logical_plan.cend(); iter++) {
            if (!GetDependentTables(*iter, db, &tables, status)) {
                DLOG(WARNING) << "Fail to get dependent tables ";
                break;
            }
        }
        if (!tables.empty()) {
            explain_output->router.SetMainDb(tables.begin()->first);
            explain_output->router.SetMainTable(tables.begin()->second);
        }
    } else {
        explain_output->router.SetMainDb(ctx.request_db_name);
        explain_output->router.SetMainTable(ctx.request_name);
        explain_output->router.Parse(ctx.physical_plan);
    }
    if (engine_mode == ::hybridse::vm::kBatchRequestMode) {
        // fill common output column info
        auto& output_common_indices = ctx.batch_request_info.output_common_column_indices;
        size_t schema_size = static_cast<size_t>(explain_output->output_schema.size());
        for (size_t idx : output_common_indices) {
            if (idx >= schema_size) {
                status->msg =  "Output common column index out of bound: " + std::to_string(idx);
                status->code = common::kCommonIndexError;
                return false;
            }
            auto* column = explain_output->output_schema.Mutable(idx);
            column->set_is_constant(true);
        }
    }
    return true;
}
bool Engine::Explain(const std::string& sql, const std::string& db, EngineMode engine_mode,
                     ExplainOutput* explain_output, base::Status* status, bool performance_sensitive) {
    const codec::Schema empty_schema;
    return Explain(sql, db, engine_mode, empty_schema, {}, explain_output, status, performance_sensitive);
}

bool Engine::Explain(const std::string& sql, const std::string& db, EngineMode engine_mode,
                     const codec::Schema& parameter_schema, ExplainOutput* explain_output, base::Status* status,
                     bool performance_sensitive) {
    return Explain(sql, db, engine_mode, parameter_schema, {}, explain_output, status, performance_sensitive);
}
bool Engine::Explain(const std::string& sql, const std::string& db, EngineMode engine_mode,
             const std::set<size_t>& common_column_indices,
             ExplainOutput* explain_output, base::Status* status,
             bool performance_sensitive) {
    const codec::Schema empty_schema;
    return Explain(sql, db, engine_mode, empty_schema, common_column_indices, explain_output, status,
        performance_sensitive);
}

void Engine::ClearCacheLocked(const std::string& db) {
    std::lock_guard<base::SpinMutex> lock(mu_);
    for (auto& cache : lru_cache_) {
        auto& mode_cache = cache.second;
        for (auto iter = mode_cache.begin(); iter != mode_cache.end(); iter++) {
            mode_cache[iter->first].erase(db);
        }
    }
}

EngineOptions Engine::GetEngineOptions() {
    return options_;
}

std::shared_ptr<CompileInfo> Engine::GetCacheLocked(const std::string& db, const std::string& sql,
                                                    EngineMode engine_mode, bool performance_sensitive) {
    std::lock_guard<base::SpinMutex> lock(mu_);
    // Check mode
    auto mode_iter = lru_cache_.find(engine_mode);
    if (mode_iter == lru_cache_.end()) {
        return nullptr;
    }
    auto& mode_cache = mode_iter->second;

    // Check performance_sensitive
    auto performance_sensitive_iter = mode_cache.find(performance_sensitive);
    if (performance_sensitive_iter == mode_cache.end()) {
        return nullptr;
    }
    auto& performance_sensitive_cache = performance_sensitive_iter->second;

    // Check db
    auto db_iter = performance_sensitive_cache.find(db);
    if (db_iter == performance_sensitive_cache.end()) {
        return nullptr;
    }
    auto& lru = db_iter->second;

    // Check SQL
    auto value = lru.get(sql);
    if (value == boost::none) {
        return nullptr;
    } else {
        return value.value();
    }
}

bool Engine::SetCacheLocked(const std::string& db, const std::string& sql, EngineMode engine_mode,
                            bool performance_sensitive, std::shared_ptr<CompileInfo> info) {
    std::lock_guard<base::SpinMutex> lock(mu_);

    auto& mode_cache = lru_cache_[engine_mode];
    auto& performance_sensitive_cache = mode_cache[performance_sensitive];
    using BoostLRU = boost::compute::detail::lru_cache<std::string, std::shared_ptr<CompileInfo>>;
    std::map<std::string, BoostLRU>::iterator db_iter = performance_sensitive_cache.find(db);
    if (db_iter == performance_sensitive_cache.end()) {
        db_iter = performance_sensitive_cache.insert(db_iter, {db, BoostLRU(options_.max_sql_cache_size())});
    }
    auto& lru = db_iter->second;
    auto value = lru.get(sql);
    if (value == boost::none || engine_mode == kBatchRequestMode) {
        lru.insert(sql, info);
        return true;
    } else {
        // TODO(xxx): Ensure compile result is stable
        DLOG(INFO) << "Engine cache already exists: " << engine_mode << " " << db << "\n" << sql;
        return false;
    }
}

RunSession::RunSession(EngineMode engine_mode) : engine_mode_(engine_mode), is_debug_(false), sp_name_("") {}
RunSession::~RunSession() {}

bool RunSession::SetCompileInfo(const std::shared_ptr<CompileInfo>& compile_info) {
    compile_info_ = compile_info;
    return true;
}

int32_t RequestRunSession::Run(const Row& in_row, Row* out_row) {
    DLOG(INFO) << "Request Row Run with main task";
    return Run(std::dynamic_pointer_cast<SqlCompileInfo>(compile_info_)->get_sql_context().cluster_job.main_task_id(),
               in_row, out_row);
}
int32_t RequestRunSession::Run(const uint32_t task_id, const Row& in_row, Row* out_row) {
    auto task = std::dynamic_pointer_cast<SqlCompileInfo>(compile_info_)
                    ->get_sql_context()
                    .cluster_job.GetTask(task_id)
                    .GetRoot();
    if (nullptr == task) {
        LOG(WARNING) << "fail to run request plan: taskid" << task_id << " not exist!";
        return -2;
    }
    DLOG(INFO) << "Request Row Run with task_id " << task_id;
    RunnerContext ctx(&std::dynamic_pointer_cast<SqlCompileInfo>(compile_info_)->get_sql_context().cluster_job, in_row,
                      sp_name_, is_debug_);
    auto output = task->RunWithCache(ctx);
    if (!output) {
        LOG(WARNING) << "Run request plan output is null";
        return -1;
    }
    bool ok = Runner::ExtractRow(output, out_row);
    if (ok) {
        return 0;
    }
    return -1;
}

int32_t BatchRequestRunSession::Run(const std::vector<Row>& request_batch, std::vector<Row>& output) {
    return Run(std::dynamic_pointer_cast<SqlCompileInfo>(compile_info_)->get_sql_context().cluster_job.main_task_id(),
               request_batch, output);
}
int32_t BatchRequestRunSession::Run(const uint32_t id, const std::vector<Row>& request_batch,
                                    std::vector<Row>& output) {
    RunnerContext ctx(&std::dynamic_pointer_cast<SqlCompileInfo>(compile_info_)->get_sql_context().cluster_job,
                      request_batch, sp_name_, is_debug_);
    auto task =
        std::dynamic_pointer_cast<SqlCompileInfo>(compile_info_)->get_sql_context().cluster_job.GetTask(id).GetRoot();
    if (nullptr == task) {
        LOG(WARNING) << "Fail to run request plan: taskid" << id << " not exist!";
        return -2;
    }
    auto handler = task->BatchRequestRun(ctx);
    if (!handler) {
        LOG(WARNING) << "Run request plan output is null";
        return -1;
    }
    bool ok = Runner::ExtractRows(handler, output);
    if (!ok) {
        return -1;
    }
    ctx.ClearCache();
    return 0;
}
int32_t BatchRunSession::Run(std::vector<Row>& rows, uint64_t limit) {
    return Run(Row(), rows, limit);
}
int32_t BatchRunSession::Run(const Row& parameter_row, std::vector<Row>& rows, uint64_t limit) {
    auto& sql_ctx = std::dynamic_pointer_cast<SqlCompileInfo>(compile_info_)->get_sql_context();
    RunnerContext ctx(&sql_ctx.cluster_job, parameter_row, is_debug_);
    auto output = sql_ctx.cluster_job.GetTask(0).GetRoot()->RunWithCache(ctx);
    if (!output) {
        LOG(WARNING) << "Run batch plan output is null";
        return -1;
    }
    switch (output->GetHanlderType()) {
        case kTableHandler: {
            auto iter = std::dynamic_pointer_cast<TableHandler>(output)->GetIterator();
            if (!iter) {
                return 0;
            }
            iter->SeekToFirst();
            while (iter->Valid()) {
                rows.push_back(iter->GetValue());
                iter->Next();
            }
            return 0;
        }
        case kRowHandler: {
            rows.push_back(std::dynamic_pointer_cast<RowHandler>(output)->GetValue());
            return 0;
        }
        case kPartitionHandler: {
            LOG(WARNING) << "Partition output is invalid";
            return -1;
        }
    }
    return 0;
}

std::shared_ptr<RowHandler> LocalTablet::SubQuery(uint32_t task_id, const std::string& db, const std::string& sql,
                                                  const Row& row, const bool is_procedure, const bool is_debug) {
    DLOG(INFO) << "Local tablet SubQuery request: task id " << task_id;
    RequestRunSession session;
    base::Status status;
    if (is_debug) {
        session.EnableDebug();
    }
    if (is_procedure) {
        if (!sp_cache_) {
            auto error = std::shared_ptr<RowHandler>(new ErrorRowHandler(common::kProcedureNotFound,
                                                                         "SubQuery Fail: procedure not found, "
                                                                         "procedure cache not exist"));
            LOG(WARNING) << error->GetStatus();
            return error;
        }
        auto request_compile_info = sp_cache_->GetRequestInfo(db, sql, status);
        if (!status.isOK()) {
            auto error = std::shared_ptr<RowHandler>(new ErrorRowHandler(status.code, "SubQuery Fail: " + status.msg));
            LOG(WARNING) << error->GetStatus();
            return error;
        }
        session.SetSpName(sql);
        session.SetCompileInfo(request_compile_info);
    } else {
        if (!engine_->Get(sql, db, session, status)) {
            auto error = std::shared_ptr<RowHandler>(new ErrorRowHandler(status.code, "SubQuery Fail: " + status.msg));
            LOG(WARNING) << error->GetStatus();
            return error;
        }
    }

    return std::shared_ptr<RowHandler>(new LocalTabletRowHandler(task_id, session, row));
}
std::shared_ptr<TableHandler> LocalTablet::SubQuery(uint32_t task_id, const std::string& db, const std::string& sql,
                                                    const std::set<size_t>& common_column_indices,
                                                    const std::vector<Row>& in_rows, const bool request_is_common,
                                                    const bool is_procedure, const bool is_debug) {
    DLOG(INFO) << "Local tablet SubQuery batch request: task id " << task_id;
    BatchRequestRunSession session;
    for (size_t idx : common_column_indices) {
        session.AddCommonColumnIdx(idx);
    }
    base::Status status;
    if (is_debug) {
        session.EnableDebug();
    }
    if (is_procedure) {
        if (!sp_cache_) {
            auto error = std::shared_ptr<TableHandler>(new ErrorTableHandler(common::kProcedureNotFound,
                                                                             "SubQuery Fail: procedure not found, "
                                                                             "procedure cache not exist"));
            LOG(WARNING) << error->GetStatus();
            return error;
        }
        auto request_compile_info = sp_cache_->GetBatchRequestInfo(db, sql, status);
        if (!status.isOK()) {
            auto error =
                std::shared_ptr<TableHandler>(new ErrorTableHandler(status.code, "SubQuery Fail: " + status.msg));
            LOG(WARNING) << error->GetStatus();
            return error;
        }
        session.SetSpName(sql);
        session.SetCompileInfo(request_compile_info);
    } else {
        if (!engine_->Get(sql, db, session, status)) {
            auto error =
                std::shared_ptr<TableHandler>(new ErrorTableHandler(status.code, "SubQuery Fail: " + status.msg));
            LOG(WARNING) << error->GetStatus();
            return error;
        }
    }
    return std::make_shared<LocalTabletTableHandler>(task_id, session, in_rows, request_is_common);
}
}  // namespace vm
}  // namespace hybridse
