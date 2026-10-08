import { FakeCamServer, TOKEN } from "../test/fake-camserver.ts";

// A stand-in for CamServer (the same remote control messages, fixed data), to work on the backend and the web UI without the C++ server.
//   npm run fake-camserver      then use  CAMSERVER_TOKEN=test-token  in .env
const fake = new FakeCamServer(Number(process.env.FAKE_PORT ?? 45651));
// A real (1x1) picture, so a browser can show it.
fake.jpeg = Buffer.from("/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////wgALCAABAAEBAREA/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxA=", "base64");
await fake.ready();
console.log(`Fake CamServer on ws://127.0.0.1:${fake.port}, token: ${TOKEN}`);
