# os/lib: the shared Lua library

`vk.lua` is the one library every BadgeOS Lua app uses (`local vk = require("vk")`). An app can only be pushed files under its own `/apps/<id>/`, so there is no shared folder on the badge: `scripts/push-apps.sh` copies this file into each app's folder as `vk.lua` when it installs the app. Change it here, then push the apps again.

What it provides (the list at the top of the file is the index): JSON (`vk.json`), JSON-RPC to `rpc_url` (`vk.rpc`, `blockhash`, `send_tx`, `confirm`), the backend listener (`vk.record`, `report`, `feed`), ESP-NOW frame helpers, the payer flow as a state machine (`vk.pay`), the payee's check of a payment before it shows PAID (`vk.receive`), one text for every refusal reason (`vk.reason_text`), nearby peers (`vk.peers`), and the Receipt-look drawing helpers (`vk.ui`).

Rules it keeps: every network helper returns `nil, message` on failure and nothing waits without a timeout; amounts are decimal strings, never Lua numbers; nothing in it is trusted by the firmware, which checks every payment again itself.

- Reference: [Lua API, "lib/vk.lua"](../../docs/os/platform/lua-api.md#libvklua).
- Tests: `test/host/test_vk.lua` on the laptop (run by `test/host/run.sh` when `lua` is installed) and the `vktest` app on a badge (`test/device/t_vk.py`).
- The harness apps in `harness/apps/` use the same file, added at push time.
