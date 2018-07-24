#ifndef SRC_NODE_PLATFORM_H_
#define SRC_NODE_PLATFORM_H_

#include <queue>
#include <unordered_map>
#include <vector>
#include <functional>

#include "libplatform/libplatform.h"
#include "node.h"
#include "node_mutex.h"
#include "uv.h"

namespace node {

class NodePlatform;
class IsolateData;
class PerIsolatePlatformData;

template <class T>
class TaskQueue {
 public:
  TaskQueue();
  ~TaskQueue() {}

  void Push(std::unique_ptr<T> task);
  std::unique_ptr<T> Pop();
  std::unique_ptr<T> BlockingPop();
  std::queue<std::unique_ptr<T>> PopAll();
  void NotifyOfCompletion();
  void BlockingDrain();
  void Stop();

 private:
  Mutex lock_;
  ConditionVariable tasks_available_;
  ConditionVariable tasks_drained_;
  int outstanding_tasks_;
  bool stopped_;
  std::queue<std::unique_ptr<T>> task_queue_;
};

struct DelayedTask {
  std::unique_ptr<v8::Task> task;
  uv_timer_t timer;
  double timeout;
  std::shared_ptr<PerIsolatePlatformData> platform_data;
};

// This acts as the foreground task runner for a given Isolate.
class PerIsolatePlatformData :
    public v8::TaskRunner,
    public std::enable_shared_from_this<PerIsolatePlatformData> {
 public:
  PerIsolatePlatformData(v8::Isolate* isolate, uv_loop_t* loop);
  ~PerIsolatePlatformData();

  void PostTask(std::unique_ptr<v8::Task> task) override;
  void PostIdleTask(std::unique_ptr<v8::IdleTask> task) override;
  void PostDelayedTask(std::unique_ptr<v8::Task> task,
                       double delay_in_seconds) override;
  bool IdleTasksEnabled() override { return false; };

  void Shutdown();

  void ref();
  int unref();

  // Returns true if work was dispatched or executed. New tasks that are
  // posted during flushing of the queue are postponed until the next
  // flushing.
  bool FlushForegroundTasksInternal();
  void CancelPendingDelayedTasks();

  const uv_loop_t* event_loop() const { return loop_; }

 private:
  void DeleteFromScheduledTasks(DelayedTask* task);

  static void FlushTasks(uv_async_t* handle);
  static void RunForegroundTask(std::unique_ptr<v8::Task> task);
  static void RunForegroundTask(uv_timer_t* timer);

  int ref_count_ = 1;
  uv_loop_t* const loop_;
  uv_async_t* flush_tasks_ = nullptr;
  TaskQueue<v8::Task> foreground_tasks_;
  TaskQueue<DelayedTask> foreground_delayed_tasks_;

  // Use a custom deleter because libuv needs to close the handle first.
  typedef std::unique_ptr<DelayedTask, std::function<void(DelayedTask*)>>
      DelayedTaskPointer;
  std::vector<DelayedTaskPointer> scheduled_delayed_tasks_;
};

// This acts as the single worker thread task runner for all Isolates.
class WorkerThreadsTaskRunner {
 public:
  explicit WorkerThreadsTaskRunner(int thread_pool_size);

  void PostTask(std::unique_ptr<v8::Task> task);
  void PostDelayedTask(std::unique_ptr<v8::Task> task,
                       double delay_in_seconds);

  void BlockingDrain();
  void Shutdown();

  int NumberOfWorkerThreads() const;

 private:
  TaskQueue<v8::Task> pending_worker_tasks_;
  std::vector<std::unique_ptr<uv_thread_t>> threads_;
};

class NodePlatform : public MultiIsolatePlatform {
 public:
  NodePlatform(int thread_pool_size, v8::TracingController* tracing_controller);
  virtual ~NodePlatform() {}

  void DrainTasks(v8::Isolate* isolate) override;
  void CancelPendingDelayedTasks(v8::Isolate* isolate) override;
  void Shutdown();

  // v8::Platform implementation.
  int NumberOfWorkerThreads() override;
  void CallOnWorkerThread(std::unique_ptr<v8::Task> task) override;
  void CallDelayedOnWorkerThread(std::unique_ptr<v8::Task> task,
                                 double delay_in_seconds) override;
  void CallOnForegroundThread(v8::Isolate* isolate, v8::Task* task) override;
  void CallDelayedOnForegroundThread(v8::Isolate* isolate, v8::Task* task,
                                     double delay_in_seconds) override;
  bool IdleTasksEnabled(v8::Isolate* isolate) override;
  double MonotonicallyIncreasingTime() override;
  double CurrentClockTimeMillis() override;
  v8::TracingController* GetTracingController() override;
  bool FlushForegroundTasks(v8::Isolate* isolate) override;

  void RegisterIsolate(IsolateData* isolate_data, uv_loop_t* loop) override;
  void UnregisterIsolate(IsolateData* isolate_data) override;

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate) override;

 private:
  std::shared_ptr<PerIsolatePlatformData> ForIsolate(v8::Isolate* isolate);

  Mutex per_isolate_mutex_;
  std::unordered_map<v8::Isolate*,
                     std::shared_ptr<PerIsolatePlatformData>> per_isolate_;

  std::unique_ptr<v8::TracingController> tracing_controller_;
  std::shared_ptr<WorkerThreadsTaskRunner> worker_thread_task_runner_;
};

}  // namespace node

#endif  // SRC_NODE_PLATFORM_H_
