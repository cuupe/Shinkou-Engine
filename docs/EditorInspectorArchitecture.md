# Typed inspector and native panels

The native editor uses Dear ImGui Docking and its official Win32/DX11 platform
windows. Drag a tab out of the main window to create a separate native window;
drag its title back onto a docking target to reattach it. Layouts, including
floating positions, are saved by ImGui. Dock tabs hide their close/menu buttons;
the toolbar's Panels menu controls visibility. A detached OS window retains its
normal close action. Full world viewport rendering and detached windows are
currently verified on DX11.

## Property contracts

`PropertyDescriptor` keeps its native getter/setter and serialized type. Optional
`editorGet`/`editorSet` expose a presentation value, such as a Vec3 of degrees for
a quaternion. `PropertyPresentation` describes that conversion. The UI model
retains `PropertyValue`, range/step, choices and component identity; formatted
text is only a diagnostic/retained compatibility projection.

The native adapter selects controls from the variant: booleans, signed/unsigned
integers, numbers, choices, strings, and separate vector axes. Axes have labels,
color accents, numeric input, drag adjustment and reset. Narrow panels stack
axes vertically. Components have independent collapsible sections. Build profile
rows use the build document's setter rather than a scene property binding.

`EditorInspectorController.cpp` resolves a binding against the selected owner
and component, validates the variant and finite numbers, and owns edit sessions.
Activation captures the original typed value, live updates do not add history,
and deactivation commits one undo entry. Escape restores the original value. Commands
and selection changes finish the previous session before changing its target.
Property history validates the live object identity and addresses components by
type. Explicit scene restore remaps identities through document preorder, so
structural undo can rebuild IDs without invalidating older property entries.
External destruction cannot redirect an edit to a replacement object.
It also permits editing ECS-owned rendering objects without attempting
to serialize GPU handles. Full scene saves retain their existing codec checks.

## Transform

Transform exposes local position, Euler XYZ rotation in degrees, and scale.
Runtime rotation and existing scene files remain normalized quaternions. The
XYZ convention is `Rx * Ry * Rz` with column vectors. The inverse uses the same
convention; at gimbal lock it chooses Z = 0. Euler winding entered by the user
is retained while the corresponding quaternion remains unchanged. The scene
format stores orientation, so reload may canonicalize equivalent Euler angles.

Setters reject NaN/infinity and zero-length quaternions atomically. Negative and
zero scale remain supported. World matrix evaluation detects legacy direct
local writes as well as changes to the parent matrix; invalid direct writes
revert to the last evaluated transform. Public `local` remains for source
compatibility; new editor code uses validated setters.

## Icons and validation

Codicons is pinned and bundled with attribution and licenses in
`third_party/codicons`. Its font merges into the editor's DPI atlas; icon buttons
retain text tooltips and fall back to text when the font is unavailable.

`shinkou_transform_inspector_tests` covers mixed rotations, sign-equivalent
quaternions, gimbal lock, parent caches, invalid input, quaternion scene
round-tripping, typed values and gesture-level undo/cancel. Invoke it with
`--benchmark` to time one million scalar Euler conversion pairs separately
from correctness. `Tools/UiCapture/InspectorInteraction.txt` drives real Win32
input for XYZ editing, detachment, edits in secondary windows, redocking,
4K and narrow panels. The capture helper can read the secondary window's actual
DX11, DX12 or Vulkan render target; recorded regions include their owning HWND.
Unattended runs use explicitly marked targeted Win32 messages. In that mode,
hardware mouse-leave notifications are ignored and ImGui's geometry fallback
infers the hovered viewport from those messages. Production uses the official
Win32 hardware cursor and viewport detection. Script pointer records distinguish
requested and observed hardware coordinates; they do not claim SendInput proof.

## Recorded desktop evidence (2026-10-24)

The dark, light and high-contrast scenarios each passed 88 posted Win32
interaction steps (264 total) at actual system DPI 144 (150%). GPU readbacks
cover 1440×900, 3840×2160 and 1024×768 clients. The inspector had a distinct
HWND after detachment and the main HWND again after redocking; typed edits
were checked inside both windows. Closing the detached panel and reopening it
also preserved its values. Evidence is under
`out/qa/inspector-accepted-{dark,light,high-contrast}-20261024`; PNG previews and
machine-readable HWND/value checks are under
`out/deliverables/inspector-20261024`.

DX12 and Vulkan now expose the same editor viewport/model capability seams and
official native platform-window adapters. Each backend additionally passed an
88-step dark inspector scenario, a 51-step world viewport/DPI scenario, and a
27-step detached-world rendering/input scenario. Vulkan synchronization
validation was enabled. Current evidence directories and consolidated previews
are indexed in `docs/EditorRendererBackends.md` and the workspace's
`out/deliverables/render-backends-20261025`.
