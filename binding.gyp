{
  "targets": [{
    "target_name": "webworker",
    "include_dirs": [
      "/usr/local/include/node",
      "<!(node -e \"require('nan')\")",
      "./",
    ],
    "sources": [
      "src/WebWorkerWrap.cc",
    ],
    "cflags_cc!": [ 
      "-fno-exceptions", "-std=c++11" 
    ],
    "libraries": [
      "-lpthread",
    ],
  }]
}