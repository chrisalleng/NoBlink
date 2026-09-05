# Appearance and camera regression evidence

The fixes were validated interactively on Ashita 4.30. The user confirmed complete geometry for
large/small hat removal and shorter boots, correct shield/two-handed animations, simultaneous
lockstyle updates, preserved targets, no blink, and stable lock-on camera during self gear changes.

## Base-mesh coverage

The decisive paired captures used the same appearance:
`0007,1000,21A8,30A7,40A7,50A7,60B2,7000,8039`.
Mesh inputs and primitive/material command hashes matched. The broken in-place actor retained
model-manager `A9..B0` bytes `01 01 01 00 01 00 00 00`; stock reconstruction had
`00 00 00 00 01 00 00 00`.

The native renderer (`FUN_01a6db40`, preferred image base `0x1A40000`) latches coverage flags from
resource byte `+0x33`. A material selects a byte at manager `+0xA8+selector`; a nonzero byte suppresses
its triangle-list/strip draws. For `hf_h`, selectors `[1,3,8]` reduced stock draw ranges
`[(0,102),(306,2),(312,15)]` to `[(312,15)]`. For `hh_h`, selectors `[3,4,4,3,8]` removed exactly
the first and fourth stock ranges. Pairs are `(startIndex, primitiveCount)`, with repeated effect
passes deduplicated for this comparison. Applying the retained flags predicts every omitted range.

Rebuilding only the eight known flags from the surviving renderable simple resources restores
exposed geometry while retaining coverage needed by current equipment. The complete chain is
validated before publication; selector zero and adjacent fields are preserved.

Native evidence is in the [FFXi RE corpus](https://github.com/chrisalleng/ffxi-web-client), bundles
`du1-93cd344a392bcc59` (renderer) and `du1-9fc50a8e02040330` (manager initialization). A local,
schema-validated full-replacement amendment `da1-b0df2722db740d9d` preserves the suppression
analysis, tagged paired captures and replay; publication to that separate corpus is pending.

## Camera pose continuity

The existing corpus bundle `du1-617977e089d42a62` establishes that `FUN_01a69d80` invokes
`FUN_01a741d0`, allocating and initializing identity matrices on binding. Preserving the model
object alone therefore does not preserve attachment positions. The camera consumes actor anchors
(the camera audit and `du1-0e3a13cf9df9d742`).

A production-path fixture reproducing that initialization fails pose continuity against version
3.1 and passes with version 3.2. The fix copies the prior finite pose into the current matrix buffer
only for an identical model, descriptor and bone count. It preserves native binding, allocation and
bone-mask ownership. The user subsequently confirmed that the brief floor-directed camera jerk
was resolved. Camera and lock-on state are not rewritten.

## Automated checks and limits

`NoBlinkPolicyTest.cpp` checks eligibility, readiness, resource retention and precise actor-field
write boundaries. `NoBlinkRuntimeTest.cpp` exercises the production functions with controlled
native-loader fixtures: immediate/delayed resources, incomplete masks, coalescing, timeout, stale
actors, coverage reductions, malformed/cyclic chains, same-skeleton pose continuity, and rejection
of mismatched/invalid poses. Win32 builds and an isolated Wine loader probe verified version 3.2,
interface 4.30 and the required plugin exports.

Fixtures do not reproduce the full game renderer, camera collision system or asynchronous native
resource manager. The in-game acceptance above supplies that evidence. The underlying repeated
native binding allocation/retain behavior remains a follow-up ownership-audit concern; these fixes
preserve native ownership and do not claim to resolve that separate lifetime question.
