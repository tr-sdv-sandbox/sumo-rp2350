#!/usr/bin/env python3
"""
ota-cycle.py — full OTA cycle orchestrator for sumo-rp2350 ota-flow.

For each version on the command line, this script:

  1. Builds the firmware (cmake -DOTA_VERSION_MAJOR/MINOR/PATCH=…
     with OTA_TBYB=ON so the bootrom requires explicit_buy on first
     trial boot).
  2. Packages the .bin as a SUIT envelope+payload via sumo-tool.
  3. Frames [4 B env_len][envelope][payload] and pushes via UDS:
       DiagSession(programming)
       RoutineControl(start, 0xFF00 eraseMemory)
       poll DID 0xF200 until 'R' (ready)
       RequestDownload(addr=0, size=framed)
       TransferData chunks
       TransferExit
       poll DID 0xF200 until 'S' (staged)
  4. Activates: RoutineControl(start, 0xF001).
     Device FUB-reboots; we reconnect ISO-TP, expect state 'T' (trial).
  5. Verifies: ReadDID(0xF195) returns the new version string.
  6. Commits: RoutineControl(start, 0xF002).
     State goes 'T' → 'K'.

Use --no-commit to skip step 6 (useful for testing the multi-boot
trial counter — the device will roll back after MAX_TRIAL_BOOTS
power cycles without a commit).

Usage:
  ./run-ota-cycle.sh 1.1.0                 # one cycle
  ./run-ota-cycle.sh 1.1.0 1.2.0 1.3.0     # three cycles
  ./run-ota-cycle.sh 1.4.0 --no-commit
"""
import argparse
import hashlib
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time

import can
import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection
from udsoncan.services import DiagnosticSessionControl, ECUReset

PHYS_TX_ID = 0x18DA42F1
PHYS_RX_ID = 0x18DAF142

HOST_DIR = pathlib.Path(__file__).resolve().parent
EXAMPLE_DIR = HOST_DIR.parent
KEYS_DIR = HOST_DIR / "keys"

SUMO_TOOL = (HOST_DIR.parent.parent.parent.parent
             / "sumo-workspace/components/sumo-rs/target/release/sumo-tool")

# Per-version build directories live in /tmp so re-running doesn't clog
# the source tree's CMakeCache.
WORK_ROOT = pathlib.Path("/tmp/sumo-rp2350-ota-flow")


# ── Build per-version firmware ──────────────────────────────────────

VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def parse_version(s: str) -> tuple[int, int, int]:
    m = VERSION_RE.match(s)
    if not m:
        sys.exit(f"version must be MAJOR.MINOR.PATCH, got {s!r}")
    return int(m[1]), int(m[2]), int(m[3])


def build_firmware(version: str, verbose: bool) -> pathlib.Path:
    """Run cmake + make for the requested version with TBYB=ON.
    Returns the path to the .bin (raw image used as SUIT firmware)."""
    major, minor, patch = parse_version(version)
    build = WORK_ROOT / f"build-{version}"
    build.mkdir(parents=True, exist_ok=True)
    cfg_cmd = [
        "cmake",
        "-S", str(EXAMPLE_DIR),
        "-B", str(build),
        "-DPICO_BOARD=pico2",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DOTA_VERSION_MAJOR={major}",
        f"-DOTA_VERSION_MINOR={minor}",
        f"-DOTA_VERSION_PATCH={patch}",
        "-DOTA_TBYB=ON",
    ]
    build_cmd = ["cmake", "--build", str(build), "-j",
                 "--target", "sumo_rp2350_ota-flow"]
    print(f"[build {version}] cmake configure …")
    subprocess.run(cfg_cmd, check=True, capture_output=not verbose)
    print(f"[build {version}] cmake --build")
    subprocess.run(build_cmd, check=True, capture_output=not verbose)
    bin_path = build / "sumo_rp2350_ota-flow.bin"
    if not bin_path.exists():
        sys.exit(f"build produced no .bin at {bin_path}")
    return bin_path


def build_envelope(version: str, fw_bin: pathlib.Path,
                   verbose: bool) -> tuple[pathlib.Path, pathlib.Path]:
    """Run sumo-tool to wrap fw_bin in a SUIT envelope (compress + encrypt).
    Returns (envelope_path, payload_path)."""
    work = WORK_ROOT / f"sumo-{version}"
    work.mkdir(parents=True, exist_ok=True)
    envelope = work / "image.suit"
    payload = work / "image.enc"
    seq = sum(int(x) * 100 ** i for i, x in enumerate(reversed(version.split("."))))
    cmd = [
        str(SUMO_TOOL), "build",
        "--signing-key", str(KEYS_DIR / "sign.key"),
        "--component", "ecu-a,firmware",
        "--seq", str(seq),
        "--vendor", "fa6b4a53d5ad5fdfbe9de663e4d41ffe",
        "--class",  "1492af1425695e48bf429b2d51f2ab45",
        "--uri", f"file:///fw-{version}.bin",
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
        sys.stderr.write(res.stdout); sys.stderr.write(res.stderr)
        sys.exit(f"sumo-tool failed (rc={res.returncode})")
    return envelope, payload


# ── UDS connection ──────────────────────────────────────────────────

def autopick_can_iface() -> str:
    ifaces = sorted(p.name for p in pathlib.Path("/sys/class/net").iterdir()
                    if p.name.startswith("can"))
    if not ifaces:
        sys.exit("no canX iface — run `sudo ./setup-can.sh` first")
    if len(ifaces) > 1:
        sys.exit(f"multiple canX: {ifaces} — pass one explicitly")
    return ifaces[0]


def make_connection(iface: str) -> tuple[PythonIsoTpConnection, can.Bus]:
    bus = can.Bus(interface="socketcan", channel=iface)
    addr = isotp.Address(isotp.AddressingMode.Normal_29bits,
                         txid=PHYS_TX_ID, rxid=PHYS_RX_ID)
    layer = isotp.CanStack(bus, address=addr,
                            params={"tx_data_min_length": 8,
                                    "tx_padding": 0xCC})
    return PythonIsoTpConnection(layer), bus


# ── DID codecs ──────────────────────────────────────────────────────

class _StateChar(udsoncan.DidCodec):
    def encode(self, v): return v.encode("ascii")
    def decode(self, p): return p.decode("ascii", errors="replace")
    def __len__(self): return 1


class _FixedBytes(udsoncan.DidCodec):
    def __init__(self, n): self.n = n
    def encode(self, v): return bytes(v)
    def decode(self, p): return bytes(p)
    def __len__(self): return self.n


class _LeUint32(udsoncan.DidCodec):
    def encode(self, v): return struct.pack("<I", v)
    def decode(self, p): return struct.unpack("<I", p)[0]
    def __len__(self): return 4


class _U8(udsoncan.DidCodec):
    def encode(self, v): return bytes([v & 0xFF])
    def decode(self, p): return p[0]
    def __len__(self): return 1


class _VarAscii(udsoncan.DidCodec):
    def encode(self, v): return v.encode("ascii")
    def decode(self, p): return p.decode("ascii", errors="replace")
    def __len__(self): raise udsoncan.DidCodec.ReadAllRemainingData


def make_client_config() -> dict:
    config = dict(udsoncan.configs.default_client_config)
    config["data_identifiers"] = {
        0xF187: _VarAscii(),
        0xF195: _VarAscii(),
        0xF18C: _VarAscii(),
        0xF200: _StateChar(),
        0xF201: _FixedBytes(32),
        0xF202: _LeUint32(),
        0xF203: _U8(),
        0xF204: _StateChar(),
    }
    config["use_server_timing"] = False
    config["request_timeout"] = 60.0
    config["p2_timeout"] = 30.0
    config["p2_star_timeout"] = 60.0
    return config


# ── Helpers ─────────────────────────────────────────────────────────

def poll_did(client, did: int, expected: str | None = None,
             timeout: float = 30.0, interval: float = 0.5) -> str:
    """Poll a single-byte state DID until it matches expected (or timeout)."""
    t0 = time.monotonic()
    while True:
        r = client.read_data_by_identifier(did)
        v = r.service_data.values[did]
        if expected is None or v == expected:
            return v
        if time.monotonic() - t0 > timeout:
            sys.exit(f"timeout polling DID 0x{did:04X}: last={v!r} "
                     f"expected={expected!r}")
        time.sleep(interval)


def push_envelope(client, envelope: pathlib.Path,
                  payload: pathlib.Path, max_block: int = 256) -> None:
    """Frame and push an envelope+payload through the OTA pipeline."""
    env_b = envelope.read_bytes()
    pl_b = payload.read_bytes()
    framed = struct.pack("<I", len(env_b)) + env_b + pl_b
    print(f"  framed: 4 + {len(env_b)} + {len(pl_b)} = {len(framed)} B")

    # eraseMemory routine — wait for ready
    client.start_routine(0xFF00)
    poll_did(client, 0xF200, expected="R", timeout=30)

    client.request_download(
        udsoncan.MemoryLocation(address=0, memorysize=len(framed),
                                 address_format=8, memorysize_format=32))

    seq = 1
    sent = 0
    while sent < len(framed):
        chunk = framed[sent:sent + max_block]
        client.transfer_data(seq, chunk)
        sent += len(chunk)
        # ISO 14229 §14.4.5.2: blockSequenceCounter wraps FF→00 (no skip).
        # uds-tiny on the device increments a uint8_t through zero, so we
        # must do the same.
        seq = (seq + 1) & 0xFF

    client.request_transfer_exit()
    poll_did(client, 0xF200, expected="S", timeout=30)


def activate_and_reconnect(client, conn, bus, iface: str
                           ) -> tuple[Client, PythonIsoTpConnection, object]:
    """Two-step automotive flow:
      1. RoutineControl 0xF001 (activate) — arms trial state in
         non-volatile storage. Device returns positive response, does
         NOT reboot. Tester can stage further ECUs at this point.
      2. ECUReset 0x11 (hardReset) — tester signals "now is safe to
         reboot". Device's ecu_reset_hook reads the armed trial flag
         and FUB-reboots into the staged slot.
    The ECUReset response may be cut short by the actual reboot; we
    catch the timeout and treat it as success."""
    print("  activate (RoutineControl 0xF001) …")
    client.start_routine(0xF001)              # device must respond OK
    print("    (armed; device still in old slot)")

    # Optional: confirm the OTA-state DID still says S after activate.
    st = client.read_data_by_identifier(0xF200) \
               .service_data.values[0xF200]
    if st != "S":
        print(f"    WARN: post-activate state = {st!r} (expected 'S')")

    print("  ECUReset (UDS 0x11 hardReset) …")
    try:
        client.ecu_reset(ECUReset.ResetType.hardReset)
    except Exception as e:
        # The device reboots before its 0x51 response can drain — we
        # expect this. Treat the timeout as success.
        print(f"    (ECUReset response interrupted: {e})")

    try: client.close()
    except Exception: pass
    try: bus.shutdown()
    except Exception: pass
    print("  waiting 5 s for device reboot + ISO-TP reset …")
    time.sleep(5)

    new_conn, new_bus = make_connection(iface)
    new_client = Client(new_conn, config=make_client_config())
    new_client.open()
    new_client.change_session(
        DiagnosticSessionControl.Session.programmingSession)
    return new_client, new_conn, new_bus


def commit(client) -> None:
    """Wait for the device's settle period to elapse, then commit.

    The device returns NRC 0x22 (conditionsNotCorrect) for any commit
    request received before its settle deadline (TRIAL_SETTLE_MS,
    default 10 s). We poll DID 0xF203 (1 = settle elapsed) so we
    don't burn a full 30-s timeout on the first attempt."""
    print("  commit: waiting for trial settle (DID 0xF203 → 1)…")
    deadline = time.time() + 30
    while time.time() < deadline:
        v = client.read_data_by_identifier(0xF203) \
                  .service_data.values[0xF203]
        if v == 1:
            break
        time.sleep(1.0)
    else:
        sys.exit("settle period never elapsed; device may be wedged")

    print("  commit (RoutineControl 0xF002) …")
    client.start_routine(0xF002)
    poll_did(client, 0xF200, expected="K", timeout=10)


# ── Main ────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("versions", nargs="+",
                    help="version(s) to OTA-cycle, e.g. 1.1.0 1.2.0")
    ap.add_argument("--iface", default=None)
    ap.add_argument("--no-commit", action="store_true",
                    help="skip the final commit (leaves device in T)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    iface = args.iface or autopick_can_iface()
    udsoncan.setup_logging()

    # Build all artifacts FIRST so the long cmake/sumo-tool work
    # doesn't idle the UDS session past its S3 timer (5 s default,
    # well shorter than a per-version build). With everything cached
    # we can race through the UDS sequence with no idle gaps.
    artifacts = []
    for i, version in enumerate(args.versions):
        print(f"══ pre-build {i+1}/{len(args.versions)}: v{version} ══")
        fw_bin = build_firmware(version, args.verbose)
        print(f"  fw  : {fw_bin.stat().st_size} B")
        envelope, payload = build_envelope(version, fw_bin, args.verbose)
        print(f"  env : {envelope.stat().st_size} B   "
              f"payload : {payload.stat().st_size} B")
        artifacts.append((version, envelope, payload))

    conn, bus = make_connection(iface)
    client = Client(conn, config=make_client_config())
    client.open()
    print(f"\nusing {iface}")

    print("\n=== DiagSession(programming) ===")
    r = client.change_session(
        DiagnosticSessionControl.Session.programmingSession)
    print(f"  ← session_echo={r.service_data.session_echo}")

    print("\n=== current state ===")
    cur_v = client.read_data_by_identifier(0xF195) \
                  .service_data.values[0xF195]
    cur_st = client.read_data_by_identifier(0xF200) \
                   .service_data.values[0xF200]
    cur_bt = client.read_data_by_identifier(0xF204) \
                   .service_data.values[0xF204]
    print(f"  running version={cur_v!r}  state={cur_st!r}  last_boot={cur_bt!r}")

    for i, (version, envelope, payload) in enumerate(artifacts):
        print(f"\n══ cycle {i+1}/{len(artifacts)}: → v{version} ══")

        # Re-affirm the session at the start of every cycle. Activation
        # reboots the device and resets the session to default, plus
        # belt-and-suspenders against any S3 timeout from polling
        # latency between cycles.
        client.change_session(
            DiagnosticSessionControl.Session.programmingSession)

        print(f"  pushing OTA")
        push_envelope(client, envelope, payload)
        print(f"    state=S (staged)")

        client, conn, bus = activate_and_reconnect(client, conn, bus, iface)

        new_v = client.read_data_by_identifier(0xF195) \
                      .service_data.values[0xF195]
        st = client.read_data_by_identifier(0xF200) \
                   .service_data.values[0xF200]
        bt = client.read_data_by_identifier(0xF204) \
                   .service_data.values[0xF204]
        print(f"  reconnected: version={new_v!r}  state={st!r}  "
              f"last_boot={bt!r}")
        if new_v != version:
            sys.exit(f"version mismatch: device reports {new_v!r}, "
                     f"expected {version!r}")
        if st != "T":
            print(f"  WARN: expected state=T, got {st!r}")

        if args.no_commit:
            print(f"  --no-commit: leaving device in state {st!r}")
        else:
            commit(client)
            final = client.read_data_by_identifier(0xF200) \
                          .service_data.values[0xF200]
            print(f"    state={final!r}")

    print("\nOK — all cycles completed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
