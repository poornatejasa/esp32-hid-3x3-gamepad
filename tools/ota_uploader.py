import asyncio
import struct
import zlib
from pathlib import Path
from bleak import BleakClient, BleakScanner

# -------------------------------------------------------
# Device
# -------------------------------------------------------

DEVICE_NAME = "Poorna_GAMR"

PROJECT_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_PATH = PROJECT_ROOT / "build" / "hid.bin"

OTA_CHUNK_SIZE = 128

# -------------------------------------------------------
# OTA UUIDs
# -------------------------------------------------------

OTA_SERVICE_UUID = "0000FFF0-0000-1000-8000-00805F9B34FB"
OTA_CONTROL_UUID = "0000FFF1-0000-1000-8000-00805F9B34FB"
OTA_DATA_UUID    = "0000FFF2-0000-1000-8000-00805F9B34FB"
OTA_STATUS_UUID  = "0000FFF3-0000-1000-8000-00805F9B34FB"

# -------------------------------------------------------
# OTA Commands
# -------------------------------------------------------

OTA_CMD_START  = 0x01
OTA_CMD_END    = 0x02
OTA_CMD_ABORT  = 0x03
OTA_CMD_REBOOT = 0x04


# -------------------------------------------------------
# Helper Functions
# -------------------------------------------------------

async def find_device():

    print("Scanning...")

    devices = await BleakScanner.discover(timeout=5.0)

    for device in devices:

        if device.name == DEVICE_NAME:
            return device

    return None

class Firmware:

    def __init__(self, path):

        self.path = path

        with open(path, "rb") as f:
            self.data = f.read()

        self.size = len(self.data)
        self.crc32 = zlib.crc32(self.data) & 0xFFFFFFFF

    def print_info(self):

        print("\n----- Firmware -----")
        print(f"Path : {self.path}")
        print(f"Size : {self.size} bytes")
        print(f"CRC32: 0x{self.crc32:08X}")
        print("--------------------")

async def read_status(client):

    status = await client.read_gatt_char(
        OTA_STATUS_UUID
    )

    state, progress, received, total = struct.unpack(
        "<BBII",
        status
    )

    print("\n----- OTA STATUS -----")
    print(f"State      : {state}")
    print(f"Progress   : {progress}%")
    print(f"Received   : {received}")
    print(f"Total Size : {total}")
    print("----------------------")

async def ota_start(client, image_size, crc32):

    packet = struct.pack(
        "<BII",
        OTA_CMD_START,
        image_size,
        crc32
    )
    print(f"START packet length: {len(packet)} bytes")
    await client.write_gatt_char(
        OTA_CONTROL_UUID,
        packet,
        response=True
    )

    print("OTA START sent")

async def ota_send_firmware(client, firmware):

    print("\nSending firmware...\n")

    bytes_sent = 0

    while bytes_sent < firmware.size:

        chunk = firmware.data[
            bytes_sent :
            bytes_sent + OTA_CHUNK_SIZE
        ]

        await client.write_gatt_char(
            OTA_DATA_UUID,
            chunk,
            response=False
        )

        await asyncio.sleep(0.002)   # 2 ms

        bytes_sent += len(chunk)

        progress = (bytes_sent * 100) // firmware.size

        print(
            f"\rProgress : {progress:3d}% ({bytes_sent}/{firmware.size})",
            end=""
        )

    print("\nFirmware transfer complete.")

async def ota_finish(client):

    packet = struct.pack(
        "<B",
        OTA_CMD_END
    )

    await client.write_gatt_char(
        OTA_CONTROL_UUID,
        packet,
        response=True
    )

    print("OTA END sent")

async def ota_abort(client):

    packet = struct.pack("<B", OTA_CMD_ABORT)

    await client.write_gatt_char(
        OTA_CONTROL_UUID,
        packet,
        response=True
    )

    print("OTA ABORT sent")

async def ota_reboot(client):

    packet = struct.pack(
        "<B",
        OTA_CMD_REBOOT
    )

    await client.write_gatt_char(
        OTA_CONTROL_UUID,
        packet,
        response=True
    )

    print("OTA REBOOT sent")
# -------------------------------------------------------
# Main
# -------------------------------------------------------

async def main():

    device = await find_device()

    if device is None:
        print("Device not found.")
        return

    print(f"Connecting to {device.name}...")

    async with BleakClient(device) as client:

        print("Connected.")
        print(f"MTU: {client.mtu_size}")
        print(f"Chunk Size: {OTA_CHUNK_SIZE}")

        firmware = Firmware(FIRMWARE_PATH)

        firmware.print_info()

        try:
            await ota_start(client, firmware.size, firmware.crc32)
            await ota_send_firmware(client, firmware)
            await ota_finish(client)
            await ota_reboot(client)

        except (KeyboardInterrupt, asyncio.CancelledError):
            print("\nUpload cancelled.")
            await ota_abort(client)
            raise

        except Exception:
            print("\nUpload failed.")
            await ota_abort(client)
            raise

    print("Disconnected.")

if __name__ == "__main__":
    asyncio.run(main())