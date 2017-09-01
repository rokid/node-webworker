(function(worker, global, script, params) {
  'use strict';

  let remoteCallbackId = 0;

  function parseRemoteObject(object) {
    for (let name in object) {
      let prop = object[name];
      if (/^method:/.test(prop)) {
        object[name] = (...args) => callRemoteMethod(prop, args);
      }
    }
  }

  function convertCallback(item) {
    if (typeof item === 'function') {
      const id = remoteCallbackId++;
      worker.queueCallback(id, (objects) => {
        objects.forEach(parseRemoteObject);
        item.apply(null, objects);
      });
      return `callback:${id}`;
    } else {
      return item;
    }
  }

  global.callRemoteMethod = function(name, items) {
    worker.defineRemoteMethod(name, 
      Array.from(items).map(convertCallback));
  };

  function handleMessageEvent() {
    if (typeof worker.onmessage === 'function') {
      worker.onmessage.apply(worker, arguments);
    }
  }

  function createInnerMessageChannel(handler) {
    return callRemoteMethod('$.prePostMessage', [handler]);
  }
  createInnerMessageChannel(handleMessageEvent);

  global.postMessage = function() {
    return callRemoteMethod('$.onmessage', arguments);
  };
  worker.postMessage = global.postMessage;

  global.setTimeout = function(timer, interval) {
    return callRemoteMethod('setTimeout', arguments);
  };

  global.setInterval = function(timer, interval) {
    return callRemoteMethod('setInterval', arguments);
  };

  global.console = {
    log: function() {
      return callRemoteMethod('$.onstdout', arguments);
    },
    debug: function() {
      return callRemoteMethod('$.onstdout', arguments);
    },
    warn: function() {
      return callRemoteMethod('$.onstderr', arguments);
    },
    error: function() {
      return callRemoteMethod('$.onstderr', arguments);
    }
  };

  // global.http = {
  //   get: function() {
  //     return callRemoteMethod('http.get', arguments);
  //   },
  //   request: function() {
  //     return callRemoteMethod('http.request', arguments);
  //   }
  // };

  function normalize(params) {
    for (let i = 0; i < params.length; i++) {
      const item = params[i];
      if (typeof item === 'object') {
        for (let key in item) {
          const val = item[key];
          if (!/^method:/.test(val)) {
            continue;
          }
          item[key] = function() {
            return callRemoteMethod(val, arguments);
          };
        }
      }
    }
    return params;
  }
  script.apply(worker, [worker].concat(normalize(params)));
});