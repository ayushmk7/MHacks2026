# BadgeOS

Firmware for the ESP32-S3 badge. BadgeOS runs apps written in Lua or C++, and it is a wallet:
the badge holds one signing key, and every payment is approved by its owner on a screen that
belongs to the firmware, which apps cannot draw over or skip.

What is in it:

- a shell in the Receipt design: boot screen, launcher, settings and dialogs;
- a wallet core, the only code that can sign with the badge key, and an approval screen;
- checks the firmware makes itself before a payment: who is being paid, that they asked for
  exactly this, and that they are here now;
- an app platform: Lua apps and native C++ apps, permissions with first-run consent, an
  ESP-NOW router, notifications;
- apps: Home, Pay, Request, History, Contacts, a game with a shop, a duel with a stake.

BadgeOS is built on Solana OS by spacemandev.

## Documentation

Everything is specified in [`docs/os/`](../docs/os/README.md): start with its README.

- Build, flash and provision: [`docs/os/guides/build-flash-provision.md`](../docs/os/guides/build-flash-provision.md)
- Every change made to an upstream file: [`docs/os/architecture/upstream-hooks.md`](../docs/os/architecture/upstream-hooks.md)
  (copied in [`UPSTREAM-HOOKS.md`](UPSTREAM-HOOKS.md))
- The Lua API BadgeOS adds: [`docs/os/platform/lua-api.md`](../docs/os/platform/lua-api.md)
- Upstream's README, kept as the reference for the upstream Lua API and the app format:
  [`docs/os/reference/upstream-readme.md`](../docs/os/reference/upstream-readme.md)
