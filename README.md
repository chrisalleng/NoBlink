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

Suppression is **off by default** — loading the plugin does not change stock behaviour until you
turn it on.

## Installation

1. Open the [Releases](https://github.com/chrisalleng/NoBlink/releases) page.
2. Download `NoBlink.dll` from the latest release's **Assets** section.
3. Drop it into your Ashita `plugins` directory.
4. In your Ashita boot script, add `/load NoBlink`.
5. Turn it on with `/noblink on`.

Win32 only — an x64 build will not load.

## Commands

```
/noblink                  # status: suppression, rebinds, held looks, parked resources
/noblink on|off           # suppression (off by default) -- this is the feature
/noblink watch <name>     # sample that character every frame
/noblink watch <index>    # same, by raw entity index
/noblink watch target     # same, latching whatever is targeted right now
/noblink watch off        # stop watching
/noblink res              # the watched character's attached-resource chain
/noblink dump             # per-frame trace, only the entity/actor words that moved
/noblink callers          # every call site that destroyed a live actor
/noblink reset            # zero counters and restart the trace
```

Everything except `on`/`off` is diagnostic. `watch` takes a character **name** by preference:
entity indices are assigned by login order and change between sessions, and watching the wrong one
still produces numbers that all look plausible.

## How it works

Two things have to be right, and both took some finding.

**Release, and its ordering.** The presentation loader only ever *appends* resources, so rebinding
in place without releasing the old look leaves the character wearing both garments at once.
Releasing mirrors what the client's own teardown does — the model-list owner at `actor+0x674`,
chain root at `+0x44`, freed through the root's virtual deleting destructor at `+0x18(1)`. But
ordering matters more than the release itself: most resources are shared across a body swap, and
releasing one drops its last reference, so clearing the chain first evicts resources that are about
to be re-requested. The new look is attached first, and each old node unlinked afterwards.

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
environment. Both paths are `build.ps1` parameters if yours differ from the defaults. Ashita needs
all three exports in `exports.def`; omitting `expDestroyPlugin` fails with a misleading "missing
required exports".

## License

Licensed under the [GNU General Public License v3.0 only](LICENSE).
