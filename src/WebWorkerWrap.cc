#include "src/WebWorkerWrap.h"
#include <uv.h>
#include <unistd.h>
#include <pthread.h>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace v8;
using namespace node;
using namespace std;

#define SKIP_RESULE(body) \
  MaybeLocal<Value> res = body; \
  (void)res;

void WebWorkerWrap::Writeln(const FunctionCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  {
    HandleScope scope(isolate);
    char* text = strdup(*String::Utf8Value(info[0]));
    fprintf(stdout, "%s\n", text);
    free(text);
  }
}

void WebWorkerWrap::Compile(const FunctionCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  {
    HandleScope scope(isolate);
    WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(isolate->GetData(0));

    char* pathname = strdup(*String::Utf8Value(info[0]));
    std::ifstream ifs(pathname);
    if (!ifs) {
      info.GetReturnValue().Set(Boolean::New(isolate, false));
      return;
    }
    std::string source(
      (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    std::stringstream contents;
    contents << "(function(exports, module, require, __dirname){" << source << "})";
    Local<Function> compiled = worker->Compile(pathname, contents.str().c_str());
    info.GetReturnValue().Set(compiled);
    free(pathname);
  }
}

void WebWorkerWrap::CheckPoint(const FunctionCallbackInfo<Value>& info) {
  pthread_testcancel();
}

void WebWorkerWrap::DefineRemoteMethod(const FunctionCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  {
    HandleScope scope(isolate);
    Local<Context> context = isolate->GetCurrentContext();
    WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(isolate->GetData(0));

    MaybeLocal<String> serializedParams = JSON::Stringify(context, info[1]->ToObject());
    worker->request_name = strdup(*String::Utf8Value(info[0]));
    worker->request_args = strdup(*String::Utf8Value(serializedParams.ToLocalChecked()));
    uv_async_send(&worker->master_handle);
    uv_sem_wait(&worker->request_locker);

    {
      // deserialize the returned string
      ValueDeserializer* deserializer = new ValueDeserializer(isolate,
        (const uint8_t*)worker->request_returns.Data(), worker->request_returns.ByteLength());
      Maybe<bool> r = deserializer->ReadHeader(context);
      if (!r.FromJust()) {
        Nan::ThrowError("Unknown Header Parsing.");
        return;
      }
      Local<Value> val = deserializer->ReadValue(context).ToLocalChecked();
      info.GetReturnValue().Set(val);
    }

    // frees the request objects
    if (worker->request_name) {
      delete worker->request_name;
      worker->request_name = NULL;
    }
    if (worker->request_args) {
      delete worker->request_args;
      worker->request_args = NULL;
    }
  }
}

void WebWorkerWrap::QueueCallback(const FunctionCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  {
    HandleScope scope(isolate);
    WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(isolate->GetData(0));
    char* id = strdup(*String::Utf8Value(info[0]));
    worker->callbacks_[id].Reset(isolate, Local<Function>::Cast(info[1]));
    free(id);
  }
}

Local<Function> WebWorkerWrap::Compile(const char* name_, const char* source_) {
  Isolate* isolate = worker_isolate;
  EscapableHandleScope scope(isolate);
  TryCatch try_catch(isolate);

  Local<Context> context = isolate->GetCurrentContext();
  MaybeLocal<String> name = Nan::New(name_);
  MaybeLocal<String> source = Nan::New(source_);
  ScriptOrigin origin(name.ToLocalChecked());
  MaybeLocal<Script> script = Script::Compile(context, source.ToLocalChecked(), &origin);
  if (script.IsEmpty()) {
    if (try_catch.HasCaught())
      WebWorkerWrap::ReportError(&try_catch);
    exit(0);
  }
  Local<Value> result = script.ToLocalChecked()->Run();
  if (result.IsEmpty()) {
    if (try_catch.HasCaught())
      WebWorkerWrap::ReportError(&try_catch);
    exit(0);
  }
  return scope.Escape(Local<Function>::Cast(result));
}

Local<Function> WebWorkerWrap::GetBootstrapScript() {
  stringstream scriptAbsPath;
  scriptAbsPath << root << "/src/bootstrap_worker.js";
  std::ifstream ifs(scriptAbsPath.str());
  std::string script_source(
    (std::istreambuf_iterator<char>(ifs)), 
    std::istreambuf_iterator<char>());
  return Compile("bootstrap_worker.js", script_source.c_str());
}

void WebWorkerWrap::ReportError(TryCatch* try_catch) {
  Isolate* isolate = v8::Isolate::GetCurrent();
  EscapableHandleScope scope(isolate);
  {
    Local<String> stack = Local<String>::Cast(try_catch->StackTrace());
    fprintf(stderr, "%s\n", *String::Utf8Value(stack));
  }
}

void WebWorkerWrap::CleanupThread(void* data) {
  WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(data);
  if (worker->worker_isolate_exited == 0) {
    worker->worker_isolate->Exit();
  }
  worker->worker_isolate->Dispose();
  worker->Deinit();
}

void WebWorkerWrap::CreateTask(void* data) {
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
  pthread_testcancel();

  WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(data);
  ArrayBuffer::Allocator* allocator = ArrayBuffer::Allocator::NewDefaultAllocator();
  Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = allocator;

  // TODO(Yorkie): Isolate::New takes long time, the isolations 
  // pool is required for performance.
  Isolate* isolate = Isolate::New(create_params);
  pthread_cleanup_push(WebWorkerWrap::CleanupThread, worker);

  worker->InitThread(isolate);
  {
    Locker locker(isolate);
    isolate->SetData(0, (void*)worker);

    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);
    Local<Context> context = Context::New(isolate);
    Context::Scope context_scope(context);
    worker->worker_context = context;
    worker->worker_context->Enter();

    TryCatch try_catch(isolate);
    try_catch.SetVerbose(false);

    Local<Function> bootstrap = worker->GetBootstrapScript();
    Local<Function> script = worker->Compile("WebWorkerProgress.js", worker->source);

    Local<Object> jsworker = Object::New(isolate);
    NODE_SET_METHOD(jsworker, "defineRemoteMethod", DefineRemoteMethod);
    NODE_SET_METHOD(jsworker, "queueCallback", QueueCallback);

    Local<Object> global = context->Global();
    NODE_SET_METHOD(global, "$writeln", WebWorkerWrap::Writeln);
    NODE_SET_METHOD(global, "$compile", WebWorkerWrap::Compile);
    NODE_SET_METHOD(global, "$checkpoint", WebWorkerWrap::CheckPoint);

    // call bootstrap_worker.js
    {
      Local<Value> argv[5];
      argv[0] = jsworker;
      argv[1] = global;
      argv[2] = script;

      ValueDeserializer* deserializer = new ValueDeserializer(isolate,
        (const uint8_t*)worker->script_args.Data(), worker->script_args.ByteLength());
      Maybe<bool> r = deserializer->ReadHeader(context);
      if (!r.FromJust()) {
        Nan::ThrowError("Unknown Header Parsing");
      } else {

        argv[3] = deserializer->ReadValue(context).ToLocalChecked();
        argv[4] = Nan::New(worker->root).ToLocalChecked();
        SKIP_RESULE(bootstrap->Call(context, jsworker, 5, argv));
      }
    }

    if (try_catch.HasCaught()) {
      WebWorkerWrap::ReportError(&try_catch);
    } else {
      // check if any callbacks.
      while (true) {
        uv_sem_wait(&worker->worker_locker);
        if (worker->should_terminate)
          break;
        Local<Function> cb = Local<Function>::New(isolate, worker->callbacks_[worker->callback_id]);
        worker->WorkerCallback(cb);

        // check if has error should be reported.
        if (try_catch.HasCaught()) {
          WebWorkerWrap::ReportError(&try_catch);
          break;
        }
      }
    }
    worker->worker_context->Exit();
    worker->worker_isolate_exited = 1;
  }
  pthread_cleanup_pop(1);
}

void WebWorkerWrap::InitThread(Isolate* isolate) {
  worker_isolate = isolate;
  uv_sem_init(&worker_locker, 0);
  uv_sem_init(&request_locker, 0);
}

void WebWorkerWrap::Deinit() {
  if (destroyed_) return;
  destroyed_ = true;

  uv_close((uv_handle_t*)&master_handle, NULL);
  uv_sem_destroy(&worker_locker);
  uv_sem_destroy(&request_locker);
  onrequest_.Reset();
  if (root != NULL)
    delete root;
  if (source != NULL)
    delete source;
  if (callback_id != NULL)
    delete callback_id;
  if (request_name != NULL)
    delete request_name;
  if (request_args != NULL)
    delete request_args;
}

void WebWorkerWrap::MasterCallback(uv_async_t* handle) {
  WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(handle->data);
  Nan::HandleScope scope;

  Isolate* isolate = Isolate::GetCurrent();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Function> onrequest = Local<Function>::New(isolate, worker->onrequest_);

  Local<Value> args[2];
  args[0] = Nan::New(worker->request_name).ToLocalChecked();
  args[1] = JSON::Parse(context, Nan::New(worker->request_args).ToLocalChecked()).ToLocalChecked();

  Local<Value> res = onrequest->Call(worker->handle(), 2, args);
  worker->request_returns = Local<SharedArrayBuffer>::Cast(res)->Externalize();
  uv_sem_post(&worker->request_locker);
}

void WebWorkerWrap::WorkerCallback(Local<Function> cb) {
  Isolate* isolate = v8::Isolate::GetCurrent();
  Local<Context> context = isolate->GetCurrentContext();
  {
    HandleScope scope(isolate);
    Local<Value> argv[1];

    ValueDeserializer* deserializer = new ValueDeserializer(isolate, 
      (const uint8_t*)callback_data.Data(), callback_data.ByteLength());
    Maybe<bool> r = deserializer->ReadHeader(context);
    if (!r.FromJust()) {
      Nan::ThrowError("Unknown Header Parsing");
    } else {
      argv[0] = deserializer->ReadValue(context).ToLocalChecked();
      SKIP_RESULE(cb->Call(context, context->Global(), 1, argv));
    }
  }
}

WebWorkerWrap::WebWorkerWrap(const char* source_) {
  source = strdup(source_);
  master_handle.data = this;
  uv_async_init(uv_default_loop(), &master_handle, &WebWorkerWrap::MasterCallback);
}

WebWorkerWrap::~WebWorkerWrap() {
  Deinit();
}

NAN_MODULE_INIT(WebWorkerWrap::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("WebWorkerWrap").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetPrototypeMethod(tpl, "send", Send);
  Nan::SetPrototypeMethod(tpl, "start", Start);
  Nan::SetPrototypeMethod(tpl, "terminate", Terminate);
  Nan::SetPrototypeMethod(tpl, "forceTerminate", ForceTerminate);
  Nan::SetAccessor(tpl->InstanceTemplate(), 
    Nan::New<String>("destroyed").ToLocalChecked(), GetDestroyed);

  Local<Function> func = Nan::GetFunction(tpl).ToLocalChecked();
  Nan::Set(target, Nan::New("WebWorkerWrap").ToLocalChecked(), func);
}

NAN_METHOD(WebWorkerWrap::New) {
  Local<String> root = info[0].As<String>();
  Local<Function> script = Local<Function>::Cast(info[1]);
  Local<Function> onrequest = Local<Function>::Cast(info[2]);
  script->SetName(Nan::New<String>("WebWorkerProgress").ToLocalChecked());
  {
    std::string f_src(*Nan::Utf8String(script->ToString()));
    std::string f_full = "(" + f_src + ")";
    const char* source = f_full.c_str();

    WebWorkerWrap* worker = new WebWorkerWrap(source);
    worker->root = strdup(*Nan::Utf8String(root));
    worker->onrequest_.Reset(info.GetIsolate(), onrequest);
    worker->Wrap(info.This());
    // FIXME(Yorkie): by scoping, deleting std::string object automatically.
  }
}

NAN_METHOD(WebWorkerWrap::Send) {
  WebWorkerWrap* worker = Nan::ObjectWrap::Unwrap<WebWorkerWrap>(info.This());
  if (worker->destroyed_)
    return Nan::ThrowError("worker is destroyed");

  // FIXME(Yorkie): free id and data before malloc new blocks
  if (worker->callback_id)
    free(worker->callback_id);
  if (worker->callback_data.ByteLength() > 0)
    free(worker->callback_data.Data());

  worker->callback_id = strdup(*String::Utf8Value(info[0]));
  worker->callback_data = Local<SharedArrayBuffer>::Cast(info[1])->Externalize();
  uv_sem_post(&worker->worker_locker);
}

NAN_METHOD(WebWorkerWrap::Start) {
  WebWorkerWrap* worker = Nan::ObjectWrap::Unwrap<WebWorkerWrap>(info.This());
  worker->script_args = Local<SharedArrayBuffer>::Cast(info[0])->Externalize();
  uv_thread_create(&worker->thread, &WebWorkerWrap::CreateTask, (void*)worker);
}

NAN_METHOD(WebWorkerWrap::Terminate) {
  WebWorkerWrap* worker = Nan::ObjectWrap::Unwrap<WebWorkerWrap>(info.This());
  worker->should_terminate = 1;
  uv_sem_post(&worker->worker_locker);
}

NAN_METHOD(WebWorkerWrap::ForceTerminate) {
  WebWorkerWrap* worker = Nan::ObjectWrap::Unwrap<WebWorkerWrap>(info.This());
  if (worker->destroyed_)
    return;
  pthread_cancel(worker->thread);
}

NAN_PROPERTY_GETTER(WebWorkerWrap::GetDestroyed) {
  WebWorkerWrap* worker = Nan::ObjectWrap::Unwrap<WebWorkerWrap>(info.This());
  if (worker->destroyed_) {
    info.GetReturnValue().Set(Nan::True());
  } else {
    info.GetReturnValue().Set(Nan::False());
  }
}

void InitModule(Handle<Object> target) {
  WebWorkerWrap::Init(target);
}

NODE_MODULE(WebWorker, InitModule);
