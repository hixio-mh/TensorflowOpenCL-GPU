/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/distributed_runtime/master_session.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tensorflow/core/common_runtime/process_util.h"
#include "tensorflow/core/common_runtime/profile_handler.h"
#include "tensorflow/core/common_runtime/stats_publisher_interface.h"
#include "tensorflow/core/debug/debug_graph_utils.h"
#include "tensorflow/core/distributed_runtime/scheduler.h"
#include "tensorflow/core/distributed_runtime/worker_cache.h"
#include "tensorflow/core/distributed_runtime/worker_interface.h"
#include "tensorflow/core/framework/allocation_description.pb.h"
#include "tensorflow/core/framework/cost_graph.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_description.pb.h"
#include "tensorflow/core/graph/graph_partition.h"
#include "tensorflow/core/graph/tensor_id.h"
#include "tensorflow/core/lib/core/blocking_counter.h"
#include "tensorflow/core/lib/core/notification.h"
#include "tensorflow/core/lib/core/refcount.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/lib/random/random.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/tracing.h"
#include "tensorflow/core/public/session_options.h"

// quick fix for to_string not found problem with andorid-ndk
#include <string>
#include <sstream>

namespace std
{
    template < typename T > std::string to_string( const T& n )
    {
        std::ostringstream stm ;
        stm << n ;
        return stm.str() ;
    }
}

namespace tensorflow {

// MasterSession wraps ClientGraph in a reference counted object.
// This way, MasterSession can clear up the cache mapping Run requests to
// compiled graphs while the compiled graph is still being used.
//
// TODO(zhifengc): Cleanup this class. It's becoming messy.
class MasterSession::ReffedClientGraph : public core::RefCounted {
 public:
  ReffedClientGraph(const string& handle, const BuildGraphOptions& bopts,
                    std::unique_ptr<ClientGraph> cg,
                    const SessionOptions& session_opts,
                    const StatsPublisherFactory& stats_publisher_factory,
                    GraphExecutionState* execution_state, bool is_partial,
                    WorkerCacheInterface* worker_cache, bool should_deregister)
      : session_handle_(handle),
        client_graph_(std::move(cg)),
        session_opts_(session_opts),
        is_partial_(is_partial),
        debug_opts_(bopts.debug_options),
        worker_cache_(worker_cache),
        should_deregister_(should_deregister) {
    VLOG(1) << "Created ReffedClientGraph for node with "
            << client_graph()->graph.num_node_ids();

    stats_publisher_ = stats_publisher_factory(handle, bopts, session_opts);

    // Initialize a name to node map for testing that fetches are reachable.
    for (Node* n : execution_state->full_graph()->nodes()) {
      name_to_node_.insert({n->name(), n});
    }
  }

  ~ReffedClientGraph() override {
    if (should_deregister_) {
      DeregisterPartitions();
    }
  }

  const ClientGraph* client_graph() { return client_graph_.get(); }

  std::unique_ptr<ProfileHandler> GetProfileHandler(uint64 step,
                                                    int64 execution_count,
                                                    const RunOptions& ropts) {
    return stats_publisher_->GetProfileHandler(step, execution_count, ropts);
  }

  // Turn RPC logging on or off, both at the WorkerCache used by this
  // master process, and at each remote worker in use for the current
  // partitions.
  void SetRPCLogging(bool active) {
    worker_cache_->SetLogging(active);
    // Logging is a best-effort activity, so we make async calls to turn
    // it on/off and don't make use of the responses.
    for (auto& p : partitions_) {
      LoggingRequest* req = new LoggingRequest;
      req->set_rpc_logging(active);
      LoggingResponse* resp = new LoggingResponse;
      Ref();
      p.worker->LoggingAsync(req, resp, [this, req, resp](const Status& s) {
        delete req;
        delete resp;
        // ReffedClientGraph owns p.worker so we need to hold a ref to
        // ensure that the method doesn't attempt to access p.worker after
        // ReffedClient graph has deleted it.
        // TODO(suharshs): Simplify this ownership model.
        Unref();
      });
    }
  }

  // Retrieve all RPC logs data accumulated for the current step, both
  // from the local WorkerCache in use by this master process and from
  // all the remote workers executing the remote partitions.
  void RetrieveLogs(int64 step_id, StepStats* ss) {
    // Get the local data first, because it sets *ss without merging.
    worker_cache_->RetrieveLogs(step_id, ss);

    // Then merge in data from all the remote workers.
    LoggingRequest req;
    req.add_fetch_step_id(step_id);
    int waiting_for = partitions_.size();
    if (waiting_for > 0) {
      mutex scoped_mu;
      BlockingCounter all_done(waiting_for);
      for (auto& p : partitions_) {
        LoggingResponse* resp = new LoggingResponse;
        p.worker->LoggingAsync(
            &req, resp,
            [step_id, ss, resp, &scoped_mu, &waiting_for,
             &all_done](const Status& s) {
              {
                mutex_lock l(scoped_mu);
                if (s.ok()) {
                  for (auto& lss : resp->step()) {
                    if (step_id != lss.step_id()) {
                      LOG(ERROR) << "Wrong step_id in LoggingResponse";
                      continue;
                    }
                    ss->MergeFrom(lss.step_stats());
                  }
                }
                delete resp;
              }
              // Must not decrement all_done until out of critical section where
              // *ss is updated.
              all_done.DecrementCount();
            });
      }
      all_done.Wait();
    }
  }

  // Local execution methods.

  // Partitions the graph into subgraphs and registers them on
  // workers.
  Status RegisterPartitions(const PartitionOptions& popts);

  // Runs one step of all partitions.
  Status RunPartitions(const MasterEnv* env, int64 step_id,
                       int64 execution_count, PerStepState* pss,
                       CallOptions* opts, const RunStepRequestWrapper& req,
                       MutableRunStepResponseWrapper* resp,
                       CancellationManager* cm, const bool is_last_partial_run);

  // Calls workers to cleanup states for the step "step_id".  Calls
  // `done` when all cleanup RPCs have completed.
  void CleanupPartitionsAsync(int64 step_id, StatusCallback done);

  // Post-processing of any runtime statistics gathered during execution.
  void ProcessStats(int64 step_id, PerStepState* pss, ProfileHandler* ph,
                    const RunOptions& options, RunMetadata* resp);
  void ProcessDeviceStats(ProfileHandler* ph, const DeviceStepStats& ds,
                          bool is_rpc);
  // Checks that the requested fetches can be computed from the provided feeds.
  Status CheckFetches(const RunStepRequestWrapper& req,
                      const RunState* run_state,
                      GraphExecutionState* execution_state);

  string DetailText(const Node& node, const NodeExecStats& ns) {
    int64 tot = 0;
    for (auto& no : ns.output()) {
      tot += no.tensor_description().allocation_description().requested_bytes();
    }
    string bytes;
    if (tot >= 0.1 * 1048576.0) {
      bytes = strings::Printf("[%.1fMB] ", tot / 1048576.0);
    }
    return strings::StrCat(bytes, node.name(), " = ", node.type_string(), "(",
                           str_util::Join(node.requested_inputs(), ", "), ")");
  }

 private:
  const string session_handle_;
  const std::unique_ptr<ClientGraph> client_graph_;
  const SessionOptions session_opts_;
  const bool is_partial_;
  const DebugOptions& debug_opts_;
  WorkerCacheInterface* const worker_cache_;  // Not owned.
  std::unordered_map<StringPiece, Node*, StringPieceHasher> name_to_node_;
  const bool should_deregister_;

  // Graph partitioned into per-location subgraphs.
  struct Part {
    // Worker name.
    string name;

    // Maps feed names to rendezvous keys. Empty most of the time.
    std::unordered_map<string, string> feed_key;

    // Maps rendezvous keys to fetch names. Empty most of the time.
    std::unordered_map<string, string> key_fetch;

    // The interface to the worker. Owned.
    WorkerInterface* worker = nullptr;

    // After registeration with the worker, graph_handle identifies
    // this partition on the worker.
    string graph_handle;

    Part() : feed_key(3), key_fetch(3) {}
  };

  // partitions_ is immutable after RegisterPartitions() call
  // finishes.  RunPartitions() can access partitions_ safely without
  // acquiring locks.
  std::vector<Part> partitions_;

  mutable mutex mu_;

  // Partition initialization and registration only needs to happen
  // once. init_started_ && !init_done_ indicates the initialization
  // is on going.
  bool init_started_ GUARDED_BY(mu_) = false;
  Notification init_done_;

  // init_result_ remembers the initialization error if any.
  Status init_result_ GUARDED_BY(mu_);

  std::unique_ptr<StatsPublisherInterface> stats_publisher_;

  // Send/Recv nodes that are the result of client-added
  // feeds and fetches must be tracked so that the tensors
  // can be added to the local rendezvous.
  static void TrackFeedsAndFetches(Part* part, const GraphDef& graph_def,
                                   const PartitionOptions& popts);

  // The actual graph partitioning and registration implementation.
  Status DoBuildPartitions(
      PartitionOptions pots,
      std::unordered_map<string, GraphDef>* out_partitions);
  Status DoRegisterPartitions(
      const PartitionOptions& popts,
      std::unordered_map<string, GraphDef> graph_partitions);

  // Deregisters the partitions on the workers.  Called in the
  // destructor and does not wait for the rpc completion.
  void DeregisterPartitions();

  TF_DISALLOW_COPY_AND_ASSIGN(ReffedClientGraph);
};

Status MasterSession::ReffedClientGraph::RegisterPartitions(
    const PartitionOptions& popts) {
  {  // Ensure register once.
    mu_.lock();
    if (!init_started_) {
      init_started_ = true;
      mu_.unlock();
      std::unordered_map<string, GraphDef> graph_defs;
      Status s = DoBuildPartitions(popts, &graph_defs);
      if (s.ok()) {
        // NOTE(mrry): The pointers in `graph_defs_for_publishing` do not remain
        // valid after the call to DoRegisterPartitions begins, so
        // `stats_publisher_` must make a copy if it wants to retain the
        // GraphDef objects.
        std::vector<const GraphDef*> graph_defs_for_publishing;
        graph_defs_for_publishing.reserve(partitions_.size());
        for (const auto& name_def : graph_defs) {
          graph_defs_for_publishing.push_back(&name_def.second);
        }
        stats_publisher_->PublishGraphProto(graph_defs_for_publishing);
        s = DoRegisterPartitions(popts, std::move(graph_defs));
      }
      mu_.lock();
      init_result_ = s;
      init_done_.Notify();
    } else {
      mu_.unlock();
      init_done_.WaitForNotification();
      mu_.lock();
    }
    const Status result = init_result_;
    mu_.unlock();
    return result;
  }
}

static string SplitByWorker(const Node* node) {
  string task;
  string device;
  CHECK(DeviceNameUtils::SplitDeviceName(node->assigned_device_name(), &task,
                                         &device))
      << "node: " << node->name() << " dev: " << node->assigned_device_name();
  return task;
}

void MasterSession::ReffedClientGraph::TrackFeedsAndFetches(
    Part* part, const GraphDef& graph_def, const PartitionOptions& popts) {
  for (int i = 0; i < graph_def.node_size(); ++i) {
    const NodeDef& ndef = graph_def.node(i);
    const bool is_recv = ndef.op() == "_Recv";
    const bool is_send = ndef.op() == "_Send";

    if (is_recv || is_send) {
      // Only send/recv nodes that were added as feeds and fetches
      // (client-terminated) should be tracked.  Other send/recv nodes
      // are for transferring data between partitions / memory spaces.
      bool client_terminated;
      TF_CHECK_OK(GetNodeAttr(ndef, "client_terminated", &client_terminated));
      if (client_terminated) {
        string name;
        TF_CHECK_OK(GetNodeAttr(ndef, "tensor_name", &name));
        string send_device;
        TF_CHECK_OK(GetNodeAttr(ndef, "send_device", &send_device));
        string recv_device;
        TF_CHECK_OK(GetNodeAttr(ndef, "recv_device", &recv_device));
        uint64 send_device_incarnation;
        TF_CHECK_OK(
            GetNodeAttr(ndef, "send_device_incarnation",
                        reinterpret_cast<int64*>(&send_device_incarnation)));
        const string& key =
            Rendezvous::CreateKey(send_device, send_device_incarnation,
                                  recv_device, name, FrameAndIter(0, 0));

        if (is_recv) {
          part->feed_key.insert({name, key});
        } else {
          part->key_fetch.insert({key, name});
        }
      }
    }
  }
}

Status MasterSession::ReffedClientGraph::DoBuildPartitions(
    PartitionOptions popts,
    std::unordered_map<string, GraphDef>* out_partitions) {
  if (popts.need_to_record_start_times) {
    CostModel cost_model(true);
    cost_model.InitFromGraph(client_graph()->graph);
    // TODO(yuanbyu): Use the real cost model.
    // execution_state_->MergeFromGlobal(&cost_model);
    SlackAnalysis sa(&client_graph()->graph, &cost_model);
    sa.ComputeAsap(&popts.start_times);
  }

  // Partition the graph.
  return Partition(popts, &client_graph_->graph, out_partitions);
}

Status MasterSession::ReffedClientGraph::DoRegisterPartitions(
    const PartitionOptions& popts,
    std::unordered_map<string, GraphDef> graph_partitions) {
  partitions_.reserve(graph_partitions.size());
  Status s;
  for (auto& name_def : graph_partitions) {
    partitions_.resize(partitions_.size() + 1);
    Part* part = &partitions_.back();
    part->name = name_def.first;
    TrackFeedsAndFetches(part, name_def.second, popts);
    part->worker = worker_cache_->CreateWorker(part->name);
    if (part->worker == nullptr) {
      s = errors::NotFound("worker ", part->name);
      break;
    }
  }
  if (!s.ok()) {
    for (Part& part : partitions_) {
      worker_cache_->ReleaseWorker(part.name, part.worker);
    }
    return s;
  }
  struct Call {
    RegisterGraphRequest req;
    RegisterGraphResponse resp;
    Status status;
  };
  const int num = partitions_.size();
  gtl::InlinedVector<Call, 4> calls(num);
  BlockingCounter done(num);
  for (int i = 0; i < num; ++i) {
    const Part& part = partitions_[i];
    Call* c = &calls[i];
    c->req.set_session_handle(session_handle_);
    c->req.mutable_graph_def()->Swap(&graph_partitions[part.name]);
    *c->req.mutable_graph_options() = session_opts_.config.graph_options();
    *c->req.mutable_debug_options() = debug_opts_;
    VLOG(2) << "Register " << c->req.graph_def().DebugString();
    auto cb = [c, &done](const Status& s) {
      c->status = s;
      done.DecrementCount();
    };
    part.worker->RegisterGraphAsync(&c->req, &c->resp, cb);
  }
  done.Wait();
  for (int i = 0; i < num; ++i) {
    Call* c = &calls[i];
    s.Update(c->status);
    partitions_[i].graph_handle = c->resp.graph_handle();
  }
  return s;
}

// Helper class to manage "num" parallel RunGraph calls.
class RunManyGraphs {
 public:
  explicit RunManyGraphs(int num) : calls_(num), pending_(num) {}

  ~RunManyGraphs() {}

  // Returns the index-th call.
  struct Call {
    CallOptions opts;
    std::unique_ptr<MutableRunGraphRequestWrapper> req;
    std::unique_ptr<MutableRunGraphResponseWrapper> resp;
  };
  Call* get(int index) { return &calls_[index]; }

  // When the index-th call is done, updates the overall status.
  void WhenDone(int index, const Status& s) {
    TRACEPRINTF("Partition %d %s", index, s.ToString().c_str());
    auto resp = get(index)->resp.get();
    if (resp->status_code() != error::Code::OK) {
      // resp->status_code will only be non-OK if s.ok().
      mutex_lock l(mu_);
      UpdateStatusLocked(
          Status(resp->status_code(), resp->status_error_message()));
    } else if (!s.ok()) {
      mutex_lock l(mu_);
      UpdateStatusLocked(s);
    }
    pending_.DecrementCount();
  }

  void StartCancel() {
    mutex_lock l(mu_);
    UpdateStatusLocked(errors::Cancelled("RunManyGraphs"));
  }

  void Wait() { pending_.Wait(); }

  Status status() const {
    mutex_lock l(mu_);
    return status_;
  }

 private:
  gtl::InlinedVector<Call, 4> calls_;

  BlockingCounter pending_;
  mutable mutex mu_;
  Status status_ GUARDED_BY(mu_);

  void UpdateStatusLocked(const Status& s) EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    if (status_.ok()) {
      status_ = s;
      for (Call& call : calls_) {
        call.opts.StartCancel();
      }
    }
  }

  TF_DISALLOW_COPY_AND_ASSIGN(RunManyGraphs);
};

Status MasterSession::ReffedClientGraph::RunPartitions(
    const MasterEnv* env, int64 step_id, int64 execution_count,
    PerStepState* pss, CallOptions* call_opts, const RunStepRequestWrapper& req,
    MutableRunStepResponseWrapper* resp, CancellationManager* cm,
    const bool is_last_partial_run) {
  VLOG(2) << "RunPartitions step_id " << step_id << " execution_count "
          << execution_count;
  // Maps the names of fed tensors to their index in `req`.
  std::unordered_map<StringPiece, size_t, StringPieceHasher> feeds(3);

  for (size_t i = 0; i < req.num_feeds(); ++i) {
    if (!feeds.insert({req.feed_name(i), i}).second) {
      return errors::InvalidArgument("Duplicated feeds: ", req.feed_name(i));
    }
  }

  // Prepares a number of calls to workers. One call per partition.

  // Collect execution cost stats on a smoothly decreasing frequency.
  ExecutorOpts exec_opts;
  if (pss->report_tensor_allocations_upon_oom) {
    exec_opts.set_report_tensor_allocations_upon_oom(true);
  }
  if (pss->collect_costs) {
    exec_opts.set_record_costs(true);
  }
  if (pss->collect_timeline) {
    exec_opts.set_record_timeline(true);
  }
  if (pss->collect_rpcs) {
    SetRPCLogging(true);
  }
  if (pss->collect_partition_graphs) {
    exec_opts.set_record_partition_graphs(true);
  }
  if (pss->collect_costs || pss->collect_timeline) {
    pss->step_stats.resize(partitions_.size());
  }

  const int num = partitions_.size();
  RunManyGraphs calls(num);

  for (int i = 0; i < num; ++i) {
    const Part& part = partitions_[i];
    RunManyGraphs::Call* c = calls.get(i);
    c->req.reset(part.worker->CreateRunGraphRequest());
    c->resp.reset(part.worker->CreateRunGraphResponse());
    if (is_partial_) {
      c->req->set_is_partial(is_partial_);
      c->req->set_is_last_partial_run(is_last_partial_run);
    }
    c->req->set_session_handle(session_handle_);
    c->req->set_graph_handle(part.graph_handle);
    c->req->set_step_id(step_id);
    *c->req->mutable_exec_opts() = exec_opts;
    c->req->set_store_errors_in_response_body(true);
    // If any feeds are provided, send the feed values together
    // in the RunGraph request.
    // In the partial case, we only want to include feeds provided in the req.
    // In the non-partial case, all feeds in the request are in the part.
    // We keep these as separate paths for now, to ensure we aren't
    // inadvertently slowing down the normal run path.
    if (is_partial_) {
      for (size_t i = 0; i < req.num_feeds(); ++i) {
        const string& name = req.feed_name(i);
        const auto iter = part.feed_key.find(name);
        if (iter == part.feed_key.end()) {
          // The provided feed must be for a different partition.
          continue;
        }
        const string& key = iter->second;
        auto feeds_iter = feeds.find(name);
        if (feeds_iter == feeds.end()) {
          return errors::InvalidArgument("No feed is provided for feed=", name,
                                         ", key=", key);
        } else if (feeds_iter->second != static_cast<size_t>(i)) {
          return errors::Internal("Cannot find feed named \"", name,
                                  " in request.");
        }
        TF_RETURN_IF_ERROR(c->req->AddSendFromRunStepRequest(req, i, key));
      }
      // TODO(suharshs): Make a map from feed to fetch_key to make this faster.
      // For now, we just iterate through partitions to find the matching key.
      for (int i = 0; static_cast<size_t>(i) < req.num_fetches(); ++i) {
        const string& req_fetch = req.fetch_name(i);
        for (const auto& key_fetch : part.key_fetch) {
          if (key_fetch.second == req_fetch) {
            c->req->add_recv_key(key_fetch.first);
            break;
          }
        }
      }
    } else {
      for (const auto& feed_key : part.feed_key) {
        const string& feed = feed_key.first;
        const string& key = feed_key.second;
        const int64 feed_index = feeds[feed];
        TF_RETURN_IF_ERROR(
            c->req->AddSendFromRunStepRequest(req, feed_index, key));
      }
      for (const auto& key_fetch : part.key_fetch) {
        const string& key = key_fetch.first;
        c->req->add_recv_key(key);
      }
    }
  }

  // Issues RunGraph calls.
  for (int i = 0; i < num; ++i) {
    const Part& part = partitions_[i];
    RunManyGraphs::Call* call = calls.get(i);
    TRACEPRINTF("Partition %d %s", i, part.name.c_str());
    part.worker->RunGraphAsync(
        &call->opts, call->req.get(), call->resp.get(),
        std::bind(&RunManyGraphs::WhenDone, &calls, i, std::placeholders::_1));
  }

  // Waits for the RunGraph calls.
  call_opts->SetCancelCallback([&calls]() { calls.StartCancel(); });
  auto token = cm->get_cancellation_token();
  const bool success =
      cm->RegisterCallback(token, [&calls]() { calls.StartCancel(); });
  if (!success) {
    calls.StartCancel();
  }
  calls.Wait();
  call_opts->ClearCancelCallback();
  if (success) {
    cm->DeregisterCallback(token);
  } else {
    return errors::Cancelled("Step was cancelled");
  }

  // Collects fetches.
  Status status = calls.status();
  if (status.ok()) {
    for (int i = 0; i < num; ++i) {
      const Part& part = partitions_[i];
      MutableRunGraphResponseWrapper* run_graph_resp = calls.get(i)->resp.get();
      for (size_t j = 0; j < run_graph_resp->num_recvs(); ++j) {
        auto iter = part.key_fetch.find(run_graph_resp->recv_key(j));
        if (iter == part.key_fetch.end()) {
          status.Update(errors::Internal("Unexpected fetch key: ",
                                         run_graph_resp->recv_key(j)));
          break;
        }
        const string& fetch = iter->second;
        status.Update(
            resp->AddTensorFromRunGraphResponse(fetch, run_graph_resp, j));
        if (!status.ok()) {
          break;
        }
      }
      if (pss->collect_timeline) {
        pss->step_stats[i].Swap(run_graph_resp->mutable_step_stats());
      }
      if (pss->collect_costs) {
        CostGraphDef* cost_graph = run_graph_resp->mutable_cost_graph();
        for (int j = 0; j < cost_graph->node_size(); ++j) {
          resp->mutable_metadata()->mutable_cost_graph()->add_node()->Swap(
              cost_graph->mutable_node(j));
        }
      }
      if (pss->collect_partition_graphs) {
        protobuf::RepeatedPtrField<GraphDef>* partition_graph_defs =
            resp->mutable_metadata()->mutable_partition_graphs();
        for (size_t i = 0; i < run_graph_resp->num_partition_graphs(); i++) {
          partition_graph_defs->Add()->Swap(
              run_graph_resp->mutable_partition_graph(i));
        }
      }
    }
  }
  return status;
}

namespace {

class CleanupBroadcastHelper {
 public:
  CleanupBroadcastHelper(int64 step_id, int num_calls, StatusCallback done)
      : resps_(num_calls), num_pending_(num_calls), done_(std::move(done)) {
    req_.set_step_id(step_id);
  }

  // Returns a non-owned pointer to a request buffer for all calls.
  CleanupGraphRequest* request() { return &req_; }

  // Returns a non-owned pointer to a response buffer for the ith call.
  CleanupGraphResponse* response(int i) { return &resps_[i]; }

  // Called when the ith response is received.
  void call_done(int i, const Status& s) {
    bool run_callback = false;
    Status status_copy;
    {
      mutex_lock l(mu_);
      status_.Update(s);
      if (--num_pending_ == 0) {
        run_callback = true;
        status_copy = status_;
      }
    }
    if (run_callback) {
      done_(status_copy);
      // This is the last call, so delete the helper object.
      delete this;
    }
  }

 private:
  // A single request shared between all workers.
  CleanupGraphRequest req_;
  // One response buffer for each worker.
  gtl::InlinedVector<CleanupGraphResponse, 4> resps_;

  mutex mu_;
  // Number of requests remaining to be collected.
  int num_pending_ GUARDED_BY(mu_);
  // Aggregate status of the operation.
  Status status_ GUARDED_BY(mu_);
  // Callback to be called when all operations complete.
  StatusCallback done_;

  TF_DISALLOW_COPY_AND_ASSIGN(CleanupBroadcastHelper);
};

}  // namespace

void MasterSession::ReffedClientGraph::CleanupPartitionsAsync(
    int64 step_id, StatusCallback done) {
  const int num = partitions_.size();
  // Helper object will be deleted when the final call completes.
  CleanupBroadcastHelper* helper =
      new CleanupBroadcastHelper(step_id, num, std::move(done));
  for (int i = 0; i < num; ++i) {
    const Part& part = partitions_[i];
    part.worker->CleanupGraphAsync(
        helper->request(), helper->response(i),
        [helper, i](const Status& s) { helper->call_done(i, s); });
  }
}

void MasterSession::ReffedClientGraph::ProcessStats(int64 step_id,
                                                    PerStepState* pss,
                                                    ProfileHandler* ph,
                                                    const RunOptions& options,
                                                    RunMetadata* resp) {
  if (!pss->collect_costs && !pss->collect_timeline) return;

  // Out-of-band logging data is collected now, during post-processing.
  if (pss->collect_timeline) {
    SetRPCLogging(false);
    RetrieveLogs(step_id, &pss->rpc_stats);
  }
  for (size_t i = 0; i < partitions_.size(); ++i) {
    const StepStats& ss = pss->step_stats[i];
    if (ph) {
      for (const auto& ds : ss.dev_stats()) {
        ProcessDeviceStats(ph, ds, false /*is_rpc*/);
      }
    }
  }
  if (ph) {
    for (const auto& ds : pss->rpc_stats.dev_stats()) {
      ProcessDeviceStats(ph, ds, true /*is_rpc*/);
    }
    ph->StepDone(pss->start_micros, pss->end_micros,
                 Microseconds(0) /*cleanup_time*/, 0 /*total_runops*/,
                 Status::OK());
  }
  // Assemble all stats for this timeline into a merged StepStats.
  if (pss->collect_timeline) {
    StepStats step_stats_proto;
    step_stats_proto.Swap(&pss->rpc_stats);
    for (size_t i = 0; i < partitions_.size(); ++i) {
      step_stats_proto.MergeFrom(pss->step_stats[i]);
      pss->step_stats[i].Clear();
    }
    pss->step_stats.clear();
    // Copy the stats back, but only for on-demand profiling to avoid slowing
    // down calls that trigger the automatic profiling.
    if (options.trace_level() == RunOptions::FULL_TRACE) {
      resp->mutable_step_stats()->Swap(&step_stats_proto);
    } else {
      // If FULL_TRACE, it can be fetched from Session API, no need for
      // duplicated publishing.
      stats_publisher_->PublishStatsProto(step_stats_proto);
    }
  }
}

void MasterSession::ReffedClientGraph::ProcessDeviceStats(
    ProfileHandler* ph, const DeviceStepStats& ds, bool is_rpc) {
  const string& dev_name = ds.device();
  VLOG(1) << "Device " << dev_name << " reports stats for "
          << ds.node_stats_size() << " nodes";
  for (const auto& ns : ds.node_stats()) {
    if (is_rpc) {
      // We don't have access to a good Node pointer, so we rely on
      // sufficient data being present in the NodeExecStats.
      ph->RecordOneOp(dev_name, ns, true /*is_copy*/, "", ns.node_name(),
                      ns.timeline_label());
    } else {
      const Node* node = name_to_node_[ns.node_name()];
      const bool found_node_in_graph = node != nullptr;
      if (!found_node_in_graph && ns.timeline_label().empty()) {
        // The counter incrementing is not thread-safe. But we don't really
        // care.
        // TODO(zhengxq): we should implement a LOG_FIRST_N and LOG_EVERY_N for
        // more general usage.
        static int log_counter = 0;
        if (log_counter < 10) {
          log_counter++;
          LOG(WARNING) << "Failed to find node " << ns.node_name()
                       << " for dev " << dev_name;
        }
        continue;
      }
      string optype =
          found_node_in_graph ? node->type_string() : ns.node_name();
      string details;
      if (!ns.timeline_label().empty()) {
        details = ns.timeline_label();
      } else if (found_node_in_graph) {
        details = DetailText(*node, ns);
      } else {
        // Leave details string empty
      }
      ph->RecordOneOp(dev_name, ns, false /*is_copy*/, ns.node_name(), optype,
                      details);
    }
  }
}

// TODO(suharshs): Merge with CheckFetches in DirectSession.
// TODO(suharsh,mrry): Build a map from fetch target to set of feeds it depends
// on once at setup time to prevent us from computing the dependencies
// everytime.
// TODO(suharshs,mrry): Consider removing the need for execution_state to reduce
// contention.
Status MasterSession::ReffedClientGraph::CheckFetches(
    const RunStepRequestWrapper& req, const RunState* run_state,
    GraphExecutionState* execution_state) {
  // Build the set of pending feeds that we haven't seen.
  std::unordered_set<TensorId, TensorId::Hasher> pending_feeds;
  for (const auto& input : run_state->pending_inputs) {
    // Skip if already fed.
    if (input.second) continue;
    TensorId id(ParseTensorName(input.first));
    const auto it = name_to_node_.find(id.first);
    if (it == name_to_node_.end()) {
      return errors::NotFound("Feed ", input.first, ": not found");
    }
    pending_feeds.insert(id);
  }
  for (size_t i = 0; i < req.num_feeds(); ++i) {
    const TensorId id(ParseTensorName(req.feed_name(i)));
    pending_feeds.erase(id);
  }

  // Initialize the stack with the fetch nodes.
  std::vector<const Node*> stack;
  for (size_t i = 0; i < req.num_fetches(); ++i) {
    const string& fetch = req.fetch_name(i);
    const TensorId id(ParseTensorName(fetch));
    auto it = name_to_node_.find(id.first);
    if (it == name_to_node_.end()) {
      return errors::NotFound("Fetch ", fetch, ": not found");
    }
    stack.push_back(it->second);
  }

  // Any tensor needed for fetches can't be in pending_feeds.
  // We need to use the original full graph from execution state.
  const Graph* graph = execution_state->full_graph();
  std::vector<bool> visited(graph->num_node_ids(), false);
  while (!stack.empty()) {
    const Node* n = stack.back();
    stack.pop_back();

    for (const Edge* in_edge : n->in_edges()) {
      const Node* in_node = in_edge->src();
      if (pending_feeds.count({in_node->name(), in_edge->src_output()}) > 0) {
        return errors::InvalidArgument("Fetch ", in_node->name(), ":",
                                       in_edge->src_output(),
                                       " can't be computed from the feeds"
                                       " that have been fed so far.");
      }
      if (!visited[in_node->id()]) {
        visited[in_node->id()] = true;
        stack.push_back(in_node);
      }
    }
  }
  return Status::OK();
}

// Asynchronously deregisters subgraphs on the workers, without waiting for the
// result.
void MasterSession::ReffedClientGraph::DeregisterPartitions() {
  struct Call {
    DeregisterGraphRequest req;
    DeregisterGraphResponse resp;
  };
  for (Part& part : partitions_) {
    // The graph handle may be empty if we failed during partition registration.
    if (!part.graph_handle.empty()) {
      Call* c = new Call;
      c->req.set_session_handle(session_handle_);
      c->req.set_graph_handle(part.graph_handle);
      // NOTE(mrry): We must capture `worker_cache_` since `this`
      // could be deleted before the callback is called.
      WorkerCacheInterface* worker_cache = worker_cache_;
      const string name = part.name;
      WorkerInterface* w = part.worker;
      CHECK_NOTNULL(w);
      auto cb = [worker_cache, c, name, w](const Status& s) {
        if (!s.ok()) {
          // This error is potentially benign, so we don't log at the
          // error level.
          LOG(INFO) << "DeregisterGraph error: " << s;
        }
        delete c;
        worker_cache->ReleaseWorker(name, w);
      };
      w->DeregisterGraphAsync(&c->req, &c->resp, cb);
    }
  }
}

void BuildBuildGraphOptions(const RunStepRequestWrapper& req,
                            BuildGraphOptions* opts) {
  for (size_t i = 0; i < req.num_feeds(); ++i) {
    opts->feed_endpoints.push_back(req.feed_name(i));
  }
  for (size_t i = 0; i < req.num_fetches(); ++i) {
    opts->fetch_endpoints.push_back(req.fetch_name(i));
  }
  for (size_t i = 0; i < req.num_targets(); ++i) {
    opts->target_nodes.push_back(req.target_name(i));
  }

  if (!req.options().debug_options().debug_tensor_watch_opts().empty()) {
    opts->debug_options = req.options().debug_options();
  }

  std::sort(opts->feed_endpoints.begin(), opts->feed_endpoints.end());
  std::sort(opts->target_nodes.begin(), opts->target_nodes.end());
  std::sort(opts->fetch_endpoints.begin(), opts->fetch_endpoints.end());
}

void BuildBuildGraphOptions(const PartialRunSetupRequest& req,
                            BuildGraphOptions* opts) {
  for (const auto& feed : req.feed()) {
    opts->feed_endpoints.push_back(feed);
  }
  for (const auto& fetch : req.fetch()) {
    opts->fetch_endpoints.push_back(fetch);
  }
  for (const auto& target : req.target()) {
    opts->target_nodes.push_back(target);
  }

  // TODO(cais): Add TFDBG support to partial runs.

  std::sort(opts->feed_endpoints.begin(), opts->feed_endpoints.end());
  std::sort(opts->target_nodes.begin(), opts->target_nodes.end());
  std::sort(opts->fetch_endpoints.begin(), opts->fetch_endpoints.end());
}

uint64 HashBuildGraphOptions(const BuildGraphOptions& opts) {
  uint64 h = 0x2b992ddfa23249d6ull;
  for (const string& name : opts.feed_endpoints) {
    h = Hash64(name.c_str(), name.size(), h);
  }
  for (const string& name : opts.target_nodes) {
    h = Hash64(name.c_str(), name.size(), h);
  }
  for (const string& name : opts.fetch_endpoints) {
    h = Hash64(name.c_str(), name.size(), h);
  }

  if (!opts.debug_options.debug_tensor_watch_opts().empty()) {
    const string watch_summary = SummarizeDebugTensorWatches(
        opts.debug_options.debug_tensor_watch_opts());
    h = Hash64(watch_summary.c_str(), watch_summary.size(), h);
  }

  return h;
}

string BuildGraphOptionsString(const BuildGraphOptions& opts) {
  string buf;
  for (const string& name : opts.feed_endpoints) {
    strings::StrAppend(&buf, " FdE: ", name);
  }
  strings::StrAppend(&buf, "\n");
  for (const string& name : opts.target_nodes) {
    strings::StrAppend(&buf, " TN: ", name);
  }
  strings::StrAppend(&buf, "\n");
  for (const string& name : opts.fetch_endpoints) {
    strings::StrAppend(&buf, " FeE: ", name);
  }
  strings::StrAppend(&buf, "\n");
  return buf;
}

MasterSession::MasterSession(
    const SessionOptions& opt, const MasterEnv* env,
    std::unique_ptr<std::vector<std::unique_ptr<Device>>> remote_devs,
    std::unique_ptr<WorkerCacheInterface> worker_cache,
    std::unique_ptr<DeviceSet> device_set,
    StatsPublisherFactory stats_publisher_factory)
    : session_opts_(opt),
      env_(env),
      handle_(strings::FpToString(random::New64())),
      remote_devs_(std::move(remote_devs)),
      worker_cache_(std::move(worker_cache)),
      devices_(std::move(device_set)),
      stats_publisher_factory_(std::move(stats_publisher_factory)),
      graph_version_(0),
      run_graphs_(5),
      partial_run_graphs_(5) {
  UpdateLastAccessTime();
  CHECK(devices_) << "device_set was null!";

  VLOG(1) << "Session " << handle_ << " #local " << env->local_devices.size()
          << " #remote " << remote_devs_->size();

  LOG(INFO) << "Start master session " << handle_
            << " with config: " << session_opts_.config.ShortDebugString();
}

MasterSession::~MasterSession() {
  for (const auto& iter : run_graphs_) iter.second->Unref();
  for (const auto& iter : partial_run_graphs_) iter.second->Unref();
}

void MasterSession::UpdateLastAccessTime() {
  last_access_time_usec_.store(Env::Default()->NowMicros());
}

Status MasterSession::Create(GraphDef* graph_def,
                             const WorkerCacheFactoryOptions& options) {
  if (session_opts_.config.use_per_session_threads() ||
      session_opts_.config.session_inter_op_thread_pool_size() > 0) {
    return errors::InvalidArgument(
        "Distributed session does not support session thread pool options.");
  }
  if (session_opts_.config.graph_options().place_pruned_graph()) {
    // TODO(b/29900832): Fix this or remove the option.
    LOG(WARNING) << "Distributed session does not support the "
                    "place_pruned_graph option.";
    session_opts_.config.mutable_graph_options()->set_place_pruned_graph(false);
  }

  GraphExecutionStateOptions execution_options;
  execution_options.device_set = devices_.get();
  execution_options.session_options = &session_opts_;
  {
    mutex_lock l(mu_);
    TF_RETURN_IF_ERROR(GraphExecutionState::MakeForBaseGraph(
        graph_def, execution_options, &execution_state_));
  }
  // TODO(b/36574172): Remove these conditions when ClusterSpec
  // propagation is supported in all servers.
  if (options.cluster_def != nullptr ||
      session_opts_.config.isolate_session_state()) {
    should_delete_worker_sessions_ = true;
    return CreateWorkerSessions(options);
  }
  return Status::OK();
}

Status MasterSession::CreateWorkerSessions(
    const WorkerCacheFactoryOptions& options) {
  std::vector<string> worker_names;
  WorkerCacheInterface* worker_cache = get_worker_cache();
  worker_cache->ListWorkers(&worker_names);

  struct WorkerGroup {
    // The worker name. (Not owned.)
    const string* name;

    // The worker referenced by name. (Not owned.)
    WorkerInterface* worker = nullptr;

    // Request and responses used for a given worker.
    CreateWorkerSessionRequest request;
    CreateWorkerSessionResponse response;
    Status status = Status::OK();
  };
  BlockingCounter done(worker_names.size());
  std::vector<WorkerGroup> workers(worker_names.size());

  // Release the workers.
  auto cleanup = gtl::MakeCleanup([this, &workers, worker_cache] {
    for (auto&& worker_group : workers) {
      if (worker_group.worker != nullptr) {
        worker_cache->ReleaseWorker(*worker_group.name, worker_group.worker);
      }
    }
  });

  Status status = Status::OK();
  // Create all the workers & kick off the computations.
  for (size_t i = 0; i < worker_names.size(); ++i) {
    workers[i].name = &worker_names[i];
    workers[i].worker = worker_cache->CreateWorker(worker_names[i]);
    workers[i].request.set_session_handle(handle_);
    if (options.cluster_def) {
      *workers[i].request.mutable_server_def()->mutable_cluster() =
          *options.cluster_def;
      workers[i].request.mutable_server_def()->set_protocol(*options.protocol);
      // Session state is always isolated when ClusterSpec propagation
      // is in use.
      workers[i].request.set_isolate_session_state(true);
    } else {
      workers[i].request.set_isolate_session_state(
          session_opts_.config.isolate_session_state());
    }

    DeviceNameUtils::ParsedName name;
    if (!DeviceNameUtils::ParseFullName(worker_names[i], &name)) {
      status = errors::Internal("Could not parse name ", worker_names[i]);
      LOG(WARNING) << status;
      return status;
    }
    if (!name.has_job || !name.has_task) {
      status = errors::Internal("Incomplete worker name ", worker_names[i]);
      LOG(WARNING) << status;
      return status;
    }

    workers[i].request.mutable_server_def()->set_job_name(name.job);
    workers[i].request.mutable_server_def()->set_task_index(name.task);
  }

  for (size_t i = 0; i < worker_names.size(); ++i) {
    auto cb = [i, &workers, &done](const Status& s) {
      workers[i].status = s;
      done.DecrementCount();
    };
    workers[i].worker->CreateWorkerSessionAsync(&workers[i].request,
                                                &workers[i].response, cb);
  }

  done.Wait();
  for (size_t i = 0; i < workers.size(); ++i) {
    status.Update(workers[i].status);
  }
  return status;
}

Status MasterSession::DeleteWorkerSessions() {
  WorkerCacheInterface* worker_cache = get_worker_cache();
  std::vector<string> worker_names;
  worker_cache->ListWorkers(&worker_names);

  struct WorkerGroup {
    // The worker name. (Not owned.)
    const string* name;

    // The worker referenced by name. (Not owned.)
    WorkerInterface* worker = nullptr;

    // Request and responses used for a given worker.
    DeleteWorkerSessionRequest request;
    DeleteWorkerSessionResponse response;
    Status status = Status::OK();
  };
  BlockingCounter done(worker_names.size());
  std::vector<WorkerGroup> workers(worker_names.size());

  // Release the workers.
  auto cleanup = gtl::MakeCleanup([this, &workers, worker_cache] {
    for (auto&& worker_group : workers) {
      if (worker_group.worker != nullptr) {
        worker_cache->ReleaseWorker(*worker_group.name, worker_group.worker);
      }
    }
  });

  Status status = Status::OK();
  // Create all the workers & kick off the computations.
  for (size_t i = 0; i < worker_names.size(); ++i) {
    workers[i].name = &worker_names[i];
    workers[i].worker = worker_cache->CreateWorker(worker_names[i]);
    workers[i].request.set_session_handle(handle_);
  }

  for (size_t i = 0; i < worker_names.size(); ++i) {
    auto cb = [i, &workers, &done](const Status& s) {
      workers[i].status = s;
      done.DecrementCount();
    };
    workers[i].worker->DeleteWorkerSessionAsync(&workers[i].request,
                                                &workers[i].response, cb);
  }

  done.Wait();
  for (size_t i = 0; i < workers.size(); ++i) {
    status.Update(workers[i].status);
  }
  return status;
}

Status MasterSession::ListDevices(ListDevicesResponse* resp) const {
  if (worker_cache_) {
    // This is a ClusterSpec-propagated session, and thus env_->local_devices
    // are invalid.

    // Mark the "client_device" as the sole local device.
    const Device* client_device = devices_->client_device();
    for (const Device* dev : devices_->devices()) {
      if (dev != client_device) {
        *(resp->add_remote_device()) = dev->attributes();
      }
    }
    *(resp->add_local_device()) = client_device->attributes();
  } else {
    for (Device* dev : env_->local_devices) {
      *(resp->add_local_device()) = dev->attributes();
    }
    for (auto&& dev : *remote_devs_) {
      *(resp->add_local_device()) = dev->attributes();
    }
  }
  return Status::OK();
}

Status MasterSession::Extend(const ExtendSessionRequest* req,
                             ExtendSessionResponse* resp) {
  UpdateLastAccessTime();
  std::unique_ptr<GraphExecutionState> extended_execution_state;
  {
    mutex_lock l(mu_);
    if (closed_) {
      return errors::FailedPrecondition("Session is closed.");
    }

    if (graph_version_ != req->current_graph_version()) {
      return errors::Aborted("Current version is ", graph_version_,
                             " but caller expected ",
                             req->current_graph_version(), ".");
    }

    CHECK(execution_state_);
    TF_RETURN_IF_ERROR(
        execution_state_->Extend(req->graph_def(), &extended_execution_state));

    CHECK(extended_execution_state);
    // The old execution state will be released outside the lock.
    execution_state_.swap(extended_execution_state);
    ++graph_version_;
    resp->set_new_graph_version(graph_version_);
  }
  return Status::OK();
}

WorkerCacheInterface* MasterSession::get_worker_cache() const {
  if (worker_cache_) {
    return worker_cache_.get();
  }
  return env_->worker_cache;
}

Status MasterSession::StartStep(const BuildGraphOptions& opts, int64* count,
                                ReffedClientGraph** rcg, bool is_partial) {
  const uint64 hash = HashBuildGraphOptions(opts);
  {
    mutex_lock l(mu_);
    // Keep track of how many times this subgraph has been executed in
    // this session.
    int64* c = &subgraph_execution_counts_[hash];
    *count = (*c)++;
    // TODO(suharshs): We cache partial run graphs and run graphs separately
    // because there is preprocessing that needs to only be run for partial
    // run calls.
    RCGMap* m = is_partial ? &partial_run_graphs_ : &run_graphs_;
    auto iter = m->find(hash);
    if (iter == m->end()) {
      // We have not seen this subgraph before. Build the subgraph and
      // cache it.
      VLOG(1) << "Unseen hash " << hash << " for "
              << BuildGraphOptionsString(opts) << " is_partial = " << is_partial
              << "\n";
      std::unique_ptr<ClientGraph> client_graph;
      TF_RETURN_IF_ERROR(execution_state_->BuildGraph(opts, &client_graph));
      WorkerCacheInterface* worker_cache = get_worker_cache();
      auto entry = new ReffedClientGraph(
          handle_, opts, std::move(client_graph), session_opts_,
          stats_publisher_factory_, execution_state_.get(), is_partial,
          worker_cache, !should_delete_worker_sessions_);
      iter = m->insert({hash, entry}).first;
      VLOG(1) << "Preparing to execute new graph";
    }
    *rcg = iter->second;
    (*rcg)->Ref();
  }
  return Status::OK();
}

void MasterSession::ClearRunsTable(std::vector<ReffedClientGraph*>* to_unref,
                                   RCGMap* rcg_map) {
  VLOG(1) << "Discarding all reffed graphs";
  for (auto p : *rcg_map) {
    ReffedClientGraph* rcg = p.second;
    if (to_unref) {
      to_unref->push_back(rcg);
    } else {
      rcg->Unref();
    }
  }
  rcg_map->clear();
}

Status MasterSession::PartialRunSetup(const PartialRunSetupRequest* req,
                                      PartialRunSetupResponse* resp) {
  std::vector<string> inputs, outputs, targets;
  for (const auto& feed : req->feed()) {
    inputs.push_back(feed);
  }
  for (const auto& fetch : req->fetch()) {
    outputs.push_back(fetch);
  }
  for (const auto& target : req->target()) {
    targets.push_back(target);
  }

  string handle = std::to_string(partial_run_handle_counter_.fetch_add(1));

  ReffedClientGraph* rcg = nullptr;
  int64 count = 0;

  // Prepare.
  BuildGraphOptions opts;
  BuildBuildGraphOptions(*req, &opts);
  TF_RETURN_IF_ERROR(StartStep(opts, &count, &rcg, true));
  // Keeps the highest 8 bits 0x01: we reserve some bits of the
  // step_id for future use.
  uint64 step_id = (random::New64() & ((1uLL << 56) - 1)) | (1uLL << 56);
  TRACEPRINTF("stepid %llu", step_id);

  rcg->Ref();
  RunState* run_state = new RunState(inputs, outputs, rcg, step_id, count);
  {
    mutex_lock l(mu_);
    partial_runs_.emplace(
        std::make_pair(handle, std::unique_ptr<RunState>(run_state)));
  }

  TF_RETURN_IF_ERROR(BuildAndRegisterPartitions(rcg));

  resp->set_partial_run_handle(handle);
  return Status::OK();
}

Status MasterSession::Run(CallOptions* opts, const RunStepRequestWrapper& req,
                          MutableRunStepResponseWrapper* resp) {
  UpdateLastAccessTime();
  {
    mutex_lock l(mu_);
    if (closed_) {
      return errors::FailedPrecondition("Session is closed.");
    }
    ++num_running_;
    // Note: all code paths must eventually call MarkRunCompletion()
    // in order to appropriate decrement the num_running_ counter.
  }
  Status status;
  if (!req.partial_run_handle().empty()) {
    status = DoPartialRun(opts, req, resp);
  } else {
    status = DoRunWithLocalExecution(opts, req, resp);
  }
  return status;
}

// Decrements num_running_ and broadcasts if num_running_ is zero.
void MasterSession::MarkRunCompletion() {
  mutex_lock l(mu_);
  --num_running_;
  if (num_running_ == 0) {
    num_running_is_zero_.notify_all();
  }
}

Status MasterSession::BuildAndRegisterPartitions(ReffedClientGraph* rcg) {
  // Registers subgraphs if haven't done so.
  PartitionOptions popts;
  popts.node_to_loc = SplitByWorker;
  // The closures potps.{new_name,get_incarnation} are called synchronously in
  // RegisterPartitions() below, so do not need a Ref()/Unref() pair to keep
  // "this" alive during the closure.
  popts.new_name = [this](const string& prefix) {
    mutex_lock l(mu_);
    return strings::StrCat(prefix, "_S", next_node_id_++);
  };
  popts.flib_def = rcg->client_graph()->flib_def.get();
  popts.get_incarnation = [this](const string& name) -> int64 {
    Device* d = devices_->FindDeviceByName(name);
    if (d == nullptr) {
      return PartitionOptions::kIllegalIncarnation;
    } else {
      return d->attributes().incarnation();
    }
  };
  popts.control_flow_added = false;
  const bool enable_bfloat16_sendrecv =
      session_opts_.config.graph_options().enable_bfloat16_sendrecv();
  popts.should_cast = [enable_bfloat16_sendrecv](const Edge* e) {
    if (e->IsControlEdge()) {
      return DT_FLOAT;
    }
    DataType dtype = BaseType(e->src()->output_type(e->src_output()));
    if (enable_bfloat16_sendrecv && dtype == DT_FLOAT) {
      return DT_BFLOAT16;
    } else {
      return dtype;
    }
  };
  if (session_opts_.config.graph_options().enable_recv_scheduling()) {
    popts.scheduling_for_recvs = true;
    popts.need_to_record_start_times = true;
  }

  TF_RETURN_IF_ERROR(rcg->RegisterPartitions(popts));

  return Status::OK();
}

Status MasterSession::DoPartialRun(CallOptions* opts,
                                   const RunStepRequestWrapper& req,
                                   MutableRunStepResponseWrapper* resp) {
  auto cleanup = gtl::MakeCleanup([this] { MarkRunCompletion(); });
  const string& prun_handle = req.partial_run_handle();
  RunState* run_state = nullptr;
  {
    mutex_lock l(mu_);
    auto it = partial_runs_.find(prun_handle);
    if (it == partial_runs_.end()) {
      return errors::InvalidArgument(
          "Must run PartialRunSetup before performing partial runs");
    }
    run_state = it->second.get();
  }

  // If this is the first partial run, initialize the PerStepState.
  if (!run_state->step_started) {
    run_state->step_started = true;
    PerStepState pss;

    const auto count = run_state->count;
    pss.collect_timeline =
        req.options().trace_level() == RunOptions::FULL_TRACE;
    pss.collect_rpcs =
            req.options().trace_level() == RunOptions::FULL_TRACE;
    pss.report_tensor_allocations_upon_oom =
        req.options().report_tensor_allocations_upon_oom();

    // Build the cost model every 'build_cost_model_every' steps after skipping
    // an
    // initial 'build_cost_model_after' steps.
    const int64 build_cost_model_after =
        session_opts_.config.graph_options().build_cost_model_after();
    const int64 build_cost_model_every =
        session_opts_.config.graph_options().build_cost_model();
    pss.collect_costs =
        build_cost_model_every > 0 &&
        ((count + 1 - build_cost_model_after) % build_cost_model_every == 0);
    pss.collect_partition_graphs = req.options().output_partition_graphs();

    std::unique_ptr<ProfileHandler> ph = run_state->rcg->GetProfileHandler(
        run_state->step_id, count, req.options());
    if (ph) {
      pss.collect_timeline = true;
      pss.collect_rpcs = ph->should_collect_rpcs();
    }

    run_state->pss = std::move(pss);
    run_state->ph = std::move(ph);
  }

  // Make sure that this is a new set of feeds that are still pending.
  for (size_t i = 0; i < req.num_feeds(); ++i) {
    const string& feed = req.feed_name(i);
    auto it = run_state->pending_inputs.find(feed);
    if (it == run_state->pending_inputs.end()) {
      return errors::InvalidArgument(
          "The feed ", feed, " was not specified in partial_run_setup.");
    } else if (it->second) {
      return errors::InvalidArgument("The feed ", feed,
                                     " has already been fed.");
    }
  }
  // Check that this is a new set of fetches that are still pending.
  for (size_t i = 0; i < req.num_fetches(); ++i) {
    const string& fetch = req.fetch_name(i);
    auto it = run_state->pending_outputs.find(fetch);
    if (it == run_state->pending_outputs.end()) {
      return errors::InvalidArgument(
          "The fetch ", fetch, " was not specified in partial_run_setup.");
    } else if (it->second) {
      return errors::InvalidArgument("The fetch ", fetch,
                                     " has already been fetched.");
    }
  }

  // Ensure that the requested fetches can be computed from the provided feeds.
  {
    mutex_lock l(mu_);
    TF_RETURN_IF_ERROR(
        run_state->rcg->CheckFetches(req, run_state, execution_state_.get()));
  }

  // Determine if this partial run satisfies all the pending inputs and outputs.
  for (size_t i = 0; i < req.num_feeds(); ++i) {
    auto it = run_state->pending_inputs.find(req.feed_name(i));
    it->second = true;
  }
  for (size_t i = 0; i < req.num_fetches(); ++i) {
    auto it = run_state->pending_outputs.find(req.fetch_name(i));
    it->second = true;
  }
  bool is_last_partial_run = run_state->PendingDone();

  Status s = run_state->rcg->RunPartitions(
      env_, run_state->step_id, run_state->count, &run_state->pss, opts, req,
      resp, &cancellation_manager_, is_last_partial_run);

  // Delete the run state if there is an error or all fetches are done.
  if (!s.ok() || is_last_partial_run) {
    ReffedClientGraph* rcg = run_state->rcg;
    run_state->pss.end_micros = Env::Default()->NowMicros();
    // Schedule post-processing and cleanup to be done asynchronously.
    Ref();
    rcg->Ref();
    rcg->ProcessStats(run_state->step_id, &run_state->pss, run_state->ph.get(),
                      req.options(), resp->mutable_metadata());
    cleanup.release();  // MarkRunCompletion called in done closure.
    rcg->CleanupPartitionsAsync(
        run_state->step_id, [this, rcg, prun_handle](const Status& s) {
          if (!s.ok()) {
            LOG(ERROR) << "Cleanup partition error: " << s;
          }
          rcg->Unref();
          MarkRunCompletion();
          Unref();
        });
    mutex_lock l(mu_);
    partial_runs_.erase(prun_handle);
  }
  return s;
}

Status MasterSession::CreateDebuggerState(
    const DebugOptions& debug_options, const RunStepRequestWrapper& req,
    int64 rcg_execution_count,
    std::unique_ptr<DebuggerStateInterface>* debugger_state) {
  TF_RETURN_IF_ERROR(
      DebuggerStateRegistry::CreateState(debug_options, debugger_state));

  std::vector<string> input_names;
  for (size_t i = 0; i < req.num_feeds(); ++i) {
    input_names.push_back(req.feed_name(i));
  }
  std::vector<string> output_names;
  for (size_t i = 0; i < req.num_fetches(); ++i) {
    output_names.push_back(req.fetch_name(i));
  }
  std::vector<string> target_names;
  for (size_t i = 0; i < req.num_targets(); ++i) {
    target_names.push_back(req.target_name(i));
  }

  // TODO(cais): We currently use -1 as a dummy value for session run count.
  // While this counter value is straightforward to define and obtain for
  // DirectSessions, it is less so for non-direct Sessions. Devise a better
  // way to get its value when the need arises.
  TF_RETURN_IF_ERROR(debugger_state->get()->PublishDebugMetadata(
      debug_options.global_step(), rcg_execution_count, rcg_execution_count,
      input_names, output_names, target_names));

  return Status::OK();
}

Status MasterSession::DoRunWithLocalExecution(
    CallOptions* opts, const RunStepRequestWrapper& req,
    MutableRunStepResponseWrapper* resp) {
  VLOG(2) << "DoRunWithLocalExecution req: " << req.DebugString();
  PerStepState pss;
  pss.start_micros = Env::Default()->NowMicros();
  auto cleanup = gtl::MakeCleanup([this] { MarkRunCompletion(); });

  // Prepare.
  BuildGraphOptions bgopts;
  BuildBuildGraphOptions(req, &bgopts);
  ReffedClientGraph* rcg = nullptr;
  int64 count = 0;
  TF_RETURN_IF_ERROR(StartStep(bgopts, &count, &rcg, false));

  // Unref "rcg" when out of scope.
  core::ScopedUnref unref(rcg);

  std::unique_ptr<DebuggerStateInterface> debugger_state;
  const DebugOptions& debug_options = req.options().debug_options();

  if (!debug_options.debug_tensor_watch_opts().empty()) {
    TF_RETURN_IF_ERROR(
        CreateDebuggerState(debug_options, req, count, &debugger_state));
  }
  TF_RETURN_IF_ERROR(BuildAndRegisterPartitions(rcg));

  // Keeps the highest 8 bits 0x01: we reserve some bits of the
  // step_id for future use.
  const uint64 step_id = (random::New64() & ((1uLL << 56) - 1)) | (1uLL << 56);
  TRACEPRINTF("stepid %llu", step_id);

  pss.collect_timeline = req.options().trace_level() == RunOptions::FULL_TRACE;
  pss.collect_rpcs =
          req.options().trace_level() == RunOptions::FULL_TRACE;
  pss.report_tensor_allocations_upon_oom =
      req.options().report_tensor_allocations_upon_oom();
  // Build the cost model every 'build_cost_model_every' steps after skipping an
  // initial 'build_cost_model_after' steps.
  const int64 build_cost_model_after =
      session_opts_.config.graph_options().build_cost_model_after();
  const int64 build_cost_model_every =
      session_opts_.config.graph_options().build_cost_model();
  pss.collect_costs =
      build_cost_model_every > 0 &&
      ((count + 1 - build_cost_model_after) % build_cost_model_every == 0);
  pss.collect_partition_graphs = req.options().output_partition_graphs();

  std::unique_ptr<ProfileHandler> ph =
      rcg->GetProfileHandler(step_id, count, req.options());
  if (ph) {
    pss.collect_timeline = true;
    pss.collect_rpcs = ph->should_collect_rpcs();
  }

  Status s = rcg->RunPartitions(env_, step_id, count, &pss, opts, req, resp,
                                &cancellation_manager_, false);
  if (s.ok()) {
    pss.end_micros = Env::Default()->NowMicros();

    // Schedule post-processing and cleanup to be done asynchronously.
    rcg->ProcessStats(step_id, &pss, ph.get(), req.options(),
                      resp->mutable_metadata());
  } else if (errors::IsCancelled(s)) {
    mutex_lock l(mu_);
    if (closed_) {
      if (garbage_collected_) {
        s = errors::Cancelled(
            "Step was cancelled because the session was garbage collected due "
            "to inactivity.");
      } else {
        s = errors::Cancelled(
            "Step was cancelled by an explicit call to `Session::Close()`.");
      }
    }
  }
  Ref();
  rcg->Ref();
  cleanup.release();  // MarkRunCompletion called in done closure.
  rcg->CleanupPartitionsAsync(step_id, [this, rcg](const Status& s) {
    if (!s.ok()) {
      LOG(ERROR) << "Cleanup partition error: " << s;
    }
    rcg->Unref();
    MarkRunCompletion();
    Unref();
  });
  return s;
}

Status MasterSession::Close() {
  {
    mutex_lock l(mu_);
    closed_ = true;  // All subsequent calls to Run() or Extend() will fail.
  }
  cancellation_manager_.StartCancel();
  std::vector<ReffedClientGraph*> to_unref;
  {
    mutex_lock l(mu_);
    while (num_running_ != 0) {
      num_running_is_zero_.wait(l);
    }
    ClearRunsTable(&to_unref, &run_graphs_);
    ClearRunsTable(&to_unref, &partial_run_graphs_);
  }
  for (ReffedClientGraph* rcg : to_unref) rcg->Unref();
  if (should_delete_worker_sessions_) {
    Status s = DeleteWorkerSessions();
    if (!s.ok()) {
      LOG(WARNING) << s;
    }
  }
  return Status::OK();
}

void MasterSession::GarbageCollect() {
  {
    mutex_lock l(mu_);
    closed_ = true;
    garbage_collected_ = true;
  }
  cancellation_manager_.StartCancel();
  Unref();
}

MasterSession::RunState::RunState(const std::vector<string>& input_names,
                                  const std::vector<string>& output_names,
                                  ReffedClientGraph* rcg, const uint64 step_id,
                                  const int64 count)
    : rcg(rcg), step_id(step_id), count(count) {
  // Initially all the feeds and fetches are pending.
  for (auto& name : input_names) {
    pending_inputs[name] = false;
  }
  for (auto& name : output_names) {
    pending_outputs[name] = false;
  }
}

MasterSession::RunState::~RunState() {
  if (rcg) rcg->Unref();
}

bool MasterSession::RunState::PendingDone() const {
  for (const auto& it : pending_inputs) {
    if (!it.second) return false;
  }
  for (const auto& it : pending_outputs) {
    if (!it.second) return false;
  }
  return true;
}

}  // end namespace tensorflow
