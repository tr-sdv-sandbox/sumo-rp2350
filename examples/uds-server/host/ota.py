#!/usr/bin/env python3
"""
SUIT-over-UDS OTA driver for sumo-rp2350 examples/uds-server (4b).

Builds (or reuses) a fixture envelope+payload using sumo-tool, frames
them as

    [4 B LE env_len] [envelope] [payload]

and pushes the framed buffer to the device via UDS RequestDownload +
TransferData + TransferExit. After TransferExit, polls DID 0xF200
(state) until the device reports OK or FAIL, then ReadDID 0xF201 for
the SHA-256 of the recovered plaintext, and compares against the
locally-computed hash of the fixture's plaintext firmware.

Usage:
  python3 ota.py            # auto-pick canX, build fixture, push
  python3 ota.py can1       # explicit interface
  python3 ota.py --no-build # reuse a previously-built fixture in
                            # /tmp/sumo-rp2350-4b/
"""
import argparse
import hashlib
import os
import pathlib
import struct
import subprocess
import sys
import time

import can
import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection
from udsoncan.services import DiagnosticSessionControl

PHYS_TX_ID = 0x18DA42F1
PHYS_RX_ID = 0x18DAF142

HOST_DIR = pathlib.Path(__file__).resolve().parent
KEYS_DIR = HOST_DIR / "keys"
WORK_DIR = pathlib.Path("/tmp/sumo-rp2350-4b")

SUMO_TOOL = (HOST_DIR.parent.parent.parent.parent
             / "sumo-workspace/components/sumo-rs/target/release/sumo-tool")


# ── Fixture build ──────────────────────────────────────────────────

FIXTURE_BLOCK = b"Sumo RP2350 checkpoint 4b - SUIT-over-UDS test. "  # 48 B


def make_plaintext(reps: int) -> bytes:
    return FIXTURE_BLOCK * reps


def build_fixture(plaintext: bytes,
                  verbose: bool = False) -> tuple[pathlib.Path, pathlib.Path]:
    """Run sumo-tool to produce envelope + external payload. Returns
    (envelope_path, payload_path)."""
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    fw_bin = WORK_DIR / "fw.bin"
    fw_bin.write_bytes(plaintext)

    envelope = WORK_DIR / "c4b.suit"
    payload = WORK_DIR / "fw.enc"

    cmd = [
        str(SUMO_TOOL), "build",
        "--signing-key", str(KEYS_DIR / "sign.key"),
        "--component", "ecu-a,firmware",
        "--seq", "4",
        "--vendor", "fa6b4a53d5ad5fdfbe9de663e4d41ffe",
        "--class",  "1492af1425695e48bf429b2d51f2ab45",
        "--uri", "file:///fw.bin",
        "--firmware", str(fw_bin),
        "--compress", "--zstd-window-log", "10",
        "--encrypt", str(KEYS_DIR / "devkey.cose"),
        "--payload-output", str(payload),
        "--output", str(envelope),
    ]
    if verbose:
        print("==> " + " ".join(cmd))
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stdout)
        sys.stderr.write(res.stderr)
        sys.exit(f"sumo-tool build failed (rc={res.returncode})")
    if verbose:
        print(res.stdout.rstrip())
    return envelope, payload


# ── UDS connection ─────────────────────────────────────────────────

def autopick_can_iface() -> str:
    ifaces = sorted(p.name for p in pathlib.Path("/sys/class/net").iterdir()
                    if p.name.startswith("can"))
    if not ifaces:
        sys.exit("no canX interface present — run `sudo ./setup-can.sh` first")
    if len(ifaces) > 1:
        sys.exit(f"multiple CAN interfaces: {ifaces} — pass one explicitly")
    return ifaces[0]


def make_connection(iface: str) -> PythonIsoTpConnection:
    bus = can.Bus(interface="socketcan", channel=iface)
    addr = isotp.Address(
        isotp.AddressingMode.Normal_29bits,
        txid=PHYS_TX_ID, rxid=PHYS_RX_ID,
    )
    layer = isotp.CanStack(
        bus, address=addr,
        params={"tx_data_min_length": 8, "tx_padding": 0xCC},
    )
    return PythonIsoTpConnection(layer)


# ── Status DIDs ────────────────────────────────────────────────────

class StateChar(udsoncan.DidCodec):
    """0xF200 — single ASCII byte (state name)."""
    def encode(self, val): return val.encode("ascii")
    def decode(self, payload): return payload.decode("ascii", errors="replace")
    def __len__(self): return 1


class FixedBytes(udsoncan.DidCodec):
    """0xF201 — fixed N raw bytes."""
    def __init__(self, n): self.n = n
    def encode(self, val): return val
    def decode(self, payload): return bytes(payload)
    def __len__(self): return self.n


class LeUint32(udsoncan.DidCodec):
    """0xF202 — 4 B little-endian uint32."""
    def encode(self, val): return struct.pack("<I", val)
    def decode(self, payload): return struct.unpack("<I", payload)[0]
    def __len__(self): return 4


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("iface", nargs="?", default=None,
                    help="SocketCAN interface (auto-pick if omitted)")
    ap.add_argument("--reps", type=int, default=32,
                    help="fixture plaintext = FIXTURE_BLOCK * reps "
                         "(48 B each). Default 32 (1.5 KB). "
                         "--reps 21845 ≈ 1 MB.")
    ap.add_argument("--no-build", action="store_true",
                    help="reuse fixture in /tmp/sumo-rp2350-4b/")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    iface = args.iface or autopick_can_iface()

    plaintext = make_plaintext(args.reps)

    # Build the fixture (or reuse).
    if args.no_build:
        envelope = WORK_DIR / "c4b.suit"
        payload = WORK_DIR / "fw.enc"
        if not envelope.exists() or not payload.exists():
            sys.exit(f"--no-build set but {envelope} / {payload} missing")
        print(f"reusing fixture from {WORK_DIR}")
    else:
        print(f"building fixture envelope ({len(plaintext)} B plaintext) ...")
        envelope, payload = build_fixture(plaintext, args.verbose)
        print(f"  envelope: {envelope.stat().st_size} B  ({envelope})")
        print(f"  payload:  {payload.stat().st_size} B  ({payload})")

    env_bytes = envelope.read_bytes()
    pl_bytes = payload.read_bytes()
    framed = struct.pack("<I", len(env_bytes)) + env_bytes + pl_bytes
    print(f"framed: 4 + {len(env_bytes)} + {len(pl_bytes)} = "
          f"{len(framed)} bytes")

    expected_sha = hashlib.sha256(plaintext).hexdigest()
    print(f"expected plaintext SHA-256: {expected_sha}")

    udsoncan.setup_logging()

    config = dict(udsoncan.configs.default_client_config)
    config["data_identifiers"] = {
        0xF200: StateChar(),
        0xF201: FixedBytes(32),
        0xF202: LeUint32(),
    }
    config["use_server_timing"] = False
    # Generous wall-clock budgets — TransferExit can take seconds on
    # 1 MB images (final tag verify + flush + hash); RoutineControl
    # responses come back fast since the erase is in the background.
    config["request_timeout"] = 60.0
    config["p2_timeout"] = 30.0
    config["p2_star_timeout"] = 60.0

    conn = make_connection(iface)
    print(f"using {iface}\n")

    with Client(conn, config=config) as client:
        print("=== DiagSession(programming) ===")
        r = client.change_session(
            DiagnosticSessionControl.Session.programmingSession)
        print(f"  ← session_echo={r.service_data.session_echo}")

        # ── 1. RoutineControl(start, 0xFF00 eraseMemory) ──
        # Kicks off a background erase of the inactive flash slot.
        # The device drives one 4 KB sector erase per main-loop
        # iteration so CAN polling stays responsive. Returns
        # immediately; we then poll DID 0xF200 until state == 'R'.
        print("\n=== RoutineControl(start, 0xFF00 eraseMemory) ===")
        client.start_routine(0xFF00)
        print("  ← started")

        print("\n=== polling DID 0xF200 until 'R' (ready) ===")
        t_start = time.monotonic()
        while True:
            r = client.read_data_by_identifier(0xF200)
            st = r.service_data.values[0xF200]
            if st == "R":
                print(f"  state='R' (ready) after {time.monotonic()-t_start:.2f} s")
                break
            if st == "X":
                sys.exit("device reported 'X' (failed) during prepare")
            time.sleep(0.5)

        print(f"\n=== RequestDownload(addr=0, size={len(framed)}) ===")
        r = client.request_download(
            udsoncan.MemoryLocation(address=0, memorysize=len(framed),
                                     address_format=8, memorysize_format=32))
        max_block = r.service_data.max_length
        print(f"  ← max_block={max_block}")

        print(f"\n=== TransferData (chunks of {max_block}) ===")
        seq = 1
        sent = 0
        while sent < len(framed):
            chunk = framed[sent:sent + max_block]
            client.transfer_data(seq, chunk)
            sent += len(chunk)
            seq = (seq + 1) & 0xFF
            if seq == 0:
                seq = 1   # 0 is reserved
            print(f"  → sent {sent}/{len(framed)}")

        print("\n=== TransferExit ===")
        client.request_transfer_exit()
        print("  ← ok")

        # Poll state until non-V (validating).
        print("\n=== polling DID 0xF200 (state) ===")
        for _ in range(40):
            r = client.read_data_by_identifier(0xF200)
            st = r.service_data.values[0xF200]
            print(f"  state={st!r}")
            if st in ("K", "X"):
                break
            time.sleep(0.1)

        if st == "K":
            r = client.read_data_by_identifier(0xF201)
            got_sha = r.service_data.values[0xF201].hex()
            print(f"\ndevice sha256:   {got_sha}")
            print(f"expected sha256: {expected_sha}")
            if got_sha == expected_sha:
                print("\nOK — SUIT-over-UDS pipeline verified end-to-end.")
                return 0
            print("\nFAIL — SHA-256 mismatch.")
            return 1

        r = client.read_data_by_identifier(0xF202)
        consumed = r.service_data.values[0xF202]
        print(f"\nFAIL — device state ended in {st!r}, consumed={consumed}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
