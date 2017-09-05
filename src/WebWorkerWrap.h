#pragma once

#include <stdio.h>
#include <uv.h>
#include <node.h>
#include <nan.h>
#include <string>
#include <map>

using namespace v8;
using namespace std;

class WebWorkerWrap : public Nan::ObjectWrap {
public:
  static NAN_MODULE_INIT(Init);

private:
  explicit WebWorkerWrap(const char*);
  ~WebWorkerWrap();
  void InitThread(Isolate*);

  const char* source;
  uv_thread_t thread;
  uv_async_t master_handle;
  uv_sem_t worker_locker;
  uv_sem_t request_locker;

  Isolate* worker_isolate;
  int should_terminate = 0;
  Local<Context> worker_context;
  std::map<std::string, Persistent<Function>> callbacks_;
  char* callback_id = nullptr;
  SharedArrayBuffer::Contents callback_data;
  SharedArrayBuffer::Contents script_args;

  char* request_name;
  char* request_args;
  SharedArrayBuffer::Contents request_returns;
  Persistent<Function> onrequest_;

  Local<Function> Compile(const char* name, const char* source);
  Local<Function> GetBootstrapScript();
  void Execute(MaybeLocal<Script> script, int argc, Local<Value> argv[]);

  static void CreateTask(void*);
  static void MasterCallback(uv_async_t*);
  void WorkerCallback(Local<Function>);

  // Worker APIs
  static void Writeln(const FunctionCallbackInfo<Value>&);
  static void Compile(const FunctionCallbackInfo<Value>&);
  static void DefineRemoteMethod(const FunctionCallbackInfo<Value>&);
  static void QueueCallback(const FunctionCallbackInfo<Value>&);

  // instance methods
  static NAN_METHOD(New);
  static NAN_METHOD(Send);
  static NAN_METHOD(Start);
  static NAN_METHOD(Terminate);
};
