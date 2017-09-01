#include "src/WebWorkerWrap.h"
#include <uv.h>
#include <unistd.h>
#include <iostream>
#include <fstream>

using namespace v8;
using namespace node;
using namespace std;

class ArrayBufferAllocator : public ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) {
    void* data = AllocateUninitialized(length);
    return data == NULL ? data : memset(data, 0, length);
  }
  virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
  virtual void Free(void* data, size_t) { free(data); }
};

void WebWorkerWrap::Writeln(const FunctionCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  {
    HandleScope scope(isolate);
    char* text = strdup(*String::Utf8Value(info[0]));
    printf("%s\n", text);
    free(text);
  }
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
  }
}

void WebWorkerWrap::QueueCallback(const FunctionCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  {
    HandleScope scope(isolate);
    WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(isolate->GetData(0));
    char* id = *String::Utf8Value(info[0]);
    worker->callbacks_[id].Reset(isolate, Local<Function>::Cast(info[1]));
  }
}

Local<Function> WebWorkerWrap::Compile(const char* name_, const char* source_) {
  Isolate* isolate = worker_isolate;
  EscapableHandleScope scope(isolate);

  Local<Context> context = isolate->GetCurrentContext();
  Local<String> name = String::NewFromUtf8(isolate, name_, NewStringType::kNormal).ToLocalChecked();
  Local<String> source = String::NewFromUtf8(isolate, source_, NewStringType::kNormal).ToLocalChecked();
  ScriptOrigin origin(name);
  MaybeLocal<Script> script = Script::Compile(context, source, &origin);
  if (script.IsEmpty()) {
    printf("script is empty\n");
    exit(0);
  }
  Local<Value> result = script.ToLocalChecked()->Run();
  if (result.IsEmpty()) {
    printf("function is empty\n");
    exit(4);
  }
  return scope.Escape(Local<Function>::Cast(result));
}

Local<Function> WebWorkerWrap::GetBootstrapScript() {
  std::ifstream ifs("src/bootstrap_worker.js");
  std::string script_source(
    (std::istreambuf_iterator<char>(ifs)), 
    std::istreambuf_iterator<char>());
  return Compile("bootstrap_worker.js", script_source.c_str());
}

void WebWorkerWrap::CreateTask(void* data) {
  WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(data);

  ArrayBufferAllocator allocator;
  Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = &allocator;

  // TODO(Yorkie): Isolate::New takes long time, the isolations 
  // pool is required for performance.
  Isolate* isolate = Isolate::New(create_params);
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

    TryCatch try_catch;
    try_catch.SetVerbose(true);

    Local<Function> bootstrap = worker->GetBootstrapScript();
    Local<Function> script = worker->Compile("WebWorkerProgress.js", worker->source);

    Local<Object> jsworker = Object::New(isolate);
    NODE_SET_METHOD(jsworker, "defineRemoteMethod", DefineRemoteMethod);
    NODE_SET_METHOD(jsworker, "queueCallback", QueueCallback);

    Local<Object> global = context->Global();
    NODE_SET_METHOD(global, "$writeln", WebWorkerWrap::Writeln);

    do {
      int argc = 2;
      Local<Value> bootstrap_argv[argc];
      bootstrap_argv[0] = jsworker;
      bootstrap_argv[1] = global;
      bootstrap->Call(context, jsworker, argc, bootstrap_argv);

      Local<Value> script_argv[1];
      script_argv[0] = jsworker;
      script->Call(context, jsworker, 1, script_argv);
      // worker->Execute(script, 0, {});
      if (try_catch.HasCaught()) {
        // TODO
        isolate->ThrowException(try_catch.Exception());
        break;
      }
    } while (0);

    // check if any callbacks
    while (true) {
      uv_sem_wait(&worker->worker_locker);
      Local<Function> cb = Local<Function>::New(isolate, worker->callbacks_[worker->callback_id]);
      worker->WorkerCallback(cb);
    }
    worker->worker_context->Exit();
  }
  isolate->Dispose();
}

void WebWorkerWrap::InitThread(Isolate* isolate) {
  worker_isolate = isolate;
  worker_handle.data = this;
  uv_sem_init(&worker_locker, 0);
  uv_sem_init(&request_locker, 0);
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

  onrequest->Call(worker->handle(), 2, args);
  uv_sem_post(&worker->request_locker);
}

void WebWorkerWrap::WorkerCallback(Local<Function> cb) {
  Isolate* isolate = v8::Isolate::GetCurrent();
  Local<Context> context = isolate->GetCurrentContext();
  {
    HandleScope scope(isolate);
    Local<Context> context = isolate->GetCurrentContext();
    Local<Value> args[1];

    ValueDeserializer* deserializer = new ValueDeserializer(isolate, 
      (const uint8_t*)callback_data.Data(), callback_data.ByteLength());
    deserializer->ReadHeader(context);
    args[0] = deserializer->ReadValue(context).ToLocalChecked();
    cb->Call(context, context->Global(), 1, args);
  }
}

WebWorkerWrap::WebWorkerWrap(const char* source_) {
  source = strdup(source_);
  master_handle.data = this;
  uv_async_init(uv_default_loop(), &master_handle, &WebWorkerWrap::MasterCallback);
}

WebWorkerWrap::~WebWorkerWrap() {
  // TODO
}

NAN_MODULE_INIT(WebWorkerWrap::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("WebWorkerWrap").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetPrototypeMethod(tpl, "send", Send);
  Nan::SetPrototypeMethod(tpl, "start", Start);
  Nan::SetPrototypeMethod(tpl, "terminate", Terminate);

  Local<Function> func = Nan::GetFunction(tpl).ToLocalChecked();
  Nan::Set(target, Nan::New("WebWorkerWrap").ToLocalChecked(), func);
}

NAN_METHOD(WebWorkerWrap::New) {
  Local<Function> script = Local<Function>::Cast(info[0]);
  Local<Function> onrequest = Local<Function>::Cast(info[1]);
  script->SetName(Nan::New<String>("WebWorkerProgress").ToLocalChecked());
  {
    std::string f_src(*Nan::Utf8String(script->ToString()));
    std::string f_full = "(" + f_src + ")";
    const char* source = f_full.c_str();

    WebWorkerWrap* worker = new WebWorkerWrap(source);
    worker->onrequest_.Reset(info.GetIsolate(), onrequest);
    worker->Wrap(info.This());
    // FIXME(Yorkie): by scoping, deleting std::string object automatically.
  }
}

NAN_METHOD(WebWorkerWrap::Send) {
  WebWorkerWrap* worker = Nan::ObjectWrap::Unwrap<WebWorkerWrap>(info.This());
  worker->callback_id = strdup(*String::Utf8Value(info[0]));
  worker->callback_data = Local<SharedArrayBuffer>::Cast(info[1])->Externalize();
  uv_sem_post(&worker->worker_locker);
}

NAN_METHOD(WebWorkerWrap::Start) {
  WebWorkerWrap* worker = Nan::ObjectWrap::Unwrap<WebWorkerWrap>(info.This());
  uv_thread_create(&worker->thread, &WebWorkerWrap::CreateTask, (void*)worker);
}

NAN_METHOD(WebWorkerWrap::Terminate) {
  WebWorkerWrap* worker = Nan::ObjectWrap::Unwrap<WebWorkerWrap>(info.This());
  uv_thread_join(&worker->thread);
}

void InitModule(Handle<Object> target) {
  WebWorkerWrap::Init(target);
}

NODE_MODULE(WebWorker, InitModule);
