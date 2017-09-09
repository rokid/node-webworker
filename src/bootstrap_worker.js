(function(worker, global, script, params, __dirname) {
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
    return worker.defineRemoteMethod(name, 
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

  const process = global.process = {
    cwd: function() {
      return callRemoteMethod('process.cwd', arguments);
    },
  };

  global.fs = {
    readFile: function() {
      return callRemoteMethod('fs.readFile', arguments);
    }
  };

  const builtins = {
    'errors': __dirname + '/src/internal/errors.js',
    'events': __dirname + '/src/internal/events.js',
    'path': __dirname + '/src/internal/path.js',
  };
  global.require = require;

  function _require(name) {
    const module = {
      exports: {}
    };
    $compile(name)(module.exports, module);
    return module.exports;
  }

  function require(name) {
    if (builtins[name])
      return _require(builtins[name]);
    else {
      const path = _require(builtins.path);
      if (path.isAbsolute(name)) {
        return _require(name);
      } else {
        return _require(path.join(process.cwd(), name));
      }
    }
  }

  // global.http = {
  //   get: function() {
  //     return callRemoteMethod('http.get', arguments);
  //   },
  //   request: function() {
  //     return callRemoteMethod('http.request', arguments);
  //   }
  // };

  function normalize(params) {
    for (let key in params) {
      const item = params[key];
      if (/^method:/.test(item)) {
        params[key] = function() {
          return callRemoteMethod(item, arguments);
        };
      } else if (Array.isArray(item) || typeof item === 'object') {
        normalize(item);
      }
    }
    return params;
  }
  script.apply(worker, [worker].concat(normalize(params)));
});