'use strict';

const WebWorker = require('../').WebWorker;
const worker = new WebWorker((self) => {
  // Note: code is in new v8 runtime
  require('./example/main.worker.js')(self);
});

let shouldStopWorker2 = false;

worker.onstdout = function(text) {
  console.log('onstdout', text);
};

worker.onstderr = function(err) {
  console.log('onstderr', err);
};

worker.onmessage = function(data) {
  if (data && data.stopWorker2 === true) {
    shouldStopWorker2 = true;
    return;
  }
  console.log('received message from worker:', data);
  setTimeout(() => {
    worker.postMessage({ host: true });
  }, 100);
};

{
  let wrongWorker = new WebWorker((self, send, check) => {
    send('start');
    while (true) {
      // Do nothing
      if (check()) break;
    }
    send('end');
  }, {
    defines: [
      function postMessage2worker(msg) {
        worker.postMessage(`from wrong worker: ${msg}`);
        if (msg === 'end') {
          worker.terminate();
        }
      },
      function check() {
        return shouldStopWorker2;
      }
    ]
  });
}