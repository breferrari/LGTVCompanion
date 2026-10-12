# This fork

This is a fork of [JPersson77/LGTVCompanion](https://github.com/JPersson77/LGTVCompanion), tuned for a PC that streams with Sunshine or Apollo to Moonlight clients while an LG OLED is its display. It follows upstream: upstream fixes are merged in regularly (see [Syncing with upstream](#syncing-with-upstream)).

Versions are the upstream version the fork is based on, plus a fork build number: `5.7.0.1` is the first fork build on top of upstream 5.7.0. Releases are published on [the fork's releases page](https://github.com/breferrari/LGTVCompanion/releases), and the built-in update check looks there, not upstream.

## What is different

### A TV in standby can no longer break a stream

When the GPU has to switch on a TV that has been in standby for a while, for example because Windows wakes the displays or the streaming host changes the resolution or HDR, the mode set can fail. Windows then keeps the monitor connected but enables no display at all, and never retries. Windows records this as a `VIDEO_DXGKRNL_BLACK_SCREEN_LIVEDUMP (0x1A8)` live dump, argument `0xA`, in `C:\Windows\LiveKernelReports\WATCHDOG`. The streaming host finds nothing to capture: the stream goes black, or the host fails to start with "Unable to find display or encoder", and only restarting it helps. Retrying the mode set does not help while the TV sleeps. Windows enables the display as soon as the TV wakes.

The fork handles this in three places:

- **Display path guard** (daemon). Detects a connected monitor with no active display path, and asks the service to wake the device. It checks every second for 15 seconds after a display change or display power on event, and while recovering, and every 10 seconds otherwise.
- **Wake with the screen off** (service). While streaming, or until the Windows displays are turned off after a stream, the device is powered on and its screen blanked as soon as it is on, so the panel stays dark. Outside of streaming the regular display power handling applies.
- **"Display blanked" remote streaming mode with an end mode.** The "Remote streaming end mode" option also applies to the blank mode. With "Display blanked" and "Display off" the TV stays awake with the screen off for the whole stream, so no mode set ever targets a sleeping TV, and goes to standby once the stream has ended and the display configuration has settled.

### A stream the host never closed no longer sticks

A client can disappear without the streaming host noticing. The host then reports a busy session for hours, and LGTV Companion keeps treating the PC as streaming: it ignores Windows waking the displays, so the TV stays off when you come back. While a stream is running, the daemon checks every 15 seconds whether the host (`sunshine.exe`) is actually using the GPU video encoder. After 90 seconds without encoding, the stream is treated as ended and the end mode applies.

### The Windows displays are turned off when the configuration has settled

When the end mode keeps the displays off, the Windows displays are turned off once there have been no display changes for 5 seconds (at most 30 seconds after the stream ended), instead of after a fixed 2 seconds. This gives the streaming host time to restore the resolution and HDR state first.

### Diagnostics

Everything above is logged in `C:\ProgramData\LGTV Companion\log.txt`, under the `Display` tag and in the daemon messages: when a display path was lost, each wake request, when and how the path came back, when the configuration settled, and when a stream was ended for lack of video. Each `Display` line ends with the remote stream and Windows display state at that moment.

## Recommended settings

For a PC that streams with Sunshine or Apollo and uses an LG OLED as its display:

| Where | Setting | Value |
|---|---|---|
| LGTV Companion | Support remote streaming | On |
| LGTV Companion | Remote streaming power off mode | Display blanked |
| LGTV Companion | Remote streaming end mode | Display off |
| LGTV Companion | Updates | Off, or notify (checks this fork) |
| Sunshine / Apollo | `global_prep_cmd` | Run `"C:\Program Files\LGTV Companion\LGTV Companion.exe" -streaming_connect` on start and `-streaming_disconnect` on undo |
| LG TV | Quick Start+ | On |
| LG TV | TV On With Mobile | On, so LGTV Companion can control the TV over the network |

## Clean Windows install

1. Download the latest `LGTV.Companion.Setup.v<version>.x64.msi` from the [releases page](https://github.com/breferrari/LGTVCompanion/releases) and install it.
2. Open LGTV Companion, add the TV and pair it (accept the prompt on the TV).
3. Apply the [recommended settings](#recommended-settings).
4. Install Sunshine or Apollo and add the `global_prep_cmd` above.

Installing a fork release over an upstream install, or over another fork build, upgrades it in place and keeps the configuration.

## Syncing with upstream

The fork keeps its own values out of upstream files, so upstream changes merge cleanly:

- `Directory.Build.props` holds the version (`ForkVersion`). `Directory.Build.targets` passes it to the C++ projects and the installer.
- `Common/fork_define.h` overrides the version and the release links. It is included at the end of `Common/common_app_define.h`.

To merge upstream:

```
git remote add upstream https://github.com/JPersson77/LGTVCompanion.git   # once
git fetch upstream
git merge upstream/master
```

Upstream changes `Version` and `ProductCode` in `LGTV Companion Setup/Product.wxs` with every release, which conflicts with the fork's line. Keep the fork's version of it: `Version="$(var.ForkVersion)"` and no `ProductCode`. Then set `ForkVersion` to the new upstream version with fork build number 1 (upstream 5.8.0 becomes `5.8.0.1`), build, test and release.

## Building and releasing

Prerequisites:

- Visual Studio with the "Desktop development with C++" workload.
- The vcpkg dependencies, as described in [Build how-to](Build%20how-to.md) (x64 is enough).
- The .NET SDK (8 or later), which builds the WiX installer project.

Build the installer:

```
pwsh Scripts\Build-Release.ps1
```

It builds everything and writes `dist\LGTV.Companion.Setup.v<version>.x64.msi`. To release, bump `ForkVersion`, commit, then tag and publish:

```
git tag v5.7.0.1
git push origin master v5.7.0.1
gh release create v5.7.0.1 dist\LGTV.Companion.Setup.v5.7.0.1.x64.msi --title "LGTV Companion 5.7.0.1" --notes "..."
```

The `x64` in the file name matters: the update check downloads the release asset whose name contains it.
