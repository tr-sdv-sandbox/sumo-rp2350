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

Usage:     python3 tester.py <can_iface>
Example:   python3 tester.py can0
"""
import argparse
import sys
import time

import can
import isotp
import udsoncan
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection
from udsoncan.services import DiagnosticSessionControl, ReadDataByIdentifier, TesterPresent


PHYS_TX_ID = 0x18DA42F1   # host (tester F1) → ECU (42) — physical req
PHYS_RX_ID = 0x18DAF142   # ECU → host — physical resp


def make_connection(iface: str) -> PythonIsoTpConnection:
    bus = can.Bus(interface="socketcan", channel=iface)
    addr = isotp.Address(
        isotp.AddressingMode.Normal_29bits,
        txid=PHYS_TX_ID, rxid=PHYS_RX_ID,
    )
    layer = isotp.NotifierBasedCanStack(
        bus, isotp.Notifier(bus), addr,
        params={
            "tx_data_min_length": 8,   # classic CAN frames
            "tx_padding": 0xCC,
        },
    )
    return PythonIsoTpConnection(layer)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("iface", help="SocketCAN interface (e.g. can0)")
    args = ap.parse_args()

    udsoncan.setup_logging()

    config = dict(udsoncan.configs.default_client_config)
    config["data_identifiers"] = {
        0xF187: udsoncan.AsciiCodec(64),   # spare-part
        0xF195: udsoncan.AsciiCodec(64),   # sw version
        0xF18C: udsoncan.AsciiCodec(64),   # vendor
    }
    config["request_timeout"] = 2.0

    conn = make_connection(args.iface)
    with Client(conn, config=config) as client:
        print("\n=== DiagSession(extended) ===")
        r = client.change_session(DiagnosticSessionControl.Session.extendedDiagnosticSession)
        print(f"  ← {r.positive!r} session_type={r.service_data.session_type}")

        for did in (0xF187, 0xF195, 0xF18C):
            print(f"\n=== ReadDID(0x{did:04X}) ===")
            r = client.read_data_by_identifier([did])
            val = r.service_data.values[did]
            print(f"  ← '{val}'")

        print("\n=== TesterPresent ×3 (suppress positive) ===")
        for _ in range(3):
            client.tester_present()
            print("  ← (suppressed)")
            time.sleep(1.0)

        print("\n=== DiagSession(default) ===")
        r = client.change_session(DiagnosticSessionControl.Session.defaultSession)
        print(f"  ← {r.positive!r} session_type={r.service_data.session_type}")

    print("\nOK — full UDS round-trip succeeded.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
