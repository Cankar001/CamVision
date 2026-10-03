# CamVision

CamVision is an open source security camera project. It uses OpenCV for image analysis and has a server/client architecture. Each client can either be a camera device or a display. Camera clients analyze the footage locally with opencv, and send the frames to the server, if movement is detected. The server stores the video and also sends the frames to all registered display clients, which show the video on connected monitors.

# Getting started

Windows (Visual Studio 2022) and Linux (including the Raspberry Pi) are supported. The project files are generated with premake5, the generated files are not part of the repository.

## Windows

```shell
git clone https://github.com/Cankar001/CamVision && cd CamVision
python Setup.py
```

`Setup.py` fetches the large files from git lfs (the bundled OpenCV 4.14 for Windows), and generates `CamVision.sln` for Visual Studio 2022. Open it, select `Debug` or `Release` with `x64` and build the solution.

## Linux (Debian, Ubuntu, Raspberry Pi OS)

This works on a normal PC (x86_64) as well as on a Raspberry Pi (32 or 64 bit ARM). Linux does not use the OpenCV files from the repository, it uses the OpenCV of the system. Everything is built on the machine, on which it will run.

### 1. Install the packages

```shell
sudo apt update
sudo apt install build-essential git python3 pkg-config libopencv-dev libssl-dev uuid-dev v4l-utils
```

- `build-essential`: compiler (GCC 9 or newer is required (Debian 11 "Bullseye" or newer, Ubuntu 20.04 or newer, Raspberry Pi OS Bullseye or newer), the project uses C++17 and std::filesystem) and `make`.
- `libopencv-dev`: OpenCV 4.x including its dependencies (GTK for the preview windows, Video4Linux for cameras). Check it with `pkg-config --modversion opencv4`, which must print a `4.x` version.
- `libssl-dev`: OpenSSL, used for the signatures of the updater.
- `uuid-dev`: only needed to build premake5 in step 2.
- `v4l-utils`: optional, lists the connected cameras.

### 2. Get premake5

The repository contains a premake5 for x86_64 Linux. `Setup.py` uses it automatically on x86_64 PCs, so you can skip this step there. **On ARM (Raspberry Pi) you have to build premake5 once yourself:**

```shell
git clone --recurse-submodules https://github.com/premake/premake-core.git
cd premake-core
make -f Bootstrap.mak linux
sudo install -m 755 bin/release/premake5 /usr/local/bin/premake5
cd ..
premake5 --version
```

This takes a few minutes on a Raspberry Pi. `Setup.py` always prefers a `premake5` found on the system.

### 3. Clone the repository and generate the makefiles

```shell
GIT_LFS_SKIP_SMUDGE=1 git clone https://github.com/Cankar001/CamVision
cd CamVision
python3 Setup.py
```

`GIT_LFS_SKIP_SMUDGE=1` only matters if git lfs is installed, it avoids downloading the Windows only OpenCV binaries (nearly 1 GB), which Linux does not need. `Setup.py` creates a `Makefile` in the main folder and in every project folder.

### 4. Build

```shell
make config=release -j2
```

- Use `-j2` on a Raspberry Pi 3 or other devices with 1 GB of RAM or less (more parallel compiler jobs can run out of memory). On a PC use `-j$(nproc)`.
- `config=debug` creates a debug build instead.
- To build a single program, name it: `make config=release CamClient` (`CamServer`, `CamDisplay`, `UpdateClient` and `UpdateServer` work as well).
- The programs are created in `<Project>/bin/<Configuration>-linux/<Project>/`, for example `CamClient/bin/Release-linux/CamClient/CamClient`.
- After pulling new changes that add or remove source files, run `python3 Setup.py` again, to regenerate the makefiles. To start from scratch: `make clean`.

### 5. Run

Start every program **from the folder, in which its executable is located**, the programs expect this working directory.

Camera client (for example on the Raspberry Pi):

```shell
cd CamClient/bin/Release-linux/CamClient
cp ../../../client.cfg.example client.cfg
nano client.cfg          # set at least server_ip, and camera_index
./CamClient
```

Server:

```shell
cd CamServer/bin/Release-linux/CamServer
cp ../../../server.cfg.example ../../../server.cfg    # the server reads server.cfg from the CamServer folder
./CamServer
```

Notes for the camera client:

- **Camera index:** it is the number of the `/dev/videoN` device. Run `v4l2-ctl --list-devices` to see the cameras. A USB camera often shows up twice (`/dev/video0` for the image, `/dev/video1` for metadata), use the first one. The client logs `Camera N: using backend ...` when the camera works.
- **Permissions:** your user must be allowed to use the camera: `sudo usermod -aG video $USER`, then log out and in again (on Raspberry Pi OS the default user is already in this group).
- **No display (headless):** set `headless = true` in `client.cfg` for devices without a monitor, otherwise the client tries to open a preview window and needs a desktop session. Stop it with `Ctrl+C`, the client then disconnects cleanly.
- **Weak devices / slow Wi-Fi:** lower the load with `send_width = 640`, `max_fps = 15` and `jpeg_quality = 65` in `client.cfg`.
- **Raspberry Pi camera modules (CSI):** the client uses Video4Linux. Current Raspberry Pi OS versions drive CSI cameras with libcamera, which does not appear as a normal `/dev/videoN` device. USB cameras work directly, CSI cameras need the libcamera V4L2 compatibility layer (not covered here).

### Start the camera client automatically (systemd)

Create `/etc/systemd/system/camvision-client.service` (adjust the user and the paths):

```ini
[Unit]
Description=CamVision camera client
After=network-online.target
Wants=network-online.target

[Service]
User=pi
WorkingDirectory=/home/pi/CamVision/CamClient/bin/Release-linux/CamClient
ExecStart=/home/pi/CamVision/CamClient/bin/Release-linux/CamClient/CamClient --headless
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```shell
sudo systemctl daemon-reload
sudo systemctl enable --now camvision-client
journalctl -u camvision-client -f      # shows the log
```

### Troubleshooting

| Problem | Solution |
|---|---|
| `Package opencv4 was not found` | Install `libopencv-dev` (step 1). On very old systems the package is called `opencv` instead of `opencv4`, use a distribution with OpenCV 4. |
| `premake5: command not found` / `Setup.py` says no premake5 found | Build and install premake5 (step 2). |
| `fatal error: openssl/...: No such file` | Install `libssl-dev`. |
| `c++: internal compiler error: Killed` or the build freezes | Out of memory: use `make -j1` and/or add swap (`sudo dphys-swapfile swapoff && sudo nano /etc/dphys-swapfile`, set `CONF_SWAPSIZE=1024`, `sudo dphys-swapfile setup && sudo dphys-swapfile swapon`). |
| `Camera N: could not be opened ...` | Wrong `camera_index`, no permission for `/dev/videoN` (see above), or the camera is used by another program. |
| `Could not connect to server` | Check `server_ip` and `server_port`, the server must be running, and UDP on the server port must be allowed by the firewall of the server. |
| `Gtk-WARNING: cannot open display` | There is no desktop session. Set `headless = true` (client) or `preview = false` (server). |

# Recordings

The server keeps the last `backup_minutes` (5 by default) of every camera in memory. You can save these minutes as a video file at any time, and you can record continuously at fixed times. The videos are AVI files (Motion JPEG): the pictures are written as they were received, without converting them again, so saving is fast and loses no quality. They play in VLC, Windows Media Player and most other players. Everything is stored in `recordings/<camera name>/` (`recordings_path`).

**Save the last minutes (on demand):**

- `CamServer --record_now=5` saves the last 5 minutes of all cameras, `CamServer --record_now=5 --record_camera=Front` only of the camera "Front". This talks to the running server (through the control port, which is only reachable from the same computer), prints the files, and quits. More than `backup_minutes` cannot be saved.
- The key **`R`** in a preview window of the server saves the last `record_default_minutes` of all cameras.
- With `record_on_unknown_person = true` the server saves the last `record_event_minutes` of a camera automatically, when it sees an unknown person (needs the face recognition). The video shows how the person came in.
- `CamServer --control_status` shows what is buffered, and `CamServer --control_stop` stops the server properly.

**Record at fixed times (schedule):** `record_schedule` in `server.cfg` lists the times, at which everything is recorded continuously, for example `22:00-06:00` (every night), or `Mon-Fri 08:00-18:00; Sat,Sun 00:00-23:59`. A time range, which ends before it starts, goes over midnight. The local time of the server is used. `record_cameras` limits it to some cameras. A recording is split into files of `record_segment_minutes` (10 by default), because a file is only playable when it is finished.

**The disk:** old videos are deleted automatically, after `record_keep_days` days (14 by default) and when all videos together are larger than `record_max_gb` gigabytes (20 by default, the oldest ones first). Set them to 0 to keep everything, but then watch the disk: a camera produces around 4 to 10 GB per day. Stop the server with `Ctrl+C` or `CamServer --control_stop`, so that the file, which is being written, is finished (otherwise the last file may not be playable).

# Displays

A display is another computer (for example a Raspberry Pi with a screen), which shows the pictures of the cameras. It connects to the server like a camera client, but tells the server that it is a display: instead of sending frames it **receives** the frames of the cameras from the server, and shows them. This way the screen can be at a different place than the camera, and any number of displays can be set up in the house, each one showing all cameras or just one.

1. **Build** the `CamDisplay` program (like the others, see "Getting started"). On Linux it needs a desktop session for its window (the packages of the Linux setup are enough).
2. **Configure** it: copy [CamDisplay/display.cfg.example](CamDisplay/display.cfg.example) to `display.cfg` in the folder of the executable and set at least `server_ip` (the address of the computer, on which the server runs). The other settings are described in the example.
3. **Start** `CamDisplay` from the folder of the executable. Everything can also be given on the command line: `CamDisplay --server_ip=192.168.1.20 --camera="Front door"`.

- **What it shows:** all cameras next to each other in a grid (1 camera fills the screen, 2 are side by side, 4 are 2 x 2, ...), or only one camera, if `camera` is set. The name of every camera is written into its picture. A camera, which stopped sending, is shown as "no signal", and disappears after 30 seconds. While the display cannot reach the server, it says so on the screen, and it connects by itself as soon as the server is there (also after the server was restarted or the network was down).
- **Fullscreen:** the window always covers the whole screen, without a border or a title bar. **`Esc` shuts the display down** (`Q` does the same, `Ctrl+C` in the terminal too).
- **Network load:** the server sends the pictures to every display (compressed, like the cameras send them to the server), at most `max_fps` frames per second per camera. A weak device or Wi-Fi needs a lower value.
- **Test without a window:** `CamDisplay --save_snapshot=picture.jpg` does not open a window, but stores the first picture it would show in the file, and quits (add `--snapshot_min_cameras=2` to wait for two cameras). This is the quickest check, that the display reaches the server and receives pictures.
- **Autostart on a Raspberry Pi** (display started with the desktop): create `~/.config/autostart/camvision-display.desktop` with the content below (adjust the path), then restart the Pi.

```ini
[Desktop Entry]
Type=Application
Name=CamVision display
Path=/home/pi/CamVision/CamDisplay/bin/Release-linux/CamDisplay
Exec=/home/pi/CamVision/CamDisplay/bin/Release-linux/CamDisplay
```

**Security:** the connection is not encrypted and not authenticated. Everybody, who can reach the server in the network, can connect a display and see all cameras. Use it only in a network you trust (the home network behind your router), and never open the port of the server to the internet.

# Face detection and recognition

The server can find faces in the camera feeds and recognize known people. It is off by default, and runs on the **server** (it analyzes the latest frame of every camera a few times per second, so the cameras stay lightweight). Faces are marked in the preview windows of the server **and on the displays** (green: known person, red: unknown person), and the log reports who is seen (once per person and camera every 30 seconds, optionally with a photo).

It uses the models YuNet (detection) and SFace (recognition) of OpenCV, no other library is needed. This needs **OpenCV 4.5.4 or newer** (the Windows build uses 4.14, on Linux check `pkg-config --modversion opencv4`). The processing runs on the CPU: use a **Release build** of the server, the Debug build is many times slower.

1. **Download the two model files** into `CamServer/models/` (links and sizes are in [CamServer/models/README.md](CamServer/models/README.md), about 37 MB together). The face detection works with the first file alone, the recognition needs both.
2. **Add the photos of the known people** into `CamServer/known_faces/`, one folder per person (see [CamServer/known_faces/README.md](CamServer/known_faces/README.md)). Photos can be added while the server is running.
3. **Turn it on:** `faces = true` in `server.cfg` (or `--faces=true`). The other settings are explained in [CamServer/server.cfg.example](CamServer/server.cfg.example).

**Faces on the displays:** the server draws the boxes and names into the pictures before it sends them to the displays, so every display shows the same as the preview of the server. The pictures keep their frame rate, the boxes are those of the latest analysis (up to `1 / face_fps` seconds old, and they disappear, if the analysis stops for 2 seconds). The server draws and compresses a picture once, even if several displays show it. This costs nothing noticeable in a Release build, but a lot in a Debug build. `face_on_displays = false` sends the plain pictures instead.

**Check the setup without a camera:** `CamServer --face_test=photo.jpg` analyzes one photo, prints the faces it finds (and who they are), and stores `photo.jpg.faces.jpg` with the faces marked. Run it from the folder of the executable like the server. A face, which is not recognized, shows the best similarity and the needed value, so `face_match_threshold` can be tuned.

The accuracy is good for frontal faces in decent light. It is a convenience feature, not a security system: do not use it to grant access to anything. Photos of people and the recognition results are personal data, handle them according to the laws, which apply to you (in the EU the GDPR).

## Emails

The server can send an email when something happens, which you want to know about:

- **An unknown person was seen:** the face recognition saw a person, who matches nobody of the known people. The email has the picture, in which the face is marked.
- **A camera went offline:** a camera stopped sending (it vanished without a goodbye: power, network or the program crashed), and when it is connected again, you get "back online". A camera, which is shut down properly, is not reported. This does not need the face analysis.

**One email instead of many:** the events are collected and sent together. The first event starts a collection window (`email_collect_seconds`, 10 seconds by default), and everything that happens in this time goes into ONE email: somebody walking past two cameras, or a power cut, which takes several cameras offline, result in one email with a list, and the pictures of the different cameras come first (up to `email_max_attachments`). Between two emails at least `email_min_interval` seconds (60 by default) pass. Events in between are not lost, they are sent together with the next email.

**Prefer one email per event?** Set `email_batch = false`: every event is then sent as an email of its own. The emails are still spaced by `email_min_interval`, an event in the meantime waits for its turn (it is not dropped), so set `email_min_interval = 0` as well, if you want every email immediately.

1. **Prerequisites:** for unknown people the face recognition must run (`faces = true`, both models, at least one known person), and **curl** must be installed. curl is part of Windows 10 and 11 already, on Linux install it with `sudo apt install curl`. The server uses curl to talk to the mail server (it takes care of the encryption and the login).
2. **Settings:** copy the `email_*` settings of [CamServer/server.cfg.example](CamServer/server.cfg.example) into your `server.cfg` and fill in the mail server of your email provider: server, port, security (`starttls` for port 587, `ssl` for port 465), login, sender and recipients. Gmail, Outlook and others require an *app password* for programs like this, which you create in the security settings of your account.
3. **Check it:** `CamServer --email_test` sends a test email with these settings and quits. If something is wrong, it says what (wrong password, server not reachable, certificate not trusted, ...).
4. Set `email = true`.

**The password:** a password in `server.cfg` can be read by everybody, who can read that file. The safer way is to leave `email_password` empty and set the environment variable `CAMVISION_EMAIL_PASSWORD` for the server instead (it overrides the setting). While an email is sent, the login data is written into a temporary file, which only the current user can read and which is deleted right afterwards (it is never passed on the command line, where other users could see it). The connection to the mail server is encrypted and the certificate is checked, unless you turn that off.

The emails are sent in the background, a slow mail server does not slow down the face analysis. If sending fails, the reason is written to the log (the unknown person is logged in any case).

# Configuration

The camera client and the server are configured with a simple settings file and/or command line arguments. Command line arguments override the file, for example `./CamClient --camera_index=2 --max_fps=15`. Use `--config=path` to load a different file. Settings, which are not set, use their default values.

The complete list with explanations is in the example files, copy them and edit the copy:

- [CamClient/client.cfg.example](CamClient/client.cfg.example): server address and port, camera index and size, headless mode, JPEG quality and bandwidth tuning (`send_width`, `max_fps`).
- [CamServer/server.cfg.example](CamServer/server.cfg.example): port, minutes of video kept per camera, timeout for dead clients, preview windows.

The client reads `client.cfg` from the folder, in which it is started. The server reads `server.cfg` from the `CamServer` project folder.

## Updater

The update server ships new camera client builds to the devices:

1. Increase `CAM_VERSION` in `CamClient/src/CamVersion.h`, build the CamClient, and start the update server (`UpdateServer`). It packs all files from its `binary_path` folder into one update, signs it, and rebuilds it automatically, whenever something in that folder changes.
2. On every device, the update client (`UpdateClient`) is started first. It asks the update server for its version. If the version differs, it downloads the update, checks the signature, installs it into its `install_path` folder, remembers the version, and starts the CamClient from there. As long as no update was installed yet, the CamClient from `fallback_path` (by default the build output of the machine) is started. If the server is not reachable or the signature is invalid, nothing is installed and the CamClient, which is already installed, is started anyway.

**Hot reloading:** the update server never has to be restarted to ship new binaries. It watches the `binary_path` folder, and you can change it in any way: replace single files, copy a new build over it, or exchange the whole folder (delete it, rename it, put a new one in its place). When the content changed and stayed the same for 3 seconds (so a running copy does not cause rebuilds in between), the server builds the new update on the side and replaces the old one only when it is complete and signed. Clients, which ask afterwards, get the new update. If the folder is missing or empty, a file is still locked by a running copy, or the build fails, the previous update stays available and the server tries again.

Clients only update, when the version **differs** from their installed version, so every new set of binaries needs a new version. The server takes the version from (in this order): the `version` setting, a `version.txt` in the binary folder (a number, shipped together with the binaries, so the folder can be exchanged on a server without any source code), or `CAM_VERSION` in `CamClient/src/CamVersion.h`. The server warns, if the files changed but the version did not. If the server is started before the binaries exist, it waits for them, and tells the clients meanwhile that they are up to date.

**Download speed:** the client keeps a window of 2048 pieces (2 MB) in flight and requests new pieces as soon as others arrive, so the speed adapts to the network (it does not flood a slow link, and it is fast on a fast one). It does not request a piece again for one second, and shows its progress once per second. The server limits the speed per client (`client_speed_limit_kb` in `update_server.cfg`, default 10000 KB/s, `0` = no limit). A 70 MB update takes about 12 seconds in a debug build on one machine, and about 10 seconds at the 10 MB/s limit on a real network. Lower the limit to protect a slow network or if many clients update at once.

**Checking for an update without installing it:** `UpdateClient --query-version` asks the update server for its version, prints just that number to the console and quits. Nothing else is printed (no log lines, no log file is written), and nothing is downloaded, installed, started or cleaned up. A program like the camera client can run it and compare the result with its own version:

```shell
UpdateClient --query-version              # prints e.g. 101
```

- Exit code `0`: the version was printed. Exit code `1`: the server did not answer (nothing on stdout, the reason is printed to stderr).
- If the server has no update to offer (it is empty or has no binaries yet), it answers with the client's own version, so "printed version differs from my version" always means that an update is available.
- `--query-timeout=SECONDS` changes how long to wait for the server (default 5). The other settings (`--server_ip`, `--server_port`, ...) work as usual.

The signing keys are created once by the update server (`public_key_path` and `private_key_path` in `update_server.cfg`) and reused for all updates. **The client pins the public key:** the first update is verified with the key from the server and the key is stored in the client's `public_key_path`, all later updates must be signed with the same key. For devices in the field it is safer to copy the server's public key file to the device before the first start. If you replace the signing key on purpose (`regenerate_keys = true`), delete the pinned key file on all clients.

The settings are explained in [UpdateServer/update_server.cfg.example](UpdateServer/update_server.cfg.example) and [UpdateClient/update_client.cfg.example](UpdateClient/update_client.cfg.example). The update server reads `update_server.cfg` and the update client `update_client.cfg` from the folder, in which they are started (the `UpdateServer` / `UpdateClient` project folder, if started from a build folder, like the other programs).

# Features

The project currently supports these features:

- self-updater: The self updater enables you, to very easily ship new versions to all in-use cameras or displays. You only have to drag-and-drop the update package into a pre-defined folder on the server, the running server listens to this pre-defined folder and recognizes a file system change, re-assembles the update into a transferrable package and ships it to all registered clients fully automated.
- Server/Client system for the camera: The Server/client system has the advantage, that each camera device doesn't have to have a large drive for the videos. It sends the camera feed over the native socket implementation to the server. The server stores the video feed of each camera in a separate ring queue, which has a configurable size. This enables the user to store the last N minutes on demand.
- Different client types: This system currently supports two different client types. The first type is a camera client, which records each frame from a connected camera and sends the frames to the server. The second type is a display client (see "Displays"), which gets a live feed from the server from each camera and can display the camera feed on a connected display. The system has these two different types, because not every camera might have a display connected directly to it. In this way, you can setup multiple raspberrys, which are located at different locations and server different roles.

# Planned features

- face detection: planned to support face detection with the Mediapipe library from Google soon.
- face recognition: planned to support face recognition with OpenCV soon.
- web interface: planned to create a web interface (with Laravel and React/Vue.js?)
- Record X last minutes: The ring queue, in which the frames from each client are stored on the server already make this feature possible, but it is planned to create a command interface or a web UI, in which this can be enabled/disabled/triggered.
- Record at specific time: planned to support recording at specific times (for example if the home owner is on vacation)

# Hardware

Hardware list is coming soon

# Screenshots

Currently I have built a very basic first prototype, including a very basic closure for the camera and for the display.

In these pictures you can see the CamClient application running on a raspberry pi 3b, with the standard touch screen display from raspberry.

![day-time](/Images/client_screenshot_day.jpeg?raw=true "picture from the client at day-time")
![night-time](/Images/client_screenshot_night.jpeg?raw=true "picture from the client at night-time")

