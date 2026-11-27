"""USO-32 serial capture: pip install pyserial. Output time is nominal, not timestamped."""
import argparse
import csv
import time
import serial

def read_exact(port, count):
    data=bytearray()
    deadline=time.monotonic()+2
    while len(data)<count:
        data.extend(port.read(count-len(data)))
        if time.monotonic()>deadline: raise TimeoutError("Нет данных УСО; проверьте LED/uso_stats")
    return bytes(data)

def main():
    p=argparse.ArgumentParser()
    p.add_argument("port",help="Например COM5")
    p.add_argument("--gain",type=int,choices=[1,2,4,8,16,32],default=1)
    p.add_argument("--freq",type=lambda s:int(s,0),choices=list(range(7))+list(range(16,23)),default=0)
    p.add_argument("--bits",type=int,choices=[24,32],default=32)
    p.add_argument("--hp",action="store_true")
    p.add_argument("--seconds",type=float,default=10)
    p.add_argument("--output",default="capture.csv")
    args=p.parse_args()
    if args.seconds<=0: p.error("--seconds должен быть положительным")
    with serial.Serial(args.port,57600,timeout=0.1,write_timeout=1) as port:
        # Abort a potentially incomplete START pair before sending STOP.
        time.sleep(0.15)
        port.write(b"\x10"); port.flush(); time.sleep(0.15)
        port.reset_input_buffer()
        port.write(b"\xaa")
        if read_exact(port,1)!=b"\x55": raise RuntimeError("УСО не готово")
        port.write(b"\x71" if args.hp else b"\x72")
        command=(0x90 if args.bits==32 else 0x20)+[1,2,4,8,16,32].index(args.gain)
        try:
            port.write(bytes([command,args.freq]))
            n=6 if args.bits==32 else 5
            with open(args.output,"w",newline="",encoding="utf-8") as out:
                writer=csv.writer(out); writer.writerow(["sample","nominal_seconds","code","input_volts"])
                for i in range(round(args.seconds*400)):
                    frame=read_exact(port,n)
                    if frame[0]!=0 or frame[-1]!=255:
                        raise RuntimeError("Нарушена граница кадра; захват остановлен")
                    code=int.from_bytes(frame[1:-1],"big",signed=True)
                    volts=code*2.5/((1<<(args.bits-1))*args.gain)
                    writer.writerow([i,i/400,code,volts])
        finally:
            port.write(b"\x10"); port.flush()
    print(args.output)

if __name__=="__main__": main()
