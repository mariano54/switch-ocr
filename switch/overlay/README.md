# Switch OCR Overlay

Tesla/libtesla HUD for Switch OCR. The overlay is a fullscreen transparent view that reads OCR and mining state from `sdmc:/config/switch-ocr/` and passes gameplay input through.

The active screenshot capture and network upload path lives in the boot2 sysmodule. The overlay does not call the Mac server directly.

## Install Path

```text
sdmc:/switch/.overlays/switch-ocr.ovl
```

## Controls

- `Minus` or `Capture`: request OCR for the current frame through the sysmodule.
- `D-Pad Left` / `D-Pad Right`: move the selected word.
- `Right Stick`: save or sync the selected word when mining is configured.

## Build

```sh
/usr/bin/make overlay-build
```

## Install

```sh
/usr/bin/make overlay-install-reload
```
