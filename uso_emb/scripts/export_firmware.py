"""Export application, bootloader and factory image with committed metadata."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import argparse
from elftools.elf.elffile import ELFFile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/"tools"))
from uso_bootloader import APP_BASE,APP_MAX,RAM_TOP,crc32,validate_image
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--nucleo', action='store_true', help='Export HSI bench application into dist/nucleo')
args = parser.parse_args()
app_env = 'uso_nucleo_hsi' if args.nucleo else 'uso_g431'
dist = root / "dist" / "nucleo" if args.nucleo else root / "dist"
dist.mkdir(exist_ok=True)
pio=Path(os.environ.get("PLATFORMIO_CORE_DIR",str(Path.home()/".platformio")))
objcopy = pio / "packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-objcopy"
if not objcopy.exists(): objcopy = objcopy.with_suffix(".exe")
for env,name,address in [(app_env,"uso32_g431",APP_BASE),("uso_bootloader","uso32_bootloader",0x08000000)]:
    build=root/".pio/build"/env
    with (build/"firmware.elf").open("rb") as f:
        elf=ELFFile(f)
        assert elf.get_section_by_name(".isr_vector")["sh_addr"]==address,"Wrong link address"
    for extension in ["bin","elf"]:
        shutil.copy2(build/f"firmware.{extension}",dist/f"{name}.{extension}")
    subprocess.run([str(objcopy),"-O","ihex",str(build/"firmware.elf"),str(dist/f"{name}.hex")],check=True)
app=(dist/"uso32_g431.bin").read_bytes(); validate_image(app)
boot=(dist/"uso32_bootloader.bin").read_bytes()
assert len(boot)<=32768
assert struct.unpack_from("<I",boot)[0]==RAM_TOP
factory=bytearray(b"\xff"*(128*1024))
factory[:len(boot)]=boot
factory[0x8000:0x8000+len(app)]=app
struct.pack_into("<IIII",factory,0x1f800,len(app),crc32(app),0x32534f55,1)
(dist/"uso32_factory.bin").write_bytes(factory)
subprocess.run([str(objcopy),"-I","binary","-O","ihex","--change-addresses","0x08000000",
                str(dist/"uso32_factory.bin"),str(dist/"uso32_factory.hex")],check=True)
hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(dist.glob("uso32*")) if p.is_file()}
sources = [root / "platformio.ini"]
for folder in ["src", "include", "scripts", "linker", "tools", "tests"]:
    sources += sorted(p for p in (root/folder).glob("*") if p.is_file())
manifest = {
    "application_environment": app_env,
    "target": "STM32G431KBU6", "boot_address": "0x08000000", "app_address":"0x08008000",
    "metadata_address":"0x0801F800", "app_max_bytes":APP_MAX, "app_size":len(app),
    "app_crc32":f"{crc32(app):08X}", "sample_rate_hz": 400,
    "platform": "ststm32@19.4.0", "cube_g4": "1.6.3", "gcc_arm": "7.2.1",
    "hardware_tested": False, "sha256": hashes,
    "source_sha256": {p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
}
(dist/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
print(f"App {len(app)} bytes at 0x{APP_BASE:08X}; boot {len(boot)} bytes; factory {len(factory)} bytes")
