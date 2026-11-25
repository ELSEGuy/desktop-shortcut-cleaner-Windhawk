# Desktop Shortcut Cleaner for Windhawk

A Windows 11 Windhawk mod for cleaning up desktop shortcuts **without changing File Explorer**.

![Hovering, selecting, dragging and cutting desktop icons: icon-shaped glow, solid drag and faded cut items](Screnshot/glow_demo.gif)

## Features

- Hide the text labels of **desktop shortcuts only**.
- Optionally hide the names of **all desktop icons**.
- Hide the standard shortcut arrow **on the desktop only**.
- Hide the blue/yellow **UAC (admin) shield** on desktop shortcuts whose target uses the *Run this program as an administrator* compatibility setting. It stays hidden after refreshing the desktop.
- Replace the white **hover/selection box** behind desktop icons with a **soft white glow that follows the icon's shape**. Hovered icons glow subtly; selected icons and drop targets glow brighter, and the glow fades in and out.
- **Solid icon while dragging:** icons dragged from the desktop move as a solid icon instead of a faded copy, and the original spot stays empty until you drop. Hidden labels, arrows and shields stay hidden in the dragged image too.
- In shortcut-only mode, **only shortcuts lose their labels**.
- File Explorer keeps its shortcut arrows, shields and normal highlight.

If **Hide all desktop icon text** is enabled, it overrides the shortcut-only text option.

A shortcut to a folder is still a shortcut, so its name is hidden when shortcut-only hiding is enabled.

### About the UAC shield

For shortcuts whose target has the *Run this program as an administrator* compatibility setting, Windows bakes the shield into the cached icon image. The mod extracts the shortcut's raw icon and draws it **only on the desktop** instead of the shielded one, so File Explorer is unchanged. The shortcut still runs as administrator. Only the icon changes.

### About the icon glow

The glow is generated from each icon's own transparency: its outline is slightly widened, blurred and filled with white, then drawn underneath the icon, so only the halo outside the icon's shape is visible. The hover strength can be tuned with `kGlowHoverAlpha` in the source (0 = none, 255 = full).

## How it differs from similar mods

- **[Hide Desktop Icon Text and Shortcut Arrows](https://windhawk.net/mods/hide-desktop-icon-text)** (kivsak): hides every label except folder names, and removes arrows by editing Explorer's shared icon list, which also affects File Explorer until Explorer restarts. This mod hides only shortcut labels (or all labels), and removes arrows on the desktop only, at draw time, so everything reverts when it's disabled.

## Known limitations

- Shield hiding covers the *Run this program as an administrator* compatibility setting only. Shortcuts to programs whose manifest requests elevation (many installers) keep their shield, as do shortcuts whose target is on a network path.

## Screenshots

### Before

![Desktop before enabling the mod](Screnshot/before.png)

### Shortcut names and arrows hidden

![Desktop with shortcut names and arrows hidden](Screnshot/Shortcut_text_and_arrow.png)

### All desktop icon names hidden

![Desktop with all icon names hidden](Screnshot/ALL.png)

### File Explorer remains unchanged

![File Explorer with normal shortcut arrows and names](Screnshot/Windows_explorer_untouched.png)

## Installation

Until the mod is available in the Windhawk catalog:

1. Install [Windhawk](https://windhawk.net/).
2. Enable **Developer mode** in Windhawk.
3. Create a new mod.
4. Replace the generated code with the contents of `desktop-shortcut-cleaner.wh.cpp`.
5. Compile the mod.
6. Enable it and configure the toggles in **Settings**.

## Compatibility

- Windows 11
- x86-64
- Windhawk

Windows updates can change Explorer internals, so future Windows builds may require adjustments.

## Credits

Desktop detection and label hiding are adapted from [kivsak](https://github.com/kivsak)'s *Hide Desktop Icon Text and Shortcut Arrows* mod (MIT license).

## License

MIT. See [LICENSE](LICENSE).
