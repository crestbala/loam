# assets

`icon.svg` is the app icon. `zeli build --targets macos` renders it into
`build/macos/Vidy.app` (AppKit + `sips` + `iconutil`, all built into macOS);
an `icon.icns` here would be used as is instead.
