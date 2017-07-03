#include <atomic>
#include <deque>
#include <glog/logging.h>
#include <memory>
#include <type_traits>

#include "engine.h"
#include "engine_impl.h"
#include "swiftcpp/thread_utils.h"

namespace engine {

// ----------------------------------------------------------------------------
// Multi-thread Engine
// ----------------------------------------------------------------------------

struct ThreadedOperation : public Operation {
  Engine::AsyncFn fn;
  // Resources that require to read.
  std::vector<ResourceHandle> read_res;
  // Resources that require to write.
  std::vector<ResourceHandle> write_res;
  // Runing context.
  RunContext ctx;
  Engine *engine;
  // Some resources are ready.
  void TellResReady(int num = 1) {
    noready_resource_count_ -= num;
    DLOG(INFO) << "tell res ready :" << noready_resource_count_;
  }
  // Whether the operation is ready to run.
  bool ReadyToExecute() { return noready_resource_count_ == 0; }

  ThreadedOperation(Engine *engine, const Engine::AsyncFn &fn,
                    const std::vector<ResourceHandle> &read_res,
                    const std::vector<ResourceHandle> &write_res)
      : engine(engine), fn(fn), read_res(read_res), write_res(write_res),
        noready_resource_count_(read_res.size() + write_res.size()) {}

private:
  // Number of resources that is not ready for this operation.
  std::atomic<int> noready_resource_count_{0};
};

// A FIFO queue for a Resource, which records all the operation dependency.
class ThreadedResource : public Resource {
public:
  using Dispatcher = std::function<void(OperationHandle)>;

  ThreadedResource(const Dispatcher &dispatcher) : dispatcher_(dispatcher) {}
  // Append a read denpendency to the queue;
  void AppendDependency(OperationHandle opr, bool is_write);

  void FinishedDependency(OperationHandle opr, bool is_write);

protected:
  template void ProcessQueueFront();

private:
  struct ResourceBlock {
    OperationHandle operation;
    bool is_write{false};
    ResourceBlock(OperationHandle operation, bool is_write)
        : operation(operation), is_write(is_write) {}
  };

  std::deque<ResourceBlock> queue_;
  std::atomic<int> pending_read_count_{0};
  std::atomic<bool> pending_write_{false};
  std::mutex mut_;
  Dispatcher dispatcher_;
};

void ThreadedResource::AppendDependency(OperationHandle opr, bool is_write) {
  DLOG(INFO) << "append " << (is_write ? " write " : " read ") << " dependency";
  std::lock_guard<std::mutex> l(mut_);
  queue_.emplace_back(opr, is_write);

  ProcessQueueFront();
}

void ThreadedResource::FinishedDependency(OperationHandle opr, bool is_write) {
  if (is_write) {
    pending_write_ = false;
  } else {
    pending_read_count_--;
  }

  std::lock_guard<std::mutex> l(mut_);
  ProcessQueueFront();
}

// NOTE Not thread safe.
void ThreadedResource::ProcessQueueFront() {
  if (pending_read_count_ > 0 || pending_write_)
    return;
  if (queue_.empty())
    return;

  // front is wirte operation
  if (queue_.front().is_write) {
    if (pending_read_count_ > 0) {
      return;
    }
    // write dependency is ready.
    auto opr = queue_.front().operation;
    auto topr = opr->template Cast<ThreadedOperation>();
    topr->TellResReady();
    if (topr->ReadyToExecute()) {
      DLOG(INFO) << "to dispatch";
      // dispatch write operation
      dispatcher_(opr);
    }
    pending_write_ = true;
    queue_.pop_front();
    // read operation
  } else {
    while (!queue_.empty() && !queue_.front().is_write) {
      auto opr = queue_.front().operation->template Cast<ThreadedOperation>();
      opr->TellResReady();
      if (opr->ReadyToExecute()) {
        dispatcher_(queue_.front().operation);
      }
      queue_.pop_front();
    }
  }
}

class MultiThreadEngine : public Engine {
public:
  virtual void PushAsync(OperationHandle opr, RunContext ctx) override {
    num_pending_tasks_++;
    auto dispatcher = [this](OperationHandle opr) {
      DLOG(INFO) << "dispatch the opr";
      this->PushToExecute(opr);
    };
    auto topr = opr->Cast<ThreadedOperation>();
    topr->ctx = ctx;
    DLOG(INFO) << "register read resources";
    for (auto res : topr->read_res) {
      DLOG(INFO) << "res";
      CHECK(res);
      res->template Cast<ThreadedResource>()->AppendDependency(opr, false);
    }
    DLOG(INFO) << "register write resources";
    for (auto res : topr->write_res) {
      res->template Cast<ThreadedResource>()->AppendDependency(opr, true);
    }
  }

  virtual void PushSync(SyncFn fn, RunContext ctx,
                        const std::vector<ResourceHandle> &read_res,
                        const std::vector<ResourceHandle> &write_res) override {
    AsyncFn afn = [&](RunContext ctx, CallbackOnComplete cb) { fn(ctx); };
    auto opr = NewOperation(afn, read_res, write_res);
    PushAsync(opr, ctx);
  }

  // virtual ResourceHandle NewResource() override {
  //   return std::make_shared<ThreadedResource>();
  // }

  virtual OperationHandle
  NewOperation(AsyncFn fn, const std::vector<ResourceHandle> &read_res,
               const std::vector<ResourceHandle> &write_res) override {
    return std::make_shared<ThreadedOperation>(this, fn, read_res, write_res);
  }

  virtual void WaitForAllFinished() override {
    std::unique_lock<std::mutex> l(mut_);
    finish_cond_.wait(
        l, [this]() { return num_pending_tasks_ == 0 || terminated_; });
    DLOG(INFO) << "WaitForAllFinished done";
  }

  virtual void
  WaitForResource(const std::vector<ResourceHandle> &res) override {
    AsyncFn fn;
    // TODO
  }

  // Push the opr to device and execute it.
  virtual void PushToExecute(OperationHandle opr) = 0;

  static CallbackOnComplete CreateCompleteCallback(Engine *engine,
                                                   OperationHandle opr) {
    static CallbackOnComplete::Fn fn = [engine](OperationHandle opr) {
      auto ptr = opr->Cast<ThreadedOperation>();
      for (const auto &var : ptr->read_res) {
        var->Cast<ThreadedResource>()->FinishedDependency(opr, false);
      }
      for (const auto &var : ptr->write_res) {
        var->Cast<ThreadedResource>()->FinishedDependency(opr, false);
      }
      auto engine_ptr = static_cast<MultiThreadEngine *>(engine);
      engine_ptr->num_pending_tasks_--;
      engine_ptr->finish_cond_.notify_all();
    };
    return CallbackOnComplete(opr, &fn, engine);
  }

protected:
  // NOTE should be updated by Terminate method.
  // Whether the engine is terminated.
  std::atomic<bool> terminated_{false};
  // number of tasks in engine.
  // NOTE should be updated in CallbackOnComplted.
  std::atomic<int> num_pending_tasks_;
  // Condition variable used to determine whether all the tasks are finished.
  std::condition_variable finish_cond_;
  std::atomic<uint64_t> sync_counter_;
  std::mutex mut_;
};

// ----------------------------------------------------------------------------
// Multi-thread Engine With Thread Pools
// ----------------------------------------------------------------------------
template <typename OprType> struct ThreadQueueBlock {
  ThreadQueueBlock(int n_threads)
      : workers(n_threads, [this] {
          OperationHandle o;
          bool suc = task_queue.Pop(&o);
          if (!suc) {
            DLOG(WARNING) << "thread " << std::this_thread::get_id()
                          << " stop ...";
            return;
          }
          auto ptr = o->Cast<OprType>();
          auto engine = ptr->engine;
          auto complete_callback =
              MultiThreadEngine::CreateCompleteCallback(ptr->engine, o);
          CHECK(ptr->fn);
          ptr->fn(ptr->ctx, complete_callback);
        }) {}

  swiftcpp::thread::TaskQueue<OperationHandle> task_queue;
  swiftcpp::thread::ThreadPool workers;
};

class MultiThreadEnginePooled final : public MultiThreadEngine {
public:
  MultiThreadEnginePooled(int n_common_threads = 10, int n_io_threads = 1)
      : common_task_workers_(n_common_threads), io_task_workers_(n_io_threads) {
  }
  ~MultiThreadEnginePooled() { Terminate(); }
  virtual ResourceHandle NewResource() override;
  virtual void PushToExecute(OperationHandle opr) override;
  virtual void Terminate() override;

private:
  ThreadQueueBlock<ThreadedOperation> common_task_workers_;
  ThreadQueueBlock<ThreadedOperation> io_task_workers_;
};

ResourceHandle MultiThreadEnginePooled::NewResource() {
  ThreadedResource::Dispatcher dispatcher = [this](OperationHandle opr) {
    DLOG(INFO) << "dispatch using PushToExecute";
    PushToExecute(opr);
  };
  return std::make_shared<ThreadedResource>(dispatcher);
}

void MultiThreadEnginePooled::Terminate() {
  if (!terminated_) {
    DLOG(WARNING) << "MultiThreadEnginePooled terminated.";
    terminated_ = true;
    common_task_workers_.task_queue.SignalForKill();
    io_task_workers_.task_queue.SignalForKill();
  }
}

void MultiThreadEnginePooled::PushToExecute(OperationHandle opr) {
  auto ptr = opr->Cast<ThreadedOperation>();
  if (ptr->ctx.property == kCPU_GPU_Copy ||
      ptr->ctx.property == kGPU_CPU_Copy) {
    DLOG(INFO) << "io task queue get a task";
    io_task_workers_.task_queue.Push(opr);
  } else {
    DLOG(INFO) << "common task queue get a task";
    common_task_workers_.task_queue.Push(opr);
  }
}

std::shared_ptr<Engine> CreateEngine(const std::string &kind,
                                     EngineProperty prop) {
  if (kind == "DebugEngine") {
    return std::make_shared<DebugEngine>();
  } else if (kind == "MultiThreadEnginePooled") {
    return std::make_shared<MultiThreadEnginePooled>(
        prop.num_cpu_threads, prop.num_threads_gpu_copy_per_device);
  }
  return nullptr;
}

}; // namespace engine
