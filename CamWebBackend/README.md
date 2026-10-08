# CamWebBackend

The backend of the CamVision web UI ([CamWeb](../CamWeb)). It is a small Node.js (TypeScript) server, which

- **logs the user in** (one admin user, password hash in `.env`, the session is a signed `HttpOnly` cookie),
- offers the **API** the web UI uses (`/api/...`),
- keeps one **WebSocket connection to the remote control of CamServer** (the `websocket_*` settings of `server.cfg`) and translates the web UI's calls into its commands,
- turns the pictures of a camera into a **live stream (MJPEG)**,
- and **serves the built web UI** (static files), so there is just one program (one port) behind the reverse proxy.

```
browser ──HTTPS──> Apache ──HTTP──> CamWebBackend (127.0.0.1:3001) ──ws──> CamServer (127.0.0.1:45651)
```

## What the web UI can do (and the CamServer command behind it)

| In the UI | API of the backend | Command of CamServer |
|---|---|---|
| Log in / out | `POST /api/login`, `POST /api/logout`, `GET /api/me` | (the backend itself) |
| Overview: cameras, displays, devices, recording | `GET /api/status` | `status`, `displays`, `devices` (and `snapshot` once for the size of a camera) |
| Save the last minutes of a camera / all cameras | `POST /api/save` | `save` |
| Recordings (list, download) | `GET /api/recordings`, `GET /api/recordings/file?name=` | `recordings` (+ the folder `RECORDINGS_DIR` for the download) |
| Live view of a camera | `POST /api/streams` (the link), `GET /api/stream/<camera>?t=` (the MJPEG stream) | `snapshot` |
| Add a device (the key is shown once) | `POST /api/devices` | `device_add` |
| Remove a device | `DELETE /api/devices/<name>` | `device_remove` |
| Stop the server | `POST /api/stop` | `stop` |
| Connection settings (host, port, token) and test | `GET/PUT /api/connection`, `POST /api/connection/test` | `ping` |

**The live view:** the web UI asks `POST /api/streams {"camera": "Front door"}`, and the backend answers with `{"url": "/api/stream/Front%20door?t=...", "format": "mjpeg"}`. The link is signed and valid for 60 seconds (an image in a page cannot send headers, so the link carries the proof). The stream itself is one loop per watched camera, which asks CamServer for the newest picture (`snapshot`, about `STREAM_FPS` times per second, on a connection of its own, so a long `save` does not stop it) and sends every picture to all viewers. It stops when the last viewer leaves, or the camera stops sending.

## Security

- The login: `scrypt` password hash, 10 tries per minute and address, a wrong answer takes at least half a second, and the answer for a wrong user name takes as long as for a wrong password.
- The session is a signed cookie, `HttpOnly`, `SameSite=Strict`, `Secure` when the visitor uses HTTPS. A request with an `Origin` of another site is refused.
- The token of CamServer is never sent to the browser (the Connection page only shows where it comes from). Saved settings are written to `data/connection.json` (mode 600). A saved token is only used for the server it was saved for: another host or port needs its token entered again, so the token cannot be sent to a place somebody just typed in.
- `device_add` hands out a key of a device. CamServer only does that for programs on its own computer, so the backend has to run on the same computer as CamServer (it does in this setup). The key is passed on to the browser once and not kept.
- Keep `HOST=127.0.0.1`: the HTTP between Apache and the backend is not encrypted, and the backend should not be reachable from the network except through Apache.
- Whoever has the admin login controls the cameras and can stop the server.

## Run it on your computer (development)

You need Node.js 22 or newer.

```bash
cd CamWebBackend
npm install
npm run hash-password          # asks for a password, prints the ADMIN_PASSWORD_HASH=... line
cp .env.example .env           # put the line into it
npm run dev                    # the backend on http://127.0.0.1:3001
```

Without the C++ server, `npm run fake-camserver` plays CamServer (same messages, fixed data, token `test-token`: set `CAMSERVER_TOKEN=test-token`).

The web UI in development, in a second terminal (it passes `/api` to the backend):

```bash
cd CamWeb
npm install
npm run dev                    # http://localhost:3000, or: NEXT_PUBLIC_BACKEND=mock npm run dev   (demo data, no backend: admin / camvision)
```

Tests: `npm test` (the API, the login, the stream and the bridge, against a fake CamServer).

## Install it on the Ubuntu server

> The full, step by step guide (CamServer as a service, Apache, certificates, firewall, updating, backups, troubleshooting) is [HOSTING.md](../HOSTING.md) in the root of the repository. This section is the short version.

Everything on the same computer as CamServer. The paths below are examples.

**1. Node.js 22**

```bash
curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash -
sudo apt install -y nodejs
```

**2. The code, and a user for it**

```bash
sudo useradd --system --create-home --home-dir /opt/camvision camvision    # or any existing user
sudo -u camvision git clone <your repository> /opt/camvision               # or copy the folders
```

**3. Build the web UI and the backend**

```bash
cd /opt/camvision/CamWeb && sudo -u camvision npm ci && sudo -u camvision npm run build     # makes CamWeb/out
cd /opt/camvision/CamWebBackend && sudo -u camvision npm ci && sudo -u camvision npm run build   # makes dist/
```

**4. Settings:** `sudo -u camvision mkdir -p data` (the unit may only write there), `sudo -u camvision cp .env.example .env`, then edit it:

- `ADMIN_PASSWORD_HASH`: from `npm run hash-password`.
- `CAMSERVER_TOKEN_FILE=/opt/camvision/CamServer/websocket_token.txt` (the file CamServer made on its first start; the user `camvision` must be able to read it), or enter the token on the Connection page later.
- `RECORDINGS_DIR=/opt/camvision/CamServer/recordings` for the download of the recordings.
- `STATIC_DIR=/opt/camvision/CamWeb/out`.

In `server.cfg` of CamServer, the remote control stays at `websocket_bind = 127.0.0.1` (the default).

**5. Start it at boot (systemd)**

```bash
sudo cp deploy/camvision-web.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now camvision-web
journalctl -u camvision-web -f          # the log
curl http://127.0.0.1:3001/api/health   # {"ok":true}
```

**6. Apache:** `deploy/apache-camvision.conf` is a virtual host with HTTPS, which passes everything to the backend. It needs its own (sub)domain, for example `cam.example.com`. Enable the modules and the site as written at the top of the file. Your other sites keep working: Apache chooses the virtual host by the name.

**7. Updating:** pull, build both again (step 3), `sudo systemctl restart camvision-web`.

## Settings (`.env`)

See [.env.example](.env.example). All have a default except the login.

| Setting | Meaning |
|---|---|
| `HOST`, `PORT` | Where the backend listens (`127.0.0.1:3001`). |
| `DATA_DIR` | Its own files: `connection.json` (the saved connection), `session-secret` (signs the cookie and the stream links; delete it to log everybody out). |
| `ADMIN_USER`, `ADMIN_PASSWORD_HASH` | The login. |
| `SESSION_HOURS` | How long a login is valid. |
| `CAMSERVER_HOST`, `CAMSERVER_PORT`, `CAMSERVER_SECURE` | The start values of the connection to CamServer (what is saved on the Connection page wins). |
| `CAMSERVER_TOKEN`, `CAMSERVER_TOKEN_FILE` | The token of the remote control. |
| `RECORDINGS_DIR` | The recordings folder of CamServer, for the download. |
| `STATIC_DIR` | The built web UI (default `../CamWeb/out`). |
| `STREAM_FPS`, `MAX_VIEWERS_PER_CAMERA` | The live view: pictures per second, and viewers per camera. |

## Changing the API

The routes are in `src/routes.ts`, the translation of the CamServer answers to what the UI needs in `src/overview.ts`, the commands of CamServer in `src/camserver/api.ts`. The web UI side is `CamWeb/lib/backend.ts` (the interface), `http-backend.ts` (the calls) and `types.ts`. If the commands of CamServer change (`Server::RegisterCommands`), `test/fake-camserver.ts` must follow.
