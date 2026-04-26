#!/usr/bin/env python3
"""
Tester for sumo-rp2350 examples/uds-server checkpoint 4a.

Pushes a small UDS sequence over CAN against the device:

    1. DiagSessionControl(extended)              0x10 0x03
    2. ReadDID(0xF187 spare-part)                0x22 0xF1 0x87
    3. ReadDID(0xF195 software-version)          0x22 0xF1 0x95
    4. ReadDID(0xF18C ECU vendor)                0x22 0xF1 0x8C
    5. TesterPresent (suppress positive resp)    0x3E 0x80      (×3, 1 s apart)
    6. DiagSessionControl(default)               0x10 0x01

Addressing matches the device-side defaults in
examples/uds-server/main.c:

    physical RX (host→device): 0x18DA42F1
    physical TX (device→host): 0x18DAF142
    29-bit extended IDs, 500 kbit/s

Requires:  python-can, can-isotp, udsoncan
Install:   pip install -r requirements.txt

Usage:     python3 tester.py [<can_iface>]
Example:   python3 tester.py            # auto-pick the lone canX
           python3 tester.py can0       # explicit
"""
import argparse
import pathlib
import sys
import time

import can
import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection
from udsoncan.services import DiagnosticSessionControl, ReadDataByIdentifier, TesterPresent


def autopick_can_iface() -> str:
    """Find the lone canX in /sys/class/net. Bail if 0 or >1."""
    ifaces = sorted(p.name for p in pathlib.Path("/sys/class/net").iterdir()
                    if p.name.startswith("can"))
    if not ifaces:
        sys.exit("no canX interface present — run `sudo ./setup-can.sh` first")
    if len(ifaces) > 1:
        sys.exit(f"multiple CAN interfaces: {ifaces} — pass one explicitly")
    return ifaces[0]


PHYS_TX_ID = 0x18DA42F1   # host (tester F1) → ECU (42) — physical req
PHYS_RX_ID = 0x18DAF142   # ECU → host — physical resp


def make_connection(iface: str) -> PythonIsoTpConnection:
    bus = can.Bus(interface="socketcan", channel=iface)
    addr = isotp.Address(
        isotp.AddressingMode.Normal_29bits,
        txid=PHYS_TX_ID, rxid=PHYS_RX_ID,
    )
    # CanStack manages its own RX thread; `udsoncan` calls
    # layer.start()/stop() via PythonIsoTpConnection.open()/close().
    layer = isotp.CanStack(
        bus, address=addr,
        params={
            "tx_data_min_length": 8,   # classic CAN frames, padded
            "tx_padding": 0xCC,
        },
    )
    return PythonIsoTpConnection(layer)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("iface", nargs="?", default=None,
                    help="SocketCAN interface (e.g. can0). "
                         "Defaults to the lone canX present.")
    args = ap.parse_args()
    iface = args.iface or autopick_can_iface()
    print(f"using {iface}")

    udsoncan.setup_logging()

    # Variable-length ASCII codec — udsoncan's AsciiCodec is fixed-
    # length, but our DID values vary. Returning ReadAllRemainingData
    # from __len__ tells udsoncan "this DID consumes all remaining
    # response bytes".
    class VarAscii(udsoncan.DidCodec):
        def encode(self, val):
            return val.encode("ascii")

        def decode(self, payload):
            return payload.decode("ascii", errors="replace")

        def __len__(self):
            raise udsoncan.DidCodec.ReadAllRemainingData

    config = dict(udsoncan.configs.default_client_config)
    config["data_identifiers"] = {
        0xF187: VarAscii(),   # spare-part
        0xF195: VarAscii(),   # sw version
        0xF18C: VarAscii(),   # vendor
    }
    # Device advertises P2=50 ms in DiagSession but slcan adds ~10 ms
    # of per-frame USB-CDC latency, so a 5-frame multi-frame ReadDID
    # response easily exceeds 50 ms. Ignore the device's advertised
    # P2 and rely on request_timeout for the wall-clock budget.
    config["use_server_timing"] = False
    config["request_timeout"] = 2.0
    config["p2_timeout"] = 1.0
    config["p2_star_timeout"] = 5.0

    conn = make_connection(iface)
    with Client(conn, config=config) as client:
        print("\n=== DiagSession(extended) ===")
        r = client.change_session(DiagnosticSessionControl.Session.extendedDiagnosticSession)
        print(f"  ← session_echo={r.service_data.session_echo}")

        for did in (0xF187, 0xF195, 0xF18C):
            print(f"\n=== ReadDID(0x{did:04X}) ===")
            r = client.read_data_by_identifier(did)
            val = r.service_data.values[did]
            print(f"  ← '{val}'")

        print("\n=== TesterPresent ×3 (suppress positive) ===")
        for _ in range(3):
            client.tester_present()
            print("  ← (suppressed)")
            time.sleep(1.0)

        print("\n=== DiagSession(default) ===")
        r = client.change_session(DiagnosticSessionControl.Session.defaultSession)
        print(f"  ← session_echo={r.service_data.session_echo}")

    print("\nOK — full UDS round-trip succeeded.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
