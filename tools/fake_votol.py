#!/usr/bin/env python3
"""Fake VOTOL EM100s controller for bench-testing the web dashboard.

Listens on TCP 127.0.0.1:6638 (same place the real app expects the ESP32
bridge) and mimics the controller: LDGET -> 7 parameter packets (from
votol_paramemu.txt, vendored next to this script), anything else -> one
telemetry frame, RESET -> reboot broadcast. Ported from VotolAIO's
"Votol-Em100s-Emulator.py" (GPL-3.0, Ming2k8-Coder) with pyserial replaced
by a plain socket. Tracked here so the bench test is self-contained.
"""
import socket
import sys
import time
from pathlib import Path

PARAM_FILE = Path(__file__).parent / "votol_paramemu.txt"
LISTEN = ("127.0.0.1", 6638)

sendheader = b"\xC0\x14"


def crc16(buffer):
    c = 0
    for i in buffer:
        c ^= i
    return c.to_bytes(1)


def map_range(x, in_min, in_max, out_min, out_max):
    return (x - in_min) * (out_max - out_min) // (in_max - in_min) + out_min


def strflt2bytes(inp, multiply=1, bytesnum=2):
    return int(float(inp[:-1]) * multiply).to_bytes(bytesnum)


class Conn:
    def __init__(self, sock):
        self.s = sock
        self.buf = b""

    def read(self):
        if not self.buf:
            self.buf = self.s.recv(4096)
        if not self.buf:
            raise EOFError
        c, self.buf = self.buf[0:1], self.buf[1:]
        return c[0]

    def write(self, data):
        self.s.sendall(data)


def packet1send(ser, p):
    ctltype = {"EM-30s\n": 5, "EM-50s\n": 0x0A, "EM-100s\n": 0x14,
               "EM-150s\n": 0x1E, "EM-200s\n": 0x28}[p[0]]
    batv = ["48V\n", "60V\n", "72V\n", "84V\n", "96V\n"].index(p[1])
    b = sendheader + b"\x05R\x01" + bytes([ctltype, batv]) \
        + strflt2bytes(p[2], 10) + strflt2bytes(p[3], 10) + strflt2bytes(p[4], 10, 1) \
        + strflt2bytes(p[5]) + strflt2bytes(p[6]) + strflt2bytes(p[7], 10) \
        + strflt2bytes(p[8]) + strflt2bytes(p[9])
    ser.write(b + crc16(b) + b"\x0d")


def packet2send(ser, p):
    b = sendheader + b"\x05R\x02" + strflt2bytes(p[10]) + strflt2bytes(p[11], 1, 1) \
        + strflt2bytes(p[12], 1, 1) + strflt2bytes(p[13], 1, 1) + strflt2bytes(p[14]) \
        + strflt2bytes(p[15]) + strflt2bytes(p[16])
    select = 0
    if p[17] == "ON\n":
        select += 1
    select += {"HIGH\n": 2, "MEDI\n": 4, "LOW\n": 8}.get(p[18], 0)
    if p[19] == "VTYPE\n":
        select += 16
    if p[20] == "SWITCH\n":
        select += 32
    if p[21] == "ON\n":
        select += 64
    if p[22] == "ON\n":
        select += 128
    b += select.to_bytes(1) + strflt2bytes(p[23], 1, 1) + strflt2bytes(p[24], 1, 1) \
        + strflt2bytes(p[25], 1, 1) + strflt2bytes(p[26], 1, 1) + strflt2bytes(p[27], 1, 1)
    ser.write(b + crc16(b) + b"\x0d")


def packet3send(ser, p):
    # per protocol doc: hall(2B) IcHT IcOT IcTL TC1(2B) TC2(2B) TC3(2B) RevRPM
    # opts poles EABS SW HW  — note: the original emulator wrote TC3 as 1 byte
    # and dropped RevRPM; this port follows the doc instead.
    hv = int(float(p[28]))
    if hv < 0:
        hall = b"\xFF" + (256 + hv).to_bytes(1)
    else:
        hall = b"\x00" + hv.to_bytes(1)
    select = 0
    if p[36] == "ON\n":
        select += 8
    if p[37] == "YES\n":
        select += 16
    if p[38] == "YES\n":
        select += 32
    if p[39] == "ON\n":
        select += 64
    if p[40] == "REV\n":
        select += 128
    b = sendheader + b"\x05R\x03" + hall + strflt2bytes(p[29], 1, 1) \
        + strflt2bytes(p[30], 1, 1) + strflt2bytes(p[31], 1, 1) + strflt2bytes(p[32]) \
        + strflt2bytes(p[33]) + strflt2bytes(p[34]) + strflt2bytes(p[35], 1, 1) \
        + select.to_bytes(1) \
        + strflt2bytes(p[41], 1, 1) + strflt2bytes(p[42], 1, 1) + strflt2bytes(p[43], 1, 1) \
        + strflt2bytes(p[44], 1, 1)
    ser.write(b + crc16(b) + b"\x0d")


def packet4send(ser, p):
    select = 0
    for i, on in [(45, 1), (46, 2), (48, 8), (49, 16), (51, 64)]:
        if p[i] == "ON\n":
            select += on
    if p[47] == "LIN\n":
        select += 4
    if p[50] == "HIGH\n":
        select += 32
    b = sendheader + b"\x05R\x04" + select.to_bytes(1) + strflt2bytes(p[52], 327.67) \
        + strflt2bytes(p[53], 1, 2) + strflt2bytes(p[54], 1, 2) + strflt2bytes(p[55], 1, 1) \
        + strflt2bytes(p[56], 1, 1) + strflt2bytes(p[57], 1, 1) + strflt2bytes(p[58]) \
        + strflt2bytes(p[59]) + strflt2bytes(p[60], 1, 2) + strflt2bytes(p[61], 1, 1)
    ser.write(b + crc16(b) + b"\x0d")


def connect_params(ser):
    try:
        f = open(PARAM_FILE)
    except OSError:
        print("param file missing:", PARAM_FILE)
        return
    p = f.readlines()
    f.close()
    packet1send(ser, p)
    packet2send(ser, p)
    packet3send(ser, p)
    packet4send(ser, p)
    ser.write(b"\xC0\x14\x05\x52\x05\xC0\x01\xC0\x00\xC0\x00\xC0\x00\xC0\x11\xC0\x05\xC0\x07\xC0\x08\x03\x9F\x0D")
    ser.write(b"\xC0\x14\x05\x52\x06\xC0\x09\xC0\x8A\xC0\x0B\xC0\x00\xC0\x00\xC0\x15\xC0\x0F\xC8\x92\x71\xFC\x0D")
    ser.write(b"\xC0\x14\x05\x52\x07\x39\xC7\x0D\xE0\x50\xFA\x00\x00\x00\x00\x27\x02\x76\x04\x5D\x00\x71\x46\x0D")
    print("sent 7 parameter packets")


def transmit_disp(ser, throttle=0.7):
    header = sendheader + b"\x0D\x59\x42"
    voltq = int(map_range(float(throttle), 0.77, 4.60, 1200, 1123))
    amped = max(0, int(map_range(float(throttle), 0.77, 4.60, 5, 5400)))
    rpm = int(map_range(float(throttle), 0.77, 4.60, 0, 1200))
    icstatus = 3 if rpm > 0 else 0
    if rpm <= 0:
        rpm = 0
    ictemp, motemp, tempcof = 40 + 50, 67 + 50, 8000
    buffer = header + voltq.to_bytes(2, signed=True) + amped.to_bytes(2, signed=True) \
        + b"\x01" + b"\x00\x00\x00\x00" + rpm.to_bytes(2, signed=True) \
        + ictemp.to_bytes(1, signed=True) + motemp.to_bytes(1, signed=True) \
        + tempcof.to_bytes(2) + b"\x88" + icstatus.to_bytes(1, signed=True)
    ser.write(buffer + crc16(buffer) + b"\x0d")


def mainloop(ser):
    count, msg = 0, ""
    while True:
        c = ser.read()
        count += 1
        if 4 <= count <= 10:
            msg += chr(c)
        if c == 0x0D:
            print("RX frame, cmd field:", msg[:5])
            if msg[:5] == "LDGET":
                time.sleep(0.3)
                connect_params(ser)
            elif msg[:5] == "RESET":
                ser.write(b"\xC0\x14\x0D\xFE\xEF\x55\xAA\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x37\x0D")
                print("RESET -> bye")
                return
            else:
                transmit_disp(ser)
            count, msg = 0, ""


if __name__ == "__main__":
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(LISTEN)
    srv.listen(1)
    print("fake VOTOL EM100s listening on %s:%d" % LISTEN)
    while True:
        sock, addr = srv.accept()
        print("client:", addr)
        try:
            mainloop(Conn(sock))
        except Exception as e:
            print("client session ended:", repr(e))
