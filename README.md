# Switch OCR Prototype

This repo contains the first proof-of-concept pieces for a Switch OCR workflow:

- `server/app.py`: local Mac HTTP server.
- `switch/hello_ocr_nro`: first libnx NRO client.
- `switch/overlay`: Tesla/libtesla overlay notes for the next UI step.

## Run the Mac Server

```sh
make server
```

The server listens on `0.0.0.0:8000` and exposes:

- `GET /health`
- `POST /hello`
- `POST /ocr`

## Build the Switch NRO

This uses the official devkitPro Docker image, so a local devkitPro install is not required.

```sh
/usr/bin/make switch-build
```

The output is:

```text
switch/hello_ocr_nro/build/switch_ocr_hello.nro
```

## Fast Iteration With Netloader

On the Switch:

1. Open hbmenu in full-memory mode.
2. Press `Y` to start netloader.

On the Mac:

```sh
/usr/bin/make switch-run SWITCH_IP=<switch-ip>
```

The root `Makefile` auto-detects the Mac LAN IP and compiles it into the NRO as the server host.

## NRO Controls

- `A`: send a JSON hello request to `POST /hello`.
- `X`: try a JPEG screenshot capture and send it to `POST /ocr`.
- `+`: exit.

The screenshot path is an experiment. If `caps:sc` does not expose useful game frames from a normal NRO, the production path should use SysDVR/sys-botbase on the Mac side and keep the Switch overlay focused on triggering OCR and rendering results.

## Build the Tesla Overlay

```sh
/usr/bin/make overlay-build
```

The output is:

```text
switch/overlay/switch-ocr.ovl
```

Install it to:

```text
sdmc:/switch/.overlays/switch-ocr.ovl
```

With DBI MTP responder running, this repo can install it directly:

```sh
/usr/bin/make overlay-install
```

Then open Tesla in-game with your configured Tesla combo, select `Switch OCR`, and press `A` to capture and OCR. The overlay calls the same Mac server at `http://<mac-ip>:8000/ocr`.

Overlay iteration is slower than NRO netload: rebuild, copy the `.ovl` to `sdmc:/switch/.overlays/`, then reload `nx-ovlloader` or reboot if the old overlay remains cached.

## Latest Frame Source

The Tesla overlay calls `POST /ocr-latest`, which OCRs:

```text
server/latest_frame.jpg
```

For a quick server test, seed it from the last uploaded NRO screenshot:

```sh
python3 tools/frame_source.py test server/last_ocr_upload.jpg
```

For SysDVR RTSP mode, keep this running on the Mac:

```sh
python3 tools/frame_source.py rtsp --url rtsp://<switch-ip>:6666/ --interval 1
```

Then open the overlay and press `A` to OCR the latest saved frame.

Tesla hotkey changes in `sdmc:/config/tesla/config.ini` may require reloading `nx-ovlloader` or rebooting Atmosphere before they apply.
