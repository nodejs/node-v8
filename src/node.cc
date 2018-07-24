// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "node_buffer.h"
#include "node_constants.h"
#include "node_javascript.h"
#include "node_code_cache.h"
#include "node_platform.h"
#include "node_version.h"
#include "node_internals.h"
#include "node_revert.h"
#include "node_debug_options.h"
#include "node_perf.h"
#include "node_context_data.h"
#include "tracing/traced_value.h"

#if defined HAVE_PERFCTR
#include "node_counters.h"
#endif

#if HAVE_OPENSSL
#include "node_crypto.h"
#endif

#if defined(NODE_HAVE_I18N_SUPPORT)
#include "node_i18n.h"
#endif

#if HAVE_INSPECTOR
#include "inspector_io.h"
#endif

#if defined HAVE_DTRACE || defined HAVE_ETW
#include "node_dtrace.h"
#endif

#include "ares.h"
#include "async_wrap-inl.h"
#include "env-inl.h"
#include "handle_wrap.h"
#include "http_parser.h"
#include "nghttp2/nghttp2ver.h"
#include "req_wrap-inl.h"
#include "string_bytes.h"
#include "tracing/agent.h"
#include "util.h"
#include "uv.h"
#if NODE_USE_V8_PLATFORM
#include "libplatform/libplatform.h"
#endif  // NODE_USE_V8_PLATFORM
#include "v8-profiler.h"
#include "zlib.h"

#ifdef NODE_ENABLE_VTUNE_PROFILING
#include "../deps/v8/src/third_party/vtune/v8-vtune.h"
#endif

#include <errno.h>
#include <fcntl.h>  // _O_RDWR
#include <limits.h>  // PATH_MAX
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <string>
#include <vector>

#if defined(NODE_HAVE_I18N_SUPPORT)
#include <unicode/uvernum.h>
#endif

#if defined(LEAK_SANITIZER)
#include <sanitizer/lsan_interface.h>
#endif

#if defined(_MSC_VER)
#include <direct.h>
#include <io.h>
#define umask _umask
typedef int mode_t;
#else
#include <pthread.h>
#include <sys/resource.h>  // getrlimit, setrlimit
#include <unistd.h>  // setuid, getuid
#endif

#if defined(__POSIX__) && !defined(__ANDROID__) && !defined(__CloudABI__)
#include <pwd.h>  // getpwnam()
#include <grp.h>  // getgrnam()
#endif

#if defined(__POSIX__)
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#elif !defined(_MSC_VER)
extern char **environ;
#endif

// This is used to load built-in modules. Instead of using
// __attribute__((constructor)), we call the _register_<modname>
// function for each built-in modules explicitly in
// node::RegisterBuiltinModules(). This is only forward declaration.
// The definitions are in each module's implementation when calling
// the NODE_BUILTIN_MODULE_CONTEXT_AWARE.
#define V(modname) void _register_##modname();
  NODE_BUILTIN_MODULES(V)
#undef V

namespace node {

using v8::Array;
using v8::ArrayBuffer;
using v8::Boolean;
using v8::Context;
using v8::EscapableHandleScope;
using v8::Exception;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Just;
using v8::Local;
using v8::Locker;
using v8::Maybe;
using v8::MaybeLocal;
using v8::Message;
using v8::Name;
using v8::NamedPropertyHandlerConfiguration;
using v8::Nothing;
using v8::Null;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::Promise;
using v8::PropertyCallbackInfo;
using v8::ScriptOrigin;
using v8::SealHandleScope;
using v8::SideEffectType;
using v8::String;
using v8::TryCatch;
using v8::Undefined;
using v8::V8;
using v8::Value;

static Mutex process_mutex;
static Mutex environ_mutex;

static bool print_eval = false;
static bool force_repl = false;
static bool syntax_check_only = false;
static bool trace_deprecation = false;
static bool throw_deprecation = false;
static bool trace_sync_io = false;
static bool no_force_async_hooks_checks = false;
static bool track_heap_objects = false;
static const char* eval_string = nullptr;
static std::vector<std::string> preload_modules;
static const int v8_default_thread_pool_size = 4;
static int v8_thread_pool_size = v8_default_thread_pool_size;
static bool prof_process = false;
static bool v8_is_profiling = false;
static bool node_is_initialized = false;
static node_module* modpending;
static node_module* modlist_builtin;
static node_module* modlist_internal;
static node_module* modlist_linked;
static node_module* modlist_addon;
static std::string trace_enabled_categories;  // NOLINT(runtime/string)
static std::string trace_file_pattern =  // NOLINT(runtime/string)
  "node_trace.${rotation}.log";
static bool abort_on_uncaught_exception = false;

// Bit flag used to track security reverts (see node_revert.h)
unsigned int reverted = 0;

#if defined(NODE_HAVE_I18N_SUPPORT)
// Path to ICU data (for i18n / Intl)
std::string icu_data_dir;  // NOLINT(runtime/string)
#endif

// used by C++ modules as well
bool no_deprecation = false;

#if HAVE_OPENSSL
// use OpenSSL's cert store instead of bundled certs
bool ssl_openssl_cert_store =
#if defined(NODE_OPENSSL_CERT_STORE)
        true;
#else
        false;
#endif

# if NODE_FIPS_MODE
// used by crypto module
bool enable_fips_crypto = false;
bool force_fips_crypto = false;
# endif  // NODE_FIPS_MODE
std::string openssl_config;  // NOLINT(runtime/string)
#endif  // HAVE_OPENSSL

// true if process warnings should be suppressed
bool no_process_warnings = false;
bool trace_warnings = false;

// Set in node.cc by ParseArgs when --preserve-symlinks is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/module.js
bool config_preserve_symlinks = false;

// Set in node.cc by ParseArgs when --preserve-symlinks-main is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/module.js
bool config_preserve_symlinks_main = false;

// Set in node.cc by ParseArgs when --experimental-modules is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/module.js
bool config_experimental_modules = false;

// Set in node.cc by ParseArgs when --experimental-vm-modules is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/vm.js
bool config_experimental_vm_modules = false;

// Set in node.cc by ParseArgs when --experimental-worker is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/worker.js
bool config_experimental_worker = false;

// Set in node.cc by ParseArgs when --experimental-repl-await is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/repl.js.
bool config_experimental_repl_await = false;

// Set in node.cc by ParseArgs when --loader is used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/internal/bootstrap/node.js
std::string config_userland_loader;  // NOLINT(runtime/string)

// Set by ParseArgs when --pending-deprecation or NODE_PENDING_DEPRECATION
// is used.
bool config_pending_deprecation = false;

// Set in node.cc by ParseArgs when --redirect-warnings= is used.
std::string config_warning_file;  // NOLINT(runtime/string)

// Set in node.cc by ParseArgs when --expose-internals or --expose_internals is
// used.
// Used in node_config.cc to set a constant on process.binding('config')
// that is used by lib/internal/bootstrap/node.js
bool config_expose_internals = false;

std::string config_process_title;  // NOLINT(runtime/string)

bool v8_initialized = false;

bool linux_at_secure = false;

// process-relative uptime base, initialized at start-up
double prog_start_time;

static Mutex node_isolate_mutex;
static v8::Isolate* node_isolate;

DebugOptions debug_options;

// Ensures that __metadata trace events are only emitted
// when tracing is enabled.
class NodeTraceStateObserver :
    public v8::TracingController::TraceStateObserver {
 public:
  void OnTraceEnabled() override {
    char name_buffer[512];
    if (uv_get_process_title(name_buffer, sizeof(name_buffer)) == 0) {
      // Only emit the metadata event if the title can be retrieved
      // successfully. Ignore it otherwise.
      TRACE_EVENT_METADATA1("__metadata", "process_name",
                            "name", TRACE_STR_COPY(name_buffer));
    }
    TRACE_EVENT_METADATA1("__metadata", "version",
                          "node", NODE_VERSION_STRING);
    TRACE_EVENT_METADATA1("__metadata", "thread_name",
                          "name", "JavaScriptMainThread");

    auto trace_process = tracing::TracedValue::Create();
    trace_process->BeginDictionary("versions");

    const char http_parser_version[] =
        NODE_STRINGIFY(HTTP_PARSER_VERSION_MAJOR)
        "."
        NODE_STRINGIFY(HTTP_PARSER_VERSION_MINOR)
        "."
        NODE_STRINGIFY(HTTP_PARSER_VERSION_PATCH);

    const char node_napi_version[] = NODE_STRINGIFY(NAPI_VERSION);
    const char node_modules_version[] = NODE_STRINGIFY(NODE_MODULE_VERSION);

    trace_process->SetString("http_parser", http_parser_version);
    trace_process->SetString("node", NODE_VERSION_STRING);
    trace_process->SetString("v8", V8::GetVersion());
    trace_process->SetString("uv", uv_version_string());
    trace_process->SetString("zlib", ZLIB_VERSION);
    trace_process->SetString("ares", ARES_VERSION_STR);
    trace_process->SetString("modules", node_modules_version);
    trace_process->SetString("nghttp2", NGHTTP2_VERSION);
    trace_process->SetString("napi", node_napi_version);

#if HAVE_OPENSSL
    // Stupid code to slice out the version string.
    {  // NOLINT(whitespace/braces)
      size_t i, j, k;
      int c;
      for (i = j = 0, k = sizeof(OPENSSL_VERSION_TEXT) - 1; i < k; ++i) {
        c = OPENSSL_VERSION_TEXT[i];
        if ('0' <= c && c <= '9') {
          for (j = i + 1; j < k; ++j) {
            c = OPENSSL_VERSION_TEXT[j];
            if (c == ' ')
              break;
          }
          break;
        }
      }
      trace_process->SetString("openssl",
                              std::string(&OPENSSL_VERSION_TEXT[i], j - i));
    }
#endif
    trace_process->EndDictionary();

    trace_process->SetString("arch", NODE_ARCH);
    trace_process->SetString("platform", NODE_PLATFORM);

    trace_process->BeginDictionary("release");
    trace_process->SetString("name", NODE_RELEASE);
#if NODE_VERSION_IS_LTS
    trace_process->SetString("lts", NODE_VERSION_LTS_CODENAME);
#endif
    trace_process->EndDictionary();
    TRACE_EVENT_METADATA1("__metadata", "node",
                          "process", std::move(trace_process));

    // This only runs the first time tracing is enabled
    controller_->RemoveTraceStateObserver(this);
    delete this;
  }

  void OnTraceDisabled() override {
    // Do nothing here. This should never be called because the
    // observer removes itself when OnTraceEnabled() is called.
    UNREACHABLE();
  }

  explicit NodeTraceStateObserver(v8::TracingController* controller) :
      controller_(controller) {}
  ~NodeTraceStateObserver() override {}

 private:
  v8::TracingController* controller_;
};

static struct {
#if NODE_USE_V8_PLATFORM
  void Initialize(int thread_pool_size) {
    tracing_agent_.reset(new tracing::Agent(trace_file_pattern));
    auto controller = tracing_agent_->GetTracingController();
    controller->AddTraceStateObserver(new NodeTraceStateObserver(controller));
    tracing::TraceEventHelper::SetTracingController(controller);
    StartTracingAgent();
    platform_ = new NodePlatform(thread_pool_size, controller);
    V8::InitializePlatform(platform_);
  }

  void Dispose() {
    platform_->Shutdown();
    delete platform_;
    platform_ = nullptr;
    tracing_agent_.reset(nullptr);
  }

  void DrainVMTasks(Isolate* isolate) {
    platform_->DrainTasks(isolate);
  }

  void CancelVMTasks(Isolate* isolate) {
    platform_->CancelPendingDelayedTasks(isolate);
  }

#if HAVE_INSPECTOR
  bool StartInspector(Environment* env, const char* script_path,
                      const DebugOptions& options) {
    // Inspector agent can't fail to start, but if it was configured to listen
    // right away on the websocket port and fails to bind/etc, this will return
    // false.
    return env->inspector_agent()->Start(
        script_path == nullptr ? "" : script_path, options);
  }

  bool InspectorStarted(Environment* env) {
    return env->inspector_agent()->IsListening();
  }
#endif  // HAVE_INSPECTOR

  void StartTracingAgent() {
    tracing_agent_->Enable(trace_enabled_categories);
  }

  void StopTracingAgent() {
    if (tracing_agent_)
      tracing_agent_->Stop();
  }

  tracing::Agent* GetTracingAgent() const {
    return tracing_agent_.get();
  }

  NodePlatform* Platform() {
    return platform_;
  }

  std::unique_ptr<tracing::Agent> tracing_agent_;
  NodePlatform* platform_;
#else  // !NODE_USE_V8_PLATFORM
  void Initialize(int thread_pool_size) {}
  void Dispose() {}
  void DrainVMTasks(Isolate* isolate) {}
  void CancelVMTasks(Isolate* isolate) {}
  bool StartInspector(Environment* env, const char* script_path,
                      const DebugOptions& options) {
    env->ThrowError("Node compiled with NODE_USE_V8_PLATFORM=0");
    return true;
  }

  void StartTracingAgent() {
    if (!trace_enabled_categories.empty()) {
      fprintf(stderr, "Node compiled with NODE_USE_V8_PLATFORM=0, "
                      "so event tracing is not available.\n");
    }
  }
  void StopTracingAgent() {}

  tracing::Agent* GetTracingAgent() const { return nullptr; }

  NodePlatform* Platform() {
    return nullptr;
  }
#endif  // !NODE_USE_V8_PLATFORM

#if !NODE_USE_V8_PLATFORM || !HAVE_INSPECTOR
  bool InspectorStarted(Environment* env) {
    return false;
  }
#endif  //  !NODE_USE_V8_PLATFORM || !HAVE_INSPECTOR
} v8_platform;

#ifdef __POSIX__
static const unsigned kMaxSignal = 32;
#endif

void PrintErrorString(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
#ifdef _WIN32
  HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);

  // Check if stderr is something other than a tty/console
  if (stderr_handle == INVALID_HANDLE_VALUE ||
      stderr_handle == nullptr ||
      uv_guess_handle(_fileno(stderr)) != UV_TTY) {
    vfprintf(stderr, format, ap);
    va_end(ap);
    return;
  }

  // Fill in any placeholders
  int n = _vscprintf(format, ap);
  std::vector<char> out(n + 1);
  vsprintf(out.data(), format, ap);

  // Get required wide buffer size
  n = MultiByteToWideChar(CP_UTF8, 0, out.data(), -1, nullptr, 0);

  std::vector<wchar_t> wbuf(n);
  MultiByteToWideChar(CP_UTF8, 0, out.data(), -1, wbuf.data(), n);

  // Don't include the null character in the output
  CHECK_GT(n, 0);
  WriteConsoleW(stderr_handle, wbuf.data(), n - 1, nullptr, nullptr);
#else
  vfprintf(stderr, format, ap);
#endif
  va_end(ap);
}

const char* signo_string(int signo) {
#define SIGNO_CASE(e)  case e: return #e;
  switch (signo) {
#ifdef SIGHUP
  SIGNO_CASE(SIGHUP);
#endif

#ifdef SIGINT
  SIGNO_CASE(SIGINT);
#endif

#ifdef SIGQUIT
  SIGNO_CASE(SIGQUIT);
#endif

#ifdef SIGILL
  SIGNO_CASE(SIGILL);
#endif

#ifdef SIGTRAP
  SIGNO_CASE(SIGTRAP);
#endif

#ifdef SIGABRT
  SIGNO_CASE(SIGABRT);
#endif

#ifdef SIGIOT
# if SIGABRT != SIGIOT
  SIGNO_CASE(SIGIOT);
# endif
#endif

#ifdef SIGBUS
  SIGNO_CASE(SIGBUS);
#endif

#ifdef SIGFPE
  SIGNO_CASE(SIGFPE);
#endif

#ifdef SIGKILL
  SIGNO_CASE(SIGKILL);
#endif

#ifdef SIGUSR1
  SIGNO_CASE(SIGUSR1);
#endif

#ifdef SIGSEGV
  SIGNO_CASE(SIGSEGV);
#endif

#ifdef SIGUSR2
  SIGNO_CASE(SIGUSR2);
#endif

#ifdef SIGPIPE
  SIGNO_CASE(SIGPIPE);
#endif

#ifdef SIGALRM
  SIGNO_CASE(SIGALRM);
#endif

  SIGNO_CASE(SIGTERM);

#ifdef SIGCHLD
  SIGNO_CASE(SIGCHLD);
#endif

#ifdef SIGSTKFLT
  SIGNO_CASE(SIGSTKFLT);
#endif


#ifdef SIGCONT
  SIGNO_CASE(SIGCONT);
#endif

#ifdef SIGSTOP
  SIGNO_CASE(SIGSTOP);
#endif

#ifdef SIGTSTP
  SIGNO_CASE(SIGTSTP);
#endif

#ifdef SIGBREAK
  SIGNO_CASE(SIGBREAK);
#endif

#ifdef SIGTTIN
  SIGNO_CASE(SIGTTIN);
#endif

#ifdef SIGTTOU
  SIGNO_CASE(SIGTTOU);
#endif

#ifdef SIGURG
  SIGNO_CASE(SIGURG);
#endif

#ifdef SIGXCPU
  SIGNO_CASE(SIGXCPU);
#endif

#ifdef SIGXFSZ
  SIGNO_CASE(SIGXFSZ);
#endif

#ifdef SIGVTALRM
  SIGNO_CASE(SIGVTALRM);
#endif

#ifdef SIGPROF
  SIGNO_CASE(SIGPROF);
#endif

#ifdef SIGWINCH
  SIGNO_CASE(SIGWINCH);
#endif

#ifdef SIGIO
  SIGNO_CASE(SIGIO);
#endif

#ifdef SIGPOLL
# if SIGPOLL != SIGIO
  SIGNO_CASE(SIGPOLL);
# endif
#endif

#ifdef SIGLOST
# if SIGLOST != SIGABRT
  SIGNO_CASE(SIGLOST);
# endif
#endif

#ifdef SIGPWR
# if SIGPWR != SIGLOST
  SIGNO_CASE(SIGPWR);
# endif
#endif

#ifdef SIGINFO
# if !defined(SIGPWR) || SIGINFO != SIGPWR
  SIGNO_CASE(SIGINFO);
# endif
#endif

#ifdef SIGSYS
  SIGNO_CASE(SIGSYS);
#endif

  default: return "";
  }
}

// Look up environment variable unless running as setuid root.
bool SafeGetenv(const char* key, std::string* text) {
#if !defined(__CloudABI__) && !defined(_WIN32)
  if (linux_at_secure || getuid() != geteuid() || getgid() != getegid())
    goto fail;
#endif

  {
    Mutex::ScopedLock lock(environ_mutex);
    if (const char* value = getenv(key)) {
      *text = value;
      return true;
    }
  }

fail:
  text->clear();
  return false;
}


void* ArrayBufferAllocator::Allocate(size_t size) {
  if (zero_fill_field_ || zero_fill_all_buffers)
    return UncheckedCalloc(size);
  else
    return UncheckedMalloc(size);
}

namespace {

bool ShouldAbortOnUncaughtException(Isolate* isolate) {
  HandleScope scope(isolate);
  Environment* env = Environment::GetCurrent(isolate);
  return env->should_abort_on_uncaught_toggle()[0] &&
         !env->inside_should_not_abort_on_uncaught_scope();
}

}  // anonymous namespace


void AddPromiseHook(v8::Isolate* isolate, promise_hook_func fn, void* arg) {
  Environment* env = Environment::GetCurrent(isolate);
  env->AddPromiseHook(fn, arg);
}

void AddEnvironmentCleanupHook(v8::Isolate* isolate,
                               void (*fun)(void* arg),
                               void* arg) {
  Environment* env = Environment::GetCurrent(isolate);
  env->AddCleanupHook(fun, arg);
}


void RemoveEnvironmentCleanupHook(v8::Isolate* isolate,
                                  void (*fun)(void* arg),
                                  void* arg) {
  Environment* env = Environment::GetCurrent(isolate);
  env->RemoveCleanupHook(fun, arg);
}

MaybeLocal<Value> InternalMakeCallback(Environment* env,
                                       Local<Object> recv,
                                       const Local<Function> callback,
                                       int argc,
                                       Local<Value> argv[],
                                       async_context asyncContext) {
  CHECK(!recv.IsEmpty());
  InternalCallbackScope scope(env, recv, asyncContext);
  if (scope.Failed()) {
    return Undefined(env->isolate());
  }

  Local<Function> domain_cb = env->domain_callback();
  MaybeLocal<Value> ret;
  if (asyncContext.async_id != 0 || domain_cb.IsEmpty() || recv.IsEmpty()) {
    ret = callback->Call(env->context(), recv, argc, argv);
  } else {
    std::vector<Local<Value>> args(1 + argc);
    args[0] = callback;
    std::copy(&argv[0], &argv[argc], args.begin() + 1);
    ret = domain_cb->Call(env->context(), recv, args.size(), &args[0]);
  }

  if (ret.IsEmpty()) {
    // NOTE: For backwards compatibility with public API we return Undefined()
    // if the top level call threw.
    scope.MarkAsFailed();
    return scope.IsInnerMakeCallback() ? ret : Undefined(env->isolate());
  }

  scope.Close();
  if (scope.Failed()) {
    return Undefined(env->isolate());
  }

  return ret;
}


// Public MakeCallback()s


MaybeLocal<Value> MakeCallback(Isolate* isolate,
                               Local<Object> recv,
                               const char* method,
                               int argc,
                               Local<Value> argv[],
                               async_context asyncContext) {
  Local<String> method_string =
      String::NewFromUtf8(isolate, method, v8::NewStringType::kNormal)
          .ToLocalChecked();
  return MakeCallback(isolate, recv, method_string, argc, argv, asyncContext);
}


MaybeLocal<Value> MakeCallback(Isolate* isolate,
                               Local<Object> recv,
                               Local<String> symbol,
                               int argc,
                               Local<Value> argv[],
                               async_context asyncContext) {
  Local<Value> callback_v = recv->Get(symbol);
  if (callback_v.IsEmpty()) return Local<Value>();
  if (!callback_v->IsFunction()) return Local<Value>();
  Local<Function> callback = callback_v.As<Function>();
  return MakeCallback(isolate, recv, callback, argc, argv, asyncContext);
}


MaybeLocal<Value> MakeCallback(Isolate* isolate,
                               Local<Object> recv,
                               Local<Function> callback,
                               int argc,
                               Local<Value> argv[],
                               async_context asyncContext) {
  // Observe the following two subtleties:
  //
  // 1. The environment is retrieved from the callback function's context.
  // 2. The context to enter is retrieved from the environment.
  //
  // Because of the AssignToContext() call in src/node_contextify.cc,
  // the two contexts need not be the same.
  Environment* env = Environment::GetCurrent(callback->CreationContext());
  Context::Scope context_scope(env->context());
  return InternalMakeCallback(env, recv, callback,
                              argc, argv, asyncContext);
}


// Legacy MakeCallback()s

Local<Value> MakeCallback(Isolate* isolate,
                          Local<Object> recv,
                          const char* method,
                          int argc,
                          Local<Value>* argv) {
  EscapableHandleScope handle_scope(isolate);
  return handle_scope.Escape(
      MakeCallback(isolate, recv, method, argc, argv, {0, 0})
          .FromMaybe(Local<Value>()));
}


Local<Value> MakeCallback(Isolate* isolate,
    Local<Object> recv,
    Local<String> symbol,
    int argc,
    Local<Value>* argv) {
  EscapableHandleScope handle_scope(isolate);
  return handle_scope.Escape(
      MakeCallback(isolate, recv, symbol, argc, argv, {0, 0})
          .FromMaybe(Local<Value>()));
}


Local<Value> MakeCallback(Isolate* isolate,
    Local<Object> recv,
    Local<Function> callback,
    int argc,
    Local<Value>* argv) {
  EscapableHandleScope handle_scope(isolate);
  return handle_scope.Escape(
      MakeCallback(isolate, recv, callback, argc, argv, {0, 0})
          .FromMaybe(Local<Value>()));
}

bool IsExceptionDecorated(Environment* env, Local<Value> er) {
  if (!er.IsEmpty() && er->IsObject()) {
    Local<Object> err_obj = er.As<Object>();
    auto maybe_value =
        err_obj->GetPrivate(env->context(), env->decorated_private_symbol());
    Local<Value> decorated;
    return maybe_value.ToLocal(&decorated) && decorated->IsTrue();
  }
  return false;
}

void AppendExceptionLine(Environment* env,
                         Local<Value> er,
                         Local<Message> message,
                         enum ErrorHandlingMode mode) {
  if (message.IsEmpty())
    return;

  HandleScope scope(env->isolate());
  Local<Object> err_obj;
  if (!er.IsEmpty() && er->IsObject()) {
    err_obj = er.As<Object>();
  }

  // Print (filename):(line number): (message).
  ScriptOrigin origin = message->GetScriptOrigin();
  node::Utf8Value filename(env->isolate(), message->GetScriptResourceName());
  const char* filename_string = *filename;
  int linenum = message->GetLineNumber(env->context()).FromJust();
  // Print line of source code.
  MaybeLocal<String> source_line_maybe = message->GetSourceLine(env->context());
  node::Utf8Value sourceline(env->isolate(),
                             source_line_maybe.ToLocalChecked());
  const char* sourceline_string = *sourceline;
  if (strstr(sourceline_string, "node-do-not-add-exception-line") != nullptr)
    return;

  // Because of how node modules work, all scripts are wrapped with a
  // "function (module, exports, __filename, ...) {"
  // to provide script local variables.
  //
  // When reporting errors on the first line of a script, this wrapper
  // function is leaked to the user. There used to be a hack here to
  // truncate off the first 62 characters, but it caused numerous other
  // problems when vm.runIn*Context() methods were used for non-module
  // code.
  //
  // If we ever decide to re-instate such a hack, the following steps
  // must be taken:
  //
  // 1. Pass a flag around to say "this code was wrapped"
  // 2. Update the stack frame output so that it is also correct.
  //
  // It would probably be simpler to add a line rather than add some
  // number of characters to the first line, since V8 truncates the
  // sourceline to 78 characters, and we end up not providing very much
  // useful debugging info to the user if we remove 62 characters.

  int script_start =
      (linenum - origin.ResourceLineOffset()->Value()) == 1 ?
          origin.ResourceColumnOffset()->Value() : 0;
  int start = message->GetStartColumn(env->context()).FromMaybe(0);
  int end = message->GetEndColumn(env->context()).FromMaybe(0);
  if (start >= script_start) {
    CHECK_GE(end, start);
    start -= script_start;
    end -= script_start;
  }

  char arrow[1024];
  int max_off = sizeof(arrow) - 2;

  int off = snprintf(arrow,
                     sizeof(arrow),
                     "%s:%i\n%s\n",
                     filename_string,
                     linenum,
                     sourceline_string);
  CHECK_GE(off, 0);
  if (off > max_off) {
    off = max_off;
  }

  // Print wavy underline (GetUnderline is deprecated).
  for (int i = 0; i < start; i++) {
    if (sourceline_string[i] == '\0' || off >= max_off) {
      break;
    }
    CHECK_LT(off, max_off);
    arrow[off++] = (sourceline_string[i] == '\t') ? '\t' : ' ';
  }
  for (int i = start; i < end; i++) {
    if (sourceline_string[i] == '\0' || off >= max_off) {
      break;
    }
    CHECK_LT(off, max_off);
    arrow[off++] = '^';
  }
  CHECK_LE(off, max_off);
  arrow[off] = '\n';
  arrow[off + 1] = '\0';

  Local<String> arrow_str = String::NewFromUtf8(env->isolate(), arrow);

  const bool can_set_arrow = !arrow_str.IsEmpty() && !err_obj.IsEmpty();
  // If allocating arrow_str failed, print it out. There's not much else to do.
  // If it's not an error, but something needs to be printed out because
  // it's a fatal exception, also print it out from here.
  // Otherwise, the arrow property will be attached to the object and handled
  // by the caller.
  if (!can_set_arrow || (mode == FATAL_ERROR && !err_obj->IsNativeError())) {
    if (env->printed_error())
      return;
    Mutex::ScopedLock lock(process_mutex);
    env->set_printed_error(true);

    uv_tty_reset_mode();
    PrintErrorString("\n%s", arrow);
    return;
  }

  CHECK(err_obj->SetPrivate(
            env->context(),
            env->arrow_message_private_symbol(),
            arrow_str).FromMaybe(false));
}


void ReportException(Environment* env,
                     Local<Value> er,
                     Local<Message> message) {
  CHECK(!er.IsEmpty());
  HandleScope scope(env->isolate());

  if (message.IsEmpty())
    message = Exception::CreateMessage(env->isolate(), er);

  AppendExceptionLine(env, er, message, FATAL_ERROR);

  Local<Value> trace_value;
  Local<Value> arrow;
  const bool decorated = IsExceptionDecorated(env, er);

  if (er->IsUndefined() || er->IsNull()) {
    trace_value = Undefined(env->isolate());
  } else {
    Local<Object> err_obj = er->ToObject(env->context()).ToLocalChecked();

    trace_value = err_obj->Get(env->stack_string());
    arrow =
        err_obj->GetPrivate(
            env->context(),
            env->arrow_message_private_symbol()).ToLocalChecked();
  }

  node::Utf8Value trace(env->isolate(), trace_value);

  // range errors have a trace member set to undefined
  if (trace.length() > 0 && !trace_value->IsUndefined()) {
    if (arrow.IsEmpty() || !arrow->IsString() || decorated) {
      PrintErrorString("%s\n", *trace);
    } else {
      node::Utf8Value arrow_string(env->isolate(), arrow);
      PrintErrorString("%s\n%s\n", *arrow_string, *trace);
    }
  } else {
    // this really only happens for RangeErrors, since they're the only
    // kind that won't have all this info in the trace, or when non-Error
    // objects are thrown manually.
    Local<Value> message;
    Local<Value> name;

    if (er->IsObject()) {
      Local<Object> err_obj = er.As<Object>();
      message = err_obj->Get(env->message_string());
      name = err_obj->Get(FIXED_ONE_BYTE_STRING(env->isolate(), "name"));
    }

    if (message.IsEmpty() ||
        message->IsUndefined() ||
        name.IsEmpty() ||
        name->IsUndefined()) {
      // Not an error object. Just print as-is.
      String::Utf8Value message(env->isolate(), er);

      PrintErrorString("%s\n", *message ? *message :
                                          "<toString() threw exception>");
    } else {
      node::Utf8Value name_string(env->isolate(), name);
      node::Utf8Value message_string(env->isolate(), message);

      if (arrow.IsEmpty() || !arrow->IsString() || decorated) {
        PrintErrorString("%s: %s\n", *name_string, *message_string);
      } else {
        node::Utf8Value arrow_string(env->isolate(), arrow);
        PrintErrorString("%s\n%s: %s\n",
                         *arrow_string,
                         *name_string,
                         *message_string);
      }
    }
  }

  fflush(stderr);

#if HAVE_INSPECTOR
  env->inspector_agent()->FatalException(er, message);
#endif
}


static void ReportException(Environment* env, const TryCatch& try_catch) {
  ReportException(env, try_catch.Exception(), try_catch.Message());
}


// Executes a str within the current v8 context.
static MaybeLocal<Value> ExecuteString(Environment* env,
                                       Local<String> source,
                                       Local<String> filename) {
  EscapableHandleScope scope(env->isolate());
  TryCatch try_catch(env->isolate());

  // try_catch must be nonverbose to disable FatalException() handler,
  // we will handle exceptions ourself.
  try_catch.SetVerbose(false);

  ScriptOrigin origin(filename);
  MaybeLocal<v8::Script> script =
      v8::Script::Compile(env->context(), source, &origin);
  if (script.IsEmpty()) {
    ReportException(env, try_catch);
    env->Exit(3);
    return MaybeLocal<Value>();
  }

  MaybeLocal<Value> result = script.ToLocalChecked()->Run(env->context());
  if (result.IsEmpty()) {
    if (try_catch.HasTerminated()) {
      env->isolate()->CancelTerminateExecution();
      return MaybeLocal<Value>();
    }
    ReportException(env, try_catch);
    env->Exit(4);
    return MaybeLocal<Value>();
  }

  return scope.Escape(result.ToLocalChecked());
}


static void GetActiveRequests(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  Local<Array> ary = Array::New(args.GetIsolate());
  Local<Context> ctx = env->context();
  Local<Function> fn = env->push_values_to_array_function();
  Local<Value> argv[NODE_PUSH_VAL_TO_ARRAY_MAX];
  size_t idx = 0;

  for (auto w : *env->req_wrap_queue()) {
    if (w->persistent().IsEmpty())
      continue;
    argv[idx] = w->object();
    if (++idx >= arraysize(argv)) {
      fn->Call(ctx, ary, idx, argv).ToLocalChecked();
      idx = 0;
    }
  }

  if (idx > 0) {
    fn->Call(ctx, ary, idx, argv).ToLocalChecked();
  }

  args.GetReturnValue().Set(ary);
}


// Non-static, friend of HandleWrap. Could have been a HandleWrap method but
// implemented here for consistency with GetActiveRequests().
void GetActiveHandles(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  Local<Array> ary = Array::New(env->isolate());
  Local<Context> ctx = env->context();
  Local<Function> fn = env->push_values_to_array_function();
  Local<Value> argv[NODE_PUSH_VAL_TO_ARRAY_MAX];
  size_t idx = 0;

  Local<String> owner_sym = env->owner_string();

  for (auto w : *env->handle_wrap_queue()) {
    if (w->persistent().IsEmpty() || !HandleWrap::HasRef(w))
      continue;
    Local<Object> object = w->object();
    Local<Value> owner = object->Get(owner_sym);
    if (owner->IsUndefined())
      owner = object;
    argv[idx] = owner;
    if (++idx >= arraysize(argv)) {
      fn->Call(ctx, ary, idx, argv).ToLocalChecked();
      idx = 0;
    }
  }
  if (idx > 0) {
    fn->Call(ctx, ary, idx, argv).ToLocalChecked();
  }

  args.GetReturnValue().Set(ary);
}


NO_RETURN void Abort() {
  DumpBacktrace(stderr);
  fflush(stderr);
  ABORT_NO_BACKTRACE();
}


NO_RETURN void Assert(const char* const (*args)[4]) {
  auto filename = (*args)[0];
  auto linenum = (*args)[1];
  auto message = (*args)[2];
  auto function = (*args)[3];

  char name[1024];
  GetHumanReadableProcessName(&name);

  fprintf(stderr, "%s: %s:%s:%s%s Assertion `%s' failed.\n",
          name, filename, linenum, function, *function ? ":" : "", message);
  fflush(stderr);

  Abort();
}


static void WaitForInspectorDisconnect(Environment* env) {
#if HAVE_INSPECTOR
  if (env->inspector_agent()->IsActive()) {
    // Restore signal dispositions, the app is done and is no longer
    // capable of handling signals.
#if defined(__POSIX__) && !defined(NODE_SHARED_MODE)
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    for (unsigned nr = 1; nr < kMaxSignal; nr += 1) {
      if (nr == SIGKILL || nr == SIGSTOP || nr == SIGPROF)
        continue;
      act.sa_handler = (nr == SIGPIPE) ? SIG_IGN : SIG_DFL;
      CHECK_EQ(0, sigaction(nr, &act, nullptr));
    }
#endif
    env->inspector_agent()->WaitForDisconnect();
  }
#endif
}


static void Exit(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  WaitForInspectorDisconnect(env);
  v8_platform.StopTracingAgent();
  env->Exit(args[0]->Int32Value());
}

extern "C" void node_module_register(void* m) {
  struct node_module* mp = reinterpret_cast<struct node_module*>(m);

  if (mp->nm_flags & NM_F_BUILTIN) {
    mp->nm_link = modlist_builtin;
    modlist_builtin = mp;
  } else if (mp->nm_flags & NM_F_INTERNAL) {
    mp->nm_link = modlist_internal;
    modlist_internal = mp;
  } else if (!node_is_initialized) {
    // "Linked" modules are included as part of the node project.
    // Like builtins they are registered *before* node::Init runs.
    mp->nm_flags = NM_F_LINKED;
    mp->nm_link = modlist_linked;
    modlist_linked = mp;
  } else {
    modpending = mp;
  }
}

inline struct node_module* FindModule(struct node_module* list,
                                      const char* name,
                                      int flag) {
  struct node_module* mp;

  for (mp = list; mp != nullptr; mp = mp->nm_link) {
    if (strcmp(mp->nm_modname, name) == 0)
      break;
  }

  CHECK(mp == nullptr || (mp->nm_flags & flag) != 0);
  return mp;
}

node_module* get_builtin_module(const char* name) {
  return FindModule(modlist_builtin, name, NM_F_BUILTIN);
}
node_module* get_internal_module(const char* name) {
  return FindModule(modlist_internal, name, NM_F_INTERNAL);
}
node_module* get_linked_module(const char* name) {
  return FindModule(modlist_linked, name, NM_F_LINKED);
}

class DLib {
 public:
#ifdef __POSIX__
  static const int kDefaultFlags = RTLD_LAZY;
#else
  static const int kDefaultFlags = 0;
#endif

  inline DLib(const char* filename, int flags)
      : filename_(filename), flags_(flags), handle_(nullptr) {}

  inline bool Open();
  inline void Close();
  inline void* GetSymbolAddress(const char* name);

  const std::string filename_;
  const int flags_;
  std::string errmsg_;
  void* handle_;
#ifndef __POSIX__
  uv_lib_t lib_;
#endif
 private:
  DISALLOW_COPY_AND_ASSIGN(DLib);
};


#ifdef __POSIX__
bool DLib::Open() {
  handle_ = dlopen(filename_.c_str(), flags_);
  if (handle_ != nullptr)
    return true;
  errmsg_ = dlerror();
  return false;
}

void DLib::Close() {
  if (handle_ == nullptr) return;
  dlclose(handle_);
  handle_ = nullptr;
}

void* DLib::GetSymbolAddress(const char* name) {
  return dlsym(handle_, name);
}
#else  // !__POSIX__
bool DLib::Open() {
  int ret = uv_dlopen(filename_.c_str(), &lib_);
  if (ret == 0) {
    handle_ = static_cast<void*>(lib_.handle);
    return true;
  }
  errmsg_ = uv_dlerror(&lib_);
  uv_dlclose(&lib_);
  return false;
}

void DLib::Close() {
  if (handle_ == nullptr) return;
  uv_dlclose(&lib_);
  handle_ = nullptr;
}

void* DLib::GetSymbolAddress(const char* name) {
  void* address;
  if (0 == uv_dlsym(&lib_, name, &address)) return address;
  return nullptr;
}
#endif  // !__POSIX__

using InitializerCallback = void (*)(Local<Object> exports,
                                     Local<Value> module,
                                     Local<Context> context);

inline InitializerCallback GetInitializerCallback(DLib* dlib) {
  const char* name = "node_register_module_v" STRINGIFY(NODE_MODULE_VERSION);
  return reinterpret_cast<InitializerCallback>(dlib->GetSymbolAddress(name));
}

inline napi_addon_register_func GetNapiInitializerCallback(DLib* dlib) {
  const char* name =
      STRINGIFY(NAPI_MODULE_INITIALIZER_BASE) STRINGIFY(NAPI_MODULE_VERSION);
  return
      reinterpret_cast<napi_addon_register_func>(dlib->GetSymbolAddress(name));
}

// DLOpen is process.dlopen(module, filename, flags).
// Used to load 'module.node' dynamically shared objects.
//
// FIXME(bnoordhuis) Not multi-context ready. TBD how to resolve the conflict
// when two contexts try to load the same shared object. Maybe have a shadow
// cache that's a plain C list or hash table that's shared across contexts?
static void DLOpen(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  auto context = env->context();

  CHECK_NULL(modpending);

  if (args.Length() < 2) {
    env->ThrowError("process.dlopen needs at least 2 arguments.");
    return;
  }

  int32_t flags = DLib::kDefaultFlags;
  if (args.Length() > 2 && !args[2]->Int32Value(context).To(&flags)) {
    return env->ThrowTypeError("flag argument must be an integer.");
  }

  Local<Object> module;
  Local<Object> exports;
  Local<Value> exports_v;
  if (!args[0]->ToObject(context).ToLocal(&module) ||
      !module->Get(context, env->exports_string()).ToLocal(&exports_v) ||
      !exports_v->ToObject(context).ToLocal(&exports)) {
    return;  // Exception pending.
  }

  node::Utf8Value filename(env->isolate(), args[1]);  // Cast
  DLib dlib(*filename, flags);
  bool is_opened = dlib.Open();

  // Objects containing v14 or later modules will have registered themselves
  // on the pending list.  Activate all of them now.  At present, only one
  // module per object is supported.
  node_module* const mp = modpending;
  modpending = nullptr;

  if (!is_opened) {
    Local<String> errmsg = OneByteString(env->isolate(), dlib.errmsg_.c_str());
    dlib.Close();
#ifdef _WIN32
    // Windows needs to add the filename into the error message
    errmsg = String::Concat(errmsg,
                            args[1]->ToString(context).ToLocalChecked());
#endif  // _WIN32
    env->isolate()->ThrowException(Exception::Error(errmsg));
    return;
  }

  if (mp == nullptr) {
    if (auto callback = GetInitializerCallback(&dlib)) {
      callback(exports, module, context);
    } else if (auto napi_callback = GetNapiInitializerCallback(&dlib)) {
      napi_module_register_by_symbol(exports, module, context, napi_callback);
    } else {
      dlib.Close();
      env->ThrowError("Module did not self-register.");
    }
    return;
  }

  // -1 is used for N-API modules
  if ((mp->nm_version != -1) && (mp->nm_version != NODE_MODULE_VERSION)) {
    // Even if the module did self-register, it may have done so with the wrong
    // version. We must only give up after having checked to see if it has an
    // appropriate initializer callback.
    if (auto callback = GetInitializerCallback(&dlib)) {
      callback(exports, module, context);
      return;
    }
    char errmsg[1024];
    snprintf(errmsg,
             sizeof(errmsg),
             "The module '%s'"
             "\nwas compiled against a different Node.js version using"
             "\nNODE_MODULE_VERSION %d. This version of Node.js requires"
             "\nNODE_MODULE_VERSION %d. Please try re-compiling or "
             "re-installing\nthe module (for instance, using `npm rebuild` "
             "or `npm install`).",
             *filename, mp->nm_version, NODE_MODULE_VERSION);

    // NOTE: `mp` is allocated inside of the shared library's memory, calling
    // `dlclose` will deallocate it
    dlib.Close();
    env->ThrowError(errmsg);
    return;
  }
  if (mp->nm_flags & NM_F_BUILTIN) {
    dlib.Close();
    env->ThrowError("Built-in module self-registered.");
    return;
  }

  mp->nm_dso_handle = dlib.handle_;
  mp->nm_link = modlist_addon;
  modlist_addon = mp;

  if (mp->nm_context_register_func != nullptr) {
    mp->nm_context_register_func(exports, module, context, mp->nm_priv);
  } else if (mp->nm_register_func != nullptr) {
    mp->nm_register_func(exports, module, mp->nm_priv);
  } else {
    dlib.Close();
    env->ThrowError("Module has no declared entry point.");
    return;
  }

  // Tell coverity that 'handle' should not be freed when we return.
  // coverity[leaked_storage]
}


static void OnFatalError(const char* location, const char* message) {
  if (location) {
    PrintErrorString("FATAL ERROR: %s %s\n", location, message);
  } else {
    PrintErrorString("FATAL ERROR: %s\n", message);
  }
  fflush(stderr);
  ABORT();
}


NO_RETURN void FatalError(const char* location, const char* message) {
  OnFatalError(location, message);
  // to suppress compiler warning
  ABORT();
}


FatalTryCatch::~FatalTryCatch() {
  if (HasCaught()) {
    HandleScope scope(env_->isolate());
    ReportException(env_, *this);
    exit(7);
  }
}


void FatalException(Isolate* isolate,
                    Local<Value> error,
                    Local<Message> message) {
  HandleScope scope(isolate);

  Environment* env = Environment::GetCurrent(isolate);
  Local<Object> process_object = env->process_object();
  Local<String> fatal_exception_string = env->fatal_exception_string();
  Local<Function> fatal_exception_function =
      process_object->Get(fatal_exception_string).As<Function>();

  if (!fatal_exception_function->IsFunction()) {
    // Failed before the process._fatalException function was added!
    // this is probably pretty bad.  Nothing to do but report and exit.
    ReportException(env, error, message);
    exit(6);
  } else {
    TryCatch fatal_try_catch(isolate);

    // Do not call FatalException when _fatalException handler throws
    fatal_try_catch.SetVerbose(false);

    // This will return true if the JS layer handled it, false otherwise
    Local<Value> caught =
        fatal_exception_function->Call(process_object, 1, &error);

    if (fatal_try_catch.HasTerminated())
      return;

    if (fatal_try_catch.HasCaught()) {
      // The fatal exception function threw, so we must exit
      ReportException(env, fatal_try_catch);
      exit(7);
    } else if (caught->IsFalse()) {
      ReportException(env, error, message);

      // fatal_exception_function call before may have set a new exit code ->
      // read it again, otherwise use default for uncaughtException 1
      Local<String> exit_code = env->exit_code_string();
      Local<Value> code;
      if (!process_object->Get(env->context(), exit_code).ToLocal(&code) ||
          !code->IsInt32()) {
        exit(1);
      }
      exit(code.As<v8::Int32>()->Value());
    }
  }
}


void FatalException(Isolate* isolate, const TryCatch& try_catch) {
  // If we try to print out a termination exception, we'd just get 'null',
  // so just crashing here with that information seems like a better idea,
  // and in particular it seems like we should handle terminations at the call
  // site for this function rather than by printing them out somewhere.
  CHECK(!try_catch.HasTerminated());

  HandleScope scope(isolate);
  if (!try_catch.IsVerbose()) {
    FatalException(isolate, try_catch.Exception(), try_catch.Message());
  }
}


static void OnMessage(Local<Message> message, Local<Value> error) {
  // The current version of V8 sends messages for errors only
  // (thus `error` is always set).
  FatalException(Isolate::GetCurrent(), error, message);
}

static Maybe<bool> ProcessEmitWarningGeneric(Environment* env,
                                             const char* warning,
                                             const char* type = nullptr,
                                             const char* code = nullptr) {
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  Local<Object> process = env->process_object();
  Local<Value> emit_warning;
  if (!process->Get(env->context(),
                    env->emit_warning_string()).ToLocal(&emit_warning)) {
    return Nothing<bool>();
  }

  if (!emit_warning->IsFunction()) return Just(false);

  int argc = 0;
  Local<Value> args[3];  // warning, type, code

  // The caller has to be able to handle a failure anyway, so we might as well
  // do proper error checking for string creation.
  if (!String::NewFromUtf8(env->isolate(),
                           warning,
                           v8::NewStringType::kNormal).ToLocal(&args[argc++])) {
    return Nothing<bool>();
  }
  if (type != nullptr) {
    if (!String::NewFromOneByte(env->isolate(),
                                reinterpret_cast<const uint8_t*>(type),
                                v8::NewStringType::kNormal)
                                    .ToLocal(&args[argc++])) {
      return Nothing<bool>();
    }
    if (code != nullptr &&
        !String::NewFromOneByte(env->isolate(),
                                reinterpret_cast<const uint8_t*>(code),
                                v8::NewStringType::kNormal)
                                    .ToLocal(&args[argc++])) {
      return Nothing<bool>();
    }
  }

  // MakeCallback() unneeded because emitWarning is internal code, it calls
  // process.emit('warning', ...), but does so on the nextTick.
  if (emit_warning.As<Function>()->Call(env->context(),
                                        process,
                                        argc,
                                        args).IsEmpty()) {
    return Nothing<bool>();
  }
  return Just(true);
}


// Call process.emitWarning(str), fmt is a snprintf() format string
Maybe<bool> ProcessEmitWarning(Environment* env, const char* fmt, ...) {
  char warning[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(warning, sizeof(warning), fmt, ap);
  va_end(ap);

  return ProcessEmitWarningGeneric(env, warning);
}


Maybe<bool> ProcessEmitDeprecationWarning(Environment* env,
                                          const char* warning,
                                          const char* deprecation_code) {
  return ProcessEmitWarningGeneric(env,
                                   warning,
                                   "DeprecationWarning",
                                   deprecation_code);
}


static Local<Object> InitModule(Environment* env,
                                 node_module* mod,
                                 Local<String> module) {
  Local<Object> exports = Object::New(env->isolate());
  // Internal bindings don't have a "module" object, only exports.
  CHECK_NULL(mod->nm_register_func);
  CHECK_NOT_NULL(mod->nm_context_register_func);
  Local<Value> unused = Undefined(env->isolate());
  mod->nm_context_register_func(exports,
                                unused,
                                env->context(),
                                mod->nm_priv);
  return exports;
}

static void ThrowIfNoSuchModule(Environment* env, const char* module_v) {
  char errmsg[1024];
  snprintf(errmsg,
           sizeof(errmsg),
           "No such module: %s",
           module_v);
  env->ThrowError(errmsg);
}

static void GetBinding(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsString());

  Local<String> module = args[0].As<String>();
  node::Utf8Value module_v(env->isolate(), module);

  node_module* mod = get_builtin_module(*module_v);
  Local<Object> exports;
  if (mod != nullptr) {
    exports = InitModule(env, mod, module);
  } else if (!strcmp(*module_v, "constants")) {
    exports = Object::New(env->isolate());
    CHECK(exports->SetPrototype(env->context(),
                                Null(env->isolate())).FromJust());
    DefineConstants(env->isolate(), exports);
  } else if (!strcmp(*module_v, "natives")) {
    exports = Object::New(env->isolate());
    DefineJavaScript(env, exports);
  } else {
    return ThrowIfNoSuchModule(env, *module_v);
  }

  args.GetReturnValue().Set(exports);
}

static void GetInternalBinding(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsString());

  Local<String> module = args[0].As<String>();
  node::Utf8Value module_v(env->isolate(), module);
  Local<Object> exports;

  node_module* mod = get_internal_module(*module_v);
  if (mod != nullptr) {
    exports = InitModule(env, mod, module);
  } else if (!strcmp(*module_v, "code_cache")) {
    // internalBinding('code_cache')
    exports = Object::New(env->isolate());
    DefineCodeCache(env, exports);
  } else {
    return ThrowIfNoSuchModule(env, *module_v);
  }

  args.GetReturnValue().Set(exports);
}

static void GetLinkedBinding(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());

  CHECK(args[0]->IsString());

  Local<String> module_name = args[0].As<String>();

  node::Utf8Value module_name_v(env->isolate(), module_name);
  node_module* mod = get_linked_module(*module_name_v);

  if (mod == nullptr) {
    char errmsg[1024];
    snprintf(errmsg,
             sizeof(errmsg),
             "No such module was linked: %s",
             *module_name_v);
    return env->ThrowError(errmsg);
  }

  Local<Object> module = Object::New(env->isolate());
  Local<Object> exports = Object::New(env->isolate());
  Local<String> exports_prop = String::NewFromUtf8(env->isolate(), "exports");
  module->Set(exports_prop, exports);

  if (mod->nm_context_register_func != nullptr) {
    mod->nm_context_register_func(exports,
                                  module,
                                  env->context(),
                                  mod->nm_priv);
  } else if (mod->nm_register_func != nullptr) {
    mod->nm_register_func(exports, module, mod->nm_priv);
  } else {
    return env->ThrowError("Linked module has no declared entry point.");
  }

  auto effective_exports = module->Get(exports_prop);

  args.GetReturnValue().Set(effective_exports);
}

static void ProcessTitleGetter(Local<Name> property,
                               const PropertyCallbackInfo<Value>& info) {
  char buffer[512];
  uv_get_process_title(buffer, sizeof(buffer));
  info.GetReturnValue().Set(String::NewFromUtf8(info.GetIsolate(), buffer));
}


static void ProcessTitleSetter(Local<Name> property,
                               Local<Value> value,
                               const PropertyCallbackInfo<void>& info) {
  node::Utf8Value title(info.GetIsolate(), value);
  TRACE_EVENT_METADATA1("__metadata", "process_name", "name",
                        TRACE_STR_COPY(*title));
  uv_set_process_title(*title);
}


static void EnvGetter(Local<Name> property,
                      const PropertyCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  if (property->IsSymbol()) {
    return info.GetReturnValue().SetUndefined();
  }
  Mutex::ScopedLock lock(environ_mutex);
#ifdef __POSIX__
  node::Utf8Value key(isolate, property);
  const char* val = getenv(*key);
  if (val) {
    return info.GetReturnValue().Set(String::NewFromUtf8(isolate, val));
  }
#else  // _WIN32
  node::TwoByteValue key(isolate, property);
  WCHAR buffer[32767];  // The maximum size allowed for environment variables.
  SetLastError(ERROR_SUCCESS);
  DWORD result = GetEnvironmentVariableW(reinterpret_cast<WCHAR*>(*key),
                                         buffer,
                                         arraysize(buffer));
  // If result >= sizeof buffer the buffer was too small. That should never
  // happen. If result == 0 and result != ERROR_SUCCESS the variable was not
  // found.
  if ((result > 0 || GetLastError() == ERROR_SUCCESS) &&
      result < arraysize(buffer)) {
    const uint16_t* two_byte_buffer = reinterpret_cast<const uint16_t*>(buffer);
    Local<String> rc = String::NewFromTwoByte(isolate, two_byte_buffer);
    return info.GetReturnValue().Set(rc);
  }
#endif
}


static void EnvSetter(Local<Name> property,
                      Local<Value> value,
                      const PropertyCallbackInfo<Value>& info) {
  Environment* env = Environment::GetCurrent(info);
  if (config_pending_deprecation && env->EmitProcessEnvWarning() &&
      !value->IsString() && !value->IsNumber() && !value->IsBoolean()) {
    if (ProcessEmitDeprecationWarning(
          env,
          "Assigning any value other than a string, number, or boolean to a "
          "process.env property is deprecated. Please make sure to convert the "
          "value to a string before setting process.env with it.",
          "DEP0104").IsNothing())
      return;
  }

  Mutex::ScopedLock lock(environ_mutex);
#ifdef __POSIX__
  node::Utf8Value key(info.GetIsolate(), property);
  node::Utf8Value val(info.GetIsolate(), value);
  setenv(*key, *val, 1);
#else  // _WIN32
  node::TwoByteValue key(info.GetIsolate(), property);
  node::TwoByteValue val(info.GetIsolate(), value);
  WCHAR* key_ptr = reinterpret_cast<WCHAR*>(*key);
  // Environment variables that start with '=' are read-only.
  if (key_ptr[0] != L'=') {
    SetEnvironmentVariableW(key_ptr, reinterpret_cast<WCHAR*>(*val));
  }
#endif
  // Whether it worked or not, always return value.
  info.GetReturnValue().Set(value);
}


static void EnvQuery(Local<Name> property,
                     const PropertyCallbackInfo<Integer>& info) {
  Mutex::ScopedLock lock(environ_mutex);
  int32_t rc = -1;  // Not found unless proven otherwise.
  if (property->IsString()) {
#ifdef __POSIX__
    node::Utf8Value key(info.GetIsolate(), property);
    if (getenv(*key))
      rc = 0;
#else  // _WIN32
    node::TwoByteValue key(info.GetIsolate(), property);
    WCHAR* key_ptr = reinterpret_cast<WCHAR*>(*key);
    SetLastError(ERROR_SUCCESS);
    if (GetEnvironmentVariableW(key_ptr, nullptr, 0) > 0 ||
        GetLastError() == ERROR_SUCCESS) {
      rc = 0;
      if (key_ptr[0] == L'=') {
        // Environment variables that start with '=' are hidden and read-only.
        rc = static_cast<int32_t>(v8::ReadOnly) |
             static_cast<int32_t>(v8::DontDelete) |
             static_cast<int32_t>(v8::DontEnum);
      }
    }
#endif
  }
  if (rc != -1)
    info.GetReturnValue().Set(rc);
}


static void EnvDeleter(Local<Name> property,
                       const PropertyCallbackInfo<Boolean>& info) {
  Mutex::ScopedLock lock(environ_mutex);
  if (property->IsString()) {
#ifdef __POSIX__
    node::Utf8Value key(info.GetIsolate(), property);
    unsetenv(*key);
#else
    node::TwoByteValue key(info.GetIsolate(), property);
    WCHAR* key_ptr = reinterpret_cast<WCHAR*>(*key);
    SetEnvironmentVariableW(key_ptr, nullptr);
#endif
  }

  // process.env never has non-configurable properties, so always
  // return true like the tc39 delete operator.
  info.GetReturnValue().Set(true);
}


static void EnvEnumerator(const PropertyCallbackInfo<Array>& info) {
  Environment* env = Environment::GetCurrent(info);
  Isolate* isolate = env->isolate();
  Local<Context> ctx = env->context();
  Local<Function> fn = env->push_values_to_array_function();
  Local<Value> argv[NODE_PUSH_VAL_TO_ARRAY_MAX];
  size_t idx = 0;

  Mutex::ScopedLock lock(environ_mutex);
#ifdef __POSIX__
  int size = 0;
  while (environ[size])
    size++;

  Local<Array> envarr = Array::New(isolate);

  for (int i = 0; i < size; ++i) {
    const char* var = environ[i];
    const char* s = strchr(var, '=');
    const int length = s ? s - var : strlen(var);
    argv[idx] = String::NewFromUtf8(isolate,
                                    var,
                                    String::kNormalString,
                                    length);
    if (++idx >= arraysize(argv)) {
      fn->Call(ctx, envarr, idx, argv).ToLocalChecked();
      idx = 0;
    }
  }
  if (idx > 0) {
    fn->Call(ctx, envarr, idx, argv).ToLocalChecked();
  }
#else  // _WIN32
  WCHAR* environment = GetEnvironmentStringsW();
  if (environment == nullptr)
    return;  // This should not happen.
  Local<Array> envarr = Array::New(isolate);
  WCHAR* p = environment;
  while (*p) {
    WCHAR* s;
    if (*p == L'=') {
      // If the key starts with '=' it is a hidden environment variable.
      p += wcslen(p) + 1;
      continue;
    } else {
      s = wcschr(p, L'=');
    }
    if (!s) {
      s = p + wcslen(p);
    }
    const uint16_t* two_byte_buffer = reinterpret_cast<const uint16_t*>(p);
    const size_t two_byte_buffer_len = s - p;
    argv[idx] = String::NewFromTwoByte(isolate,
                                       two_byte_buffer,
                                       String::kNormalString,
                                       two_byte_buffer_len);
    if (++idx >= arraysize(argv)) {
      fn->Call(ctx, envarr, idx, argv).ToLocalChecked();
      idx = 0;
    }
    p = s + wcslen(s) + 1;
  }
  if (idx > 0) {
    fn->Call(ctx, envarr, idx, argv).ToLocalChecked();
  }
  FreeEnvironmentStringsW(environment);
#endif

  info.GetReturnValue().Set(envarr);
}


static void GetParentProcessId(Local<Name> property,
                               const PropertyCallbackInfo<Value>& info) {
  info.GetReturnValue().Set(Integer::New(info.GetIsolate(), uv_os_getppid()));
}


static Local<Object> GetFeatures(Environment* env) {
  EscapableHandleScope scope(env->isolate());

  Local<Object> obj = Object::New(env->isolate());
#if defined(DEBUG) && DEBUG
  Local<Value> debug = True(env->isolate());
#else
  Local<Value> debug = False(env->isolate());
#endif  // defined(DEBUG) && DEBUG

  obj->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "debug"), debug);
  obj->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "uv"), True(env->isolate()));
  // TODO(bnoordhuis) ping libuv
  obj->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "ipv6"), True(env->isolate()));

#ifdef HAVE_OPENSSL
  Local<Boolean> have_openssl = True(env->isolate());
#else
  Local<Boolean> have_openssl = False(env->isolate());
#endif

  obj->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "tls_alpn"), have_openssl);
  obj->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "tls_sni"), have_openssl);
  obj->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "tls_ocsp"), have_openssl);
  obj->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "tls"), have_openssl);

  return scope.Escape(obj);
}


static void DebugPortGetter(Local<Name> property,
                            const PropertyCallbackInfo<Value>& info) {
  Mutex::ScopedLock lock(process_mutex);
  int port = debug_options.port();
#if HAVE_INSPECTOR
  if (port == 0) {
    Environment* env = Environment::GetCurrent(info);
    if (auto io = env->inspector_agent()->io())
      port = io->port();
  }
#endif  // HAVE_INSPECTOR
  info.GetReturnValue().Set(port);
}


static void DebugPortSetter(Local<Name> property,
                            Local<Value> value,
                            const PropertyCallbackInfo<void>& info) {
  Mutex::ScopedLock lock(process_mutex);
  debug_options.set_port(value->Int32Value());
}


static void DebugProcess(const FunctionCallbackInfo<Value>& args);
static void DebugEnd(const FunctionCallbackInfo<Value>& args);

namespace {

#define READONLY_PROPERTY(obj, str, var)                                      \
  do {                                                                        \
    obj->DefineOwnProperty(env->context(),                                    \
                           OneByteString(env->isolate(), str),                \
                           var,                                               \
                           v8::ReadOnly).FromJust();                          \
  } while (0)

#define READONLY_DONT_ENUM_PROPERTY(obj, str, var)                            \
  do {                                                                        \
    obj->DefineOwnProperty(env->context(),                                    \
                           OneByteString(env->isolate(), str),                \
                           var,                                               \
                           static_cast<v8::PropertyAttribute>(v8::ReadOnly |  \
                                                              v8::DontEnum))  \
        .FromJust();                                                          \
  } while (0)

}  // anonymous namespace

void SetupProcessObject(Environment* env,
                        int argc,
                        const char* const* argv,
                        int exec_argc,
                        const char* const* exec_argv) {
  HandleScope scope(env->isolate());

  Local<Object> process = env->process_object();

  auto title_string = FIXED_ONE_BYTE_STRING(env->isolate(), "title");
  CHECK(process->SetAccessor(
      env->context(),
      title_string,
      ProcessTitleGetter,
      env->is_main_thread() ? ProcessTitleSetter : nullptr,
      env->as_external(),
      v8::DEFAULT,
      v8::None,
      SideEffectType::kHasNoSideEffect).FromJust());

  // process.version
  READONLY_PROPERTY(process,
                    "version",
                    FIXED_ONE_BYTE_STRING(env->isolate(), NODE_VERSION));

  // process.versions
  Local<Object> versions = Object::New(env->isolate());
  READONLY_PROPERTY(process, "versions", versions);

  const char http_parser_version[] = NODE_STRINGIFY(HTTP_PARSER_VERSION_MAJOR)
                                     "."
                                     NODE_STRINGIFY(HTTP_PARSER_VERSION_MINOR)
                                     "."
                                     NODE_STRINGIFY(HTTP_PARSER_VERSION_PATCH);
  READONLY_PROPERTY(versions,
                    "http_parser",
                    FIXED_ONE_BYTE_STRING(env->isolate(), http_parser_version));
  // +1 to get rid of the leading 'v'
  READONLY_PROPERTY(versions,
                    "node",
                    OneByteString(env->isolate(), NODE_VERSION + 1));
  READONLY_PROPERTY(versions,
                    "v8",
                    OneByteString(env->isolate(), V8::GetVersion()));
  READONLY_PROPERTY(versions,
                    "uv",
                    OneByteString(env->isolate(), uv_version_string()));
  READONLY_PROPERTY(versions,
                    "zlib",
                    FIXED_ONE_BYTE_STRING(env->isolate(), ZLIB_VERSION));
  READONLY_PROPERTY(versions,
                    "ares",
                    FIXED_ONE_BYTE_STRING(env->isolate(), ARES_VERSION_STR));

  const char node_modules_version[] = NODE_STRINGIFY(NODE_MODULE_VERSION);
  READONLY_PROPERTY(
      versions,
      "modules",
      FIXED_ONE_BYTE_STRING(env->isolate(), node_modules_version));
  READONLY_PROPERTY(versions,
                    "nghttp2",
                    FIXED_ONE_BYTE_STRING(env->isolate(), NGHTTP2_VERSION));
  const char node_napi_version[] = NODE_STRINGIFY(NAPI_VERSION);
  READONLY_PROPERTY(
      versions,
      "napi",
      FIXED_ONE_BYTE_STRING(env->isolate(), node_napi_version));

#if HAVE_OPENSSL
  // Stupid code to slice out the version string.
  {  // NOLINT(whitespace/braces)
    size_t i, j, k;
    int c;
    for (i = j = 0, k = sizeof(OPENSSL_VERSION_TEXT) - 1; i < k; ++i) {
      c = OPENSSL_VERSION_TEXT[i];
      if ('0' <= c && c <= '9') {
        for (j = i + 1; j < k; ++j) {
          c = OPENSSL_VERSION_TEXT[j];
          if (c == ' ')
            break;
        }
        break;
      }
    }
    READONLY_PROPERTY(
        versions,
        "openssl",
        OneByteString(env->isolate(), &OPENSSL_VERSION_TEXT[i], j - i));
  }
#endif

  // process.arch
  READONLY_PROPERTY(process, "arch", OneByteString(env->isolate(), NODE_ARCH));

  // process.platform
  READONLY_PROPERTY(process,
                    "platform",
                    OneByteString(env->isolate(), NODE_PLATFORM));

  // process.release
  Local<Object> release = Object::New(env->isolate());
  READONLY_PROPERTY(process, "release", release);
  READONLY_PROPERTY(release, "name",
                    OneByteString(env->isolate(), NODE_RELEASE));

#if NODE_VERSION_IS_LTS
  READONLY_PROPERTY(release, "lts",
                    OneByteString(env->isolate(), NODE_VERSION_LTS_CODENAME));
#endif

// if this is a release build and no explicit base has been set
// substitute the standard release download URL
#ifndef NODE_RELEASE_URLBASE
# if NODE_VERSION_IS_RELEASE
#  define NODE_RELEASE_URLBASE "https://nodejs.org/download/release/"
# endif
#endif

#if defined(NODE_RELEASE_URLBASE)
#  define NODE_RELEASE_URLPFX NODE_RELEASE_URLBASE "v" NODE_VERSION_STRING "/"
#  define NODE_RELEASE_URLFPFX NODE_RELEASE_URLPFX "node-v" NODE_VERSION_STRING

  READONLY_PROPERTY(release,
                    "sourceUrl",
                    OneByteString(env->isolate(),
                    NODE_RELEASE_URLFPFX ".tar.gz"));
  READONLY_PROPERTY(release,
                    "headersUrl",
                    OneByteString(env->isolate(),
                    NODE_RELEASE_URLFPFX "-headers.tar.gz"));
#  ifdef _WIN32
  READONLY_PROPERTY(release,
                    "libUrl",
                    OneByteString(env->isolate(),
                    strcmp(NODE_ARCH, "ia32") ? NODE_RELEASE_URLPFX "win-"
                                                NODE_ARCH "/node.lib"
                                              : NODE_RELEASE_URLPFX
                                                "win-x86/node.lib"));
#  endif
#endif

  // process.argv
  Local<Array> arguments = Array::New(env->isolate(), argc);
  for (int i = 0; i < argc; ++i) {
    arguments->Set(i, String::NewFromUtf8(env->isolate(), argv[i]));
  }
  process->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "argv"), arguments);

  // process.execArgv
  Local<Array> exec_arguments = Array::New(env->isolate(), exec_argc);
  for (int i = 0; i < exec_argc; ++i) {
    exec_arguments->Set(i, String::NewFromUtf8(env->isolate(), exec_argv[i]));
  }
  process->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "execArgv"),
               exec_arguments);

  // create process.env
  Local<ObjectTemplate> process_env_template =
      ObjectTemplate::New(env->isolate());
  process_env_template->SetHandler(NamedPropertyHandlerConfiguration(
          EnvGetter,
          EnvSetter,
          EnvQuery,
          EnvDeleter,
          EnvEnumerator,
          env->as_external()));

  Local<Object> process_env =
      process_env_template->NewInstance(env->context()).ToLocalChecked();
  process->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "env"), process_env);

  READONLY_PROPERTY(process, "pid",
                    Integer::New(env->isolate(), uv_os_getpid()));
  READONLY_PROPERTY(process, "features", GetFeatures(env));

  CHECK(process->SetAccessor(env->context(),
                             FIXED_ONE_BYTE_STRING(env->isolate(), "ppid"),
                             GetParentProcessId).FromJust());

  // -e, --eval
  if (eval_string) {
    READONLY_PROPERTY(process,
                      "_eval",
                      String::NewFromUtf8(env->isolate(), eval_string));
  }

  // -p, --print
  if (print_eval) {
    READONLY_PROPERTY(process, "_print_eval", True(env->isolate()));
  }

  // -c, --check
  if (syntax_check_only) {
    READONLY_PROPERTY(process, "_syntax_check_only", True(env->isolate()));
  }

  // -i, --interactive
  if (force_repl) {
    READONLY_PROPERTY(process, "_forceRepl", True(env->isolate()));
  }

  // -r, --require
  if (!preload_modules.empty()) {
    Local<Array> array = Array::New(env->isolate());
    for (unsigned int i = 0; i < preload_modules.size(); ++i) {
      Local<String> module = String::NewFromUtf8(env->isolate(),
                                                 preload_modules[i].c_str());
      array->Set(i, module);
    }
    READONLY_PROPERTY(process,
                      "_preload_modules",
                      array);

    preload_modules.clear();
  }

  // --no-deprecation
  if (no_deprecation) {
    READONLY_PROPERTY(process, "noDeprecation", True(env->isolate()));
  }

  // --no-warnings
  if (no_process_warnings) {
    READONLY_PROPERTY(process, "noProcessWarnings", True(env->isolate()));
  }

  // --trace-warnings
  if (trace_warnings) {
    READONLY_PROPERTY(process, "traceProcessWarnings", True(env->isolate()));
  }

  // --throw-deprecation
  if (throw_deprecation) {
    READONLY_PROPERTY(process, "throwDeprecation", True(env->isolate()));
  }

#ifdef NODE_NO_BROWSER_GLOBALS
  // configure --no-browser-globals
  READONLY_PROPERTY(process, "_noBrowserGlobals", True(env->isolate()));
#endif  // NODE_NO_BROWSER_GLOBALS

  // --prof-process
  if (prof_process) {
    READONLY_PROPERTY(process, "profProcess", True(env->isolate()));
  }

  // --trace-deprecation
  if (trace_deprecation) {
    READONLY_PROPERTY(process, "traceDeprecation", True(env->isolate()));
  }

  // TODO(refack): move the following 3 to `node_config`
  // --inspect-brk
  if (debug_options.wait_for_connect()) {
    READONLY_DONT_ENUM_PROPERTY(process,
                                "_breakFirstLine", True(env->isolate()));
  }

  if (debug_options.break_node_first_line()) {
    READONLY_DONT_ENUM_PROPERTY(process,
                                "_breakNodeFirstLine", True(env->isolate()));
  }

  // --inspect --debug-brk
  if (debug_options.deprecated_invocation()) {
    READONLY_DONT_ENUM_PROPERTY(process,
                                "_deprecatedDebugBrk", True(env->isolate()));
  }

  // --debug or, --debug-brk without --inspect
  if (debug_options.invalid_invocation()) {
    READONLY_DONT_ENUM_PROPERTY(process,
                                "_invalidDebug", True(env->isolate()));
  }

  // --security-revert flags
#define V(code, _, __)                                                        \
  do {                                                                        \
    if (IsReverted(SECURITY_REVERT_ ## code)) {                               \
      READONLY_PROPERTY(process, "REVERT_" #code, True(env->isolate()));      \
    }                                                                         \
  } while (0);
  SECURITY_REVERSIONS(V)
#undef V

  size_t exec_path_len = 2 * PATH_MAX;
  char* exec_path = new char[exec_path_len];
  Local<String> exec_path_value;
  if (uv_exepath(exec_path, &exec_path_len) == 0) {
    exec_path_value = String::NewFromUtf8(env->isolate(),
                                          exec_path,
                                          String::kNormalString,
                                          exec_path_len);
  } else {
    exec_path_value = String::NewFromUtf8(env->isolate(), argv[0]);
  }
  process->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "execPath"),
               exec_path_value);
  delete[] exec_path;

  auto debug_port_string = FIXED_ONE_BYTE_STRING(env->isolate(), "debugPort");
  CHECK(process->SetAccessor(env->context(),
                             debug_port_string,
                             DebugPortGetter,
                             env->is_main_thread() ? DebugPortSetter : nullptr,
                             env->as_external()).FromJust());

  // define various internal methods
  if (env->is_main_thread()) {
    env->SetMethod(process, "_debugProcess", DebugProcess);
    env->SetMethod(process, "_debugEnd", DebugEnd);
    env->SetMethod(process,
                   "_startProfilerIdleNotifier",
                   StartProfilerIdleNotifier);
    env->SetMethod(process,
                   "_stopProfilerIdleNotifier",
                   StopProfilerIdleNotifier);
    env->SetMethod(process, "abort", Abort);
    env->SetMethod(process, "chdir", Chdir);
    env->SetMethod(process, "umask", Umask);
  }
  env->SetMethod(process, "_getActiveRequests", GetActiveRequests);
  env->SetMethod(process, "_getActiveHandles", GetActiveHandles);
  env->SetMethod(process, "_kill", Kill);

  env->SetMethodNoSideEffect(process, "cwd", Cwd);
  env->SetMethod(process, "dlopen", DLOpen);
  env->SetMethod(process, "reallyExit", Exit);
  env->SetMethodNoSideEffect(process, "uptime", Uptime);

#if defined(__POSIX__) && !defined(__ANDROID__) && !defined(__CloudABI__)
  env->SetMethodNoSideEffect(process, "getuid", GetUid);
  env->SetMethodNoSideEffect(process, "geteuid", GetEUid);
  env->SetMethodNoSideEffect(process, "getgid", GetGid);
  env->SetMethodNoSideEffect(process, "getegid", GetEGid);
  env->SetMethodNoSideEffect(process, "getgroups", GetGroups);
#endif  // __POSIX__ && !defined(__ANDROID__) && !defined(__CloudABI__)
}


#undef READONLY_PROPERTY


void SignalExit(int signo) {
  uv_tty_reset_mode();
  v8_platform.StopTracingAgent();
#ifdef __FreeBSD__
  // FreeBSD has a nasty bug, see RegisterSignalHandler for details
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  CHECK_EQ(sigaction(signo, &sa, nullptr), 0);
#endif
  raise(signo);
}


static MaybeLocal<Function> GetBootstrapper(
    Environment* env,
    Local<String> source,
    Local<String> script_name) {
  EscapableHandleScope scope(env->isolate());

  TryCatch try_catch(env->isolate());

  // Disable verbose mode to stop FatalException() handler from trying
  // to handle the exception. Errors this early in the start-up phase
  // are not safe to ignore.
  try_catch.SetVerbose(false);

  // Execute the bootstrapper javascript file
  MaybeLocal<Value> bootstrapper_v = ExecuteString(env, source, script_name);
  if (bootstrapper_v.IsEmpty())  // This happens when execution was interrupted.
    return MaybeLocal<Function>();

  if (try_catch.HasCaught())  {
    ReportException(env, try_catch);
    exit(10);
  }

  CHECK(bootstrapper_v.ToLocalChecked()->IsFunction());
  return scope.Escape(bootstrapper_v.ToLocalChecked().As<Function>());
}

static bool ExecuteBootstrapper(Environment* env, Local<Function> bootstrapper,
                                int argc, Local<Value> argv[],
                                Local<Value>* out) {
  bool ret = bootstrapper->Call(
      env->context(), Null(env->isolate()), argc, argv).ToLocal(out);

  // If there was an error during bootstrap then it was either handled by the
  // FatalException handler or it's unrecoverable (e.g. max call stack
  // exceeded). Either way, clear the stack so that the AsyncCallbackScope
  // destructor doesn't fail on the id check.
  // There are only two ways to have a stack size > 1: 1) the user manually
  // called MakeCallback or 2) user awaited during bootstrap, which triggered
  // _tickCallback().
  if (!ret) {
    env->async_hooks()->clear_async_id_stack();
  }

  return ret;
}


void LoadEnvironment(Environment* env) {
  HandleScope handle_scope(env->isolate());

  TryCatch try_catch(env->isolate());
  // Disable verbose mode to stop FatalException() handler from trying
  // to handle the exception. Errors this early in the start-up phase
  // are not safe to ignore.
  try_catch.SetVerbose(false);

  // The bootstrapper scripts are lib/internal/bootstrap/loaders.js and
  // lib/internal/bootstrap/node.js, each included as a static C string
  // defined in node_javascript.h, generated in node_javascript.cc by
  // node_js2c.
  Local<String> loaders_name =
      FIXED_ONE_BYTE_STRING(env->isolate(), "internal/bootstrap/loaders.js");
  MaybeLocal<Function> loaders_bootstrapper =
      GetBootstrapper(env, LoadersBootstrapperSource(env), loaders_name);
  Local<String> node_name =
      FIXED_ONE_BYTE_STRING(env->isolate(), "internal/bootstrap/node.js");
  MaybeLocal<Function> node_bootstrapper =
      GetBootstrapper(env, NodeBootstrapperSource(env), node_name);

  if (loaders_bootstrapper.IsEmpty() || node_bootstrapper.IsEmpty()) {
    // Execution was interrupted.
    return;
  }

  // Add a reference to the global object
  Local<Object> global = env->context()->Global();

#if defined HAVE_DTRACE || defined HAVE_ETW
  InitDTrace(env, global);
#endif

#if defined HAVE_PERFCTR
  InitPerfCounters(env, global);
#endif

  // Enable handling of uncaught exceptions
  // (FatalException(), break on uncaught exception in debugger)
  //
  // This is not strictly necessary since it's almost impossible
  // to attach the debugger fast enough to break on exception
  // thrown during process startup.
  try_catch.SetVerbose(true);

  env->SetMethod(env->process_object(), "_rawDebug", RawDebug);

  // Expose the global object as a property on itself
  // (Allows you to set stuff on `global` from anywhere in JavaScript.)
  global->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "global"), global);

  // Create binding loaders
  v8::Local<v8::Function> get_binding_fn =
      env->NewFunctionTemplate(GetBinding)->GetFunction(env->context())
          .ToLocalChecked();

  v8::Local<v8::Function> get_linked_binding_fn =
      env->NewFunctionTemplate(GetLinkedBinding)->GetFunction(env->context())
          .ToLocalChecked();

  v8::Local<v8::Function> get_internal_binding_fn =
      env->NewFunctionTemplate(GetInternalBinding)->GetFunction(env->context())
          .ToLocalChecked();

  Local<Value> loaders_bootstrapper_args[] = {
    env->process_object(),
    get_binding_fn,
    get_linked_binding_fn,
    get_internal_binding_fn,
    Boolean::New(env->isolate(), debug_options.break_node_first_line())
  };

  // Bootstrap internal loaders
  Local<Value> bootstrapped_loaders;
  if (!ExecuteBootstrapper(env, loaders_bootstrapper.ToLocalChecked(),
                           arraysize(loaders_bootstrapper_args),
                           loaders_bootstrapper_args,
                           &bootstrapped_loaders)) {
    return;
  }

  // Bootstrap Node.js
  Local<Object> bootstrapper = Object::New(env->isolate());
  SetupBootstrapObject(env, bootstrapper);
  Local<Value> bootstrapped_node;
  Local<Value> node_bootstrapper_args[] = {
    env->process_object(),
    bootstrapper,
    bootstrapped_loaders
  };
  if (!ExecuteBootstrapper(env, node_bootstrapper.ToLocalChecked(),
                           arraysize(node_bootstrapper_args),
                           node_bootstrapper_args,
                           &bootstrapped_node)) {
    return;
  }
}

static void PrintHelp() {
  // XXX: If you add an option here, please also add it to doc/node.1 and
  // doc/api/cli.md
  printf("Usage: node [options] [ -e script | script.js | - ] [arguments]\n"
         "       node inspect script.js [arguments]\n"
         "\n"
         "Options:\n"
         "  -                          script read from stdin (default; \n"
         "                             interactive mode if a tty)\n"
         "  --                         indicate the end of node options\n"
         "  --abort-on-uncaught-exception\n"
         "                             aborting instead of exiting causes a\n"
         "                             core file to be generated for analysis\n"
#if HAVE_OPENSSL && NODE_FIPS_MODE
         "  --enable-fips              enable FIPS crypto at startup\n"
#endif  // NODE_FIPS_MODE && NODE_FIPS_MODE
#if defined(NODE_HAVE_I18N_SUPPORT)
         "  --experimental-modules     experimental ES Module support\n"
         "                             and caching modules\n"
#endif  // defined(NODE_HAVE_I18N_SUPPORT)
         "  --experimental-repl-await  experimental await keyword support\n"
         "                             in REPL\n"
#if defined(NODE_HAVE_I18N_SUPPORT)
         "  --experimental-vm-modules  experimental ES Module support\n"
         "                             in vm module\n"
#endif  // defined(NODE_HAVE_I18N_SUPPORT)
         "  --experimental-worker      experimental threaded Worker support\n"
#if HAVE_OPENSSL && NODE_FIPS_MODE
         "  --force-fips               force FIPS crypto (cannot be disabled)\n"
#endif  // HAVE_OPENSSL && NODE_FIPS_MODE
#if defined(NODE_HAVE_I18N_SUPPORT)
         "  --icu-data-dir=dir         set ICU data load path to dir\n"
         "                             (overrides NODE_ICU_DATA)\n"
#if !defined(NODE_HAVE_SMALL_ICU)
         "                             note: linked-in ICU data is present\n"
#endif
#endif  // defined(NODE_HAVE_I18N_SUPPORT)
#if HAVE_INSPECTOR
         "  --inspect-brk[=[host:]port]\n"
         "                             activate inspector on host:port\n"
         "                             and break at start of user script\n"
         "  --inspect-port=[host:]port\n"
         "                             set host:port for inspector\n"
         "  --inspect[=[host:]port]    activate inspector on host:port\n"
         "                             (default: 127.0.0.1:9229)\n"
#endif  // HAVE_INSPECTOR
         "  --napi-modules             load N-API modules (no-op - option\n"
         "                             kept for compatibility)\n"
         "  --no-deprecation           silence deprecation warnings\n"
         "  --no-force-async-hooks-checks\n"
         "                             disable checks for async_hooks\n"
         "  --no-warnings              silence all process warnings\n"
#if HAVE_OPENSSL
         "  --openssl-config=file      load OpenSSL configuration from the\n"
         "                             specified file (overrides\n"
         "                             OPENSSL_CONF)\n"
#endif  // HAVE_OPENSSL
         "  --pending-deprecation      emit pending deprecation warnings\n"
#if defined(NODE_HAVE_I18N_SUPPORT)
         "  --preserve-symlinks        preserve symbolic links when resolving\n"
         "  --preserve-symlinks-main   preserve symbolic links when resolving\n"
         "                             the main module\n"
#endif
         "  --prof                     generate V8 profiler output\n"
         "  --prof-process             process V8 profiler output generated\n"
         "                             using --prof\n"
         "  --redirect-warnings=file\n"
         "                             write warnings to file instead of\n"
         "                             stderr\n"
         "  --throw-deprecation        throw an exception on deprecations\n"
         "  --title=title              the process title to use on start up\n"
#if HAVE_OPENSSL
         "  --tls-cipher-list=val      use an alternative default TLS cipher "
         "list\n"
#endif  // HAVE_OPENSSL
         "  --trace-deprecation        show stack traces on deprecations\n"
         "  --trace-event-categories   comma separated list of trace event\n"
         "                             categories to record\n"
         "  --trace-event-file-pattern Template string specifying the\n"
         "                             filepath for the trace-events data, it\n"
         "                             supports ${rotation} and ${pid}\n"
         "                             log-rotation id. %%2$u is the pid.\n"
         "  --trace-events-enabled     track trace events\n"
         "  --trace-sync-io            show stack trace when use of sync IO\n"
         "                             is detected after the first tick\n"
         "  --trace-warnings           show stack traces on process warnings\n"
         "  --track-heap-objects       track heap object allocations for heap "
         "snapshots\n"
#if HAVE_OPENSSL
         "  --use-bundled-ca           use bundled CA store"
#if !defined(NODE_OPENSSL_CERT_STORE)
         " (default)"
#endif
         "\n"
         "  --use-openssl-ca           use OpenSSL's default CA store"
#if defined(NODE_OPENSSL_CERT_STORE)
         " (default)"
#endif
#endif  // HAVE_OPENSSL
         "\n"
         "  --v8-options               print v8 command line options\n"
         "  --v8-pool-size=num         set v8's thread pool size\n"
         "  --zero-fill-buffers        automatically zero-fill all newly "
         "allocated\n"
         "                             Buffer and SlowBuffer instances\n"
         "  -c, --check                syntax check script without executing\n"
         "  -e, --eval script          evaluate script\n"
         "  -h, --help                 print node command line options\n"
         "  -i, --interactive          always enter the REPL even if stdin\n"
         "                             does not appear to be a terminal\n"
         "  -p, --print                evaluate script and print result\n"
         "  -r, --require              module to preload (option can be "
         "repeated)\n"
         "  -v, --version              print Node.js version\n"
         "\n"
         "Environment variables:\n"
         "NODE_DEBUG                   ','-separated list of core modules\n"
         "                             that should print debug information\n"
         "NODE_DEBUG_NATIVE            ','-separated list of C++ core debug\n"
         "                             categories that should print debug\n"
         "                             output\n"
         "NODE_DISABLE_COLORS          set to 1 to disable colors in the REPL\n"
         "NODE_EXTRA_CA_CERTS          path to additional CA certificates\n"
         "                             file\n"
#if defined(NODE_HAVE_I18N_SUPPORT)
         "NODE_ICU_DATA                data path for ICU (Intl object) data\n"
#if !defined(NODE_HAVE_SMALL_ICU)
         "                             (will extend linked-in data)\n"
#endif
#endif  // defined(NODE_HAVE_I18N_SUPPORT)
         "NODE_NO_WARNINGS             set to 1 to silence process warnings\n"
#if !defined(NODE_WITHOUT_NODE_OPTIONS)
         "NODE_OPTIONS                 set CLI options in the environment\n"
         "                             via a space-separated list\n"
#endif  // !defined(NODE_WITHOUT_NODE_OPTIONS)
#ifdef _WIN32
         "NODE_PATH                    ';'-separated list of directories\n"
#else
         "NODE_PATH                    ':'-separated list of directories\n"
#endif
         "                             prefixed to the module search path\n"
         "NODE_PENDING_DEPRECATION     set to 1 to emit pending deprecation\n"
         "                             warnings\n"
#if defined(NODE_HAVE_I18N_SUPPORT)
         "NODE_PRESERVE_SYMLINKS       set to 1 to preserve symbolic links\n"
         "                             when resolving and caching modules\n"
#endif
         "NODE_REDIRECT_WARNINGS       write warnings to path instead of\n"
         "                             stderr\n"
         "NODE_REPL_HISTORY            path to the persistent REPL history\n"
         "                             file\n"
         "OPENSSL_CONF                 load OpenSSL configuration from file\n"
         "\n"
         "Documentation can be found at https://nodejs.org/\n");
}


static bool ArgIsAllowed(const char* arg, const char* allowed) {
  for (; *arg && *allowed; arg++, allowed++) {
    // Like normal strcmp(), except that a '_' in `allowed` matches either a '-'
    // or '_' in `arg`.
    if (*allowed == '_') {
      if (!(*arg == '_' || *arg == '-'))
        return false;
    } else {
      if (*arg != *allowed)
        return false;
    }
  }

  // "--some-arg=val" is allowed for "--some-arg"
  if (*arg == '=')
    return true;

  // Both must be null, or one string is just a prefix of the other, not a
  // match.
  return !*arg && !*allowed;
}


static void CheckIfAllowedInEnv(const char* exe, bool is_env,
                                const char* arg) {
  if (!is_env)
    return;

  static const char* whitelist[] = {
    // Node options, sorted in `node --help` order for ease of comparison.
    // Please, update NODE_OPTIONS section in cli.md if changed.
    "--enable-fips",
    "--experimental-modules",
    "--experimental-repl-await",
    "--experimental-vm-modules",
    "--experimental-worker",
    "--force-fips",
    "--icu-data-dir",
    "--inspect",
    "--inspect-brk",
    "--inspect-port",
    "--loader",
    "--napi-modules",
    "--no-deprecation",
    "--no-force-async-hooks-checks",
    "--no-warnings",
    "--openssl-config",
    "--pending-deprecation",
    "--redirect-warnings",
    "--require",
    "--throw-deprecation",
    "--title",
    "--tls-cipher-list",
    "--trace-deprecation",
    "--trace-event-categories",
    "--trace-event-file-pattern",
    "--trace-events-enabled",
    "--trace-sync-io",
    "--trace-warnings",
    "--track-heap-objects",
    "--use-bundled-ca",
    "--use-openssl-ca",
    "--v8-pool-size",
    "--zero-fill-buffers",
    "-r",

    // V8 options (define with '_', which allows '-' or '_')
    "--abort_on_uncaught_exception",
    "--max_old_space_size",
    "--perf_basic_prof",
    "--perf_prof",
    "--stack_trace_limit",
  };

  for (unsigned i = 0; i < arraysize(whitelist); i++) {
    const char* allowed = whitelist[i];
    if (ArgIsAllowed(arg, allowed))
      return;
  }

  fprintf(stderr, "%s: %s is not allowed in NODE_OPTIONS\n", exe, arg);
  exit(9);
}


// Parse command line arguments.
//
// argv is modified in place. exec_argv and v8_argv are out arguments that
// ParseArgs() allocates memory for and stores a pointer to the output
// vector in.  The caller should free them with delete[].
//
// On exit:
//
//  * argv contains the arguments with node and V8 options filtered out.
//  * exec_argv contains both node and V8 options and nothing else.
//  * v8_argv contains argv[0] plus any V8 options
static void ParseArgs(int* argc,
                      const char** argv,
                      int* exec_argc,
                      const char*** exec_argv,
                      int* v8_argc,
                      const char*** v8_argv,
                      bool is_env) {
  const unsigned int nargs = static_cast<unsigned int>(*argc);
  const char** new_exec_argv = new const char*[nargs];
  const char** new_v8_argv = new const char*[nargs];
  const char** new_argv = new const char*[nargs];
#if HAVE_OPENSSL
  bool use_bundled_ca = false;
  bool use_openssl_ca = false;
#endif  // HAVE_OPENSSL

  for (unsigned int i = 0; i < nargs; ++i) {
    new_exec_argv[i] = nullptr;
    new_v8_argv[i] = nullptr;
    new_argv[i] = nullptr;
  }

  // exec_argv starts with the first option, the other two start with argv[0].
  unsigned int new_exec_argc = 0;
  unsigned int new_v8_argc = 1;
  unsigned int new_argc = 1;
  new_v8_argv[0] = argv[0];
  new_argv[0] = argv[0];

  unsigned int index = 1;
  bool short_circuit = false;
  while (index < nargs && argv[index][0] == '-' && !short_circuit) {
    const char* const arg = argv[index];
    unsigned int args_consumed = 1;

    CheckIfAllowedInEnv(argv[0], is_env, arg);

    if (debug_options.ParseOption(argv[0], arg)) {
      // Done, consumed by DebugOptions::ParseOption().
    } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
      printf("%s\n", NODE_VERSION);
      exit(0);
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      PrintHelp();
      exit(0);
    } else if (strcmp(arg, "--eval") == 0 ||
               strcmp(arg, "-e") == 0 ||
               strcmp(arg, "--print") == 0 ||
               strcmp(arg, "-pe") == 0 ||
               strcmp(arg, "-p") == 0) {
      bool is_eval = strchr(arg, 'e') != nullptr;
      bool is_print = strchr(arg, 'p') != nullptr;
      print_eval = print_eval || is_print;
      // --eval, -e and -pe always require an argument.
      if (is_eval == true) {
        args_consumed += 1;
        eval_string = argv[index + 1];
        if (eval_string == nullptr) {
          fprintf(stderr, "%s: %s requires an argument\n", argv[0], arg);
          exit(9);
        }
      } else if ((index + 1 < nargs) &&
                 argv[index + 1] != nullptr &&
                 argv[index + 1][0] != '-') {
        args_consumed += 1;
        eval_string = argv[index + 1];
        if (strncmp(eval_string, "\\-", 2) == 0) {
          // Starts with "\\-": escaped expression, drop the backslash.
          eval_string += 1;
        }
      }
    } else if (strcmp(arg, "--require") == 0 ||
               strcmp(arg, "-r") == 0) {
      const char* module = argv[index + 1];
      if (module == nullptr) {
        fprintf(stderr, "%s: %s requires an argument\n", argv[0], arg);
        exit(9);
      }
      args_consumed += 1;
      preload_modules.push_back(module);
    } else if (strcmp(arg, "--check") == 0 || strcmp(arg, "-c") == 0) {
      syntax_check_only = true;
    } else if (strcmp(arg, "--interactive") == 0 || strcmp(arg, "-i") == 0) {
      force_repl = true;
    } else if (strcmp(arg, "--no-deprecation") == 0) {
      no_deprecation = true;
    } else if (strcmp(arg, "--napi-modules") == 0) {
      // no-op
    } else if (strcmp(arg, "--no-warnings") == 0) {
      no_process_warnings = true;
    } else if (strcmp(arg, "--trace-warnings") == 0) {
      trace_warnings = true;
    } else if (strncmp(arg, "--redirect-warnings=", 20) == 0) {
      config_warning_file = arg + 20;
    } else if (strcmp(arg, "--trace-deprecation") == 0) {
      trace_deprecation = true;
    } else if (strcmp(arg, "--trace-sync-io") == 0) {
      trace_sync_io = true;
    } else if (strcmp(arg, "--no-force-async-hooks-checks") == 0) {
      no_force_async_hooks_checks = true;
    } else if (strcmp(arg, "--trace-events-enabled") == 0) {
      if (trace_enabled_categories.empty())
        trace_enabled_categories = "v8,node,node.async_hooks";
    } else if (strcmp(arg, "--trace-event-categories") == 0) {
      const char* categories = argv[index + 1];
      if (categories == nullptr) {
        fprintf(stderr, "%s: %s requires an argument\n", argv[0], arg);
        exit(9);
      }
      args_consumed += 1;
      trace_enabled_categories = categories;
    } else if (strcmp(arg, "--trace-event-file-pattern") == 0) {
      const char* file_pattern = argv[index + 1];
      if (file_pattern == nullptr) {
        fprintf(stderr, "%s: %s requires an argument\n", argv[0], arg);
        exit(9);
      }
      args_consumed += 1;
      trace_file_pattern = file_pattern;
    } else if (strcmp(arg, "--track-heap-objects") == 0) {
      track_heap_objects = true;
    } else if (strcmp(arg, "--throw-deprecation") == 0) {
      throw_deprecation = true;
    } else if (strncmp(arg, "--security-revert=", 18) == 0) {
      const char* cve = arg + 18;
      Revert(cve);
    } else if (strncmp(arg, "--title=", 8) == 0) {
      config_process_title = arg + 8;
    } else if (strcmp(arg, "--preserve-symlinks") == 0) {
      config_preserve_symlinks = true;
    } else if (strcmp(arg, "--preserve-symlinks-main") == 0) {
      config_preserve_symlinks_main = true;
    } else if (strcmp(arg, "--experimental-modules") == 0) {
      config_experimental_modules = true;
      new_v8_argv[new_v8_argc] = "--harmony-dynamic-import";
      new_v8_argc += 1;
      new_v8_argv[new_v8_argc] = "--harmony-import-meta";
      new_v8_argc += 1;
    } else if (strcmp(arg, "--experimental-vm-modules") == 0) {
      config_experimental_vm_modules = true;
    } else if (strcmp(arg, "--experimental-worker") == 0) {
      config_experimental_worker = true;
    } else if (strcmp(arg, "--experimental-repl-await") == 0) {
      config_experimental_repl_await = true;
    }  else if (strcmp(arg, "--loader") == 0) {
      const char* module = argv[index + 1];
      if (!config_experimental_modules) {
        fprintf(stderr, "%s: %s requires --experimental-modules be enabled\n",
            argv[0], arg);
        exit(9);
      }
      if (module == nullptr) {
        fprintf(stderr, "%s: %s requires an argument\n", argv[0], arg);
        exit(9);
      }
      args_consumed += 1;
      config_userland_loader = module;
    } else if (strcmp(arg, "--prof-process") == 0) {
      prof_process = true;
      short_circuit = true;
    } else if (strcmp(arg, "--zero-fill-buffers") == 0) {
      zero_fill_all_buffers = true;
    } else if (strcmp(arg, "--pending-deprecation") == 0) {
      config_pending_deprecation = true;
    } else if (strcmp(arg, "--v8-options") == 0) {
      new_v8_argv[new_v8_argc] = "--help";
      new_v8_argc += 1;
    } else if (strncmp(arg, "--v8-pool-size=", 15) == 0) {
      v8_thread_pool_size = atoi(arg + 15);
#if HAVE_OPENSSL
    } else if (strncmp(arg, "--tls-cipher-list=", 18) == 0) {
      default_cipher_list = arg + 18;
    } else if (strncmp(arg, "--use-openssl-ca", 16) == 0) {
      ssl_openssl_cert_store = true;
      use_openssl_ca = true;
    } else if (strncmp(arg, "--use-bundled-ca", 16) == 0) {
      use_bundled_ca = true;
      ssl_openssl_cert_store = false;
#if NODE_FIPS_MODE
    } else if (strcmp(arg, "--enable-fips") == 0) {
      enable_fips_crypto = true;
    } else if (strcmp(arg, "--force-fips") == 0) {
      force_fips_crypto = true;
#endif /* NODE_FIPS_MODE */
    } else if (strncmp(arg, "--openssl-config=", 17) == 0) {
      openssl_config.assign(arg + 17);
#endif /* HAVE_OPENSSL */
#if defined(NODE_HAVE_I18N_SUPPORT)
    } else if (strncmp(arg, "--icu-data-dir=", 15) == 0) {
      icu_data_dir.assign(arg + 15);
#endif
    } else if (strcmp(arg, "--expose-internals") == 0 ||
               strcmp(arg, "--expose_internals") == 0) {
      config_expose_internals = true;
    } else if (strcmp(arg, "-") == 0) {
      break;
    } else if (strcmp(arg, "--") == 0) {
      index += 1;
      break;
    } else if (strcmp(arg, "--abort-on-uncaught-exception") == 0 ||
               strcmp(arg, "--abort_on_uncaught_exception") == 0) {
      abort_on_uncaught_exception = true;
      // Also a V8 option.  Pass through as-is.
      new_v8_argv[new_v8_argc] = arg;
      new_v8_argc += 1;
    } else {
      // V8 option.  Pass through as-is.
      new_v8_argv[new_v8_argc] = arg;
      new_v8_argc += 1;
    }

    memcpy(new_exec_argv + new_exec_argc,
           argv + index,
           args_consumed * sizeof(*argv));

    new_exec_argc += args_consumed;
    index += args_consumed;
  }

#if HAVE_OPENSSL
  if (use_openssl_ca && use_bundled_ca) {
    fprintf(stderr,
            "%s: either --use-openssl-ca or --use-bundled-ca can be used, "
            "not both\n",
            argv[0]);
    exit(9);
  }
#endif

  if (eval_string != nullptr && syntax_check_only) {
    fprintf(stderr,
            "%s: either --check or --eval can be used, not both\n", argv[0]);
    exit(9);
  }

  // Copy remaining arguments.
  const unsigned int args_left = nargs - index;

  if (is_env && args_left) {
    fprintf(stderr, "%s: %s is not supported in NODE_OPTIONS\n",
            argv[0], argv[index]);
    exit(9);
  }

  memcpy(new_argv + new_argc, argv + index, args_left * sizeof(*argv));
  new_argc += args_left;

  *exec_argc = new_exec_argc;
  *exec_argv = new_exec_argv;
  *v8_argc = new_v8_argc;
  *v8_argv = new_v8_argv;

  // Copy new_argv over argv and update argc.
  memcpy(argv, new_argv, new_argc * sizeof(*argv));
  delete[] new_argv;
  *argc = static_cast<int>(new_argc);
}


static void StartInspector(Environment* env, const char* path,
                           DebugOptions debug_options) {
#if HAVE_INSPECTOR
  CHECK(!env->inspector_agent()->IsListening());
  v8_platform.StartInspector(env, path, debug_options);
#endif  // HAVE_INSPECTOR
}


#ifdef __POSIX__
void RegisterSignalHandler(int signal,
                           void (*handler)(int signal),
                           bool reset_handler) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
#ifndef __FreeBSD__
  // FreeBSD has a nasty bug with SA_RESETHAND reseting the SA_SIGINFO, that is
  // in turn set for a libthr wrapper. This leads to a crash.
  // Work around the issue by manually setting SIG_DFL in the signal handler
  sa.sa_flags = reset_handler ? SA_RESETHAND : 0;
#endif
  sigfillset(&sa.sa_mask);
  CHECK_EQ(sigaction(signal, &sa, nullptr), 0);
}


void DebugProcess(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  if (args.Length() != 1) {
    return env->ThrowError("Invalid number of arguments.");
  }

  pid_t pid;
  int r;

  pid = args[0]->IntegerValue();
  r = kill(pid, SIGUSR1);
  if (r != 0) {
    return env->ThrowErrnoException(errno, "kill");
  }
}
#endif  // __POSIX__


#ifdef _WIN32
static int GetDebugSignalHandlerMappingName(DWORD pid, wchar_t* buf,
    size_t buf_len) {
  return _snwprintf(buf, buf_len, L"node-debug-handler-%u", pid);
}


static void DebugProcess(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = args.GetIsolate();
  DWORD pid;
  HANDLE process = nullptr;
  HANDLE thread = nullptr;
  HANDLE mapping = nullptr;
  wchar_t mapping_name[32];
  LPTHREAD_START_ROUTINE* handler = nullptr;

  if (args.Length() != 1) {
    env->ThrowError("Invalid number of arguments.");
    goto out;
  }

  pid = (DWORD) args[0]->IntegerValue();

  process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                            PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                            PROCESS_VM_READ,
                        FALSE,
                        pid);
  if (process == nullptr) {
    isolate->ThrowException(
        WinapiErrnoException(isolate, GetLastError(), "OpenProcess"));
    goto out;
  }

  if (GetDebugSignalHandlerMappingName(pid,
                                       mapping_name,
                                       arraysize(mapping_name)) < 0) {
    env->ThrowErrnoException(errno, "sprintf");
    goto out;
  }

  mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name);
  if (mapping == nullptr) {
    isolate->ThrowException(WinapiErrnoException(isolate,
                                             GetLastError(),
                                             "OpenFileMappingW"));
    goto out;
  }

  handler = reinterpret_cast<LPTHREAD_START_ROUTINE*>(
      MapViewOfFile(mapping,
                    FILE_MAP_READ,
                    0,
                    0,
                    sizeof *handler));
  if (handler == nullptr || *handler == nullptr) {
    isolate->ThrowException(
        WinapiErrnoException(isolate, GetLastError(), "MapViewOfFile"));
    goto out;
  }

  thread = CreateRemoteThread(process,
                              nullptr,
                              0,
                              *handler,
                              nullptr,
                              0,
                              nullptr);
  if (thread == nullptr) {
    isolate->ThrowException(WinapiErrnoException(isolate,
                                                 GetLastError(),
                                                 "CreateRemoteThread"));
    goto out;
  }

  // Wait for the thread to terminate
  if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) {
    isolate->ThrowException(WinapiErrnoException(isolate,
                                                 GetLastError(),
                                                 "WaitForSingleObject"));
    goto out;
  }

 out:
  if (process != nullptr)
    CloseHandle(process);
  if (thread != nullptr)
    CloseHandle(thread);
  if (handler != nullptr)
    UnmapViewOfFile(handler);
  if (mapping != nullptr)
    CloseHandle(mapping);
}
#endif  // _WIN32


static void DebugEnd(const FunctionCallbackInfo<Value>& args) {
#if HAVE_INSPECTOR
  Environment* env = Environment::GetCurrent(args);
  if (env->inspector_agent()->IsListening()) {
    env->inspector_agent()->Stop();
  }
#endif
}


inline void PlatformInit() {
#ifdef __POSIX__
#if HAVE_INSPECTOR
  sigset_t sigmask;
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGUSR1);
  const int err = pthread_sigmask(SIG_SETMASK, &sigmask, nullptr);
#endif  // HAVE_INSPECTOR

  // Make sure file descriptors 0-2 are valid before we start logging anything.
  for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd += 1) {
    struct stat ignored;
    if (fstat(fd, &ignored) == 0)
      continue;
    // Anything but EBADF means something is seriously wrong.  We don't
    // have to special-case EINTR, fstat() is not interruptible.
    if (errno != EBADF)
      ABORT();
    if (fd != open("/dev/null", O_RDWR))
      ABORT();
  }

#if HAVE_INSPECTOR
  CHECK_EQ(err, 0);
#endif  // HAVE_INSPECTOR

#ifndef NODE_SHARED_MODE
  // Restore signal dispositions, the parent process may have changed them.
  struct sigaction act;
  memset(&act, 0, sizeof(act));

  // The hard-coded upper limit is because NSIG is not very reliable; on Linux,
  // it evaluates to 32, 34 or 64, depending on whether RT signals are enabled.
  // Counting up to SIGRTMIN doesn't work for the same reason.
  for (unsigned nr = 1; nr < kMaxSignal; nr += 1) {
    if (nr == SIGKILL || nr == SIGSTOP)
      continue;
    act.sa_handler = (nr == SIGPIPE) ? SIG_IGN : SIG_DFL;
    CHECK_EQ(0, sigaction(nr, &act, nullptr));
  }
#endif  // !NODE_SHARED_MODE

  RegisterSignalHandler(SIGINT, SignalExit, true);
  RegisterSignalHandler(SIGTERM, SignalExit, true);

  // Raise the open file descriptor limit.
  struct rlimit lim;
  if (getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur != lim.rlim_max) {
    // Do a binary search for the limit.
    rlim_t min = lim.rlim_cur;
    rlim_t max = 1 << 20;
    // But if there's a defined upper bound, don't search, just set it.
    if (lim.rlim_max != RLIM_INFINITY) {
      min = lim.rlim_max;
      max = lim.rlim_max;
    }
    do {
      lim.rlim_cur = min + (max - min) / 2;
      if (setrlimit(RLIMIT_NOFILE, &lim)) {
        max = lim.rlim_cur;
      } else {
        min = lim.rlim_cur;
      }
    } while (min + 1 < max);
  }
#endif  // __POSIX__
#ifdef _WIN32
  for (int fd = 0; fd <= 2; ++fd) {
    auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE ||
        GetFileType(handle) == FILE_TYPE_UNKNOWN) {
      // Ignore _close result. If it fails or not depends on used Windows
      // version. We will just check _open result.
      _close(fd);
      if (fd != _open("nul", _O_RDWR))
        ABORT();
    }
  }
#endif  // _WIN32
}


void ProcessArgv(int* argc,
                 const char** argv,
                 int* exec_argc,
                 const char*** exec_argv,
                 bool is_env = false) {
  // Parse a few arguments which are specific to Node.
  int v8_argc;
  const char** v8_argv;
  ParseArgs(argc, argv, exec_argc, exec_argv, &v8_argc, &v8_argv, is_env);

  // TODO(bnoordhuis) Intercept --prof arguments and start the CPU profiler
  // manually?  That would give us a little more control over its runtime
  // behavior but it could also interfere with the user's intentions in ways
  // we fail to anticipate.  Dillema.
  for (int i = 1; i < v8_argc; ++i) {
    if (strncmp(v8_argv[i], "--prof", sizeof("--prof") - 1) == 0) {
      v8_is_profiling = true;
      break;
    }
  }

#ifdef __POSIX__
  // Block SIGPROF signals when sleeping in epoll_wait/kevent/etc.  Avoids the
  // performance penalty of frequent EINTR wakeups when the profiler is running.
  // Only do this for v8.log profiling, as it breaks v8::CpuProfiler users.
  if (v8_is_profiling) {
    uv_loop_configure(uv_default_loop(), UV_LOOP_BLOCK_SIGNAL, SIGPROF);
  }
#endif

  // The const_cast doesn't violate conceptual const-ness.  V8 doesn't modify
  // the argv array or the elements it points to.
  if (v8_argc > 1)
    V8::SetFlagsFromCommandLine(&v8_argc, const_cast<char**>(v8_argv), true);

  // Anything that's still in v8_argv is not a V8 or a node option.
  for (int i = 1; i < v8_argc; i++) {
    fprintf(stderr, "%s: bad option: %s\n", argv[0], v8_argv[i]);
  }
  delete[] v8_argv;
  v8_argv = nullptr;

  if (v8_argc > 1) {
    exit(9);
  }
}


void Init(int* argc,
          const char** argv,
          int* exec_argc,
          const char*** exec_argv) {
  // Initialize prog_start_time to get relative uptime.
  prog_start_time = static_cast<double>(uv_now(uv_default_loop()));

  // Register built-in modules
  RegisterBuiltinModules();

  // Make inherited handles noninheritable.
  uv_disable_stdio_inheritance();

#if defined(NODE_V8_OPTIONS)
  // Should come before the call to V8::SetFlagsFromCommandLine()
  // so the user can disable a flag --foo at run-time by passing
  // --no_foo from the command line.
  V8::SetFlagsFromString(NODE_V8_OPTIONS, sizeof(NODE_V8_OPTIONS) - 1);
#endif

  {
    std::string text;
    config_pending_deprecation =
        SafeGetenv("NODE_PENDING_DEPRECATION", &text) && text[0] == '1';
  }

  // Allow for environment set preserving symlinks.
  {
    std::string text;
    config_preserve_symlinks =
        SafeGetenv("NODE_PRESERVE_SYMLINKS", &text) && text[0] == '1';
  }

  {
    std::string text;
    config_preserve_symlinks_main =
        SafeGetenv("NODE_PRESERVE_SYMLINKS_MAIN", &text) && text[0] == '1';
  }

  if (config_warning_file.empty())
    SafeGetenv("NODE_REDIRECT_WARNINGS", &config_warning_file);

#if HAVE_OPENSSL
  if (openssl_config.empty())
    SafeGetenv("OPENSSL_CONF", &openssl_config);
#endif

#if !defined(NODE_WITHOUT_NODE_OPTIONS)
  std::string node_options;
  if (SafeGetenv("NODE_OPTIONS", &node_options)) {
    // Smallest tokens are 2-chars (a not space and a space), plus 2 extra
    // pointers, for the prepended executable name, and appended NULL pointer.
    size_t max_len = 2 + (node_options.length() + 1) / 2;
    const char** argv_from_env = new const char*[max_len];
    int argc_from_env = 0;
    // [0] is expected to be the program name, fill it in from the real argv.
    argv_from_env[argc_from_env++] = argv[0];

    char* cstr = strdup(node_options.c_str());
    char* initptr = cstr;
    char* token;
    while ((token = strtok(initptr, " "))) {  // NOLINT(runtime/threadsafe_fn)
      initptr = nullptr;
      argv_from_env[argc_from_env++] = token;
    }
    argv_from_env[argc_from_env] = nullptr;
    int exec_argc_;
    const char** exec_argv_ = nullptr;
    ProcessArgv(&argc_from_env, argv_from_env, &exec_argc_, &exec_argv_, true);
    delete[] exec_argv_;
    delete[] argv_from_env;
    free(cstr);
  }
#endif

  ProcessArgv(argc, argv, exec_argc, exec_argv);

  // Set the process.title immediately after processing argv if --title is set.
  if (!config_process_title.empty())
    uv_set_process_title(config_process_title.c_str());

#if defined(NODE_HAVE_I18N_SUPPORT)
  // If the parameter isn't given, use the env variable.
  if (icu_data_dir.empty())
    SafeGetenv("NODE_ICU_DATA", &icu_data_dir);
  // Initialize ICU.
  // If icu_data_dir is empty here, it will load the 'minimal' data.
  if (!i18n::InitializeICUDirectory(icu_data_dir)) {
    fprintf(stderr,
            "%s: could not initialize ICU "
            "(check NODE_ICU_DATA or --icu-data-dir parameters)\n",
            argv[0]);
    exit(9);
  }
#endif

  // We should set node_is_initialized here instead of in node::Start,
  // otherwise embedders using node::Init to initialize everything will not be
  // able to set it and native modules will not load for them.
  node_is_initialized = true;
}


void RunAtExit(Environment* env) {
  env->RunAtExitCallbacks();
}


uv_loop_t* GetCurrentEventLoop(v8::Isolate* isolate) {
  HandleScope handle_scope(isolate);
  auto context = isolate->GetCurrentContext();
  if (context.IsEmpty())
    return nullptr;
  return Environment::GetCurrent(context)->event_loop();
}


void AtExit(void (*cb)(void* arg), void* arg) {
  auto env = Environment::GetThreadLocalEnv();
  AtExit(env, cb, arg);
}


void AtExit(Environment* env, void (*cb)(void* arg), void* arg) {
  CHECK_NOT_NULL(env);
  env->AtExit(cb, arg);
}


void RunBeforeExit(Environment* env) {
  env->RunBeforeExitCallbacks();

  if (!uv_loop_alive(env->event_loop()))
    EmitBeforeExit(env);
}


void EmitBeforeExit(Environment* env) {
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());
  Local<Object> process_object = env->process_object();
  Local<String> exit_code = env->exit_code_string();
  Local<Value> args[] = {
    FIXED_ONE_BYTE_STRING(env->isolate(), "beforeExit"),
    process_object->Get(env->context(), exit_code).ToLocalChecked()
        ->ToInteger(env->context()).ToLocalChecked()
  };
  MakeCallback(env->isolate(),
               process_object, "emit", arraysize(args), args,
               {0, 0}).ToLocalChecked();
}


int EmitExit(Environment* env) {
  // process.emit('exit')
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());
  Local<Object> process_object = env->process_object();
  process_object->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "_exiting"),
                      True(env->isolate()));

  Local<String> exit_code = env->exit_code_string();
  int code = process_object->Get(env->context(), exit_code).ToLocalChecked()
      ->Int32Value(env->context()).ToChecked();

  Local<Value> args[] = {
    FIXED_ONE_BYTE_STRING(env->isolate(), "exit"),
    Integer::New(env->isolate(), code)
  };

  MakeCallback(env->isolate(),
               process_object, "emit", arraysize(args), args,
               {0, 0}).ToLocalChecked();

  // Reload exit code, it may be changed by `emit('exit')`
  return process_object->Get(env->context(), exit_code).ToLocalChecked()
      ->Int32Value(env->context()).ToChecked();
}


ArrayBufferAllocator* CreateArrayBufferAllocator() {
  return new ArrayBufferAllocator();
}


void FreeArrayBufferAllocator(ArrayBufferAllocator* allocator) {
  delete allocator;
}


IsolateData* CreateIsolateData(Isolate* isolate, uv_loop_t* loop) {
  return new IsolateData(isolate, loop, nullptr);
}


IsolateData* CreateIsolateData(
    Isolate* isolate,
    uv_loop_t* loop,
    MultiIsolatePlatform* platform) {
  return new IsolateData(isolate, loop, platform);
}


IsolateData* CreateIsolateData(
    Isolate* isolate,
    uv_loop_t* loop,
    MultiIsolatePlatform* platform,
    ArrayBufferAllocator* allocator) {
  return new IsolateData(isolate, loop, platform, allocator->zero_fill_field());
}


void FreeIsolateData(IsolateData* isolate_data) {
  delete isolate_data;
}


Environment* CreateEnvironment(IsolateData* isolate_data,
                               Local<Context> context,
                               int argc,
                               const char* const* argv,
                               int exec_argc,
                               const char* const* exec_argv) {
  Isolate* isolate = context->GetIsolate();
  HandleScope handle_scope(isolate);
  Context::Scope context_scope(context);
  auto env = new Environment(isolate_data, context,
                             v8_platform.GetTracingAgent());
  env->Start(argc, argv, exec_argc, exec_argv, v8_is_profiling);
  return env;
}


void FreeEnvironment(Environment* env) {
  env->RunCleanup();
  delete env;
}


MultiIsolatePlatform* GetMainThreadMultiIsolatePlatform() {
  return v8_platform.Platform();
}


MultiIsolatePlatform* CreatePlatform(
    int thread_pool_size,
    v8::TracingController* tracing_controller) {
  return new NodePlatform(thread_pool_size, tracing_controller);
}


MultiIsolatePlatform* InitializeV8Platform(int thread_pool_size) {
  v8_platform.Initialize(thread_pool_size);
  return v8_platform.Platform();
}


void FreePlatform(MultiIsolatePlatform* platform) {
  delete platform;
}


Local<Context> NewContext(Isolate* isolate,
                          Local<ObjectTemplate> object_template) {
  auto context = Context::New(isolate, nullptr, object_template);
  if (context.IsEmpty()) return context;
  HandleScope handle_scope(isolate);

  context->SetEmbedderData(
      ContextEmbedderIndex::kAllowWasmCodeGeneration, True(isolate));

  {
    // Run lib/internal/per_context.js
    Context::Scope context_scope(context);
    Local<String> per_context = NodePerContextSource(isolate);
    v8::ScriptCompiler::Source per_context_src(per_context, nullptr);
    Local<v8::Script> s = v8::ScriptCompiler::Compile(
        context,
        &per_context_src).ToLocalChecked();
    s->Run(context).ToLocalChecked();
  }

  return context;
}


inline int Start(Isolate* isolate, IsolateData* isolate_data,
                 int argc, const char* const* argv,
                 int exec_argc, const char* const* exec_argv) {
  HandleScope handle_scope(isolate);
  Local<Context> context = NewContext(isolate);
  Context::Scope context_scope(context);
  Environment env(isolate_data, context, v8_platform.GetTracingAgent());
  env.Start(argc, argv, exec_argc, exec_argv, v8_is_profiling);

  const char* path = argc > 1 ? argv[1] : nullptr;
  StartInspector(&env, path, debug_options);

  if (debug_options.inspector_enabled() && !v8_platform.InspectorStarted(&env))
    return 12;  // Signal internal error.

  env.set_abort_on_uncaught_exception(abort_on_uncaught_exception);

  if (no_force_async_hooks_checks) {
    env.async_hooks()->no_force_checks();
  }

  {
    Environment::AsyncCallbackScope callback_scope(&env);
    env.async_hooks()->push_async_ids(1, 0);
    LoadEnvironment(&env);
    env.async_hooks()->pop_async_id(1);
  }

  env.set_trace_sync_io(trace_sync_io);

  {
    SealHandleScope seal(isolate);
    bool more;
    env.performance_state()->Mark(
        node::performance::NODE_PERFORMANCE_MILESTONE_LOOP_START);
    do {
      uv_run(env.event_loop(), UV_RUN_DEFAULT);

      v8_platform.DrainVMTasks(isolate);

      more = uv_loop_alive(env.event_loop());
      if (more)
        continue;

      RunBeforeExit(&env);

      // Emit `beforeExit` if the loop became alive either after emitting
      // event, or after running some callbacks.
      more = uv_loop_alive(env.event_loop());
    } while (more == true);
    env.performance_state()->Mark(
        node::performance::NODE_PERFORMANCE_MILESTONE_LOOP_EXIT);
  }

  env.set_trace_sync_io(false);

  const int exit_code = EmitExit(&env);

  WaitForInspectorDisconnect(&env);

  env.set_can_call_into_js(false);
  env.stop_sub_worker_contexts();
  uv_tty_reset_mode();
  env.RunCleanup();
  RunAtExit(&env);

  v8_platform.DrainVMTasks(isolate);
  v8_platform.CancelVMTasks(isolate);
#if defined(LEAK_SANITIZER)
  __lsan_do_leak_check();
#endif

  return exit_code;
}

bool AllowWasmCodeGenerationCallback(
    Local<Context> context, Local<String>) {
  Local<Value> wasm_code_gen =
    context->GetEmbedderData(ContextEmbedderIndex::kAllowWasmCodeGeneration);
  return wasm_code_gen->IsUndefined() || wasm_code_gen->IsTrue();
}

Isolate* NewIsolate(ArrayBufferAllocator* allocator, uv_loop_t* event_loop) {
  Isolate::CreateParams params;
  params.array_buffer_allocator = allocator;
#ifdef NODE_ENABLE_VTUNE_PROFILING
  params.code_event_handler = vTune::GetVtuneCodeEventHandler();
#endif

  Isolate* isolate = Isolate::Allocate();
  if (isolate == nullptr)
    return nullptr;

  // Register the isolate on the platform before the isolate gets initialized,
  // so that the isolate can access the platform during initialization.
  v8_platform.Platform()->RegisterIsolate(isolate, event_loop);
  Isolate::Initialize(isolate, params);

  isolate->AddMessageListener(OnMessage);
  isolate->SetAbortOnUncaughtExceptionCallback(ShouldAbortOnUncaughtException);
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  isolate->SetFatalErrorHandler(OnFatalError);
  isolate->SetAllowWasmCodeGenerationCallback(AllowWasmCodeGenerationCallback);

  return isolate;
}

inline int Start(uv_loop_t* event_loop,
                 int argc, const char* const* argv,
                 int exec_argc, const char* const* exec_argv) {
  std::unique_ptr<ArrayBufferAllocator, decltype(&FreeArrayBufferAllocator)>
      allocator(CreateArrayBufferAllocator(), &FreeArrayBufferAllocator);
  Isolate* const isolate = NewIsolate(allocator.get(), event_loop);
  if (isolate == nullptr)
    return 12;  // Signal internal error.

  {
    Mutex::ScopedLock scoped_lock(node_isolate_mutex);
    CHECK_NULL(node_isolate);
    node_isolate = isolate;
  }

  int exit_code;
  {
    Locker locker(isolate);
    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);
    std::unique_ptr<IsolateData, decltype(&FreeIsolateData)> isolate_data(
        CreateIsolateData(
            isolate,
            event_loop,
            v8_platform.Platform(),
            allocator.get()),
        &FreeIsolateData);
    if (track_heap_objects) {
      isolate->GetHeapProfiler()->StartTrackingHeapObjects(true);
    }
    exit_code =
        Start(isolate, isolate_data.get(), argc, argv, exec_argc, exec_argv);
  }

  {
    Mutex::ScopedLock scoped_lock(node_isolate_mutex);
    CHECK_EQ(node_isolate, isolate);
    node_isolate = nullptr;
  }

  isolate->Dispose();
  v8_platform.Platform()->UnregisterIsolate(isolate);

  return exit_code;
}

int Start(int argc, char** argv) {
  atexit([] () { uv_tty_reset_mode(); });
  PlatformInit();
  performance::performance_node_start = PERFORMANCE_NOW();

  CHECK_GT(argc, 0);

  // Hack around with the argv pointer. Used for process.title = "blah".
  argv = uv_setup_args(argc, argv);

  // This needs to run *before* V8::Initialize().  The const_cast is not
  // optional, in case you're wondering.
  int exec_argc;
  const char** exec_argv;
  Init(&argc, const_cast<const char**>(argv), &exec_argc, &exec_argv);

#if HAVE_OPENSSL
  {
    std::string extra_ca_certs;
    if (SafeGetenv("NODE_EXTRA_CA_CERTS", &extra_ca_certs))
      crypto::UseExtraCaCerts(extra_ca_certs);
  }
#ifdef NODE_FIPS_MODE
  // In the case of FIPS builds we should make sure
  // the random source is properly initialized first.
  OPENSSL_init();
#endif  // NODE_FIPS_MODE
  // V8 on Windows doesn't have a good source of entropy. Seed it from
  // OpenSSL's pool.
  V8::SetEntropySource(crypto::EntropySource);
#endif  // HAVE_OPENSSL

  InitializeV8Platform(v8_thread_pool_size);
  V8::Initialize();
  performance::performance_v8_start = PERFORMANCE_NOW();
  v8_initialized = true;
  const int exit_code =
      Start(uv_default_loop(), argc, argv, exec_argc, exec_argv);
  v8_platform.StopTracingAgent();
  v8_initialized = false;
  V8::Dispose();

  // uv_run cannot be called from the time before the beforeExit callback
  // runs until the program exits unless the event loop has any referenced
  // handles after beforeExit terminates. This prevents unrefed timers
  // that happen to terminate during shutdown from being run unsafely.
  // Since uv_run cannot be called, uv_async handles held by the platform
  // will never be fully cleaned up.
  v8_platform.Dispose();

  delete[] exec_argv;
  exec_argv = nullptr;

  return exit_code;
}

// Call built-in modules' _register_<module name> function to
// do module registration explicitly.
void RegisterBuiltinModules() {
#define V(modname) _register_##modname();
  NODE_BUILTIN_MODULES(V)
#undef V
}

}  // namespace node

#if !HAVE_INSPECTOR
void Initialize() {}

NODE_BUILTIN_MODULE_CONTEXT_AWARE(inspector, Initialize)
#endif  // !HAVE_INSPECTOR
