'use strict';

let aliveTimer = setInterval(() => {
  console.log('i am still in alive...', Date());
}, 2000);

module.exports = function(self) {

  self.onmessage = function(data) {
    console.log('received the message from host', JSON.stringify(data));
  };

  self.postMessage({
    worker: true
  });

  setTimeout(() => {
    self.postMessage({
      stopWorker2: true
    });
  }, 6000);

};
