# Switch OCR

Always-on, in-game Japanese OCR for the Nintendo Switch. Press `Minus` or `Capture` in any game; the Atmosphere sysmodule captures a `caps:sc` JPEG screenshot, uploads it to the Mac server at `/ocr-upload`, and the Tesla overlay draws the Japanese text, selected word, reading, definition, frequency, kanji info, and mining status over the game.

## Architecture

```text
[Switch sysmodule] -- HTTPS/HMAC or LAN HTTP --> [Mac server]
                                                        |
                                                        +--> Gemini OCR
                                                        +--> JMDict, frequency, kanji dictionaries
                                                        +--> ISSEN mining sync, when configured

[Tesla overlay] <---- sdmc:/config/switch-ocr/*.json/text
```

- `switch/sysmodule/`: boot2 sysmodule that polls input, captures screenshots with `caps:sc`, uploads JPEGs, handles mining requests, and writes HUD state under `sdmc:/config/switch-ocr/`.
- `switch/overlay/`: fullscreen transparent Tesla HUD. It reads sysmodule output files and passes gameplay input through.
- `server/`: Python HTTP server on the Mac. Remote clients use HMAC-authenticated HTTPS through Cloudflare Tunnel; LAN clients can fall back to the compiled local host when `remote.json` is absent.

## Requirements

### Switch

- Homebrew-enabled Nintendo Switch booting Atmosphere through Hekate.
- `nx-ovlloader` and Tesla-Menu.
- DBI or another MTP responder for installing files over USB.

### Mac Server

- macOS 14 or later.
- Python 3.9 or later.
- Docker Desktop or compatible Docker runtime for Switch builds via `devkitpro/devkita64`.
- Google AI Studio API key.
- Homebrew packages:

```sh
brew install libmtp pkg-config cloudflared
```

`libmtp` and `pkg-config` build the MTP install helper. `cloudflared` is needed for the stable remote HTTPS path.

## Setup

### 1. Configure the Mac

Clone the repo and create `.env` in the repo root:

```sh
GOOGLE_AI_STUDIO_API_KEY=your_key_here
```

Mining through ISSEN is optional and uses environment variables only:

```sh
ISSEN_EMAIL=you@example.com
ISSEN_PASSWORD=your_password
ISSEN_LANGUAGE=japanese
```

`.env` is git-ignored. Do not commit credentials.

Dictionary enrichment uses user-provided Yomitan/Yomichan ZIP dictionaries. The project does not ship dictionary data; download dictionaries separately, for example from the [Yomichan dictionary guide](https://learnjapanese.moe/yomichan/), and check each dictionary's license before use.

For reproducible setup, keep the ZIP files anywhere readable by the server and point `.env` at them:

```sh
YOMITAN_TERM_DICTIONARY_PATHS=/path/to/[Bilingual] JMdict (Recommended).zip
YOMITAN_FREQUENCY_PATHS=/path/to/[Freq] Example Frequency.zip
YOMITAN_KANJI_PATHS=/path/to/KANJIDIC.zip
```

Each variable accepts multiple ZIP paths separated with the OS path separator (`:` on macOS/Linux). If these variables are not set, the server auto-detects dictionaries on the current user's Desktop:

- Terms: ZIPs matching `*JMdict*Recommended*.zip`, with `[Bilingual] JMdict (Recommended).zip` preferred.
- Frequency: ZIPs whose filenames start with `[Freq]`.
- Kanji: ZIPs whose filenames include `KANJIDIC`.

Supported archives are Yomitan/Yomichan ZIP dictionaries containing `term_bank_*.json` for terms, `term_meta_bank_*.json` for frequency, or `kanji_bank_*.json` for kanji. Restart the server after changing `.env` or replacing dictionary ZIPs.

For LAN-only testing, set `MAC_IP` in `Makefile` to this Mac's LAN address before building the sysmodule. Remote HTTPS users can keep the baked LAN host as a fallback because `remote.json` takes precedence.

### 2. Run the Server

For quick testing:

```sh
make server
```

For an always-on LaunchAgent:

```sh
mkdir -p ~/Library/LaunchAgents ~/Library/Logs/SwitchOCR
sed "s|REPLACE_ME_HOME|$HOME|g" scripts/com.switchocr.server.plist \
    > ~/Library/LaunchAgents/com.switchocr.server.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.switchocr.server.plist
launchctl kickstart -k gui/$(id -u)/com.switchocr.server
```

Logs go to `~/Library/Logs/SwitchOCR/server.{out,err}.log`. Verify locally:

```sh
curl http://127.0.0.1:8742/health
```

For LAN fallback, verify from another machine on the same network:

```sh
curl http://<mac-ip>:8742/health
```

### 3. Configure Remote HTTPS

```text
https://ocr.example.com/ocr-upload
```

Remote HTTPS is optional, but recommended outside a trusted LAN. One common setup is a Cloudflare Tunnel routing your hostname to the server on port `8742`. A typical `~/.cloudflared/config.yml` looks like:

```yaml
tunnel: switchocr
credentials-file: <home>/.cloudflared/<tunnel-id>.json
ingress:
  - hostname: ocr.example.com
    service: http://127.0.0.1:8742
  - service: http_status:404
```

Install the included LaunchAgent:

```sh
mkdir -p ~/Library/LaunchAgents ~/Library/Logs/SwitchOCR
sed "s|REPLACE_ME_HOME|$HOME|g" scripts/com.switchocr.cloudflare-tunnel.plist \
    > ~/Library/LaunchAgents/com.switchocr.cloudflare-tunnel.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.switchocr.cloudflare-tunnel.plist
launchctl kickstart -k gui/$(id -u)/com.switchocr.cloudflare-tunnel
```

Verify the tunnel:

```sh
curl https://ocr.example.com/health
```

Do not expose `/ocr-upload` publicly without the server-side API key registry installed.

### 4. Provision the Remote API Key

Create a server key registry and Switch `remote.json`:

```sh
make remote-key-create REMOTE_ENDPOINT=https://ocr.example.com/ocr-upload REMOTE_KEY_NAME=<key-name>
```

This writes:

```text
~/.switchocr/api_keys.json     # server-side key registry, chmod 600
tmp/switchocr_remote.json      # client config to install on the Switch
```

Run this command on the server host so `~/.switchocr/api_keys.json` is created where the server will read it. Install the Switch config over DBI MTP:

```sh
make remote-key-install
```

When `sdmc:/config/switch-ocr/remote.json` exists, the sysmodule uses HTTPS/DNS and signs requests to its `endpoint`. When it is absent, the sysmodule uses the compiled LAN HTTP fallback.

Remote HMAC timestamps accept a wide clock-skew window by default to tolerate Switch clock drift after reboot/CFW. Override that window with `SWITCHOCR_TIMESTAMP_SKEW_SECONDS` if needed.

### 5. Build and Install Switch Artifacts

Build with the devkitPro Docker image:

```sh
/usr/bin/make MAC_IP=<mac-ip> sysmodule-build overlay-build
```

`MAC_IP` is compiled into the sysmodule as the LAN HTTP fallback. Remote HTTPS users still install `remote.json`; when present, it takes precedence over the compiled fallback.

Outputs:

```text
switch/sysmodule/out/atmosphere/contents/42000000534F4352/exefs.nsp
switch/sysmodule/out/atmosphere/contents/42000000534F4352/toolbox.json
switch/sysmodule/out/atmosphere/contents/42000000534F4352/flags/boot2.flag
switch/overlay/switch-ocr.ovl
```

On the Switch, run DBI MTP responder, then install:

```sh
/usr/bin/make sysmodule-install overlay-install-reload
```

Reboot into Atmosphere so the boot2 sysmodule loads. If installing manually, copy:

```text
switch/sysmodule/out/atmosphere/contents/42000000534F4352/  ->  sdmc:/atmosphere/contents/42000000534F4352/
switch/overlay/switch-ocr.ovl                                ->  sdmc:/switch/.overlays/switch-ocr.ovl
tmp/switchocr_remote.json                                     ->  sdmc:/config/switch-ocr/remote.json
```

Tesla launch combo defaults to `L + Down + Right Stick`. Configure it in `sdmc:/config/tesla/config.ini` if needed.

## Usage

1. Boot the Switch into Atmosphere.
2. Launch any game.
3. Open the Switch OCR overlay from Tesla. First launch after reboot may need to be triggered twice while `nx-ovlloader` loads the overlay from disk.
4. Press `Minus` or `Capture` to OCR the current frame. The HUD shows loading state, then the OCR result.
5. Press `D-Pad Left` / `D-Pad Right` to move the selected word.
6. Press `Right Stick` to save or sync the selected word through ISSEN when mining is configured.

The overlay is a fullscreen transparent HUD with pass-through input. If a new OCR request starts while another is running, the newest request wins.

## Endpoints

- `GET /health` — liveness check.
- `POST /ocr-upload` — OCRs an uploaded Switch JPEG. This is the active Switch capture path.
- `GET /mining/status` — returns mining provider status and saved words.
- `POST /mine-word` — saves or syncs the selected word.
- `POST /screenshot-probe` — saves a raw uploaded screenshot for capture diagnostics.
- `POST /log` — accepts concise Switch-side log messages.

Remote requests to protected endpoints require HMAC headers unless they come from an allowed private LAN client.

## Troubleshooting

- **Overlay needs to be opened twice the first time.** This is normal `nx-ovlloader` cold-load behavior; subsequent opens are immediate.
- **`Missing GOOGLE_AI_STUDIO_API_KEY` in server logs.** Put `.env` at the repo root and restart the LaunchAgent.
- **LAN OCR upload fails.** Confirm `MAC_IP` / `SERVER_HOST` points at this Mac, the server is reachable on port `8742`, and the Switch and Mac are on the same network. Remove `remote.json` only when intentionally testing LAN fallback.
- **Remote OCR returns an auth error.** Recreate and reinstall `remote.json`, copy the matching `~/.switchocr/api_keys.json` to the server host, and restart the server. If the error mentions timestamp skew, make sure the server is running the current code or set `SWITCHOCR_TIMESTAMP_SKEW_SECONDS` explicitly.
- **Remote OCR reports `curl request failed: 60`.** Rebuild, reinstall, and reboot into the current sysmodule; older builds may still be using certificate validation settings that are not compatible with the Switch environment.
- **HUD never appears in-game.** Confirm `nx-ovlloader`, Tesla-Menu, and `switch-ocr.ovl` are installed, then check the Tesla combo in `sdmc:/config/tesla/config.ini`.
- **Switch boots to OFW intermittently.** This is usually modchip glitch failure or Hekate autoboot configuration, not Switch OCR.

## Project Layout

```text
server/                       Mac OCR HTTP server
  app.py                      HTTP endpoints
  auth.py                     HMAC API-key verification
  mining/                     ISSEN sync support
  ocr/                        Gemini OCR and dictionary enrichment
switch/
  sysmodule/                  Atmosphere boot2 sysmodule
  overlay/                    Tesla transparent HUD
tools/                        MTP helper and remote-key utility
run_server.sh                 LaunchAgent server entrypoint
```

## Credits

Built on [libnx](https://github.com/switchbrew/libnx), [libtesla](https://github.com/WerWolv/libtesla), Google Gemini, JMDict, frequency dictionaries, and kanji dictionaries.
