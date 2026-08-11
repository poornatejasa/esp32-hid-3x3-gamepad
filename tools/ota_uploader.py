import asyncio
import argparse
import struct
import zlib
from pathlib import Path
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# -------------------------------------------------------
# Device
# -------------------------------------------------------

DEVICE_NAME = "Poorna_GAMR"

PROJECT_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_PATH = PROJECT_ROOT / "build" / "hid.bin"

# The firmware accepts packets up to 512 bytes.  The usable BLE payload is
# negotiated per connection, so the actual chunk size is selected at runtime.
OTA_MAX_CHUNK_SIZE = 512
OTA_WRITE_BURST_SIZE = 8
OTA_BURST_PAUSE_SECONDS = 0.003
OTA_READY_RETRIES = 5
OTA_READY_RETRY_SECONDS = 0.5
OTA_CONNECT_RETRIES = 5
PROGRESS_UPDATE_SECONDS = 0.25

# -------------------------------------------------------
# OTA UUIDs
# -------------------------------------------------------

OTA_SERVICE_UUID = "0000FFF0-0000-1000-8000-00805F9B34FB"
OTA_CONTROL_UUID = "0000FFF1-0000-1000-8000-00805F9B34FB"
OTA_DATA_UUID    = "0000FFF2-0000-1000-8000-00805F9B34FB"
OTA_STATUS_UUID  = "0000FFF3-0000-1000-8000-00805F9B34FB"
DEVICE_ID_UUID   = "00002A25-0000-1000-8000-00805F9B34FB"

# -------------------------------------------------------
# OTA Commands
# -------------------------------------------------------

OTA_CMD_ENTER  = 0x00
OTA_CMD_START  = 0x01
OTA_CMD_END    = 0x02
OTA_CMD_ABORT  = 0x03
OTA_CMD_REBOOT = 0x04


# -------------------------------------------------------
# Helper Functions
# -------------------------------------------------------

async def select_device(*names):

    print(f"Scanning for {' or '.join(names)}...")

    devices = await BleakScanner.discover(timeout=5)
    matches = [device for device in devices if device.name in names]

    if not matches:
        return None

    if len(matches) == 1:
        return matches[0]

    print("Nearby GAMR devices:")
    for index, device in enumerate(matches):
        print(f"  [{index}] {device.name}  [{device.address}]")

    while True:
        try:
            selected = int(input("Select device: ").strip())
            return matches[selected]
        except (ValueError, IndexError):
            print("Enter one of the listed device numbers.")

async def print_device_id(client):
    """Print the standard Device Information serial number when available."""
    try:
        device_id = await client.read_gatt_char(DEVICE_ID_UUID)
        print(f"Device ID: {bytes(device_id).decode('utf-8')}")
    except Exception:
        # Supports updating firmware built before the Device Information service.
        pass

class Firmware:

    def __init__(self, path):

        self.path = path

        if not path.is_file():
            raise FileNotFoundError(f"Firmware image not found: {path}")

        self.data = path.read_bytes()

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

async def wait_for_ota_service(client):
    """Wait until the Windows GATT session accepts protected OTA requests."""
    for attempt in range(1, OTA_READY_RETRIES + 1):
        try:
            await read_status(client)
            return
        except (OSError, AssertionError) as exc:
            if attempt == OTA_READY_RETRIES:
                raise RuntimeError(
                    "OTA GATT session did not become ready after pairing"
                ) from exc

            print(
                f"OTA service not ready yet ({exc}); retrying "
                f"{attempt}/{OTA_READY_RETRIES}..."
            )
            await asyncio.sleep(OTA_READY_RETRY_SECONDS)

async def close_client(client):
    """Release WinRT GATT services even when its connection state is stale."""
    try:
        await client.disconnect()
    except Exception:
        pass

async def connect_to_ota_firmware(device):
    """Connect with a fresh WinRT GATT session until protected OTA I/O works."""
    last_error = None

    for attempt in range(1, OTA_CONNECT_RETRIES + 1):
        client = BleakClient(device, pair=True, timeout=30.0)
        try:
            await client.connect()
            print("Connected to OTA firmware.")
            await wait_for_ota_service(client)
            return client
        except (BleakError, OSError, AssertionError, RuntimeError) as exc:
            last_error = exc
            await close_client(client)

        if attempt < OTA_CONNECT_RETRIES:
            print(
                f"OTA connection was not ready ({last_error}); reconnecting "
                f"{attempt}/{OTA_CONNECT_RETRIES}..."
            )
            await asyncio.sleep(OTA_READY_RETRY_SECONDS)

    raise RuntimeError("Unable to establish an encrypted OTA connection") from last_error

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

    data_characteristic = client.services.get_characteristic(OTA_DATA_UUID)
    if data_characteristic is None:
        raise RuntimeError("OTA data characteristic was not discovered")

    # On some platforms the negotiated maximum is initially reported as the
    # default 20-byte payload. Give the Bluetooth stack a moment to update it.
    for _ in range(20):
        write_size = data_characteristic.max_write_without_response_size
        if write_size > 20:
            break
        await asyncio.sleep(0.25)

    chunk_size = min(write_size, OTA_MAX_CHUNK_SIZE)
    if chunk_size <= 0:
        raise RuntimeError("Invalid BLE write-without-response payload size")

    print(f"Negotiated write payload: {write_size} bytes")
    print(f"OTA chunk size: {chunk_size} bytes")

    bytes_sent = 0
    packets_sent = 0
    start_time = asyncio.get_running_loop().time()
    last_progress_time = start_time
    last_progress = -1

    while bytes_sent < firmware.size:

        chunk = firmware.data[
            bytes_sent :
            bytes_sent + chunk_size
        ]

        await client.write_gatt_char(
            data_characteristic,
            chunk,
            response=False
        )

        bytes_sent += len(chunk)
        packets_sent += 1

        progress = (bytes_sent * 100) // firmware.size
        elapsed = asyncio.get_running_loop().time() - start_time
        rate_kib_s = (bytes_sent / 1024) / elapsed if elapsed > 0 else 0

        # Terminal output is substantially slower than BLE writes on Windows.
        # Refreshing periodically keeps the progress display useful without
        # reducing transfer throughput for every packet.
        if (progress != last_progress and
                (elapsed - (last_progress_time - start_time) >= PROGRESS_UPDATE_SECONDS or
                 bytes_sent == firmware.size)):
            print(
                f"\rProgress : {progress:3d}% ({bytes_sent}/{firmware.size})"
                f"  {rate_kib_s:5.1f} KiB/s",
                end="",
                flush=True,
            )
            last_progress = progress
            last_progress_time = asyncio.get_running_loop().time()

        # Write-without-response is intentionally unacknowledged. Yielding
        # between short bursts prevents overflowing the Windows/NimBLE queues
        # without imposing a delay after every packet.
        if packets_sent % OTA_WRITE_BURST_SIZE == 0:
            await asyncio.sleep(OTA_BURST_PAUSE_SECONDS)

    elapsed = asyncio.get_running_loop().time() - start_time
    print(f"\nFirmware transfer complete in {elapsed:.1f} seconds.")

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

async def main(device_address=None):

    # OTA is available in normal firmware. The device never reboots into an
    # alternate OTA profile, so it remains a working HID device until the
    # update is successfully applied.
    if device_address:
        # This mirrors the phone app's saved-device path. It can work when
        # Windows already knows the bonded HID device and a scan is not useful.
        device = device_address
        print(f"Connecting to known GAMR device: {device_address}")
    else:
        print("Searching for OTA-capable GAMR device...")
        device = await select_device(DEVICE_NAME)
        if device is None:
            print("No available GAMR device found. It may already be connected.")
            print("Retry with --device <Bluetooth-address> to attempt a direct connection.")
            return

    client = await connect_to_ota_firmware(device)
    try:
        await print_device_id(client)
        try:
            print(f"MTU: {client.mtu_size}")
        except AssertionError:
            # Bleak's WinRT backend can omit this cached property even after
            # usable GATT I/O. ota_send_firmware reads the actual write limit.
            print("MTU: not reported by Windows")
        print(f"Configured maximum chunk size: {OTA_MAX_CHUNK_SIZE}")

        firmware = Firmware(FIRMWARE_PATH)
        firmware.print_info()
        try:
            await ota_start(client, firmware.size, firmware.crc32)
            await ota_send_firmware(client, firmware)
            await ota_finish(client)
            await ota_reboot(client)

        except (KeyboardInterrupt, asyncio.CancelledError):
            print("\nUpload cancelled.")
            try:
                await ota_abort(client)
            except Exception:
                pass
            raise

        except Exception:
            print("\nUpload failed.")
            raise
    finally:
        await close_client(client)

    print("Disconnected.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Upload GAMR firmware over its normal encrypted BLE service."
    )
    parser.add_argument(
        "--device",
        metavar="BLUETOOTH_ADDRESS",
        help=(
            "Known BLE address to connect without scanning, for example "
            "98:A3:16:7E:53:C2"
        ),
    )
    args = parser.parse_args()
    asyncio.run(main(args.device))
