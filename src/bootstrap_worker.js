(function(worker, global) {
  'use strict';

  let remoteCallbackId = 0;
  let apis = global;

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

  apis.callRemoteMethod = function(name, items) {
    worker.defineRemoteMethod(name, 
      Array.from(items).map(convertCallback));
  };

  function handleMessageEvent() {
    if (typeof worker.onmessage === 'function') {
      worker.onmessage.apply(worker, arguments);
    }
  }

  function createInnerMessageChannel(handler) {
    return apis.callRemoteMethod('$.prePostMessage', [handler]);
  }
  createInnerMessageChannel(handleMessageEvent);

  apis.postMessage = function() {
    return apis.callRemoteMethod('$.onmessage', arguments);
  };
  worker.postMessage = apis.postMessage;

  apis.setTimeout = function(timer, interval) {
    return apis.callRemoteMethod('setTimeout', arguments);
  };

  apis.setInterval = function(timer, interval) {
    return apis.callRemoteMethod('setInterval', arguments);
  };

  apis.console = {
    log: function() {
      return apis.callRemoteMethod('$.onstdout', arguments);
    },
    debug: function() {
      return apis.callRemoteMethod('$.onstdout', arguments);
    },
    warn: function() {
      return apis.callRemoteMethod('$.onstderr', arguments);
    },
    error: function() {
      return apis.callRemoteMethod('$.onstderr', arguments);
    }
  };

  // apis.http = {
  //   get: function() {
  //     return callRemoteMethod('http.get', arguments);
  //   },
  //   request: function() {
  //     return callRemoteMethod('http.request', arguments);
  //   }
  // };

})