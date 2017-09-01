'use strict';

const WebWorker = require('./').WebWorker;

const worker = new WebWorker((self) => {
  self.postMessage({foo: 'bar'});
  self.onmessage = function(data) {
    console.log('worker', data);
  };
  setInterval(() => {
    console.log(Date.now());
  }, 1000);
}, {
  timeout: 3000,
  defines: []
});

worker.onstdout = function(text) {
  console.log('onstdout', text);
};

worker.onstderr = function(err) {
  console.log('onstderr', err);
};

worker.onmessage = function(data) {
  console.log('master', data);
  setTimeout(() => {
    worker.postMessage(200);
  }, 100)
  worker.postMessage(100);
};

setTimeout(() => {
  console.log('ready to terminal');
  worker.terminate();
}, 5000);