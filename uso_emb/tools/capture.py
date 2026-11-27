"""USO-32 serial capture (Legacy uco24/uco32 & Protocol V2 CRC-16): pip install pyserial."""
import argparse
import csv
import struct
import time

try:
    import serial
except ImportError:
    serial = None

V2_MAGIC_H2D = b"\xA5\x5A"
V2_MAGIC_D2H = b"\x5A\xA5"
V2_PROTO_VERSION = 2
V2_HDR_SIZE = 7
V2_MAX_PAYLOAD = 54

V2_CMD_HELLO = 0x01
V2_CMD_START = 0x03
V2_CMD_STOP = 0x04
V2_RSP_HELLO = 0x81
V2_RSP_START = 0x83
V2_MSG_DATA_BATCH = 0x90


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def v2_encode(msg_type: int, seq: int, payload: bytes = b"", flags: int = 0) -> bytes:
    if len(payload) > V2_MAX_PAYLOAD:
        raise ValueError("V2 payload exceeds 54 bytes")
    ver_flags = ((V2_PROTO_VERSION & 0x0F) << 4) | (flags & 0x0F)
    hdr = struct.pack("<2sBBHB", V2_MAGIC_H2D, ver_flags, msg_type, seq & 0xFFFF, len(payload))
    body = hdr + payload
    crc = crc16_ccitt(body)
    return body + struct.pack("<H", crc)


def v2_read_frame(port, buf: bytearray, timeout_s: float = 2.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        chunk = port.read(64)
        if chunk:
            buf.extend(chunk)
        while len(buf) >= 2:
            idx = buf.find(V2_MAGIC_D2H)
            if idx < 0:
                del buf[:-1]
                break
            if idx > 0:
                del buf[:idx]
            if len(buf) < V2_HDR_SIZE:
                break
            plen = buf[6]
            if plen > V2_MAX_PAYLOAD or ((buf[2] >> 4) & 0x0F) != V2_PROTO_VERSION:
                del buf[:2]
                continue
            total = V2_HDR_SIZE + plen + 2
            if len(buf) < total:
                break
            frame = bytes(buf[:total])
            del buf[:total]
            expected = struct.unpack_from("<H", frame, V2_HDR_SIZE + plen)[0]
            if crc16_ccitt(frame[:V2_HDR_SIZE + plen]) == expected:
                msg_type = frame[3]
                seq = struct.unpack_from("<H", frame, 4)[0]
                return msg_type, seq, frame[V2_HDR_SIZE:V2_HDR_SIZE + plen]
    raise TimeoutError("Нет ответа УСО32 V2; проверьте подключение и прошивку v3.0+")


def read_exact(port, count):
    data = bytearray()
    deadline = time.monotonic() + 2
    while len(data) < count:
        data.extend(port.read(count - len(data)))
        if time.monotonic() > deadline:
            raise TimeoutError("Нет данных УСО; проверьте LED/uso_stats")
    return bytes(data)


def run_v2_capture(args):
    if serial is None:
        raise RuntimeError("Установите pyserial: pip install pyserial")
    gain_idx = [1, 2, 4, 8, 16, 32].index(args.gain)
    seq = 1
    rx_buf = bytearray()
    with serial.Serial(args.port, 57600, timeout=0.05, write_timeout=1) as port:
        time.sleep(0.1)
        port.write(v2_encode(V2_CMD_STOP, seq)); seq += 1
        port.flush(); time.sleep(0.08)
        port.reset_input_buffer()

        port.write(v2_encode(V2_CMD_HELLO, seq)); seq += 1
        port.flush()
        msg_type, _, payload = v2_read_frame(port, rx_buf)
        if msg_type != V2_RSP_HELLO or len(payload) != 28:
            raise RuntimeError(f"Некорректный ответ CMD_HELLO: type=0x{msg_type:02X}, len={len(payload)}")
        ver, uso_id, maj, minor, patch, adc_id, ready, fault, caps, clk_hz, uid = struct.unpack("<BBBBBBBBII12s", payload)
        print(
            f"УСО32 #{uso_id} V{ver} FW {maj}.{minor}.{patch} | ADS1262=0x{adc_id:02X} | "
            f"fCLK={clk_hz} Hz | ready={ready} fault={fault} | caps=0x{caps:08X} | UID={uid.hex()}"
        )

        adc_flags = 0x02  # VBIAS
        if args.chop:
            adc_flags |= 0x01
        stream_flags = 0x01  # AUTO_RECOVER
        carrier_mhz = int(round(args.carrier * 1000.0))
        hpf_mhz = 100 if args.hp else 0

        # uso_v2_config_t wire format (16 bytes)
        cfg_payload = struct.pack(
            "<HBBBBBBBBHHBB",
            carrier_mhz,
            0xFF if args.v2_adaptive else args.dr,
            args.decim,
            gain_idx,
            args.adc_filter,
            adc_flags,
            args.inpmux,
            args.lpf_mode,
            args.batch,
            0,        # dsp_lpf_cutoff_dhz = auto
            hpf_mhz,
            stream_flags,
            0,        # reserved
        )

        port.write(v2_encode(V2_CMD_START, seq, cfg_payload)); seq += 1
        port.flush()
        msg_type, _, sp = v2_read_frame(port, rx_buf, timeout_s=2.5)
        if msg_type != V2_RSP_START or len(sp) != 12 or sp[0] != 0:
            raise RuntimeError(f"CMD_START отклонён: type=0x{msg_type:02X}, payload={sp.hex()}")
        status, dr_code, decim, batch_sz, active_gain, active_flt, _, _, eff_fs_dhz, hw_fs_dhz = struct.unpack("<BBBBBBBBHH", sp)
        eff_fs = eff_fs_dhz / 10.0
        print(
            f"Старт потока V2: Fs={eff_fs:.1f} Гц (АЦП {hw_fs_dhz / 10.0:.1f} Гц, DR=0x{dr_code:02X}, ÷{decim}), "
            f"batch={batch_sz}, flt={active_flt}, gain={1 << active_gain}x"
        )

        target_samples = max(1, round(args.seconds * eff_fs))
        received = 0
        overranges = 0
        try:
            with open(args.output, "w", newline="", encoding="utf-8") as out:
                writer = csv.writer(out)
                writer.writerow(["sample_idx", "mcu_ms", "seconds", "code", "input_volts", "overrange"])
                while received < target_samples:
                    msg_type, _, bp = v2_read_frame(port, rx_buf, timeout_s=2.0)
                    if msg_type != V2_MSG_DATA_BATCH or len(bp) < 16:
                        continue
                    ts_ms, first_idx, fs_dhz, bflags, g_cnt = struct.unpack_from("<IIHBB", bp, 0)
                    g_idx = (g_cnt >> 4) & 0x0F
                    count = g_cnt & 0x0F
                    gain_val = 1 << min(g_idx, 5)
                    ovf = 1 if (bflags & 0x01) else 0
                    if ovf:
                        overranges += 1
                    fs_now = max(1.0, fs_dhz / 10.0)
                    for i in range(count):
                        if 12 + (i + 1) * 4 > len(bp):
                            break
                        code = struct.unpack_from("<i", bp, 12 + i * 4)[0]
                        volts = code * 2.5 / ((1 << 31) * gain_val)
                        idx = first_idx + i
                        writer.writerow([idx, ts_ms, f"{idx / fs_now:.6f}", code, f"{volts:.9f}", ovf])
                        received += 1
                        if received >= target_samples:
                            break
        finally:
            port.write(v2_encode(V2_CMD_STOP, seq)); seq += 1
            port.flush()
    print(f"Готово: {args.output} ({received} отсчётов, перегрузок PGA: {overranges})")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("port", help="Например COM5")
    p.add_argument("--gain", type=int, choices=[1, 2, 4, 8, 16, 32], default=1)
    p.add_argument("--freq", type=lambda s: int(s, 0), choices=list(range(7)) + list(range(16, 23)), default=0)
    p.add_argument("--bits", type=int, choices=[24, 32], default=32)
    p.add_argument("--hp", action="store_true")
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--output", default="capture.csv")
    # Protocol V2 options
    p.add_argument("--v2", action="store_true", help="Использовать новый протокол УСО32 V2 (CRC-16, пакеты)")
    p.add_argument("--v2-adaptive", action="store_true", default=True, help="Адаптивный выбор Fs под --carrier")
    p.add_argument("--no-v2-adaptive", dest="v2_adaptive", action="store_false")
    p.add_argument("--carrier", type=float, default=5.0, help="Несущая прибора, Гц (для адаптивного режима V2)")
    p.add_argument("--dr", type=lambda s: int(s, 0), default=0x08, help="Код ADS1262 DR (0x04..0x0A)")
    p.add_argument("--decim", type=int, default=0, help="Децимация на МК (0=авто, 1..16)")
    p.add_argument("--adc-filter", type=int, choices=[0, 1, 2, 3, 4], default=3, help="Фильтр ADS1262 (0=Sinc1..3=Sinc4,4=FIR)")
    p.add_argument("--lpf-mode", type=int, choices=[0, 1, 2], default=0, help="ФНЧ МК (0=Bypass,1=Butter4,2=Legacy10)")
    p.add_argument("--inpmux", type=lambda s: int(s, 0), default=0x89, help="INPMUX (0x89=AIN8/9, 0xFF=short, 0xBB=temp)")
    p.add_argument("--chop", action="store_true", help="Включить чоппер ADS1262 (CHOP)")
    p.add_argument("--batch", type=int, default=0, help="Отсчётов в USB-пакете V2 (0=авто, 1..10)")
    args = p.parse_args()
    if args.seconds <= 0:
        p.error("--seconds должен быть положительным")
    if args.v2:
        run_v2_capture(args)
        return
    if serial is None:
        raise RuntimeError("Установите pyserial: pip install pyserial")

    with serial.Serial(args.port, 57600, timeout=0.1, write_timeout=1) as port:
        # Abort a potentially incomplete START pair before sending STOP.
        time.sleep(0.15)
        port.write(b"\x10"); port.flush(); time.sleep(0.15)
        port.reset_input_buffer()
        port.write(b"\xaa")
        if read_exact(port, 1) != b"\x55":
            raise RuntimeError("УСО не готово")
        port.write(b"\x71" if args.hp else b"\x72")
        command = (0x90 if args.bits == 32 else 0x20) + [1, 2, 4, 8, 16, 32].index(args.gain)
        try:
            port.write(bytes([command, args.freq]))
            n = 6 if args.bits == 32 else 5
            with open(args.output, "w", newline="", encoding="utf-8") as out:
                writer = csv.writer(out)
                writer.writerow(["sample", "nominal_seconds", "code", "input_volts"])
                for i in range(round(args.seconds * 400)):
                    frame = read_exact(port, n)
                    if frame[0] != 0 or frame[-1] not in (0xFF, 0xFE):
                        raise RuntimeError("Нарушена граница кадра; захват остановлен")
                    code = int.from_bytes(frame[1:-1], "big", signed=True)
                    volts = code * 2.5 / ((1 << (args.bits - 1)) * args.gain)
                    writer.writerow([i, i / 400, code, volts])
        finally:
            port.write(b"\x10"); port.flush()
    print(args.output)


if __name__ == "__main__":
    main()
