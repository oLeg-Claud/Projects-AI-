#!/usr/bin/env python3
"""USO-32 USB CDC updater, wire protocol UBL1/UBR1. No hardware I/O on import."""
import argparse
import secrets
import struct
import sys
import time
import zlib
from pathlib import Path

APP_BASE = 0x08008000
APP_MAX = 94 * 1024
RAM_TOP = 0x20007FF8
CHUNK = 256
ENTER = bytes.fromhex("08 12 34 56 78")
ERRORS = {1:"invalid request",2:"invalid state / image not committed",3:"address/length/order",
          4:"flash read/write",5:"image CRC",6:"image vectors"}

class BootError(Exception):
    pass

def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF

def request_frame(cmd, seq, payload=b""):
    if len(payload) > 260: raise BootError("Payload too large")
    header = b"UBL1" + struct.pack("<BHH", cmd, seq, len(payload)) + payload
    return header + struct.pack("<I", crc32(header))

def validate_image(data, base=APP_BASE, maximum=APP_MAX):
    if base != APP_BASE or maximum != APP_MAX:
        raise BootError("Device memory layout differs from this updater")
    if not 8 <= len(data) <= maximum:
        raise BootError(f"Image must contain 8..{maximum} bytes")
    sp, pc = struct.unpack_from("<II", data)
    if not (0x20000000 < sp <= RAM_TOP and sp % 8 == 0):
        raise BootError("Invalid initial stack pointer (wrong image or old linker script)")
    if not (pc & 1 and base <= (pc & ~1) < base + len(data)):
        raise BootError(f"Image must be linked at 0x{base:08X}; use uso32_g431.bin, not factory.bin")

def read_exact(port, n, timeout=3):
    end = time.monotonic() + timeout
    data = bytearray()
    while len(data) < n:
        data.extend(port.read(n-len(data)))
        if len(data) < n and time.monotonic() >= end:
            raise BootError(f"Response timeout ({len(data)}/{n} bytes)")
    return bytes(data)

def query_info(port, timeout=3):
    port.write(b"\xA1")
    if read_exact(port,1,timeout) != b"\xA1":
        raise BootError("INFO unavailable: stop streaming before this command")
    data=bytearray(); end=time.monotonic()+timeout
    while time.monotonic()<end and len(data)<127:
        b=port.read(1)
        if b==b"\0":
            try: return data.decode("ascii")
            except UnicodeDecodeError as exc: raise BootError("Invalid INFO string") from exc
        data.extend(b)
    raise BootError("INFO string missing NUL terminator")

class Client:
    def __init__(self, port, timeout=10):
        self.port=port; self.timeout=timeout; self.seq=secrets.randbelow(65536)
        self.buffer=bytearray()

    def _response(self, cmd, seq):
        end=time.monotonic()+self.timeout
        while time.monotonic()<end:
            # Read only the missing frame bytes. Waiting for 64 bytes after a
            # short RUN ACK races the device's USB disconnect on Windows.
            needed = 1
            if self.buffer[:4] == b"UBR1":
                if len(self.buffer) < 9:
                    needed = 9-len(self.buffer)
                else:
                    size = struct.unpack_from("<H", self.buffer, 7)[0]
                    if size <= 260:
                        needed = max(1, 13+size-len(self.buffer))
            self.buffer.extend(self.port.read(needed))
            while True:
                pos=self.buffer.find(b"UBR1")
                if pos<0:
                    self.buffer[:]=self.buffer[-3:]
                    break
                if pos: del self.buffer[:pos]
                if len(self.buffer)<9: break
                got_cmd,got_seq,n=struct.unpack_from("<BHH",self.buffer,4)
                if n>260:
                    del self.buffer[0]; continue
                total=13+n
                if len(self.buffer)<total: break
                frame=bytes(self.buffer[:total]); del self.buffer[:total]
                if crc32(frame[:-4])!=struct.unpack_from("<I",frame,total-4)[0]: continue
                if got_cmd!=(cmd|0x80) or got_seq!=seq: continue
                payload=frame[9:-4]
                if not payload: raise BootError("Response has no status")
                if payload[0]: raise BootError(f"Loader error {payload[0]}: {ERRORS.get(payload[0],'unknown')}")
                return payload[1:]
        raise TimeoutError("Bootloader response timed out")

    def transact(self, cmd, payload=b""):
        seq=self.seq; self.seq=(self.seq+1)&65535
        packet=request_frame(cmd,seq,payload)
        for attempt in range(3):
            self.port.write(packet)
            try: return self._response(cmd,seq)
            except TimeoutError:
                if attempt==2: raise BootError("No valid reply after 3 attempts; reconnect and restart update")
        raise AssertionError("unreachable")

    def hello(self):
        data=self.transact(1)
        if len(data)!=18: raise BootError("Unexpected HELLO length")
        version,base,maximum,page,chunk,valid=struct.unpack("<BIIIIB",data)
        if (version,base,maximum,page,chunk)!=(1,APP_BASE,APP_MAX,2048,CHUNK):
            raise BootError("Unsupported bootloader protocol or memory layout")
        return bool(valid)

    def flash(self, data, progress=None):
        validate_image(data)
        self.hello()  # No erase before both file and device parameters are checked.
        self.transact(2,struct.pack("<II",len(data),crc32(data)))
        for offset in range(0,len(data),CHUNK):
            block=data[offset:offset+CHUNK]
            reply=self.transact(3,struct.pack("<I",offset)+block)
            if reply!=struct.pack("<I",offset+len(block)): raise BootError("Unexpected write offset")
            if progress: progress(offset+len(block),len(data))
        self.transact(4)
        if not self.hello(): raise BootError("Image not committed after verification")

def open_port(name):
    import serial
    return serial.Serial(name,57600,timeout=0.05,write_timeout=3)

def ports():
    from serial.tools.list_ports import comports
    return list(comports())

def reconnect_boot(name, serial_number, explicit=None, timeout=15):
    end=time.monotonic()+timeout
    time.sleep(0.3)
    while time.monotonic()<end:
        available=ports()
        candidates=[p for p in available if
                    (p.device==explicit if explicit else
                     (p.serial_number==serial_number if serial_number else p.device==name))]
        if len(candidates)>1: raise BootError("Multiple matching devices; specify --boot-port")
        for entry in candidates:
            if serial_number and entry.serial_number and entry.serial_number!=serial_number:
                raise BootError("--boot-port belongs to another device")
            p=None
            try:
                p=open_port(entry.device)
                info=query_info(p,0.5)
                if "USO32_Boot_" in info:
                    Client(p).hello()
                    return p, info
            except (OSError,BootError):
                pass
            if p: p.close()
        time.sleep(0.2)
    raise BootError("Bootloader did not reappear. Check cable, COM port, and initial SWD installation")

def ensure_boot(port, name, explicit=None):
    info=query_info(port)
    if "USO32_Boot_" in info:
        Client(port).hello(); return port,info
    if "USO32_App_" not in info: raise BootError("Selected port is not a supported USO-32 application")
    identity=next((p.serial_number for p in ports() if p.device==name),None)
    port.write(ENTER)
    if read_exact(port,2)!=b"\x08\xAA": raise BootError("Boot entry ACK must be 08 AA")
    port.close()
    return reconnect_boot(name,identity,explicit)

def main(argv=None):
    parser=argparse.ArgumentParser(description="USO-32 USB bootloader console (idle device only)")
    parser.add_argument("--port",help="COM5 or /dev/ttyACM0")
    parser.add_argument("--boot-port",help="Explicit port after reboot if automatic detection is unavailable")
    sub=parser.add_subparsers(dest="command",required=True)
    sub.add_parser("list",help="List serial ports")
    sub.add_parser("info",help="A1: build date, time, firmware name")
    sub.add_parser("enter",help="08 12 34 56 78: enter bootloader")
    sub.add_parser("run",help="Run a verified application from bootloader")
    flashing=sub.add_parser("flash",help="Write an application .bin, verify and run")
    flashing.add_argument("image",type=Path)
    flashing.add_argument("--no-run",action="store_true")
    args=parser.parse_args(argv)
    port=None
    try:
        if args.command=="list":
            for p in ports(): print(f"{p.device}: {p.description}; serial={p.serial_number}")
            return 0
        if not args.port: parser.error("--port is required")
        image=None
        if args.command=="flash":
            image=args.image.read_bytes(); validate_image(image)
        port=open_port(args.port)
        if args.command=="info": print(query_info(port)); return 0
        port,info=ensure_boot(port,args.port,args.boot_port)
        print(info)
        if args.command=="enter": return 0
        client=Client(port); client.hello()
        if image is not None:
            client.flash(image,lambda done,total: print(f"\rWritten {done}/{total} ({100*done/total:.1f}%)",end="",flush=True))
            print(f"\nVerified and committed. CRC32={crc32(image):08X}")
        if args.command=="run" or (args.command=="flash" and not args.no_run):
            client.transact(5)
            print("RUN acknowledged; device is restarting into the application.")
        return 0
    except (BootError,OSError,ImportError) as exc:
        print(f"ERROR: {exc}",file=sys.stderr); return 1
    except KeyboardInterrupt:
        print("\nInterrupted. If erase already started, repeat the update in bootloader mode.",file=sys.stderr); return 130
    finally:
        if port is not None: port.close()

if __name__=="__main__": sys.exit(main())
