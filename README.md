Node.js remote debugger
=======================

This is an experiment with intent to add debugging capabilities to Node.js
The UI part is supposed to be regular Chrome DevTools talking to the
v8 inspector via the remote debugging protocol.

## Build & run

Clone v8_inspector branch. This is a slightly modified version of Node.js that uses v8_inspector to support remote debugging with Chrome DevTools:
```text
$ git clone -b v8_inspector https://github.com/repenaxa/node/
$ cd node
```

Pull inspector implementation as git submodule:
```text
$ git submodule update --init --remote --recursive
```

Generate .ninja files with gyp and build:
```text
$ ./configure --with-inspector
$ ./tools/gyp_node.py -f ninja
$ ninja -C out/Release/ node  -j10
```

Run Node with remote debugging enabled:
```text
$ ./out/Release/node --inspect examples/test.js
```

The command will execute the script passed as the argument and start to wait for remote debugger
connection on port 5858. To connect with DevTools, start Chromium with *--remote-debugging-targets=localhost:5858*
and navigate to *chrome:inspect*. There is going to be a link to the Node debugger under "Devices > Remote Targets" section.

Alternatively you could open the following URL directly:

```text
chrome-devtools://devtools/bundled/inspector.html?ws=localhost:5858/node
```

DevTools UI will load in the tab and connect to the node instance executing test.js file. Sources panel should list your JavaScript scripts.

* If you navigate to localhost:3333 in another tab, execution should pause on debugger; statement in test.js
* You can try stepping and breakpoint debugging
* Console evaluation with support for ES6 and object formatting
* Sampling JavaScript profiler with flamechart
* Heap profiler and recording heap allocations
