# keybindz

A minimal GUI for managing MangoWM keybindings without editing config files by hand.

Writes `~/.config/mango/keybindz.conf` in native MangoWM `bind=` syntax.  
Black background, gray monospace text, floating window.

---

## Dependencies

```bash
sudo pacman -S gtk3 wayland wayland-protocols wayland-utils base-devel
```

`wayland-utils` provides `wayland-scanner`, needed at build time.

---

## Build

```bash
cd ~/Projects/keybindz
make
```

The Makefile auto-generates `inhibit-protocol.h` and `inhibit-protocol.c` from
the system's `wayland-protocols` XML, then compiles everything.

### Install to `~/.local/bin`

```bash
make install
```

### Clean build artifacts

```bash
make clean
```

---

## Usage

```bash
keybindz
```

### Adding a binding

1. Click the left entry field (`click → press combo`).
2. Press your key combination — e.g. `Super+Return`, `Ctrl+Alt+T`, `F5`.  
   The captured combo appears in the field and focus moves to the command entry.
3. Type the command to run in the right field — e.g. `foot` or `bash -c 'notify-send hello'`.
4. Check **shell** if the command needs a shell (`spawn_shell`) — required for pipes,
   subshells, `$()`, redirects. Leave unchecked for simple executables (`spawn`).
5. Click **add**.
6. Click **save** to write the file to disk.

### Removing a binding

Click **✕** on any row, then **save**.

### Modifier keys

| Pressed key  | Written as |
|-------------|------------|
| Super / Win | `SUPER`    |
| Ctrl        | `CTRL`     |
| Alt         | `ALT`      |
| Shift       | `SHIFT`    |
| (none)      | `none`     |

Combinations are joined with `+`: `SUPER+SHIFT`, `CTRL+ALT`, etc.

### Key capture and compositor shortcuts

keybindz uses the **Wayland keyboard shortcuts inhibitor** protocol
(`zwp_keyboard_shortcuts_inhibit_manager_v1`).  
While the capture field is focused, MangoWM forwards all keyboard events directly
to keybindz instead of processing them as compositor shortcuts.  
This means you can capture `Super+Return`, `Alt+F4`, `Ctrl+Alt+Delete`, etc.
without triggering WM actions.

---

## Integrating with MangoWM

### 1. Load keybindz.conf

Add the following line to `~/.config/mango/config.conf` (or `bind.conf`):

```
include ~/.config/mango/keybindz.conf
```

Then reload MangoWM:

```
Super+r   (or your configured reload binding)
```

### 2. Floating window rule

Add to `~/.config/mango/rule.conf` so keybindz always opens as a floating window:

```
windowrule=isfloating:1,appid:org.keybindz
```

---

## Output format

`keybindz.conf` uses standard MangoWM `bind=` syntax:

```
# keybindz.conf — managed by keybindz
bind=SUPER,Return,spawn,foot
bind=SUPER+SHIFT,s,spawn_shell,grim -g "$(slurp)" ~/screenshot.png
bind=none,XF86AudioRaiseVolume,spawn,~/.config/mango/scripts/volume.sh up
```

- `spawn` — runs the command directly (no shell).
- `spawn_shell` — runs via shell; required for `$()`, pipes, `&&`, etc.

The file is plain text and safe to edit manually between keybindz sessions.
