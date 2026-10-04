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

- [ ] **Web interface**, built as a separate backend, which connects to the server over the WebSocket (see README, section Remote control; `save`, the status and the device commands work already). The backend holds the token, does the login of the users and the TLS, and gives the browser a UI. What the server still needs for that, in the order of value:
  - [ ] **Events.** A UI needs pushed updates (a camera came or went, a person was seen, a recording was saved, a device connected), otherwise it has to poll `status`. The server can send them to all logged-in clients already (`WebSocketServer::Broadcast`, `CommandDispatcher::MakeEvent`), but nothing sends one yet.
  - [ ] **Live pictures.** The WebSocket is text only with messages up to 64 KB, it cannot carry video. A `snapshot` command or a small HTTP endpoint with the latest JPEG of every camera (the backend turns it into MJPEG or polled pictures for the browser), or binary WebSocket messages for live frames. Start with the snapshot or MJPEG.
  - [ ] **Recordings in the browser.** `recordings` lists the files, but the browser cannot get them. Either the backend reads the recordings folder (same computer), or the server serves the files over HTTP. AVI with Motion JPEG does not play in browsers, so MP4 (H.264) or a player for the pictures is needed later, and thumbnails.
  - [ ] **Settings commands.** Change settings while the server runs (recording schedule, face recognition, known faces, emails, preview), so that everything can be controlled from the UI.
  - [ ] **Roles for the token.** One token can do everything (also stop the server and make device keys). For read-only users, add roles or several tokens (or do it in the backend). Also a limit for wrong tokens per address (now per connection) and a way to change the token without a restart.
  - [ ] **Several connections of the backend.** A long command (a big `save`) holds up its connection, commands of one connection run one after the other, and there are at most 8 connections. The backend should use a connection per session or a small pool, and log in again after a lost connection. Maybe a command to follow the progress of long commands.
  - [ ] **Encryption** (`wss://`). Now only a tunnel or a proxy in front of it, which is fine while the backend runs on the same computer (loopback), but not if they are separated.
  - [ ] Not tested on Linux beyond the unit tests (the CI).
- [ ] **Security, next steps** (the devices are authenticated and the connections are encrypted, see README, section Security): renewing the key of a device without visiting it (now: remove it and add it again), forward secrecy (a handshake with temporary keys, so that recorded connections cannot be read when a key is stolen later), keys in a safer place than a text file (a secret store of the system), encrypting the recordings and the known faces on the disk, and protecting the control port (`record_now`) and the updater messages the same way. Not tested on Linux yet beyond the unit tests (the CI), and not tested on a Raspberry Pi (the encryption runs in software there, a Pi 3 should manage a few cameras, but this is not measured).
- [ ] **Recordings, next steps** a way to save from a display (a key on the display, so the people in the house can do it without a computer), a list and download of the recordings in the web interface, other video formats (MP4 in browsers), and finishing a file that was left unfinished by a crash (the AVI index is written at the end). The control port is not authenticated (it only listens on this computer).
- [ ] Hardware section of the README ("coming soon").

## Cleanup and quality

- [ ] Camera error handling in `CamClient/src/Camera.cpp`: `GenerateFrames` has static retry counters, which are never reset, and `Zoom()` probably builds a wrong rectangle (it passes the maximum coordinates, where width and height are expected).
- [ ] **Tests (CamTests), what is still missing.** The first tests cover Cam-Core, the protocol, the recorder and the process handling (see README, section Tests). Not covered yet, in the order of value: the update flow (UpdateServer and UpdateClient together on loopback: download, signature, pinned key, hot reload, stopping the CamClient), the server with a fake camera and a fake display (connect, frames, heartbeat, timeout), the mailer against a fake SMTP server, the notifier batching, and the face recognition with a test picture. The updater and the server are programs, so they first need to be split into a library and a thin `main`, to be testable without starting them. The CI builds and tests CamTests only, not the programs themselves (they need OpenCV, which is large).
