# USB component layout

USB support is intentionally not implemented yet. When it is added, keep its
adapter code in these folders:

- `transport/` — USB device lifecycle and connection state.
- `services/` — USB-facing HID, OTA, and configuration endpoints.
- `control/` — USB control-command handling.
- `include/` — public USB component headers.

The services must call `hid_core`, `ota`, and the future shared configuration
component; they must not duplicate those cores.
