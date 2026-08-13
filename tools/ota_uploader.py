import asyncio
import argparse
import struct
import sys
import zlib
from pathlib import Path
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice
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


class DiscoveredDevice:
    """A GAMR device found through advertising or Windows' paired-device cache."""

    def __init__(self, device, source):
        self.device = device
        self.source = source

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

def device_key(device):
    return device.address.upper()


def windows_known_ble_device(address, name):
    """Create a BLEDevice that makes Bleak's WinRT backend skip scanning."""
    return BLEDevice(address, name, details=None)


async def find_windows_known_devices():
    """Return paired GAMR devices remembered by Windows, if any.

    BleakScanner can only discover advertisements.  Windows also retains a
    catalogue of paired BLE devices, which lets us attempt GATT access to a
    GAMR HID device that is already known to this laptop.
    """
    if sys.platform != "win32":
        return []

    from winrt.windows.devices.bluetooth import BluetoothLEDevice
    from winrt.windows.devices.enumeration import DeviceInformation

    selector = BluetoothLEDevice.get_device_selector_from_pairing_state(True)
    device_infos = await DeviceInformation.find_all_async_aqs_filter(selector)
    matches = []

    for info in device_infos:
        if DEVICE_NAME.lower() not in info.name.lower():
            continue

        device = await BluetoothLEDevice.from_id_async(info.id)
        if device is None:
            continue

        try:
            address = f"{device.bluetooth_address:012X}"
            address = ":".join(address[i:i + 2] for i in range(0, 12, 2))
            matches.append(
                DiscoveredDevice(
                    windows_known_ble_device(address, info.name or DEVICE_NAME),
                    "Windows paired device",
                )
            )
        finally:
            device.close()

    return matches


async def select_device():

    print(f"Scanning for {DEVICE_NAME}...")
    advertised = await BleakScanner.discover(timeout=5)
    matches = [
        DiscoveredDevice(device, "advertising")
        for device in advertised
        if device.name == DEVICE_NAME
    ]

    # Advertising is the preferred path.  Ask Windows for remembered paired
    # devices only when no current advertisement was found.
    known_devices = []
    if not matches:
        print("No GAMR advertisement found; checking Windows paired BLE devices...")
        try:
            known_devices = await find_windows_known_devices()
        except OSError as exc:
            # The advertisement path remains fully usable if Windows device
            # enumeration is temporarily unavailable.
            print(f"Could not read Windows paired devices: {exc}")

    seen_addresses = {device_key(match.device) for match in matches}
    for known in known_devices:
        if device_key(known.device) not in seen_addresses:
            matches.append(known)
            seen_addresses.add(device_key(known.device))

    if not matches:
        return None

    if len(matches) == 1:
        return matches[0].device

    print("Available GAMR devices:")
    for index, match in enumerate(matches):
        print(f"  [{index}] {match.device.name}  [{match.device.address}]  {match.source}")

    while True:
        try:
            selected = int(input("Select device: ").strip())
            return matches[selected].device
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
        # Passing a string to BleakClient causes its WinRT backend to scan
        # first. A BLEDevice carries the address directly and therefore tries
        # the Windows GATT path without requiring a fresh advertisement.
        device = windows_known_ble_device(device_address, DEVICE_NAME)
        print(f"Connecting through Windows GATT: {device_address}")
    else:
        print("Searching for OTA-capable GAMR device...")
        device = await select_device()
        if device is None:
            print("No advertising or Windows-paired GAMR device found.")
            print("Retry with --device <Bluetooth-address> to try a known device directly.")
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
