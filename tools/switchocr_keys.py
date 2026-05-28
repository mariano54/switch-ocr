#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import secrets
import subprocess
import time
from pathlib import Path
from typing import Any


DEFAULT_ENDPOINT = "https://ocr.example.com/ocr-upload"
DEFAULT_KEY_ID = "switch-main"
DEFAULT_KEYS_PATH = Path("~/.switchocr/api_keys.json").expanduser()
DEFAULT_REMOTE_CONFIG = Path("tmp/switchocr_remote.json")
DEFAULT_MTP_TOOL = Path("/tmp/switch_mtp_tool")


def load_registry(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"keys": []}
    data = json.loads(path.read_text(encoding="utf-8"))
    return data if isinstance(data, dict) and isinstance(data.get("keys"), list) else {"keys": []}


def save_registry(path: Path, registry: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(path, 0o600)


def upsert_key(registry: dict[str, Any], key_id: str, secret: str) -> None:
    keys = registry.setdefault("keys", [])
    for item in keys:
        if isinstance(item, dict) and item.get("id") == key_id:
            item["secret"] = secret
            item["updated_at"] = int(time.time())
            return
    keys.append({"id": key_id, "secret": secret, "created_at": int(time.time())})


def write_remote_config(path: Path, endpoint: str, key_id: str, secret: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    config = {"endpoint": endpoint, "key_id": key_id, "secret": secret}
    path.write_text(json.dumps(config, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(path, 0o600)


def create(args: argparse.Namespace) -> None:
    key_id = args.name
    secret = secrets.token_urlsafe(32)
    registry = load_registry(args.keys_path)
    upsert_key(registry, key_id, secret)
    save_registry(args.keys_path, registry)
    write_remote_config(args.remote_config, args.endpoint, key_id, secret)
    print(f"Created key {key_id}")
    print(f"Server registry: {args.keys_path}")
    print(f"Switch config: {args.remote_config}")


def install_mtp(args: argparse.Namespace) -> None:
    if not args.remote_config.exists():
        raise SystemExit(f"Missing remote config: {args.remote_config}")
    if not args.mtp_tool.exists():
        raise SystemExit(f"Missing MTP tool: {args.mtp_tool}")

    remote = "sd:/config/switch-ocr/remote.json"
    subprocess.run([str(args.mtp_tool), "delete", remote], check=False)
    subprocess.run([str(args.mtp_tool), "put", str(args.remote_config), remote], check=True)
    print(f"Installed {args.remote_config} -> {remote}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Create and install Switch OCR API keys.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    create_parser = subparsers.add_parser("create")
    create_parser.add_argument("--name", default=DEFAULT_KEY_ID)
    create_parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    create_parser.add_argument("--keys-path", type=Path, default=DEFAULT_KEYS_PATH)
    create_parser.add_argument("--remote-config", type=Path, default=DEFAULT_REMOTE_CONFIG)
    create_parser.set_defaults(func=create)

    install_parser = subparsers.add_parser("install-mtp")
    install_parser.add_argument("--remote-config", type=Path, default=DEFAULT_REMOTE_CONFIG)
    install_parser.add_argument("--mtp-tool", type=Path, default=DEFAULT_MTP_TOOL)
    install_parser.set_defaults(func=install_mtp)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
