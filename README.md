# NoBlink

An [Ashita v4](https://www.ashitaxi.com/) plugin for the Final Fantasy XI client that keeps
characters visible, and targetable, while they change visible equipment.

When someone changes a piece of visible gear, the client destroys their entire presentation and
rebuilds it from scratch. They wink out of existence for the rebuild, and **anyone targeting them
loses their target**. It is not lag, and it is not proportional to how much gear changed — the
client simply has no in-place path for a gear change. Its only load path is the actor constructor,
where the resource chain starts empty, so a change means destroy and rebuild.

NoBlink skips that teardown and rebinds the presentation on the live actor instead. The actor is
never destroyed, so there is nothing to blink and nothing to lose a target on.

After the rebind completes, NoBlink also invalidates the live actor's cached locomotion clip. The
stock rebuild normally does this by constructing a new actor; without it, a weapon-style change
such as sword-and-shield to a two-handed weapon can keep using the old shield run animation.

Lockstyle and other multi-slot appearance changes are serialized through the same safe one-slot
path. NoBlink compares incoming appearance packets with the already-rendered entity before the
client applies them, lets one changed model through, and queues the final look. Each remaining model
is applied only after the prior rebind and any deferred resource loads have completed. This works
for the local character and remote players, including lockstyle changes that do not originate from
a local command, without handing the update back to the stock blinking teardown.

## Installation

1. Open the [Releases](https://github.com/chrisalleng/NoBlink/releases) page.
2. Download `NoBlink.dll` from the latest release's **Assets** section.
3. Drop it into your Ashita `plugins` directory.
4. In your Ashita boot script, add `/load NoBlink`.

It is active as soon as it loads, for you and for everyone around you.

Win32 only — an x64 build will not load.

## Commands

```
/noblink                  # print the commands and the current setting
/noblink on|off           # the whole plugin
/noblink self [on|off]    # apply it to your own character (omit to toggle)
/noblink others [on|off]  # apply it to everyone else (omit to toggle)
```

All three default to **on**. Turning `self` or `others` off leaves that half of the game behaving
exactly as it does without the plugin, which makes it easy to see the difference side by side.

## How it works

Two things have to be right, and both took some finding.

**Release, and its ordering.** The presentation loader expects an empty resource chain, but freeing
the old look first evicts shared resources that are about to be requested again. NoBlink detaches
the old chain without destroying it, runs the loader against the empty head, and then appends the
still-live old nodes behind the replacement. That empty view is important when an equipped item has
no visual model: it forces the loader to attach the unchanged base geometry, such as a character's
face and hair, rather than relying on the copy in the old chain. Once the replacement is complete,
the old nodes are unlinked and freed through their virtual deleting destructor at `+0x18(1)`.

**Knowing when the new look is complete.** This is the subtle half. For any character who is not
you, `FUN_01b141f0` does not do the work at all — it allocates a task, parks it at `actor+0xa08`,
and returns. The model rebuild, every attach, and every deferred-resource registration happen later,
inside that task. And when the task does run, any look slot whose resource is not already resident
attaches *nothing*: it registers a completion callback and moves on, so that body part has no node
at all until the load lands, typically some 800ms later.

So completion needs both halves. `actor+0xa08` going null says the task has run — necessary, since
until then nothing is attached — and at that moment the nodes it bound synchronously plus the count
it parked is exactly how many replacements must exist. Arrival against that target, counted by node
identity, is what finally releases the old look.

Releasing on either half alone puts the character on screen without a body part. Measured per frame,
resource count across one body swap:

```
releasing when the loader call returns     11 -> 0 -> 7 -> (825ms) -> 8 -> 9
releasing when the task's parked
  resources have actually arrived          11 -> 19 -> (823ms) -> 9
```

The `0` is a frame wearing nothing; the `7` is 825ms with a missing chestpiece. The second line has
neither, and never drops below the settled count. The `19` is both looks attached at once while the
outstanding resource loads — the deliberate trade, and the reason a body part stays on screen.

## Addresses

None are hardcoded. Every function is found at runtime by a byte signature that occurs exactly once
in the image, relative call displacements wildcarded. The PC actor vtable and the resource-link
arena are read out of relocated immediates rather than baked in, so both follow ASLR. Initialization
verifies every resolved address lies inside the loaded `FFXiMain.dll` and refuses to hook otherwise.
All hooks restore their original bytes on unload.

This means the plugin is tied to the retail client build the signatures were taken against. If the
client updates and a signature stops matching, it logs the failure and declines to hook rather than
patching something it does not recognise.

## Building

```powershell
.\build.ps1            # build NoBlink.dll here
.\build.ps1 -Install   # ...and copy it into the Ashita plugins directory
```

Requires the Ashita v4 SDK and a Win32 MSVC toolchain — build from `vcvars32.bat`, not the x64
environment. Both paths are `build.ps1` parameters if yours differ from the defaults. The build
runs the appearance-policy regression checks before compiling the plugin. Ashita needs all three
exports in `exports.def`; omitting `expDestroyPlugin` fails with a misleading "missing required
exports".

## License

Licensed under the [GNU General Public License v3.0 only](LICENSE).
