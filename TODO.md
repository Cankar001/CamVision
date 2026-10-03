# TODO

Open work for CamVision. Items marked with a path point at the code where they belong.

## Filesystem

- [ ] `FileSystem::WriteFile` overwrites existing files on Linux, but refuses to on Windows. Decide on one behavior.

## Updater

- [ ] **Update replaced during a download.** A client, which is in the middle of a download when the server rebuilds its update, ends with a signature failure. It then keeps the installed CamClient and retries on its next start. A clean handover needs a protocol change (for example an update ID in the messages).
- [ ] **No replay or downgrade protection.** The signature covers only the update file, not the version number, so somebody who can spoof the UDP packets of the server could offer an old signed update under a higher version. Fix by signing the version together with the file (protocol change).
- [ ] **Key rotation is manual.** After a lost or leaked signing key, use `regenerate_keys = true` and delete the pinned public key on every client. There is no secure way to hand clients a new key.
- [ ] **Stopping the CamClient before an update** works on Windows (tested with a stand-in program: it quits on Ctrl+C, no force needed). Not compiled or run on Linux yet (`LinuxProcess.cpp`, SIGTERM then SIGKILL). A CamClient without a console (started as a service) cannot receive Ctrl+C on Windows and is killed after the timeout, so it has no chance to say goodbye to the server.
- [ ] **Pinned key on real devices.** The first update is trusted blindly, unless the public key of the server is copied to the device in advance (see README, section Updater). Make that part of the device setup.
- [ ] Very long file copies, which pause for more than 3 seconds, can still trigger an extra rebuild of the update in the hot reload.

## Features not started

- [ ] **No authentication or encryption** between cameras, displays and the server: everybody in the network can connect a display and see all cameras (or send frames as a camera). Needs a shared secret or key per device, and encrypted frames, before the server is used outside a trusted home network.
- [ ] Web interface.
- [ ] **Recordings, next steps** (saving the last minutes on demand and recording on schedule work, see README, section Recordings): a way to save from a display (a key on the display, so the people in the house can do it without a computer), a list and download of the recordings in the web interface, other video formats (MP4 in browsers), and finishing a file that was left unfinished by a crash (the AVI index is written at the end). The control port is not authenticated (it only listens on this computer).
- [ ] Hardware section of the README ("coming soon").

## Cleanup and quality

- [ ] Camera error handling in `CamClient/src/Camera.cpp`: `GenerateFrames` has static retry counters, which are never reset, and `Zoom()` probably builds a wrong rectangle (it passes the maximum coordinates, where width and height are expected).
- [ ] **Tests (CamTests), what is still missing.** The first tests cover Cam-Core, the protocol, the recorder and the process handling (see README, section Tests). Not covered yet, in the order of value: the update flow (UpdateServer and UpdateClient together on loopback: download, signature, pinned key, hot reload, stopping the CamClient), the server with a fake camera and a fake display (connect, frames, heartbeat, timeout), the mailer against a fake SMTP server, the notifier batching, and the face recognition with a test picture. The updater and the server are programs, so they first need to be split into a library and a thin `main`, to be testable without starting them. The CI builds and tests CamTests only, not the programs themselves (they need OpenCV, which is large).
