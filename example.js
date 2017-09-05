'use strict';

const WebWorker = require('./').WebWorker;

const worker = new WebWorker((self, ro) => {

  const EventEmitter = require('events');
  const path = require('path');

  // ro.foo((res) => {
  //   console.log(res);
  // });
  // self.postMessage({foo: 'bar'});
  // self.onmessage = function(data) {
  //   console.log('worker', data);
  // };
  // setInterval(() => {
  //   console.log(Date.now());
  // }, 5);
}, {
  // timeout: 30000,
  defines: [{
    foo: (cb) => {
      console.log('call from worker');
      if (cb)
        return cb(false);
    },
    bar: 'foobar'
  }]
});

// worker.onstdout = function(text) {
//   console.log('onstdout', text);
// };

// worker.onstderr = function(err) {
//   console.log('onstderr', err);
// };

// worker.onmessage = function(data) {
//   console.log('master', data);
//   setTimeout(() => {
//     worker.postMessage(200);
//   }, 100)
//   worker.postMessage(100);
// };

setInterval(() => {
  var w = new WebWorker((self) => {
    console.log('from lite worker')
  }, {
    // timeout: 5000
  });
}, 1000);