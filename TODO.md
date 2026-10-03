# TODO

Open work for CamVision. Items marked with a path point at the code where they belong.

## Filesystem

- [ ] `FileSystem::WriteFile` overwrites existing files on Linux, but refuses to on Windows. Decide on one behavior.

## Updater

- [ ] **Key rotation is manual.** After a lost or leaked signing key, use `regenerate_keys = true` and delete the pinned public key on every client. There is no secure way to hand clients a new key.
- [ ] **Stopping the CamClient before an update** is only tested on Windows. The Linux code (`LinuxProcess.cpp`, SIGTERM then SIGKILL) is run by the CI tests, but not with the real update client. A CamClient, which runs as a Windows service under another account than the update client, may not be allowed to receive the stop request (the event is created with the default access rights), it is killed after the timeout then.
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
