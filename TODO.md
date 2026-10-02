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

- [ ] **No authentication or encryption** between cameras, displays and the server: everybody in the network can connect a display and see all cameras (or send frames as a camera). Needs a shared secret or key per device, and encrypted frames, before the server is used outside a trusted home network.
- [ ] **Emails, next steps**: one summary email for several unknown people in a short time, emails for other events (a camera went offline).
- [ ] Web interface.
- [ ] Record the last X minutes on demand, and record at specific times. The ring buffer on the server already holds the frames of the last minutes, but nothing saves them to disk yet.
- [ ] Hardware section of the README ("coming soon").

## Cleanup and quality

- [ ] Camera error handling in `CamClient/src/Camera.cpp`: `GenerateFrames` has static retry counters, which are never reset, and `Zoom()` probably builds a wrong rectangle (it passes the maximum coordinates, where width and height are expected).
- [ ] No automated tests and no CI.
