# Hosting CamVision (server, web backend and web UI) on Ubuntu

This guide installs the whole server side on **one Ubuntu machine** (22.04 or 24.04, a physical machine at home is fine) and puts the **Apache** you already run in front of it:

```
 camera clients  ──UDP 45645──────────────────────────────┐
 displays        ──UDP 45645──────────────────────────────┤
                                                          ▼
 browser ──HTTPS 443──> Apache ──HTTP──> CamWebBackend ──ws──> CamServer
                      (reverse proxy)   127.0.0.1:3001        127.0.0.1:45651
                                        login, API, live      recordings, buffer,
                                        stream, serves the    face recognition
                                        web UI (CamWeb)
```

| Part | What it is | Runs as | Listens on |
|---|---|---|---|
| **CamServer** | C++ program: receives the cameras, buffers and records video, recognizes faces, has the remote control (WebSocket) | systemd service `camvision-server` | UDP `45645` (cameras, displays; the whole network), TCP `127.0.0.1:45651` (remote control), UDP `127.0.0.1:45650` (control port) |
| **CamWebBackend** | Node.js program: login, API, live stream, serves the built web UI | systemd service `camvision-web` | TCP `127.0.0.1:3001` |
| **CamWeb** | The web UI. It is built once into static files (`CamWeb/out`), which the backend serves. There is no Node process for it at runtime. | (files) | (through the backend) |
| **Apache** | Your existing reverse proxy: HTTPS in front of the backend | the apache2 service | `80`, `443` |

Only **Apache (80/443)** and **UDP 45645** are meant to be reachable from other computers. Everything else listens on the machine itself.

Everything below uses these example values. Replace them with yours:

| Example | Meaning |
|---|---|
| `/opt/camvision` | where the repository lives |
| `camvision` | the Linux user that runs CamServer and the backend |
| `cam.example.com` | the (sub)domain of the web UI |

> **The web UI needs its own host name** (`cam.example.com`), it does not work below a path like `https://example.com/cam/`. Your other sites are not affected: Apache picks the virtual host by the name.

---

## 0. Before you start

- A user with `sudo` on the Ubuntu machine, and the machine has an internet connection for the installation.
- About **5 GB** free for the build, and plenty of disk for the recordings (see [step 12](#12-disk-space-and-maintenance): a minute of video is about 70–150 MB).
- A name for the web UI that points to the machine (`cam.example.com`, a name in your router's DNS, or an entry in the `hosts` file of the computers that use it).
- Decide how browsers will reach it, because that decides the certificate in [step 9](#9-apache):
  - from the **internet or any network** through a real domain: Let's Encrypt works;
  - only in your **home network**: a certificate from your own CA, or Tailscale (see [step 13](#13-reaching-it-from-outside-the-house)).

---

## 1. Install the packages

```bash
sudo apt update
sudo apt install -y build-essential git python3 pkg-config libopencv-dev libssl-dev uuid-dev v4l-utils curl
```

- `curl` is also what CamServer uses to send emails.
- `libopencv-dev` must give a 4.x version: `pkg-config --modversion opencv4` (4.5.4 or newer is needed for the face recognition).

Node.js **22 LTS** (Ubuntu's own package is too old):

```bash
curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash -
sudo apt install -y nodejs
node -v        # v22.x
```

---

## 2. Create the user and get the code

One user runs both CamServer and the backend. That matters: the backend must be able to read the token file and the recordings of CamServer.

```bash
sudo useradd --system --no-create-home --home-dir /opt/camvision --shell /usr/sbin/nologin camvision
sudo mkdir /opt/camvision && sudo chown camvision:camvision /opt/camvision
sudo -u camvision -H bash -c 'cd /opt/camvision && GIT_LFS_SKIP_SMUDGE=1 git clone <URL of your repository> .'
```

`GIT_LFS_SKIP_SMUDGE=1` skips the large Windows-only OpenCV files (nearly 1 GB), which Linux does not need.

From here on, commands run as the `camvision` user with `sudo -u camvision -H …`, or open a shell as that user once (the user has no login shell, so use `-s /bin/bash`):

```bash
sudo -u camvision -H -s /bin/bash      # a shell as camvision; leave it with "exit"
cd /opt/camvision
```

---

## 3. Build CamServer

As `camvision`:

```bash
cd /opt/camvision
python3 Setup.py                       # creates the makefiles (the repository has premake5 for x86_64 PCs)
make config=release CamServer -j2      # -j1 if the machine has little memory
ls bin/Release/CamServer               # the program
```

Use the **Release** build: the face recognition is many times slower in Debug. On ARM (Raspberry Pi) see "Linux" in the [README](README.md) (premake5 must be built there first).

The program finds its folder on its own: started as `/opt/camvision/bin/Release/CamServer`, its working directory becomes `/opt/camvision/CamServer` (the settings, the keys, the recordings are there).

---

## 4. Configure CamServer

```bash
cd /opt/camvision/CamServer
cp server.cfg.example server.cfg
nano server.cfg
```

The settings that matter on a server without a monitor:

```ini
# There is no desktop: no preview windows (they need a display).
preview = false

# Keys for cameras and displays (on by default). Leave it on.
auth = true

# The remote control, which the web backend uses. Keep it on this machine only.
websocket_port = 45651
websocket_bind = 127.0.0.1

# Disk: delete recordings older than this many days / when the folder is larger than this.
record_keep_days = 14
record_max_gb = 50

# Faces (needs the two model files in CamServer/models, see CamServer/models/README.md):
faces = true
```

Everything else is explained in the file itself. If you do not use the face recognition, set `faces = false`.

**Do not change `websocket_bind` to `0.0.0.0`.** That connection is not encrypted, and the backend is on the same machine anyway.

---

## 5. Run CamServer as a service

Create `/etc/systemd/system/camvision-server.service`:

```ini
[Unit]
Description=CamVision server (cameras, recording, face recognition)
After=network-online.target
Wants=network-online.target

[Service]
User=camvision
WorkingDirectory=/opt/camvision/CamServer
ExecStart=/opt/camvision/bin/Release/CamServer
Restart=on-failure
RestartSec=5
# Finishing the videos, which are being recorded, takes a moment when the service stops.
TimeoutStopSec=30

NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now camvision-server
journalctl -u camvision-server -f       # the log; Ctrl+C leaves it
```

Check that it is up:

```bash
ss -tulpn | grep -E '45645|45650|45651'
# udp 0.0.0.0:45645      (cameras, displays)
# udp 127.0.0.1:45650    (control port)
# tcp 127.0.0.1:45651    (remote control)
sudo ls -l /opt/camvision/CamServer/websocket_token.txt     # made on the first start, owner camvision, mode 600
```

---

## 6. Build the web UI (CamWeb)

As `camvision`:

```bash
cd /opt/camvision/CamWeb
npm ci
npm run build          # creates CamWeb/out: the whole web UI as static files
ls out/index.html
```

---

## 7. Set up the web backend (CamWebBackend)

As `camvision`:

```bash
cd /opt/camvision/CamWebBackend
npm ci
npm run build          # creates dist/
mkdir -p data          # the backend keeps its own files here (the systemd unit only allows writing there)
cp .env.example .env
chmod 600 .env
```

**The password hash** for the login (it asks for the password, which is not stored anywhere):

```bash
npm run hash-password
# prints:  ADMIN_PASSWORD_HASH=scrypt$16384$8$1$....
```

Edit `.env`:

```ini
HOST=127.0.0.1
PORT=3001
DATA_DIR=./data

ADMIN_USER=admin
ADMIN_PASSWORD_HASH=scrypt$16384$8$1$...        # the line from above, exactly as printed
SESSION_HOURS=12

CAMSERVER_HOST=127.0.0.1
CAMSERVER_PORT=45651
CAMSERVER_SECURE=false
CAMSERVER_TOKEN_FILE=/opt/camvision/CamServer/websocket_token.txt

RECORDINGS_DIR=/opt/camvision/CamServer/recordings
STATIC_DIR=/opt/camvision/CamWeb/out
```

- Write the hash **without quotes**. It contains `$`, which is fine in this file (it is not expanded).
- Do not put the plain password anywhere.
- `RECORDINGS_DIR` is what makes the recordings downloadable in the web UI. It is the `recordings_path` of CamServer (`recordings` in `CamServer/` by default).
- The full list of settings is in [CamWebBackend/.env.example](CamWebBackend/.env.example) and the [README of the backend](CamWebBackend/README.md).

**Try it by hand once**, before making it a service:

```bash
node --env-file=.env dist/index.js
# in a second terminal:
curl http://127.0.0.1:3001/api/health          # {"ok":true}
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:3001/dashboard    # 200 (the web UI)
```

Stop it again with Ctrl+C.

---

## 8. Run the backend as a service

The repository has the unit: [CamWebBackend/deploy/camvision-web.service](CamWebBackend/deploy/camvision-web.service). It expects the paths of this guide.

```bash
sudo cp /opt/camvision/CamWebBackend/deploy/camvision-web.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now camvision-web
journalctl -u camvision-web -f
curl http://127.0.0.1:3001/api/health
```

The unit starts after the network, restarts on failure, and may only write into `CamWebBackend/data`. Add `After=camvision-server.service` to the `[Unit]` section if you want it to start after CamServer (it does not need to: the backend connects when the first request comes, and again when the connection is gone).

---

## 9. Apache

### 9.1 Modules

```bash
sudo a2enmod proxy proxy_http headers ssl rewrite
```

### 9.2 The certificate

Pick what fits where the browsers are:

| Situation | Certificate |
|---|---|
| A real domain, port 80 of the machine reachable from the internet | Let's Encrypt: `sudo apt install certbot python3-certbot-apache`, then `sudo certbot --apache -d cam.example.com` |
| A real domain, but the machine is **not** reachable from outside | Let's Encrypt with the DNS challenge (`sudo certbot certonly --manual --preferred-challenges dns -d cam.example.com`, or a plugin for your DNS provider) |
| Only the home network, a name from your router | A certificate from your own CA (for example with `mkcert`, installed on the devices that use it) |
| Reached through Tailscale | `sudo tailscale cert <machine>.<tailnet>.ts.net` |

### 9.3 The virtual host

The repository has a ready one: [CamWebBackend/deploy/apache-camvision.conf](CamWebBackend/deploy/apache-camvision.conf). Copy it, replace `cam.example.com` and the paths of the certificate:

```bash
sudo cp /opt/camvision/CamWebBackend/deploy/apache-camvision.conf /etc/apache2/sites-available/camvision.conf
sudo nano /etc/apache2/sites-available/camvision.conf
sudo a2ensite camvision
sudo apache2ctl configtest          # Syntax OK
sudo systemctl reload apache2
```

What the important lines do (all are in the file):

- `ProxyPreserveHost On`: the backend sees the original host name. It checks the `Origin` of every change request against it, so a page of another site cannot give commands.
- `RequestHeader set X-Forwarded-Proto "https"`: the backend then marks the login cookie as `Secure`. **Only set it on the HTTPS virtual host.**
- `ProxyPass / http://127.0.0.1:3001/`: everything (the web UI, the API, the live stream) goes to the backend.
- `ProxyTimeout 3600`: the live view is a response that never ends.

**Without HTTPS** (a trusted home network, for a first test): use only a `<VirtualHost *:80>` with `ProxyPreserveHost On` and the two `ProxyPass` lines, and **no** `X-Forwarded-Proto` header. The login works (the cookie is then not `Secure`), but the password travels unencrypted. Do not do this beyond a first test.

### 9.4 Test it

From another computer (replace the name):

```bash
curl -i https://cam.example.com/api/health
# HTTP/2 200   {"ok":true}
```

Then open `https://cam.example.com` in a browser.

---

## 10. Firewall

If `ufw` is active (`sudo ufw status`):

```bash
sudo ufw allow 80,443/tcp                                   # Apache
sudo ufw allow from 192.168.1.0/24 to any port 45645 proto udp   # cameras and displays: your network only
# the SSH rule you already have
```

Do **not** open `3001`, `45650` or `45651`. They listen on `127.0.0.1` and are not reachable from the network anyway; the firewall rule is a second layer.

**Never forward UDP 45645 from your router to the internet.** The connection of the cameras is authenticated and encrypted, but the server is made for a trusted network. If a camera is somewhere else, connect it through a VPN (see [step 13](#13-reaching-it-from-outside-the-house)).

---

## 11. First login and connecting devices

1. Open `https://cam.example.com`, log in with `ADMIN_USER` and the password you gave to `hash-password`.
2. **Connection** page: the state must say *Connected*. The token comes from the token file of CamServer. If it says *Not connected*, see the troubleshooting table below. "Test connection" tries the settings without saving them.
3. **Overview → Add a device**: make a key for each camera and display. The key is shown **once**: copy the two lines into `client.cfg` (camera) or `display.cfg` (display) on that device. This only works because the backend runs on the same machine as CamServer (CamServer hands out keys to programs on its own computer only).
4. On each camera computer, in `client.cfg`:
   ```ini
   server_ip = <the IP address of the Ubuntu machine>
   server_port = 45645
   key = <the key from the web UI>
   name = <the name you gave it>
   ```
   Start `CamClient` (for an always-on camera, run it as a service as described under "Start the camera client automatically" in the [README](README.md)).
5. The camera shows up on the Overview within seconds. Click it for the live view.

---

## 12. Disk space and maintenance

**Disk.** Recordings are Motion JPEG (the camera's pictures as they are): about **70–150 MB per minute** per camera at 720p, much more than a normal video. Set `record_keep_days` and `record_max_gb` in `server.cfg` so the disk never fills up (CamServer deletes the oldest recordings). Saving a clip from the web UI with faces drawn in takes about 15 seconds per minute of video.

**Logs.**

```bash
journalctl -u camvision-server -n 100 --no-pager
journalctl -u camvision-web -n 100 --no-pager
sudo journalctl --vacuum-time=30d          # keep 30 days of logs
```

**Updating** (after you pushed changes to the repository):

```bash
sudo -u camvision -H bash -c '
  cd /opt/camvision && git pull &&
  python3 Setup.py &&
  make config=release CamServer -j2 &&
  cd CamWeb && npm ci && npm run build &&
  cd ../CamWebBackend && npm ci && npm run build'
sudo systemctl restart camvision-server camvision-web
```

The backend serves the files of `CamWeb/out` live, but restart it anyway so it takes up new settings. Everybody has to log in again only if you delete `CamWebBackend/data/session-secret`.

**Backups.** What cannot be rebuilt (copy it regularly, and keep the copy private, it holds secrets):

| File | What it is |
|---|---|
| `CamServer/server.cfg` | the settings (email password, if you use it) |
| `CamServer/devices.cfg` | the keys of all cameras and displays |
| `CamServer/websocket_token.txt` | the token of the remote control |
| `CamServer/known_faces/` | the photos of the known people |
| `CamWebBackend/.env` | the login hash and the settings |
| `CamWebBackend/data/` | the saved connection settings and the session secret |

The recordings are as big as they are, back them up (or not) as you like.

**Restart after a power failure:** both services are `enabled`, so they start with the machine. CamServer first, then the backend; the order does not matter.

---

## 13. Reaching it from outside the house

Do not open ports on your router for this. The login protects the web UI, but it controls your cameras. Better options:

- **Tailscale** (or WireGuard): install it on the Ubuntu machine and on your phone and laptop (`curl -fsSL https://tailscale.com/install.sh | sh`, `sudo tailscale up`). Your devices then reach the machine like at home, from anywhere, and nothing is exposed. `sudo tailscale cert` gives a real HTTPS certificate for the machine name, which you can use in the Apache virtual host. Cameras at other places can send to the Tailscale address of the machine.
- If you must expose Apache (port 443) to the internet: use a strong password, keep the machine updated (`sudo apt install unattended-upgrades`), and consider `fail2ban` (`sudo apt install fail2ban`). The login is already limited to 10 tries per minute and address, but a second layer is better. Never expose `45645`, `45650`, `45651` or `3001`.

---

## 14. Security checklist

- [ ] `websocket_bind = 127.0.0.1` in `server.cfg`, and `HOST=127.0.0.1` in `.env`.
- [ ] HTTPS on the virtual host, with `RequestHeader set X-Forwarded-Proto "https"` there.
- [ ] `auth = true` in `server.cfg` (device keys).
- [ ] A strong admin password (the hash is made with `npm run hash-password`).
- [ ] `.env` has mode `600`, and `CamServer/websocket_token.txt` too (CamServer creates it that way).
- [ ] Only ports 80, 443 and UDP 45645 (from your own network) are open in the firewall, and nothing is forwarded by the router.
- [ ] The two services run as the user `camvision`, not as root.
- [ ] `record_keep_days` / `record_max_gb` are set.
- [ ] The machine gets security updates (`unattended-upgrades`).

---

## Troubleshooting

| Problem | Cause and fix |
|---|---|
| Apache: `502 Bad Gateway` | The backend is not running: `systemctl status camvision-web`, `journalctl -u camvision-web -n 50`. |
| The backend does not start: *"There is no login yet"* | `ADMIN_PASSWORD_HASH` is empty or not in `.env`. Run `npm run hash-password`. |
| The backend does not start: *"Wrong setting"* | A value in `.env` is not valid (the message names it). |
| Login always says *Wrong user name or password* | The hash in `.env` is not for that password, or `.env` was changed without restarting (`sudo systemctl restart camvision-web`; it reads `.env` only at the start). The log says whether the user name or the password was wrong. A **plain password** in `ADMIN_PASSWORD_HASH` never works, it must be the hash. |
| Login gives `403` ("The request comes from another site") | Apache does not pass the host: `ProxyPreserveHost On` is missing, or you open the page under another name than the one in the virtual host. |
| Login works, but you are logged out immediately | The cookie is `Secure` (HTTPS) but the page is opened over HTTP, or the other way round. Use the HTTPS address, and set `X-Forwarded-Proto` only on the HTTPS virtual host. |
| Connection page: *"CamServer is not reachable"* | CamServer is not running (`systemctl status camvision-server`), or `CAMSERVER_PORT` is not its `websocket_port`, or `websocket_port = 0`. |
| Connection page: *"There is no token"* / *"did not accept the token"* | The backend cannot read `CamServer/websocket_token.txt` (is it the same user? `sudo -u camvision cat /opt/camvision/CamServer/websocket_token.txt`), or the token was replaced. Enter the right token on the Connection page (it is saved in `data/connection.json`), or fix `CAMSERVER_TOKEN_FILE`. |
| CamServer exits at once, with an error about a window or display | `preview = false` is missing in `server.cfg` (the preview windows need a desktop). |
| No camera shows up | The camera cannot reach UDP `45645` (firewall, wrong `server_ip`), or its key is wrong (the log of `camvision-server` shows the connection attempts and why one failed). Make the key again in the web UI and copy both lines into the `client.cfg` of the camera. |
| Live view stays on *"Waiting for the video"* | Apache buffers the stream. Check that no `mod_cache`/`mod_deflate` rule applies to `/api/stream/`; test the stream on the machine with `curl -s http://127.0.0.1:3001/…` (the link from the page) to see if it works without Apache. |
| Live view: *"Too many people watch"* | The limit per camera is `MAX_VIEWERS_PER_CAMERA` (default 8). |
| The picture has no boxes around the faces | `faces = false`, or the models are missing in `CamServer/models`, or `face_on_displays = false` (the web view follows that setting). |
| Saving a clip fails or takes very long | A minute of video takes about 15 seconds with the faces drawn in; `face_on_saved_clips = false` makes it fast but plain. The log of CamServer says more. |
| Recordings are listed, but the download says *"Downloads are off"* | `RECORDINGS_DIR` is not set in `.env`, or points to the wrong folder. |
| *"Cannot write … read-only file system"* in the log of the backend | The unit only allows writing into `CamWebBackend/data` (`ReadWritePaths`). `DATA_DIR` must be that folder. |
| The web UI shows old pages after an update | Hard reload the browser (Ctrl+Shift+R). The files of `_next/static` have the hash of their content in the name, so only the page files are ever stale. |

If something is not in this table: `journalctl -u camvision-web -n 100` and `journalctl -u camvision-server -n 100` show what happened, and `curl http://127.0.0.1:3001/api/health` tells whether the backend answers at all.
