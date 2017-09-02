'use strict';

const test = require('tape');
const WebWorker = require('../').WebWorker;

test('single worker', (t) => {
  t.plan(1);
  let worker = new WebWorker(function() {
    console.log('foobar');
  });
  worker.onstdout = function(data) {
    t.equal(data, 'foobar');
    t.end();
    worker.terminate();
    process.exit();
  };
});