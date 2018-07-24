#include "node_internals.h"
#include "async_wrap.h"
#include "v8-profiler.h"
#include "node_buffer.h"
#include "node_platform.h"
#include "node_file.h"
#include "node_context_data.h"
#include "node_worker.h"
#include "tracing/agent.h"

#include <stdio.h>
#include <algorithm>

namespace node {

using v8::Context;
using v8::Function;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Message;
using v8::Number;
using v8::Private;
using v8::StackFrame;
using v8::StackTrace;
using v8::String;
using v8::Symbol;
using v8::TryCatch;
using v8::Value;
using worker::Worker;

int const Environment::kNodeContextTag = 0x6e6f64;
void* Environment::kNodeContextTagPtr = const_cast<void*>(
    static_cast<const void*>(&Environment::kNodeContextTag));

IsolateData::IsolateData(Isolate* isolate,
                         uv_loop_t* event_loop,
                         MultiIsolatePlatform* platform,
                         uint32_t* zero_fill_field) :
    isolate_(isolate),
    event_loop_(event_loop),
    zero_fill_field_(zero_fill_field),
    platform_(platform) {
  if (platform_ != nullptr)
    platform_->RegisterIsolate(isolate_, event_loop);

  // Create string and private symbol properties as internalized one byte
  // strings after the platform is properly initialized.
  //
  // Internalized because it makes property lookups a little faster and
  // because the string is created in the old space straight away.  It's going
  // to end up in the old space sooner or later anyway but now it doesn't go
  // through v8::Eternal's new space handling first.
  //
  // One byte because our strings are ASCII and we can safely skip V8's UTF-8
  // decoding step.

#define V(PropertyName, StringValue)                                        \
    PropertyName ## _.Set(                                                  \
        isolate,                                                            \
        Private::New(                                                       \
            isolate,                                                        \
            String::NewFromOneByte(                                         \
                isolate,                                                    \
                reinterpret_cast<const uint8_t*>(StringValue),              \
                v8::NewStringType::kInternalized,                           \
                sizeof(StringValue) - 1).ToLocalChecked()));
  PER_ISOLATE_PRIVATE_SYMBOL_PROPERTIES(V)
#undef V
#define V(PropertyName, StringValue)                                        \
    PropertyName ## _.Set(                                                  \
        isolate,                                                            \
        Symbol::New(                                                        \
            isolate,                                                        \
            String::NewFromOneByte(                                         \
                isolate,                                                    \
                reinterpret_cast<const uint8_t*>(StringValue),              \
                v8::NewStringType::kInternalized,                           \
                sizeof(StringValue) - 1).ToLocalChecked()));
  PER_ISOLATE_SYMBOL_PROPERTIES(V)
#undef V
#define V(PropertyName, StringValue)                                        \
    PropertyName ## _.Set(                                                  \
        isolate,                                                            \
        String::NewFromOneByte(                                             \
            isolate,                                                        \
            reinterpret_cast<const uint8_t*>(StringValue),                  \
            v8::NewStringType::kInternalized,                               \
            sizeof(StringValue) - 1).ToLocalChecked());
  PER_ISOLATE_STRING_PROPERTIES(V)
#undef V
}

IsolateData::~IsolateData() {
  if (platform_ != nullptr)
    platform_->UnregisterIsolate(isolate_);
}


void InitThreadLocalOnce() {
  CHECK_EQ(0, uv_key_create(&Environment::thread_local_env));
}

Environment::Environment(IsolateData* isolate_data,
                         Local<Context> context,
                         tracing::Agent* tracing_agent)
    : isolate_(context->GetIsolate()),
      isolate_data_(isolate_data),
      tracing_agent_(tracing_agent),
      immediate_info_(context->GetIsolate()),
      tick_info_(context->GetIsolate()),
      timer_base_(uv_now(isolate_data->event_loop())),
      printed_error_(false),
      trace_sync_io_(false),
      abort_on_uncaught_exception_(false),
      emit_env_nonstring_warning_(true),
      makecallback_cntr_(0),
      should_abort_on_uncaught_toggle_(isolate_, 1),
#if HAVE_INSPECTOR
      inspector_agent_(new inspector::Agent(this)),
#endif
      http_parser_buffer_(nullptr),
      fs_stats_field_array_(isolate_, kFsStatsFieldsLength * 2),
      fs_stats_field_bigint_array_(isolate_, kFsStatsFieldsLength * 2),
      context_(context->GetIsolate(), context) {
  // We'll be creating new objects so make sure we've entered the context.
  v8::HandleScope handle_scope(isolate());
  v8::Context::Scope context_scope(context);
  set_as_external(v8::External::New(isolate(), this));

  AssignToContext(context, ContextInfo(""));

  destroy_async_id_list_.reserve(512);
  performance_state_.reset(new performance::performance_state(isolate()));
  performance_state_->Mark(
      performance::NODE_PERFORMANCE_MILESTONE_ENVIRONMENT);
  performance_state_->Mark(
      performance::NODE_PERFORMANCE_MILESTONE_NODE_START,
      performance::performance_node_start);
  performance_state_->Mark(
      performance::NODE_PERFORMANCE_MILESTONE_V8_START,
      performance::performance_v8_start);

  // By default, always abort when --abort-on-uncaught-exception was passed.
  should_abort_on_uncaught_toggle_[0] = 1;

  std::string debug_cats;
  SafeGetenv("NODE_DEBUG_NATIVE", &debug_cats);
  set_debug_categories(debug_cats, true);

  isolate()->GetHeapProfiler()->AddBuildEmbedderGraphCallback(
      BuildEmbedderGraph, this);
}

Environment::~Environment() {
  isolate()->GetHeapProfiler()->RemoveBuildEmbedderGraphCallback(
      BuildEmbedderGraph, this);

  // Make sure there are no re-used libuv wrapper objects.
  // CleanupHandles() should have removed all of them.
  CHECK(file_handle_read_wrap_freelist_.empty());

  v8::HandleScope handle_scope(isolate());

#if HAVE_INSPECTOR
  // Destroy inspector agent before erasing the context. The inspector
  // destructor depends on the context still being accessible.
  inspector_agent_.reset();
#endif

  context()->SetAlignedPointerInEmbedderData(
      ContextEmbedderIndex::kEnvironment, nullptr);

  delete[] heap_statistics_buffer_;
  delete[] heap_space_statistics_buffer_;
  delete[] http_parser_buffer_;
}

void Environment::Start(int argc,
                        const char* const* argv,
                        int exec_argc,
                        const char* const* exec_argv,
                        bool start_profiler_idle_notifier) {
  HandleScope handle_scope(isolate());
  Context::Scope context_scope(context());

  CHECK_EQ(0, uv_timer_init(event_loop(), timer_handle()));
  uv_unref(reinterpret_cast<uv_handle_t*>(timer_handle()));

  uv_check_init(event_loop(), immediate_check_handle());
  uv_unref(reinterpret_cast<uv_handle_t*>(immediate_check_handle()));

  uv_idle_init(event_loop(), immediate_idle_handle());

  uv_check_start(immediate_check_handle(), CheckImmediate);

  // Inform V8's CPU profiler when we're idle.  The profiler is sampling-based
  // but not all samples are created equal; mark the wall clock time spent in
  // epoll_wait() and friends so profiling tools can filter it out.  The samples
  // still end up in v8.log but with state=IDLE rather than state=EXTERNAL.
  // TODO(bnoordhuis) Depends on a libuv implementation detail that we should
  // probably fortify in the API contract, namely that the last started prepare
  // or check watcher runs first.  It's not 100% foolproof; if an add-on starts
  // a prepare or check watcher after us, any samples attributed to its callback
  // will be recorded with state=IDLE.
  uv_prepare_init(event_loop(), &idle_prepare_handle_);
  uv_check_init(event_loop(), &idle_check_handle_);
  uv_unref(reinterpret_cast<uv_handle_t*>(&idle_prepare_handle_));
  uv_unref(reinterpret_cast<uv_handle_t*>(&idle_check_handle_));

  // Register clean-up cb to be called to clean up the handles
  // when the environment is freed, note that they are not cleaned in
  // the one environment per process setup, but will be called in
  // FreeEnvironment.
  RegisterHandleCleanups();

  if (start_profiler_idle_notifier) {
    StartProfilerIdleNotifier();
  }

  auto process_template = FunctionTemplate::New(isolate());
  process_template->SetClassName(FIXED_ONE_BYTE_STRING(isolate(), "process"));

  auto process_object =
      process_template->GetFunction()->NewInstance(context()).ToLocalChecked();
  set_process_object(process_object);

  SetupProcessObject(this, argc, argv, exec_argc, exec_argv);

  static uv_once_t init_once = UV_ONCE_INIT;
  uv_once(&init_once, InitThreadLocalOnce);
  uv_key_set(&thread_local_env, this);
}

void Environment::RegisterHandleCleanups() {
  HandleCleanupCb close_and_finish = [](Environment* env, uv_handle_t* handle,
                                        void* arg) {
    handle->data = env;

    env->CloseHandle(handle, [](uv_handle_t* handle) {});
  };

  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(timer_handle()),
      close_and_finish,
      nullptr);
  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(immediate_check_handle()),
      close_and_finish,
      nullptr);
  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(immediate_idle_handle()),
      close_and_finish,
      nullptr);
  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(&idle_prepare_handle_),
      close_and_finish,
      nullptr);
  RegisterHandleCleanup(
      reinterpret_cast<uv_handle_t*>(&idle_check_handle_),
      close_and_finish,
      nullptr);
}

void Environment::CleanupHandles() {
  for (ReqWrap<uv_req_t>* request : req_wrap_queue_)
    request->Cancel();

  for (HandleWrap* handle : handle_wrap_queue_)
    handle->Close();

  for (HandleCleanup& hc : handle_cleanup_queue_)
    hc.cb_(this, hc.handle_, hc.arg_);
  handle_cleanup_queue_.clear();

  while (handle_cleanup_waiting_ != 0 ||
         request_waiting_ != 0 ||
         !handle_wrap_queue_.IsEmpty()) {
    uv_run(event_loop(), UV_RUN_ONCE);
  }

  file_handle_read_wrap_freelist_.clear();
}

void Environment::StartProfilerIdleNotifier() {
  if (profiler_idle_notifier_started_)
    return;

  profiler_idle_notifier_started_ = true;

  uv_prepare_start(&idle_prepare_handle_, [](uv_prepare_t* handle) {
    Environment* env = ContainerOf(&Environment::idle_prepare_handle_, handle);
    env->isolate()->SetIdle(true);
  });

  uv_check_start(&idle_check_handle_, [](uv_check_t* handle) {
    Environment* env = ContainerOf(&Environment::idle_check_handle_, handle);
    env->isolate()->SetIdle(false);
  });
}

void Environment::StopProfilerIdleNotifier() {
  profiler_idle_notifier_started_ = false;
  uv_prepare_stop(&idle_prepare_handle_);
  uv_check_stop(&idle_check_handle_);
}

void Environment::PrintSyncTrace() const {
  if (!trace_sync_io_)
    return;

  HandleScope handle_scope(isolate());
  Local<v8::StackTrace> stack =
      StackTrace::CurrentStackTrace(isolate(), 10, StackTrace::kDetailed);

  fprintf(stderr, "(node:%d) WARNING: Detected use of sync API\n",
          uv_os_getpid());

  for (int i = 0; i < stack->GetFrameCount() - 1; i++) {
    Local<StackFrame> stack_frame = stack->GetFrame(i);
    node::Utf8Value fn_name_s(isolate(), stack_frame->GetFunctionName());
    node::Utf8Value script_name(isolate(), stack_frame->GetScriptName());
    const int line_number = stack_frame->GetLineNumber();
    const int column = stack_frame->GetColumn();

    if (stack_frame->IsEval()) {
      if (stack_frame->GetScriptId() == Message::kNoScriptIdInfo) {
        fprintf(stderr, "    at [eval]:%i:%i\n", line_number, column);
      } else {
        fprintf(stderr,
                "    at [eval] (%s:%i:%i)\n",
                *script_name,
                line_number,
                column);
      }
      break;
    }

    if (fn_name_s.length() == 0) {
      fprintf(stderr, "    at %s:%i:%i\n", *script_name, line_number, column);
    } else {
      fprintf(stderr,
              "    at %s (%s:%i:%i)\n",
              *fn_name_s,
              *script_name,
              line_number,
              column);
    }
  }
  fflush(stderr);
}

void Environment::RunCleanup() {
  CleanupHandles();

  while (!cleanup_hooks_.empty()) {
    // Copy into a vector, since we can't sort an unordered_set in-place.
    std::vector<CleanupHookCallback> callbacks(
        cleanup_hooks_.begin(), cleanup_hooks_.end());
    // We can't erase the copied elements from `cleanup_hooks_` yet, because we
    // need to be able to check whether they were un-scheduled by another hook.

    std::sort(callbacks.begin(), callbacks.end(),
              [](const CleanupHookCallback& a, const CleanupHookCallback& b) {
      // Sort in descending order so that the most recently inserted callbacks
      // are run first.
      return a.insertion_order_counter_ > b.insertion_order_counter_;
    });

    for (const CleanupHookCallback& cb : callbacks) {
      if (cleanup_hooks_.count(cb) == 0) {
        // This hook was removed from the `cleanup_hooks_` set during another
        // hook that was run earlier. Nothing to do here.
        continue;
      }

      cb.fn_(cb.arg_);
      cleanup_hooks_.erase(cb);
    }
    CleanupHandles();
  }
}

void Environment::RunBeforeExitCallbacks() {
  for (ExitCallback before_exit : before_exit_functions_) {
    before_exit.cb_(before_exit.arg_);
  }
  before_exit_functions_.clear();
}

void Environment::BeforeExit(void (*cb)(void* arg), void* arg) {
  before_exit_functions_.push_back(ExitCallback{cb, arg});
}

void Environment::RunAtExitCallbacks() {
  for (ExitCallback at_exit : at_exit_functions_) {
    at_exit.cb_(at_exit.arg_);
  }
  at_exit_functions_.clear();
}

void Environment::AtExit(void (*cb)(void* arg), void* arg) {
  at_exit_functions_.push_back(ExitCallback{cb, arg});
}

void Environment::AddPromiseHook(promise_hook_func fn, void* arg) {
  auto it = std::find_if(
      promise_hooks_.begin(), promise_hooks_.end(),
      [&](const PromiseHookCallback& hook) {
        return hook.cb_ == fn && hook.arg_ == arg;
      });
  if (it != promise_hooks_.end()) {
    it->enable_count_++;
    return;
  }
  promise_hooks_.push_back(PromiseHookCallback{fn, arg, 1});

  if (promise_hooks_.size() == 1) {
    isolate_->SetPromiseHook(EnvPromiseHook);
  }
}

bool Environment::RemovePromiseHook(promise_hook_func fn, void* arg) {
  auto it = std::find_if(
      promise_hooks_.begin(), promise_hooks_.end(),
      [&](const PromiseHookCallback& hook) {
        return hook.cb_ == fn && hook.arg_ == arg;
      });

  if (it == promise_hooks_.end()) return false;

  if (--it->enable_count_ > 0) return true;

  promise_hooks_.erase(it);
  if (promise_hooks_.empty()) {
    isolate_->SetPromiseHook(nullptr);
  }

  return true;
}

void Environment::EnvPromiseHook(v8::PromiseHookType type,
                                 v8::Local<v8::Promise> promise,
                                 v8::Local<v8::Value> parent) {
  Local<v8::Context> context = promise->CreationContext();

  // Grow the embedder data if necessary to make sure we are not out of bounds
  // when reading the magic number.
  context->SetAlignedPointerInEmbedderData(
      ContextEmbedderIndex::kContextTagBoundary, nullptr);
  int* magicNumberPtr = reinterpret_cast<int*>(
      context->GetAlignedPointerFromEmbedderData(
          ContextEmbedderIndex::kContextTag));
  if (magicNumberPtr != Environment::kNodeContextTagPtr) {
    return;
  }

  Environment* env = Environment::GetCurrent(context);
  for (const PromiseHookCallback& hook : env->promise_hooks_) {
    hook.cb_(type, promise, parent, hook.arg_);
  }
}

void Environment::RunAndClearNativeImmediates() {
  size_t count = native_immediate_callbacks_.size();
  if (count > 0) {
    size_t ref_count = 0;
    std::vector<NativeImmediateCallback> list;
    native_immediate_callbacks_.swap(list);
    auto drain_list = [&]() {
      v8::TryCatch try_catch(isolate());
      for (auto it = list.begin(); it != list.end(); ++it) {
#ifdef DEBUG
        v8::SealHandleScope seal_handle_scope(isolate());
#endif
        it->cb_(this, it->data_);
        if (it->refed_)
          ref_count++;
        if (UNLIKELY(try_catch.HasCaught())) {
          if (!try_catch.HasTerminated())
            FatalException(isolate(), try_catch);

          // Bail out, remove the already executed callbacks from list
          // and set up a new TryCatch for the other pending callbacks.
          std::move_backward(it, list.end(), list.begin() + (list.end() - it));
          list.resize(list.end() - it);
          return true;
        }
      }
      return false;
    };
    while (drain_list()) {}

#ifdef DEBUG
    CHECK_GE(immediate_info()->count(), count);
#endif
    immediate_info()->count_dec(count);
    immediate_info()->ref_count_dec(ref_count);
  }
}


void Environment::ScheduleTimer(int64_t duration_ms) {
  uv_timer_start(timer_handle(), RunTimers, duration_ms, 0);
}

void Environment::ToggleTimerRef(bool ref) {
  if (ref) {
    uv_ref(reinterpret_cast<uv_handle_t*>(timer_handle()));
  } else {
    uv_unref(reinterpret_cast<uv_handle_t*>(timer_handle()));
  }
}

void Environment::RunTimers(uv_timer_t* handle) {
  Environment* env = Environment::from_timer_handle(handle);

  if (!env->can_call_into_js())
    return;

  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  Local<Object> process = env->process_object();
  InternalCallbackScope scope(env, process, {0, 0});

  Local<Function> cb = env->timers_callback_function();
  MaybeLocal<Value> ret;
  Local<Value> arg = env->GetNow();
  // This code will loop until all currently due timers will process. It is
  // impossible for us to end up in an infinite loop due to how the JS-side
  // is structured.
  do {
    TryCatch try_catch(env->isolate());
    try_catch.SetVerbose(true);
    ret = cb->Call(env->context(), process, 1, &arg);
  } while (ret.IsEmpty() && env->can_call_into_js());

  // NOTE(apapirovski): If it ever becomes possibble that `call_into_js` above
  // is reset back to `true` after being previously set to `false` then this
  // code becomes invalid and needs to be rewritten. Otherwise catastrophic
  // timers corruption will occurr and all timers behaviour will become
  // entirely unpredictable.
  if (ret.IsEmpty())
    return;

  // To allow for less JS-C++ boundary crossing, the value returned from JS
  // serves a few purposes:
  // 1. If it's 0, no more timers exist and the handle should be unrefed
  // 2. If it's > 0, the value represents the next timer's expiry and there
  //    is at least one timer remaining that is refed.
  // 3. If it's < 0, the absolute value represents the next timer's expiry
  //    and there are no timers that are refed.
  int64_t expiry_ms =
      ret.ToLocalChecked()->IntegerValue(env->context()).FromJust();

  uv_handle_t* h = reinterpret_cast<uv_handle_t*>(handle);

  if (expiry_ms != 0) {
    int64_t duration_ms =
        llabs(expiry_ms) - (uv_now(env->event_loop()) - env->timer_base());

    env->ScheduleTimer(duration_ms > 0 ? duration_ms : 1);

    if (expiry_ms > 0)
      uv_ref(h);
    else
      uv_unref(h);
  } else {
    uv_unref(h);
  }
}


void Environment::CheckImmediate(uv_check_t* handle) {
  Environment* env = Environment::from_immediate_check_handle(handle);

  if (env->immediate_info()->count() == 0)
    return;

  HandleScope scope(env->isolate());
  Context::Scope context_scope(env->context());

  env->RunAndClearNativeImmediates();

  if (!env->can_call_into_js())
    return;

  do {
    MakeCallback(env->isolate(),
                 env->process_object(),
                 env->immediate_callback_function(),
                 0,
                 nullptr,
                 {0, 0}).ToLocalChecked();
  } while (env->immediate_info()->has_outstanding() && env->can_call_into_js());

  if (env->immediate_info()->ref_count() == 0)
    env->ToggleImmediateRef(false);
}

void Environment::ToggleImmediateRef(bool ref) {
  if (ref) {
    // Idle handle is needed only to stop the event loop from blocking in poll.
    uv_idle_start(immediate_idle_handle(), [](uv_idle_t*){ });
  } else {
    uv_idle_stop(immediate_idle_handle());
  }
}


Local<Value> Environment::GetNow() {
  uv_update_time(event_loop());
  uint64_t now = uv_now(event_loop());
  CHECK_GE(now, timer_base());
  now -= timer_base();
  if (now <= 0xffffffff)
    return Integer::New(isolate(), static_cast<uint32_t>(now));
  else
    return Number::New(isolate(), static_cast<double>(now));
}


void Environment::set_debug_categories(const std::string& cats, bool enabled) {
  std::string debug_categories = cats;
  while (!debug_categories.empty()) {
    std::string::size_type comma_pos = debug_categories.find(',');
    std::string wanted = ToLower(debug_categories.substr(0, comma_pos));

#define V(name)                                                          \
    {                                                                    \
      static const std::string available_category = ToLower(#name);      \
      if (available_category.find(wanted) != std::string::npos)          \
        set_debug_enabled(DebugCategory::name, enabled);                 \
    }

    DEBUG_CATEGORY_NAMES(V)

    if (comma_pos == std::string::npos)
      break;
    // Use everything after the `,` as the list for the next iteration.
    debug_categories = debug_categories.substr(comma_pos + 1);
  }
}

void CollectExceptionInfo(Environment* env,
                          v8::Local<v8::Object> obj,
                          int errorno,
                          const char* err_string,
                          const char* syscall,
                          const char* message,
                          const char* path,
                          const char* dest) {
  obj->Set(env->errno_string(), v8::Integer::New(env->isolate(), errorno));

  obj->Set(env->context(), env->code_string(),
           OneByteString(env->isolate(), err_string)).FromJust();

  if (message != nullptr) {
    obj->Set(env->context(), env->message_string(),
             OneByteString(env->isolate(), message)).FromJust();
  }

  v8::Local<v8::Value> path_buffer;
  if (path != nullptr) {
    path_buffer =
      Buffer::Copy(env->isolate(), path, strlen(path)).ToLocalChecked();
    obj->Set(env->context(), env->path_string(), path_buffer).FromJust();
  }

  v8::Local<v8::Value> dest_buffer;
  if (dest != nullptr) {
    dest_buffer =
      Buffer::Copy(env->isolate(), dest, strlen(dest)).ToLocalChecked();
    obj->Set(env->context(), env->dest_string(), dest_buffer).FromJust();
  }

  if (syscall != nullptr) {
    obj->Set(env->context(), env->syscall_string(),
             OneByteString(env->isolate(), syscall)).FromJust();
  }
}

void Environment::CollectExceptionInfo(v8::Local<v8::Value> object,
                                       int errorno,
                                       const char* syscall,
                                       const char* message,
                                       const char* path) {
  if (!object->IsObject() || errorno == 0)
    return;

  v8::Local<v8::Object> obj = object.As<v8::Object>();
  const char* err_string = node::errno_string(errorno);

  if (message == nullptr || message[0] == '\0') {
    message = strerror(errorno);
  }

  node::CollectExceptionInfo(this, obj, errorno, err_string,
                             syscall, message, path, nullptr);
}

void Environment::CollectUVExceptionInfo(v8::Local<v8::Value> object,
                                         int errorno,
                                         const char* syscall,
                                         const char* message,
                                         const char* path,
                                         const char* dest) {
  if (!object->IsObject() || errorno == 0)
    return;

  v8::Local<v8::Object> obj = object.As<v8::Object>();
  const char* err_string = uv_err_name(errorno);

  if (message == nullptr || message[0] == '\0') {
    message = uv_strerror(errorno);
  }

  node::CollectExceptionInfo(this, obj, errorno, err_string,
                             syscall, message, path, dest);
}


void Environment::AsyncHooks::grow_async_ids_stack() {
  const uint32_t old_capacity = async_ids_stack_.Length() / 2;
  const uint32_t new_capacity = old_capacity * 1.5;
  AliasedBuffer<double, v8::Float64Array> new_buffer(
      env()->isolate(), new_capacity * 2);

  for (uint32_t i = 0; i < old_capacity * 2; ++i)
    new_buffer[i] = async_ids_stack_[i];
  async_ids_stack_ = std::move(new_buffer);

  env()->async_hooks_binding()->Set(
      env()->context(),
      env()->async_ids_stack_string(),
      async_ids_stack_.GetJSArray()).FromJust();
}

uv_key_t Environment::thread_local_env = {};

void Environment::Exit(int exit_code) {
  if (is_main_thread())
    exit(exit_code);
  else
    worker_context_->Exit(exit_code);
}

void Environment::stop_sub_worker_contexts() {
  while (!sub_worker_contexts_.empty()) {
    Worker* w = *sub_worker_contexts_.begin();
    remove_sub_worker_context(w);
    w->Exit(1);
    w->JoinThread();
  }
}

void Environment::BuildEmbedderGraph(v8::Isolate* isolate,
                                     v8::EmbedderGraph* graph,
                                     void* data) {
  MemoryTracker tracker(isolate, graph);
  static_cast<Environment*>(data)->ForEachBaseObject([&](BaseObject* obj) {
    tracker.Track(obj);
  });
}


// Not really any better place than env.cc at this moment.
void BaseObject::DeleteMe(void* data) {
  BaseObject* self = static_cast<BaseObject*>(data);
  delete self;
}

Local<Object> BaseObject::WrappedObject() const {
  return object();
}

bool BaseObject::IsRootNode() const {
  return !persistent_handle_.IsWeak();
}

}  // namespace node
