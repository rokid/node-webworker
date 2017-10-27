# node-webworker

[Web Worker][] implementation for Node.js. This module provides multi-threads programming for
Node.js user-land by borrowing the concept of the standard Web Worker.

## Get Started

```js
'use strict';
const WebWorker = require('webworker-ng').WebWorker;
const worker = new WebWorker((self) => {
  self.postMessage('foobar', {yorkie: true});
  self.on('message', (data) => {
    console.log(data);
  });
}, {
  timeout: 5000,  // terminate after timeout
  defines: [],    // the parameters/objects passing to Worker
});

worker.postMessage({foo: 'bar'});
worker.onmessage = function(data) {
  // get message from worker
};
```

## APIs

The node-webworker provides the class `WebWorker` for creating a separate worker.

#### Creating a webworker script

A `WebWorker` instance is corresponding a script to run, in low-level implementation, it creates
a thread to sperate the v8 stack from the main.

The constructor `WebWorker(script, options)` accepts:

- {Function} `script` the source code function which runs in worker. Will support the path to run.
- {Object} `options` the options to config the worker
  - {Number} `options.timeout` the microsecond number to terminate forcily.
  - {Array} `options.define` the array of parameters to pass in worker context.

#### Messaging

In host environment, namely the Node.js, every `WebWorker` instance owns the method `postMessage` and
the listener `onmessage`.

```js
worker.postMessage('some data');
worker.onmessage = function(data) {
  // TODO
};
```

Correspondingly, in every worker context, it owns the same like the following:

```js
const worker = new WebWorker(function(self) {
  self.onmessage = function(data) { .. };
  self.postMessage('some data sent from worker');
});
```

#### Collecting `stdout` and `stderr`

In worker context, the methods under `console` as `console.log()` and `console.error` are delegated by `node-webworker`
which allows user to collect worker logs by worker as the following:

```js
const worker = new WebWorker(function(self) {
  console.log('this sent to stdout');
  console.error('this sent to stderr');
});
worker.onstdout = function(msg) { ... };
worker.onstderr = function(msg) { ... };
```

#### Modular worker

In web worker, you can use the following builtin modules:

- `events` - the Node.js official `events` mirror module
- `path` - the Node.js official `path` mirror module

The other hand, you can also load your CommonJS file into your worker:

```js
const worker = new WebWorker(function(self) {
  require('./tests/load-worker.js');
});
```

Note: The default path routing is from the `process.cwd()`.

## Installation

```sh
$ npm install webworker-ng --save
```

## Tests

```sh
$ npm test
```

## License

[Apache License 2.0](LICENSE)

[Web Worker]: https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Using_web_workers
