// Test script to be debugged.

var http = require('http');
var fs = require('fs');
var index = fs.readFileSync('examples/test.js');

http.createServer(function (req, res) {
  console.log("Accepted connection (" + req.url + "): " + new Error().stack);
  res.writeHead(200, {'Content-Type': 'text/plain'});
  res.end(index);
  debugger;
}).listen(3333);