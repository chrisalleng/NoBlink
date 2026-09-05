# NoBlink

An [Ashita v4](https://www.ashitaxi.com/) plugin for Final Fantasy XI that keeps player characters
visible and targetable while they change equipment. Win32, tested with Ashita 4.30.

Version **3.2** restores exposed scalp and leg geometry when removing larger equipment, refreshes
weapon animations, handles simultaneous lockstyle changes, and preserves the camera position during
gear swaps while locked onto a target.

## Installation

Build `NoBlink.dll` or download an available build from [Releases](https://github.com/chrisalleng/NoBlink/releases).
Place it in Ashita's `plugins` directory, then use `/load NoBlink`. Unload an already loaded version
before loading the replacement. The plugin enables itself for your character and other players.

## Commands

```
/noblink                  # show settings and commands
/noblink on|off           # enable or disable the plugin
/noblink self [on|off]     # apply to your character; omit value to toggle
/noblink others [on|off]   # apply to other players; omit value to toggle
```

## How it works

The stock gear-change path destroys and reconstructs the primary actor. NoBlink intercepts eligible
player appearance changes and keeps that actor alive. It loads replacement resources against an
empty look chain while retaining the old resources until replacement is ready. Readiness requires
the expected resource count, all nine appearance completion bits, and no pending presentation task.
A final resident-resource pass handles arrival order; newer appearances are coalesced during loading.
Multi-slot updates are serialized, including lockstyle changes.

After retiring the old resources, NoBlink rebuilds the eight equipment-coverage flags from the
surviving resources. These flags suppress sections of the base mesh beneath equipment; retaining
flags from a removed hat or tall boots leaves holes in the exposed character. It also invalidates
graphics setup and the cached weapon motion selection.

The native skeleton binder initializes new bone matrices even when the model object survives.
NoBlink carries the current pose across a rebind of the same skeleton, preventing a temporary camera
anchor drop while locked on. Changed skeletons, invalid matrices and mismatched bone counts are
excluded. Actor identity and target state remain intact.

Hooks use checked retail byte signatures and restore their original bytes on unload. Unsupported
signatures cause initialization to fail without leaving hooks installed. Non-player actors and
ineligible appearances retain the stock behavior. A bounded hold prevents an incomplete resource
load from retaining the old look indefinitely.

## Building and testing

Use the x86 Visual Studio developer environment with the Ashita v4 SDK:

```powershell
.\build.ps1 -SdkPath 'C:\ffxi\Ashita-v4beta\plugins\sdk'
.\build.ps1 -SdkPath 'C:\ffxi\Ashita-v4beta\plugins\sdk' -Install
```

The build runs `NoBlinkPolicyTest.cpp` and `NoBlinkRuntimeTest.cpp` before compiling the DLL.
`-Install` stages a separate file and replaces the destination without truncating a mapped DLL.
Installation failure leaves the existing file intact. Reload the plugin to activate a replacement.

The portable policy tests can also run with:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -pedantic NoBlinkPolicyTest.cpp -o /tmp/noblink-policy-test
/tmp/noblink-policy-test
```

The runtime test requires Win32 and the SDK. It exercises production rebind, coverage and pose
logic using controlled resource-loading fixtures. These tests complement in-game acceptance:

- Large and small modeled hats to no hat: complete scalp/hair, no blink or lost target.
- Tall boots to shorter boots: continuous exposed legs.
- Shield to two-handed weapon: correct weapon animation.
- Multiple lockstyle changes: complete appearance without blinking or target loss.
- Own gear swap while locked onto a target: no camera dip into the floor.

All five checks passed in the user's Ashita 4.30 session. Temporary render/camera probes used during
diagnosis are excluded from the release source. See [validation notes](docs/rebind-validation.md)
for the native evidence and test limits. Do not bundle `msvcp140.dll` or `vcruntime140.dll`.

## License

[GNU General Public License v3.0 only](LICENSE).
