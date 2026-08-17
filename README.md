# KEF LSX Windows Drivers

Windows-native tooling for tighter integration between a **KEF LSX** and
Windows' own audio UI, built around
[kefctl](https://github.com/zarif98/kefctl) (a command-line tool for
controlling KEF speakers).

Instead of only being controllable from the command line, this project makes
the speakers show up as a real audio device in Windows' Sound settings &mdash;
including a proper "disconnected" state when they're powered off, and
mute/unmute/volume controls that actually reach the real speaker.

## What's here

* **`tools/AutoOn.cpp`** &mdash; watches the default Windows audio output for
  activity and automatically powers the speakers on whenever playback starts
  after a period of silence. Native C++, no runtime dependencies. Runs
  invisibly in the background at logon.

* **`tools/KefEndpointBridge.cpp`** &mdash; polls a virtual audio endpoint's
  mute/volume state and mirrors it to the real speakers via `kefctl`:
  unmuting powers the speakers on, muting powers them off, moving the volume
  slider sets the real volume.

* **`.github/workflows/release.yml`** &mdash; builds both tools, and (best
  effort) builds a test-signed virtual audio driver package from Microsoft's
  own [SysVAD sample](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad),
  attaching everything to a GitHub Release on tag push.

Both tools expect `kefctl` on `PATH` or one directory up, and use the same
`~/.kefctl` config file for the speaker host as the main project.

## The virtual audio device

To get a real, native-feeling entry in Windows' sound settings (rather than
only ever being controllable via the command line), the speakers are backed
by a virtual audio device built from Microsoft's own SysVAD sample driver,
unmodified &mdash; there's no original driver code here, just the sample
compiled and installed locally. Enabling/disabling that device's PnP state
is what produces the native "disconnected" indicator; `KefEndpointBridge`
watches its mute/volume for the interactive controls.

### Why you need to disable Secure Boot to use it

The driver in this repo's releases is **test-signed**, not properly signed
by Microsoft. Windows will not load an unsigned/test-signed kernel driver
while Secure Boot is enabled &mdash; this is enforced at the boot chain
level and cannot be bypassed by an administrator account, a driver install
dialog, or any other in-Windows override. It's not a permissions problem;
it's Secure Boot doing exactly what it's designed to do.

To install the driver, you need to, in order:

1. Reboot into UEFI/BIOS firmware settings and disable Secure Boot (this is
   a firmware-level setting; there's no way to do it from within Windows).
2. Disable Hypervisor-enforced Code Integrity (HVCI) specifically &mdash;
   *not* the hypervisor itself, which Hyper-V and other features still need:

   ```powershell
   New-Item -Path "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" -Force
   Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" -Name "Enabled" -Value 0 -Type DWord
   Set-ItemProperty -Path "HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceGuard" -Name "HypervisorEnforcedCodeIntegrity" -Value 0 -Type DWord
   ```
3. Enable test-signing mode and reboot:

   ```powershell
   bcdedit /set testsigning on
   ```

Disabling only HVCI (rather than the whole hypervisor) is the version of
this that's actually stable to run &mdash; killing the hypervisor entirely
was tried first during development and caused an inconsistent, eventually
crashing configuration. This is a real, if narrow, reduction in your
machine's security posture for as long as it's active: test-signing mode
allows *any* test-signed or unsigned driver to load, not just this one.

The alternative is getting the driver properly signed through Microsoft's
[attestation signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)
process, which requires an EV code-signing certificate (~$300&ndash;500/yr)
and a formal Hardware Developer Program registration &mdash; a real option,
just not a casual one for a project like this.

## Installing

Grab the latest release's zip files (`windows-tools.zip` and, if that CI job
succeeded, `sysvad-driver-package.zip`) from the
[Releases page](https://github.com/zarif98/kef-lsx-windows-drivers/releases).

1. Extract `windows-tools.zip` somewhere alongside a checkout of
   [kefctl](https://github.com/zarif98/kefctl) (the tools expect `kefctl` to
   be one directory up, or on `PATH`).
2. If you want the virtual device too: follow the Secure Boot / HVCI /
   test-signing steps above, then install the driver package with `devcon`
   (included in the WDK) or `pnputil`.
3. Run `AutoOn.exe` and/or `KefEndpointBridge.exe`, or set them up to launch
   at logon via a shortcut in your Startup folder.

## Copyright and License

The SysVAD sample this project's virtual device is built from is
Copyright (C) Microsoft Corporation, MIT licensed.

`AutoOn.cpp` and `KefEndpointBridge.cpp` are original to this project.
