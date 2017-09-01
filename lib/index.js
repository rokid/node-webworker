'use strict';

const v8 = require('v8');
const WebWorkerWrap = require('bindings')('webworker').WebWorkerWrap;

const defaultOptions = {
  timeout: undefined,
  defines: [],
};

/**
 * @class WebWorker
 */
class WebWorker {
  /**
   * @method constructor
   * @param {Function|String} script - the path to a worker script or script function directly.
   */
  constructor(script, options=defaultOptions) {
    this._handle = new WebWorkerWrap(script, this.onRequest.bind(this));
    this._refs = {};

    // messaging objects
    this._postMessage = null;
    this._queuedMessages = [];

    // events
    this._onstdout = null;
    this._onstderr = null;
    this._onmessage = null;
    // TODO(Yorkie): current not supported
    this._onerror = null;

    // init
    this._terminateTimer = options.timeout ? 
      setTimeout(this.terminate.bind(this), options.timeout) : null;
    this._handle.start(options.defines);
  }
  /**
   * @property {Function} onstdout
   */
  get onstdout() {
    return (...args) => {
      if (typeof this._onstdout === 'function')
        this._onstdout(Array.from(args).join(' '));
      else
        console.log.apply(console, args);
    };
  }
  set onstdout(val) {
    this._onstdout = val;
  }
  /**
   * @property {Function} onstderr
   */
  get onstderr() {
    return (...args) => {
      if (typeof this._onstderr === 'function')
        this._onstderr(Array.from(args).join(' '));
      else
        console.error.apply(console, args);
    };
  }
  set onstderr(val) {
    this._onstderr = val;
  }
  /**
   * @property {Function} onmessage
   */
  get onmessage() {
    return (...args) => {
      if (typeof this._onmessage === 'function')
        this._onmessage.apply(this, args);
    };
  }
  set onmessage(val) {
    this._onmessage = val;
  }
  /**
   * @property {Function} onerror
   */
  set onerror(val) {
    this._onerror = val;
  }
  /**
   * @method postMessage
   * @param {Object} msg - message to pass
   */
  postMessage(msg) {
    if (typeof this._postMessage === 'function') {
      this._postMessage(msg);
    } else {
      this._queuedMessages.push(msg);
    }
  }
  /**
   * @method terminate
   */
  terminate() {
    clearTimeout(this._terminateTimer);
    this._handle.terminate();
  }
  /**
   * @method send
   * @param {String} id
   * @param {Array} args
   */
  send(id, args) {
    this._handle.send(id, 
      v8.serialize(args.map((val) => {
        return this.normalize(id, val);
      })).buffer
    );
  }
  /**
   * @method prePostMessage
   * @param {Function} sender
   */
  prePostMessage(sender) {
    for (let i = 0; i < this._queuedMessages.length; i++) {
      sender.call(this, this._queuedMessages[i]);
    }
    this._queuedMessages.length = 0;
    this._postMessage = sender.bind(this);
  }
  /**
   * @method normalize
   * @param {String} id
   * @param {Object} data
   * @returns the normalized object.
   */
  normalize(id, data) {
    // FIXME(Yorkie): for buffer, we will use SharedArrayBuffer to share 
    // buffer between stacks.
    if (Buffer.isBuffer(data)) {
      return data.buffer;
    }

    // FIXME(Yorkie): if data is not a plain object, return data...
    if (typeof data !== 'object') {
      return data;
    }

    // start common normalizing progress.
    const normalized = {};
    for (let name in data) {
      let prop = data[name];
      if (typeof prop === 'function') {
        const key = `method:${id}#${name}`;
        normalized[name] = key;
        this._refs[key] = data;
      } else if (typeof prop !== 'object') {
        normalized[name] = prop;
      }
    }
    return normalized;
  }
  /**
   * @method onRequest
   * @param {String} name
   * @param {Array} args
   */
  onRequest(name, args) {
    args = args.map((arg) => {
      if (/callback:/.test(arg)) {
        const id = arg.split(':')[1];
        return (...args) => this.send(id, args);
      } else {
        return arg;
      }
    });
    let host = global;
    let fname;

    if (this._refs[name]) {
      host = this._refs[name];
      fname = name.replace(/^method:[a-z0-9]*#/i, '');
    } else {
      let mpath = name.split('.');
      if (mpath[0] === '$') {
        host = this;
        mpath = mpath.slice(1);
      }
      if (mpath.length === 1) {
        fname = mpath[0];
      } else {
        while (true) {
          const symbol = mpath.shift();
          host = host[symbol];
          if (mpath.length === 1) {
            fname = mpath[0];
            break;
          }
        }
      }
    }

    if (typeof host[fname] === 'function') {
      host[fname].apply(host, args);
    } else {
      throw new Error(`${name} is not a function`);
    }
  }
}

exports.WebWorker = WebWorker;