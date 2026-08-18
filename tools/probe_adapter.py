#!/usr/bin/env python3
"""
Prove the adapter's USB CDC stack works, without involving the freeze dryer.

Plug the adapter's *native USB* socket (the one marked USB, not UART) into this
PC. The PC then plays the role the dryer normally plays: it becomes the USB
host, opens the CDC port, and speaks the dryer's own protocol at it.

We send REQINFO, which the adapter answers with a GOTIT frame. That exercises
the entire path end to end - USB enumeration, the CDC pipe, frame reassembly,
parsing, and the transmit path - so the result is unambiguous:

  GOTIT received      -> the adapter firmware is fine. The fault is on the
                         dryer side or in the cable between them.
  port opens, silence -> the adapter enumerates but its CDC pipe is not
                         carrying data. That is a firmware bug we can chase
                         here, with no dryer needed.
  no port at all      -> it is not enumerating as CDC on this PC either.

Usage:  python tools/probe_adapter.py [COMx]
"""
import sys, time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing:  python -m pip install pyserial")

# Espressif's default TinyUSB identity, which this firmware uses.
VID, PID = 0x303A, 0x4001


def find_port():
    cands = []
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            cands.append((p.device, "VID:PID matches the adapter"))
        elif p.vid == VID:
            cands.append((p.device, f"Espressif VID, PID {p.pid:#06x}"))
    return cands


def main():
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        found = find_port()
        print("Ports matching an Espressif USB device:")
        for dev, why in found:
            print(f"  {dev}  ({why})")
        if not found:
            print("  none.")
            print("\nAll serial ports currently present:")
            for p in list_ports.comports():
                print(f"  {p.device}  {p.description}  "
                      f"VID={p.vid and hex(p.vid)} PID={p.pid and hex(p.pid)}")
            print("\nThe adapter is not enumerating as a CDC device on this PC.")
            print("Check the cable is in the socket marked USB, not UART.")
            return 2
        port = found[0][0]

    print(f"\nOpening {port} ...")
    # Opening the port asserts DTR and sends SET_LINE_CODING - both should show
    # up in the adapter's device log, which is itself a useful signal.
    with serial.Serial(port, 115200, timeout=1) as s:
        time.sleep(0.3)
        s.reset_input_buffer()
        print("Sending: REQINFO")
        s.write(b"REQINFO\r")
        s.flush()

        deadline = time.time() + 8
        buf = b""
        while time.time() < deadline:
            chunk = s.read(256)
            if chunk:
                buf += chunk
                if b"\r" in buf:
                    break

    if not buf:
        print("\nNo reply in 8s.")
        print("The port opened, so the adapter enumerated - but nothing came")
        print("back over the CDC pipe. Check 'bytes received' in the web UI:")
        print("  rose  -> our RX works; the transmit path is the problem.")
        print("  still 0 -> the pipe is not delivering data in either direction.")
        return 1

    print(f"\nReceived {len(buf)} bytes:")
    print("  " + buf.decode("ascii", "replace").strip())
    if b"GOTIT" in buf:
        print("\nGOTIT received - the adapter's USB CDC and protocol stack are")
        print("fully working. The fault is on the dryer side or in the cable.")
        return 0
    print("\nReplied, but not with GOTIT. USB works; check the protocol layer.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
