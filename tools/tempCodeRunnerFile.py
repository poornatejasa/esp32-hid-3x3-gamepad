import asyncio
from bleak import BleakScanner


async def main():
    print("Scanning for 10 seconds...\n")

    devices = await BleakScanner.discover(timeout=10.0)

    for device in devices:
        print(device)


asyncio.run(main())