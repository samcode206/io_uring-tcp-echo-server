import { createConnection } from "node:net";


const socket = createConnection("9919", () => {
    console.log('connected');
});


process.stdin.pipe(socket);