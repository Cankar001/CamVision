# TODO

Open work for CamVision. Items marked with a path point at the code where they belong.

## Filesystem

- [ ] `FileSystem::WriteFile` overwrites existing files on Linux, but refuses to on Windows. Decide on one behavior.

## Updater

- [ ] **Update replaced during a download.** A client, which is in the middle of a download when the server rebuilds its update, ends with a signature failure. It then keeps the installed CamClient and retries on its next start. A clean handover needs a protocol change (for example an update ID in the messages).
- [ ] **No replay or downgrade protection.** The signature covers only the update file, not the version number, so somebody who can spoof the UDP packets of the server could offer an old signed update under a higher version. Fix by signing the version together with the file (protocol change).
- [ ] **Key rotation is manual.** After a lost or leaked signing key, use `regenerate_keys = true` and delete the pinned public key on every client. There is no secure way to hand clients a new key.
- [ ] **Pinned key on real devices.** The first update is trusted blindly, unless the public key of the server is copied to the device in advance (see README, section Updater). Make that part of the device setup.
- [ ] Very long file copies, which pause for more than 3 seconds, can still trigger an extra rebuild of the update in the hot reload.

## Features not started

- [ ] **CamDisplay follow-ups** (the display itself works, see README, section Displays): test it on a real Raspberry Pi with a screen (decoding speed with several cameras, `max_fps`), and let the display choose cameras at runtime (keys to switch between a grid and one camera).
- [ ] **No authentication or encryption** between cameras, displays and the server: everybody in the network can connect a display and see all cameras (or send frames as a camera). Needs a shared secret or key per device, and encrypted frames, before the server is used outside a trusted home network.
- [ ] **`ProcessFrame`** in `CamClient/src/Client.cpp`: empty. This is where movement detection and face analysis belong.
- [ ] **Face detection and recognition, next steps** (the first version runs on the server, see README): test it with real models and photos (nothing was tested with the model files yet, only the setup and error paths), tune the thresholds, measure the CPU load with several cameras (use a Release build), and check it on the Pi (OpenCV >= 4.5.4 needed, Raspberry Pi OS Bookworm has 4.6).
- [ ] **Send an email, when an unknown person is seen.** The hook exists: `Server::NotifyUnknownPerson` in `CamServer/src/Server.cpp` is called once per cooldown and camera, and currently only logs `[TODO email] Would send an email ...` with the subject, the text and the snapshot. Still to do: SMTP settings in `server.cfg` (server, port, user, password, sender, recipients), sending without blocking the face analysis, the snapshot as an attachment, and a limit for the number of emails.
- [ ] Face analysis follow-ups: run it on the camera client too (send frames only when somebody is seen), start a recording on an event ("Record X last minutes"), a command to add a known person from a camera frame, and an API for the web interface.
- [ ] Web interface.
- [ ] Record the last X minutes on demand, and record at specific times. The ring buffer on the server already holds the frames of the last minutes, but nothing saves them to disk yet.
- [ ] Hardware section of the README ("coming soon").

## Cleanup and quality

- [ ] Camera error handling in `CamClient/src/Camera.cpp`: `GenerateFrames` has static retry counters, which are never reset, and `Zoom()` probably builds a wrong rectangle (it passes the maximum coordinates, where width and height are expected).
- [ ] No automated tests and no CI.

## Done

For reference, what is finished (details are in the git history):

- Frame transfer from camera client to server (UDP, JPEG compressed, reassembly per client, incomplete frames are dropped), plus a server-side timeout for dead clients with automatic reconnect.
- OpenCV upgrade to 4.14.0 (fixes the Elgato Facecam Pro).
- Settings files and command line arguments for camera client, server and updater, and bandwidth tuning (`send_width`, `max_fps`, `jpeg_quality`).
- Headless mode for the camera client.
- Linux platform code (timer, file system, file watcher, crypto with keys and signatures, which are compatible with Windows), build setup for ARM, README instructions.
- Updater: version handling, key pinning, safe install with fallback to the installed CamClient, hot reloading of the binary folder (replace single files or exchange the whole folder), `--query-version`, and a fast, adaptive download.
