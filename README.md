# XFCE4 Classic-Menu

A GNOME2/MATE-style classic menu for XFCE4, implemented as a panel plugin.

## What's implemented

- **Applications**
    - fully populated from the system's XDG application menu via garcon, organized by category. The Settings and System categories are intentionally excluded here and surfaced under the System menu instead.
- **Places**
    - mirrors your Thunar sidebar bookmarks exactly, by reading the same GTK bookmarks file Thunar uses. Includes Home, your personal bookmarks, Computer, Network, and Connect to Server.
    - Supports drill-down navigation into folders and the Computer entry, configurable in Preferences:
        - **None** — no drill-down; all entries are plain clickable items.
        - **Folders only** — hovering a folder reveals a submenu of its immediate subdirectories, recursively. Clicking a folder opens it in the file manager.
        - **Folders and AppImages** — as above, plus `.AppImage` files appear in submenus. AppImages are launched directly on click; if the executable bit is not set, the user is prompted to set it first.
        - **All contents** — as above, but all files appear in submenus, each opened by their default application on click.
    - The Computer entry always shows a submenu listing all drives. Mounted drives support drill-down navigation (respecting the drill-down setting above) and open in the file manager on click. Unmounted drives are mounted and opened on click.
    - AppImage icons are loaded asynchronously from the freedesktop thumbnail cache, or extracted directly from the AppImage and cached for future use.
- **System**
    - Settings Manager, Preferences and Administration submenus (also populated via garcon), Help, About XFCE, Lock Screen, and Log Out.

## Building

```shell
make
sudo make install
xfce4-panel --restart
```

## Credits and Development

This project is based on [xfce_classic-menu](https://github.com/8bitprodigy/xfce_classic-menu) by 8bitprodigy. The original author's copyright and permissive license are retained in [LICENSE](LICENSE).

This version adds features, usability improvements, and XFCE desktop integration. Development of these enhancements was assisted by OpenAI's ChatGPT and tested on Linux Mint XFCE.

## License

This code is released into the public domain.
Since not all jurisdictions recognize the public domain, it is also made available under the terms of the 0BSD license. See the [`LICENSE`](LICENSE) file for details.
