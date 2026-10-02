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
- Different client types: This system currently supports two different client types. The first type is a camera client, which records each frame from a connected camera and sends the frames to the server. The second type is a display client, which gets a live feed from the server from each camera and can display the camera feed on a connected display. The system has these two different types, because not every camera might have a display connected directly to it. In this way, you can setup multiple raspberrys, which are located at different locations and server different roles.

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

