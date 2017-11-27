(function(worker, global, script, params, __dirname) {
  'use strict';

  let remoteCallbackId = 0;

  function parseRemoteObject(object) {
    if (object.type === 'Error') {
      let err = new Error(object.message);
      err.stack = object.stack;
      return err;
    } else {
      for (let name in object) {
        let prop = object[name];
        if (/^method:/.test(prop)) {
          object[name] = (...args) => callRemoteMethod(prop, args);
        }
      }
      return object;
    }
  }

  function convertCallback(item) {
    if (typeof item === 'function') {
      const id = remoteCallbackId++;
      worker.queueCallback(id, (objects) => {
        item.apply(null, 
          objects.map(parseRemoteObject));
      });
      return `callback:${id}`;
    } else if (item instanceof Error) {
      const errData = {
        name: item.name,
        message: item.message,
        stack: item.stack,
      };
      return `error:${JSON.stringify(errData)}`;
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

  global.clearTimeout = function(timer, interval) {
    return callRemoteMethod('clearTimeout', arguments);
  };

  global.setInterval = function(timer, interval) {
    return callRemoteMethod('setInterval', arguments);
  };

  global.clearInterval = function(timer) {
    return callRemoteMethod('clearInterval', arguments);
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

  // load the builtins modules internally, treats all dirname to be
  // __dirname of this source file
  function __import__(name) {
    const module = {
      dirname: __dirname,
      exports: {},
    };
    name = builtins[name] ? builtins[name] : name;
    let func = $compile(name);
    if (typeof func !== 'function')
      throw new Error(`Cannot find module '${name}'`);

    func(
      module.exports, 
      module, 
      __import__, // require
      __dirname); // __dirname
    return module.exports;
  }

  function __importExternal__(filename, dirname, mName) {
    // for external modules, allows the name without .js
    // TODO(Yorkie): walk for package.json?
    if (!/.js$/.test(filename)) {
      filename += '.js';
    }
    const module = {
      dirname,
      exports: {},
    };

    const func = $compile(filename);
    if (typeof func !== 'function')
      throw new Error(`Cannot find module '${mName}' from ${filename}`);

    func(
      module.exports, 
      module, 
      require.bind(module), // require
      dirname);             // __dirname
    return module.exports;
  }

  function require(name) {
    const path = __import__(builtins.path);
    const dirname = path.dirname(name);
    if (builtins[name]) {
      return __import__(builtins[name], dirname);
    } else {
      if (path.isAbsolute(name)) {
        return __importExternal__(name, dirname);
      } else {
        let base = process.cwd();
        if (this && this.dirname) {
          base = this.dirname;
        }
        const filename = path.join(base, name);
        const dirname = path.dirname(filename);
        return __importExternal__(filename, dirname, name);
      }
    }
  }

  // TODO(Yazhong): disable http method
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