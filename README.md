# Desktop Shortcut Cleaner for Windhawk

A Windows 11 Windhawk mod for cleaning up desktop shortcuts **without changing File Explorer**.

## Features

- Hide the text labels of **desktop shortcuts only**.
- Optionally hide the names of **all desktop icons**.
- Hide the standard shortcut arrow **on the desktop only**.
- Real folders and normal files keep their names when shortcut-only hiding is used.
- Shortcut arrows remain visible in File Explorer.

### Default settings

| Setting | Default |
| --- | --- |
| Hide desktop shortcut text | On |
| Hide all desktop icon text | Off |
| Hide desktop shortcut arrows | On |

If **Hide all desktop icon text** is enabled, it overrides the shortcut-only text option.

A shortcut to a folder is still a shortcut, so its name is hidden when shortcut-only hiding is enabled.

## Installation

Until the mod is available in the Windhawk catalog:

1. Install [Windhawk](https://windhawk.net/).
2. Enable **Developer mode** in Windhawk.
3. Create a new mod.
4. Replace the generated code with the contents of `desktop-shortcut-cleaner.wh.cpp`.
5. Compile the mod.
6. Enable it and configure the three toggles in **Settings**.

## Compatibility

- Windows 11
- x86-64
- Windhawk

Windows updates can change Explorer internals, so future Windows builds may require adjustments.

## Credits

Based in part on **Hide Desktop Icon Text and Shortcut Arrows** by **kivsak**.

The desktop detection and text-hiding approach were adapted from that mod. This project uses a different shortcut-arrow rendering implementation so the arrow is hidden on the desktop without being removed from File Explorer.

## License

MIT. See [LICENSE](LICENSE).
