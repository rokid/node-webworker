#include "src/WebWorkerWrap.h"
#include <uv.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sstream>

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

void WebWorkerWrap::Compile(const FunctionCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  {
    HandleScope scope(isolate);
    WebWorkerWrap* worker = reinterpret_cast<WebWorkerWrap*>(isolate->GetData(0));

    char* pathname = strdup(*String::Utf8Value(info[0]));
    std::ifstream ifs(pathname);
    std::string source(
      (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    std::stringstream contents;
    contents << "(function(exports, module, require, __dirname){" << source << "})";
    Local<Function> compiled = worker->Compile(pathname, contents.str().c_str());
    info.GetReturnValue().Set(compiled);
    free(pathname);
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

    {
      // deserialize the returned string
      ValueDeserializer* deserializer = new ValueDeserializer(isolate,
        (const uint8_t*)worker->request_returns.Data(), worker->request_returns.ByteLength());
      deserializer->ReadHeader(context);
      Local<Value> val = deserializer->ReadValue(context).ToLocalChecked();
      info.GetReturnValue().Set(val);
    }

    // frees the request objects
    free(worker->request_name);
    free(worker->request_args);
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
  stringstream scriptAbsPath;
  scriptAbsPath << root << "/src/bootstrap_worker.js";
  std::ifstream ifs(scriptAbsPath.str());
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
    NODE_SET_METHOD(global, "$compile", WebWorkerWrap::Compile);

    // call bootstrap_worker.js
    {
      Local<Value> argv[5];
      argv[0] = jsworker;
      argv[1] = global;
      argv[2] = script;

      ValueDeserializer* deserializer = new ValueDeserializer(isolate,
        (const uint8_t*)worker->script_args.Data(), worker->script_args.ByteLength());
      deserializer->ReadHeader(context);
      argv[3] = deserializer->ReadValue(context).ToLocalChecked();
      argv[4] = Nan::New(worker->root).ToLocalChecked();

      bootstrap->Call(context, jsworker, 5, argv);
    }

    if (try_catch.HasCaught()) {
      // TODO
      isolate->ThrowException(try_catch.Exception());
    }

    // check if any callbacks
    while (true) {
      uv_sem_wait(&worker->worker_locker);
      if (worker->should_terminate)
        break;
      Local<Function> cb = Local<Function>::New(isolate, worker->callbacks_[worker->callback_id]);
      worker->WorkerCallback(cb);
    }
    worker->worker_context->Exit();
  }
  isolate->Dispose();
  // release 
  uv_close((uv_handle_t*)&worker->master_handle, NULL);
  uv_sem_destroy(&worker->worker_locker);
  uv_sem_destroy(&worker->request_locker);
}

void WebWorkerWrap::InitThread(Isolate* isolate) {
  worker_isolate = isolate;
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
    deserializer->ReadHeader(context);
    argv[0] = deserializer->ReadValue(context).ToLocalChecked();
    cb->Call(context, context->Global(), 1, argv);
  }
}

WebWorkerWrap::WebWorkerWrap(const char* source_) {
  source = strdup(source_);
  master_handle.data = this;
  uv_async_init(uv_default_loop(), &master_handle, &WebWorkerWrap::MasterCallback);
}

WebWorkerWrap::~WebWorkerWrap() {
  free(const_cast<char*>(root));
  free(const_cast<char*>(source));
  free(callback_id);
  free(request_name);
  free(request_args);
  onrequest_.Reset();
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

void InitModule(Handle<Object> target) {
  WebWorkerWrap::Init(target);
}

NODE_MODULE(WebWorker, InitModule);
