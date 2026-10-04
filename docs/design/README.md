# Design

Visual direction for the project's two user interfaces.

| File | For | What it is |
|---|---|---|
| [frontend.md](frontend.md) | the dashboard web app (`dashboard/web/`) | a frontend design guide: typography, colour and theme, motion and backgrounds, with the principle of making bold, coherent choices instead of default ones |
| [os-mockups/index.html](os-mockups/index.html) | the badge (BadgeOS) | a live simulation of every screen in the **Receipt** design, light and dark. Serve the folder (`python3 -m http.server 8765` in `os-mockups/`) and open `http://127.0.0.1:8765`; keys 1 and 2 switch the mode. It was drawn before the rename and still prints `Badge OS` in two words; the device says `BadgeOS` |

The badge's design is decided and specified in [docs/os/ui/ui.md](../os/ui/ui.md#theme) and [docs/os/ui/shell.md](../os/ui/shell.md); the mockup is their visual reference. When a document and the mockup disagree about a pixel, the mockup wins; about behaviour, the document wins.
