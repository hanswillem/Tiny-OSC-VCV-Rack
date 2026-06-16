# Manual Release Checklist

This is the current manual release process for TinyOSC. It assumes a local VCV Rack 2 SDK and a macOS Apple Silicon target.

## Before Release

1. Update `version` in `plugin.json`.
2. Update any user-facing notes in `README.md`.
3. Confirm SVG assets in `res/` have physical dimensions, not `width="100%" height="100%"`.
4. Confirm `plugin.json` metadata is accurate for the public repo:
   - `license`
   - `author`
   - `pluginUrl`
   - `sourceUrl`
   - `manualUrl`
   - `changelogUrl`
5. Start VCV Rack locally and smoke-test:
   - module appears in the browser
   - port and address fields persist
   - matching OSC address flashes the data light
   - scale and offset knobs affect output live
   - `1.0` input with default scale/offset outputs `10V`

## Build

```bash
export RACK_DIR=/path/to/Rack-SDK
make clean
make dist
```

The package should appear in `dist/` as:

```text
TinyOSC-<version>-mac-arm64.vcvplugin
```

## Install Test

```bash
make install
```

Restart Rack and test the installed plugin, not just a local development copy.

## GitHub Release

1. Commit the release changes.
2. Tag the release:

```bash
git tag v<version>
git push origin main --tags
```

3. Create a GitHub Release for the tag.
4. Upload the `.vcvplugin` file from `dist/`.
5. Include short release notes with:
   - supported platform
   - new changes
   - known limitations

## Current Platform Notes

- Built package target: macOS Apple Silicon (`mac-arm64`).
- Source should be close to macOS/Linux portable.
- Windows is not supported yet because the UDP receiver currently uses POSIX sockets.
