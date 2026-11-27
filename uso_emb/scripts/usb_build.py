Import("env")
from pathlib import Path

framework = Path(env.PioPlatform().get_package_dir("framework-stm32cubeg4"))
usb = framework / "Middlewares/ST/STM32_USB_Device_Library"
env.Append(CPPPATH=[str(usb / "Core/Inc"), str(usb / "Class/CDC/Inc")])
# PlatformIO's Cube library manifests compile core/CDC and exclude templates.
# LDF finds them through the application's usbd_core/usbd_cdc includes.
