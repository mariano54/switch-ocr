# Switch OCR

Always-on, in-game Japanese OCR overlay for the Nintendo Switch. Pressing `Minus` in any game captures the current frame via SysDVR, sends it to a local Mac server that calls Gemini for OCR, and draws the transcription as a transparent HUD over the game.

## Architecture

```text
[Switch sysmodule] --(TCP)--> [Mac server] --(SysDVR bridge)--> [Switch SysDVR sysmodule]
                                  |
                                  +--> Gemini (Japanese OCR + furigana + definitions)
                                  +--> JMDict + frequency + kanji dictionaries
```

- `switch/sysmodule/`: Atmosphere boot sysmodule. Polls `Minus` and D-pad input, sends OCR HTTP requests, writes results to `sdmc:/config/switch-ocr/`.
- `switch/overlay/`: Tesla overlay that auto-opens as a fullscreen transparent HUD, reads result files, draws the sentence + selected word + definition.
- `server/`: Python HTTP server on the Mac. The current sysmodule uploads a `caps:sc` JPEG to `/ocr-upload`; the older SysDVR `/ocr-latest` path remains as a disabled fallback/debug path.

## Requirements

### Switch

- Homebrew-enabled Nintendo Switch (modchip or RCM-injectable).
- **Hekate** bootloader. Confirmed working: `v6.x`.
- **Atmosphere** CFW. Confirmed working: `v1.7.x`.
- **nx-ovlloader** sysmodule (provides Tesla overlay loader). Confirmed working: `v2.0.x`.
- **Tesla-Menu** overlay. Confirmed working: `v1.4.x`.
- **SysDVR** sysmodule, configured for **TCP Bridge** mode (NOT RTSP / Simple Network). Confirmed working: `v6.3`.
- DBI or another MTP responder for installing artifacts over USB.

Fill in your exact versions in `Tested Config` below.

### Mac (server)

- macOS 14 or later. Confirmed: macOS 26.x on Apple Silicon.
- Python 3.9 or later.
- Docker Desktop or compatible (for building Switch artifacts via `devkitpro/devkita64`).
- Google AI Studio API key.
- Homebrew packages:

```sh
brew install ffmpeg libmtp pkg-config cloudflared
```

Package usage:

- `ffmpeg` — legacy SysDVR frame capture/debug path.
- `libmtp` + `pkg-config` — build and use the MTP install helper.
- `cloudflared` — optional remote access via Cloudflare Tunnel.

### Network

- Switch and Mac on the same LAN (Ethernet preferred on both for SysDVR throughput).
- Switch IP must be reachable on the Mac.
- Mac IP must be reachable on the Switch.

## Setup

### 1. Configure the Switch

Install on the SD card under the right paths:

- `sdmc:/atmosphere/contents/00FF0000A53BB665/` — SysDVR sysmodule.
- `sdmc:/atmosphere/contents/420000000007E51A/` — nx-ovlloader + Tesla-Menu.

Open **SysDVR-Settings** and set streaming mode to **TCP Bridge**, then enable streaming. This is the most common source of capture failures.

Tesla launch combo defaults to `L + ↓ + RS`. Configure it in `sdmc:/config/tesla/config.ini` if needed.

### 2. Configure the Mac

Clone the repo and add the Gemini key to `.env` in the repo root:

```sh
GOOGLE_AI_STUDIO_API_KEY=your_key_here
```

`.env` is git-ignored. Do not commit it.

Edit `Makefile` to point `MAC_IP` at this Mac, and `switch/sysmodule/Makefile` / `switch/overlay/Makefile` if you want non-default `SERVER_HOST` / `SERVER_PORT` baked into the Switch binaries.

`server/frame_capture.py` reads `SWITCH_IP` from the environment (default `192.168.0.136`). The Mac LaunchAgent (see step 5) sets it via `run_server.sh`; otherwise export it before starting the server.

### 3. Build the Switch artifacts

Uses the official devkitPro Docker image, no local devkitPro install needed:

```sh
/usr/bin/make sysmodule-build overlay-build
```

Outputs:

```text
switch/sysmodule/out/atmosphere/contents/42000000534F4352/exefs.nsp
switch/sysmodule/out/atmosphere/contents/42000000534F4352/toolbox.json
switch/sysmodule/out/atmosphere/contents/42000000534F4352/flags/boot2.flag
switch/overlay/switch-ocr.ovl
```

### 4. Install on the Switch (MTP)

On the Switch, run DBI MTP responder. Then:

```sh
/usr/bin/make sysmodule-install overlay-install-reload
```

Reboot the Switch into Atmosphere so the new boot2 sysmodule loads.

Alternatively, mount the SD card and copy the files manually:

```text
switch/sysmodule/out/atmosphere/contents/42000000534F4352/  ->  sdmc:/atmosphere/contents/42000000534F4352/
switch/overlay/switch-ocr.ovl                                ->  sdmc:/switch/.overlays/switch-ocr.ovl
```

### 5. Run the Mac server

For quick testing:

```sh
make server
```

For an always-on service, install the included LaunchAgent. The plist wraps the server in `ssh localhost` because SysDVR-Client needs a real user session to complete its TCP bridge handshake; plain `nohup`/`launchd` fails reliably on macOS.

```sh
# 1. Create a passwordless loopback SSH key and authorize it on this Mac.
ssh-keygen -t ed25519 -f ~/.ssh/switchocr_local_ed25519 -N ""
cat ~/.ssh/switchocr_local_ed25519.pub >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys

# 2. Make sure "Remote Login" is enabled: System Settings -> General -> Sharing.

# 3. Install and start the LaunchAgent.
mkdir -p ~/Library/LaunchAgents ~/Library/Logs/SwitchOCR
sed "s|REPLACE_ME_HOME|$HOME|g" scripts/com.switchocr.server.plist \
    > ~/Library/LaunchAgents/com.switchocr.server.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.switchocr.server.plist
launchctl kickstart -k gui/$(id -u)/com.switchocr.server
```

Logs go to `~/Library/Logs/SwitchOCR/server.{out,err}.log`. Verify:

```sh
curl http://<mac-ip>:8742/health
```

### 6. Optional Cloudflare Tunnel

Cloudflare Tunnel can expose the Mac mini without a static ISP IP or router port forwarding. Keep the Python server private on `127.0.0.1:8742` for production; the current LAN LaunchAgent uses `0.0.0.0` for Switch LAN testing.

For a no-account public smoke test from the Mac mini:

```sh
./scripts/run_cloudflare_quick_tunnel.sh
```

`cloudflared` prints a temporary `https://*.trycloudflare.com` URL. Test:

```sh
curl https://<temporary-host>/health
```

For a stable production tunnel:

```sh
cloudflared tunnel login
cloudflared tunnel create switchocr
cloudflared tunnel route dns switchocr ocr.example.com
```

Create `~/.cloudflared/config.yml` on the Mac mini:

```yaml
tunnel: switchocr
credentials-file: /Users/YOUR_USER/.cloudflared/<tunnel-id>.json
ingress:
  - hostname: ocr.example.com
    service: http://127.0.0.1:8742
  - service: http_status:404
```

Then install the LaunchAgent template:

```sh
mkdir -p ~/Library/LaunchAgents ~/Library/Logs/SwitchOCR
sed "s|REPLACE_ME_HOME|$HOME|g" scripts/com.switchocr.cloudflare-tunnel.plist \
    > ~/Library/LaunchAgents/com.switchocr.cloudflare-tunnel.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.switchocr.cloudflare-tunnel.plist
launchctl kickstart -k gui/$(id -u)/com.switchocr.cloudflare-tunnel
```

Do not expose `/ocr-upload` publicly without adding HMAC authentication, replay protection, request size limits, and rate limits.

## Usage

1. Boot the Switch into Atmosphere. Confirm SysDVR is running and in TCP Bridge mode.
2. Launch any game.
3. Trigger the Tesla launch combo to open the Switch OCR overlay. **First launch after reboot may need to be triggered twice** while nx-ovlloader loads the overlay from disk; subsequent opens are immediate.
4. The overlay is a fullscreen transparent HUD. Gameplay input is passed through, so you can keep playing.
5. Press `Minus` to OCR the current frame. The HUD shows `Loading...` then the transcription.
6. Press `D-Pad Left` / `Right` to move the selected word; the second row shows reading, definition, frequency, and kanji info.

If you press `Minus` again while a request is in flight, the previous one is aborted and only the newest result is displayed.

## Endpoints

- `GET /health` — liveness check.
- `POST /ocr-upload` — OCRs an uploaded Switch JPEG. Used by the Switch sysmodule.
- `POST /ocr-latest` — older SysDVR latest-frame path, kept as a fallback/debug endpoint.
- `POST /ocr` — OCR an uploaded image (debugging).
- `POST /hello` — echo endpoint (debugging).

## Tested Config

| Component | Version |
| --- | --- |
| Switch firmware | 18.x |
| Hekate | v6.x |
| Atmosphere | v1.7.x |
| nx-ovlloader | v2.0.x |
| Tesla-Menu | v1.4.x |
| SysDVR sysmodule | v6.3 |
| DBI | v680+ |
| macOS | 26.x (Apple Silicon) |
| Python | 3.9.6 |
| ffmpeg | 8.1 |
| Docker | 29.x |
| SysDVR-Client | 6.3 |

Update these to match your actual setup; some of the Switch-side versions need to be filled in from `sdmc:/atmosphere/`.

## Troubleshooting

- **`Mac frame capture failed` in the HUD.** SysDVR is not in TCP Bridge mode, or it isn't actually streaming. Open SysDVR-Settings on the Switch and switch the mode. Earlier RTSP URLs like `rtsp://<switch-ip>:6666/` indicate it is in RTSP/Simple Network mode, which is incompatible with this server.
- **`SysDVR bridge did not start recording` in the server logs.** SysDVR is reachable but the handshake failed. Reboot the Switch or toggle SysDVR streaming off/on.
- **`Connection reset by peer` from `SysDVR-Client`.** Same as above; the Switch closed the bridge handshake. Confirm TCP Bridge mode.
- **`moov atom not found` in server logs.** SysDVR-Client recorded but the mp4 was truncated. Usually transient; the next request should succeed. If it keeps happening, increase `SYSDVR_RECORD_SECONDS`.
- **Switch OCR overlay needs to be opened twice the first time.** Known nx-ovlloader cold-load behavior; the first invoke loads the overlay binary into memory, the second invoke shows it. Subsequent opens are instant.
- **`Missing GOOGLE_AI_STUDIO_API_KEY`** in server logs. The `.env` is not where the server expects, or the daemon was restarted before it was copied. Place it at the repo root.
- **HUD never appears in-game.** Make sure `nx-ovlloader` and `Tesla-Menu` are both installed under `sdmc:/atmosphere/contents/420000000007E51A/`, and the Tesla combo is configured.
- **Switch boots to OFW intermittently.** Modchip glitch failure or Hekate autoboot misconfigured. Not a SwitchOCR issue.

## Project Layout

```text
server/                       Mac OCR HTTP server (Python)
  app.py                      HTTP endpoints
  frame_capture.py            SysDVR bridge capture wrapper
  ocr/                        Gemini call, JMDict, frequency, kanji
switch/
  sysmodule/                  Atmosphere boot2 sysmodule (input + HTTP client)
  overlay/                    Tesla overlay (fullscreen transparent HUD)
  hello_ocr_nro/              Legacy NRO prototype (kept for reference)
tools/                        MTP upload helper and frame-source utilities
run_server.sh                 LaunchAgent entrypoint on Mac
```

## Credits

Built on top of [SysDVR](https://github.com/exelix11/SysDVR), [libnx](https://github.com/switchbrew/libnx), [libtesla](https://github.com/WerWolv/libtesla), and Google Gemini for OCR. JMDict and frequency data are loaded by `server/ocr/jmdict_dictionary.py` and `server/ocr/frequency_dictionary.py`.
