# os/templates: starting points for new apps

What `scripts/new-app.sh` copies to make a new app. Each template is a small app that already runs: a two-row list in the Receipt look ("Count" goes up on SELECT, "Reset" puts it back to zero, CANCEL exits), commented line by line so it can be turned into a real app.

| Template | Becomes | Placeholders `new-app.sh` fills |
|---|---|---|
| `lua_app/` (`app.ini`, `config.lua`, `main.lua`) | `apps/<id>/` | `__ID__`, `__NAME__` |
| `native_app/app.cpp` | `src/native_apps/<id>/<id>.cpp` | `__ID__`, `__NAME__`, `__CLASS__`, `__TITLE__`, `__LAUNCHER__` |

```bash
cd os
scripts/new-app.sh my_app "My app" [--category games]            # Lua
scripts/new-app.sh my_app "My app" --native [--category games]   # native C++
```

Do not run a template in place: the placeholders are not valid names. Edit a template only to change what every future app starts from; apps already made from it are not affected. Guide: [extending, "Add an app"](../../docs/os/guides/extending.md#add-an-app).
