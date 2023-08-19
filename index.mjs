import { createServer } from "node:net";

function onStart() {
  console.log("Echo server started on 127.0.0.1:9919");
}

function onError(err) {
  // console.error(`error: ${err.message}`);
}
var i = 0;
function echo(socket) {
  socket.on("error", onError);

  socket.pipe(socket).on("error", onError);
}

const server = createServer(echo);

server.on("error", onError);

server.listen(9919, "127.0.0.1", 1024, onStart);
