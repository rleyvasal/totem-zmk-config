#!/usr/bin/env python3
"""Show the Totem's left and right battery levels on Windows, over Bluetooth.

Reads the keyboard's Battery Service through Windows' own Bluetooth stack, so it
works while the keyboard is paired and connected, with no USB cable. Windows'
Settings shows only one of the two levels; this shows both.

    py tools\\totem_battery_windows.py              # print both levels once
    py tools\\totem_battery_windows.py --watch 300  # check every 5 minutes, warn at 20% and 10%
    py tools\\totem_battery_windows.py --list       # list paired Bluetooth LE devices

Needs Python 3.9+ on Windows 10 or 11 and the WinRT bindings:

    py -m pip install winrt-runtime winrt-Windows.Devices.Bluetooth ^
        winrt-Windows.Devices.Bluetooth.GenericAttributeProfile ^
        winrt-Windows.Devices.Enumeration winrt-Windows.Storage.Streams ^
        winrt-Windows.Foundation winrt-Windows.Foundation.Collections

How the halves are told apart (same rules as the ZMK Battery Mac app): the
keyboard has two Battery Level characteristics (0x2A19). On this firmware
(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP) the one with the User
Description "Left" is the left half and the plain one is the right half. Upstream
ZMK has them the other way round: plain = left, "Peripheral 0" = right. A Battery
Level Status (0x2BED) next to the plain level says whether that half is charging.

The warning levels assume firmware with CONFIG_ZMK_BATTERY_LIPO_DISCHARGE_CURVE,
where 10% is about 3.69 V, just above the 3.61-3.64 V where the right half
started lagging and dropping the split link.
"""

import argparse
import asyncio
import os
import subprocess
import sys
import uuid
from datetime import datetime

BATTERY_SERVICE = uuid.UUID("0000180f-0000-1000-8000-00805f9b34fb")
BATTERY_LEVEL = uuid.UUID("00002a19-0000-1000-8000-00805f9b34fb")
BATTERY_LEVEL_STATUS = uuid.UUID("00002bed-0000-1000-8000-00805f9b34fb")
USER_DESCRIPTION = uuid.UUID("00002901-0000-1000-8000-00805f9b34fb")

LOW_AT = 20
CRITICAL_AT = 10
THRESHOLDS = (LOW_AT, CRITICAL_AT)
# Re-arm a threshold once the level is this far above it again, so a reading
# that wobbles around a threshold doesn't warn over and over.
HYSTERESIS = 5


# --- Pure logic (no Windows needed; tested on any OS) -------------------------

def named_half(description):
    """'Left' -> 'left', 'Peripheral 0' -> 'right', anything else -> None."""
    text = description.lower()
    if "left" in text or "central" in text:
        return "left"
    if "right" in text or "peripheral" in text:
        return "right"
    return None


def half_for(description, siblings):
    """The half for a level with this User Description (None = it has none),
    given every level's description on the same device."""
    if description is not None:
        return named_half(description)
    named = {named_half(d) for d in siblings if d is not None} - {None}
    if named == {"left"}:
        return "right"
    if named == {"right"}:
        return "left"
    return None


def charging_from_status(value):
    """Battery Level Status: flags byte, then a little-endian Power State whose
    bits 5-6 are the charge state. True charging, False discharging, None unknown."""
    if len(value) < 3:
        return None
    power_state = value[1] | (value[2] << 8)
    state = (power_state >> 5) & 0x3
    if state == 1:
        return True
    if state in (2, 3):
        return False
    return None


class LowBatteryAlerts:
    """Which warning to give, if any, for each new reading."""

    def __init__(self):
        self.fired = {}

    def crossed(self, half, percent, charging):
        """The lowest threshold this level crosses for the first time, or None."""
        done = self.fired.setdefault(half, set())
        if charging:
            done.clear()
            return None
        done.intersection_update({t for t in done if percent <= t + HYSTERESIS})
        newly = {t for t in THRESHOLDS if percent <= t and t not in done}
        done.update(newly)
        return min(newly) if newly else None


def warning_text(half, percent, threshold):
    if threshold <= CRITICAL_AT:
        return f"Charge the {half} half now: {percent}% (below {CRITICAL_AT}%)"
    return f"{half.capitalize()} half battery low: {percent}%"


def format_levels(levels):
    """{'left': (63, None), 'right': (9, True)} -> 'L 63%  R 9% charging'."""
    parts = []
    for half in ("left", "right"):
        if half in levels:
            percent, charging = levels[half]
            parts.append(f"{half[0].upper()} {percent}%" + (" charging" if charging else ""))
        else:
            parts.append(f"{half[0].upper()} --")
    return "  ".join(parts)


# --- Windows Bluetooth (WinRT) -------------------------------------------------

def _winrt():
    try:
        from winrt.windows.devices.bluetooth import (
            BluetoothCacheMode, BluetoothConnectionStatus, BluetoothLEDevice)
        from winrt.windows.devices.bluetooth.genericattributeprofile import GattCommunicationStatus
        from winrt.windows.devices.enumeration import DeviceInformation
    except ImportError as err:
        sys.exit(f"WinRT bindings not found ({err}). This script runs on Windows; "
                 "install the packages listed at the top of this file.")
    return BluetoothCacheMode, BluetoothConnectionStatus, BluetoothLEDevice, \
        GattCommunicationStatus, DeviceInformation


def _method(obj, *names):
    """pywinrt 2.x gives overloads their own names; 1.x used one name. Take the first that exists."""
    for name in names:
        method = getattr(obj, name, None)
        if method is not None:
            return method
    raise AttributeError(f"{type(obj).__name__} has none of {names}")


def _bytes(buffer):
    try:
        return bytes(buffer)
    except TypeError:
        from winrt.windows.storage.streams import DataReader
        reader = DataReader.from_buffer(buffer)
        return bytes(reader.read_byte() for _ in range(buffer.length))


async def paired_devices():
    """(name, connected, device) for every paired Bluetooth LE device."""
    _, ConnStatus, LEDevice, _, DeviceInformation = _winrt()
    selector = LEDevice.get_device_selector_from_pairing_state(True)
    find_all = _method(DeviceInformation, "find_all_async_aqs_filter", "find_all_async")
    found = []
    for info in await find_all(selector):
        device = await LEDevice.from_id_async(info.id)
        if device is None:
            continue
        found.append((device.name or info.name,
                      device.connection_status == ConnStatus.CONNECTED, device))
    return found


async def read_value(gatt_object, uncached=True):
    CacheMode, _, _, GattStatus, _ = _winrt()
    mode = CacheMode.UNCACHED if uncached else CacheMode.CACHED
    read = _method(gatt_object, "read_value_with_cache_mode_async", "read_value_async")
    result = await read(mode)
    if result.status != GattStatus.SUCCESS:
        return None
    return _bytes(result.value)


async def read_levels(device):
    """{'left': (percent, charging or None), 'right': ...} from one device."""
    CacheMode, _, _, GattStatus, _ = _winrt()
    get_services = _method(device, "get_gatt_services_for_uuid_with_cache_mode_async",
                           "get_gatt_services_for_uuid_async")
    services = await get_services(BATTERY_SERVICE, CacheMode.UNCACHED)
    if services.status != GattStatus.SUCCESS:
        raise RuntimeError(f"couldn't open the Battery Service (status {services.status})")

    raw = []  # (description or None, percent, charging or None)
    for service in services.services:
        chars = await service.get_characteristics_async()
        if chars.status != GattStatus.SUCCESS:
            continue
        charging = None
        levels = []
        for char in chars.characteristics:
            if char.uuid == BATTERY_LEVEL_STATUS:
                value = await read_value(char)
                if value is not None:
                    charging = charging_from_status(value)
            elif char.uuid == BATTERY_LEVEL:
                value = await read_value(char)
                if not value:
                    continue
                description = None
                get_desc = _method(char, "get_descriptors_for_uuid_async")
                descs = await get_desc(USER_DESCRIPTION)
                if descs.status == GattStatus.SUCCESS and len(descs.descriptors) > 0:
                    text = await read_value(descs.descriptors[0], uncached=False)
                    if text is not None:
                        description = text.decode("utf-8", errors="replace").rstrip("\x00")
                levels.append([description, value[0]])
        # The status belongs to the plain (undescribed) level in the same service.
        for description, percent in levels:
            raw.append((description, percent, charging if description is None else None))

    siblings = [d for d, _, _ in raw]
    result = {}
    for description, percent, charging in raw:
        half = half_for(description, siblings)
        if half is not None:
            result[half] = (percent, charging)
    if not result and len(raw) == 1:
        result["left"] = (raw[0][1], raw[0][2])  # not a split: one level
    return result


async def find_keyboard(name):
    devices = await paired_devices()
    matches = [d for d in devices if name.lower() in (d[0] or "").lower()]
    connected = [d for d in matches if d[1]]
    if connected:
        return connected[0]
    if matches:
        return matches[0]
    return None


# --- Warnings -----------------------------------------------------------------

def notify(text):
    """A Windows toast through PowerShell, falling back to a beep."""
    script = (
        "$t=[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,"
        "ContentType=WindowsRuntime];"
        "$x=$t::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02);"
        "$n=$x.GetElementsByTagName('text');"
        "$n.Item(0).AppendChild($x.CreateTextNode('Totem battery'))|Out-Null;"
        "$n.Item(1).AppendChild($x.CreateTextNode($env:TOTEM_TOAST))|Out-Null;"
        "$t::CreateToastNotifier('{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}"
        "\\WindowsPowerShell\\v1.0\\powershell.exe')"
        ".Show([Windows.UI.Notifications.ToastNotification]::new($x))"
    )
    try:
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                       env={**os.environ, "TOTEM_TOAST": text}, check=True,
                       capture_output=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        try:
            import winsound
            winsound.MessageBeep(winsound.MB_ICONEXCLAMATION)
        except ImportError:
            pass


# --- Main ---------------------------------------------------------------------

async def once(name):
    found = await find_keyboard(name)
    if found is None:
        return None, f"no paired Bluetooth device named like '{name}' (try --list)"
    device_name, connected, device = found
    if not connected:
        return None, f"{device_name} is paired but not connected"
    try:
        return await read_levels(device), device_name
    except (RuntimeError, OSError) as err:
        return None, f"{device_name}: {err}"


async def main_async(args):
    if args.list:
        for name, connected, device in await paired_devices():
            state = "connected" if connected else "not connected"
            print(f"{name!r:30} {state:14} {device.bluetooth_address:012X}")
        return 0

    alerts = LowBatteryAlerts()
    while True:
        now = datetime.now().strftime("%H:%M:%S")
        levels, status = await once(args.name)
        if levels is None:
            print(f"{now} {status}", flush=True)
        else:
            print(f"{now} {status}: {format_levels(levels)}", flush=True)
            for half, (percent, charging) in levels.items():
                threshold = alerts.crossed(half, percent, bool(charging))
                if threshold is not None:
                    text = warning_text(half, percent, threshold)
                    print(f"{now} WARNING {text}", flush=True)
                    if args.watch:
                        notify(text)
        if not args.watch:
            return 0 if levels else 1
        await asyncio.sleep(args.watch)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--name", default="Totem",
                        help="part of the keyboard's Bluetooth name (default: Totem)")
    parser.add_argument("--watch", type=float, metavar="SECONDS",
                        help="keep checking every SECONDS and show a Windows notification "
                             f"at {LOW_AT}%% and {CRITICAL_AT}%%")
    parser.add_argument("--list", action="store_true", help="list paired Bluetooth LE devices")
    args = parser.parse_args()
    try:
        sys.exit(asyncio.run(main_async(args)))
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
