# Switch OCR Overlay

The final in-game UI should live here as a Tesla/libtesla overlay.

Target behavior:

- Open over a running game through `nx-ovlloader`.
- Render a small translucent panel with OCR status/results.
- Send a lightweight request to the Mac server, reusing the same protocol as the NRO prototype.
- Avoid doing heavy OCR or video decoding on the Switch.

Expected install path:

```text
sdmc:/switch/.overlays/switch-ocr.ovl
```

Recommended iteration:

1. Keep `switch/hello_ocr_nro` as the network/capture lab.
2. Once the server protocol is stable, port only the UI and HTTP client code into a libtesla overlay.
3. Use SysDVR or sys-botbase on the Mac side for reliable game-frame capture.

## Current Prototype

The current overlay builds to:

```text
switch/overlay/switch-ocr.ovl
```

It opens as a Tesla overlay, shows a small Switch OCR menu, and calls the Mac OCR server compiled into the build with `SERVER_HOST`.

Controls:

- `A` on `Capture and OCR`: capture a JPEG screenshot and send it to `/ocr`.
- `X`: shortcut for the same capture action while the overlay is open.

Build:

```sh
/usr/bin/make overlay-build
```

Install:

```text
sdmc:/switch/.overlays/switch-ocr.ovl
```
