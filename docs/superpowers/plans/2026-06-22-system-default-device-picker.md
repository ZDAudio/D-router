# System Default Device Picker — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> Local plan (not committed to the public repo). Spec:
> `docs/superpowers/specs/2026-06-22-system-default-device-picker-design.md`.

**Goal:** Let the user set the macOS default output/input device straight from the
Audio Setup tab (a stand-in for System Settings → Sound), via a per-direction
dropdown that lists all system devices plus a ★ badge on the current-default strip.

**Architecture:** A new message-thread-only CoreAudio helper `SystemAudioDevices`
(list / getDefault / setDefault for a direction) drives a `ComboBox` added to each
`DeviceVolumePanel`; the panel's existing 8 Hz timer keeps the selection + a strip
★ in sync with the OS. Setting the default is independent of D-Router's own routing.

**Tech Stack:** C++20, JUCE 8 (`juce::ComboBox`, `juce::Label`, `juce::Array`),
CoreAudio HAL (`AudioObjectGetPropertyData`/`SetPropertyData` on
`kAudioObjectSystemObject`), CMake.

**Testing note:** This feature is entirely CoreAudio IPC + JUCE UI — there is **no
headless-testable pure logic**, so (like `DeviceVolume` and the network/UI tasks of
the auto-update feature) it is not unit-tested in `dcorerouter_tests`; each task
builds + commits, and Task 5 is real-device verification. This mirrors the existing
project precedent, not a shortcut.

---

## File map

- **Create** `Source/Engine/SystemAudioDevices.h` — JUCE-free-style API (`AudioDeviceRef`, `SystemAudioDevices`).
- **Create** `Source/Engine/SystemAudioDevices.cpp` — CoreAudio implementation.
- **Modify** `CMakeLists.txt` — add the new `.cpp` to `target_sources(dcorerouter …)`.
- **Modify** `Source/UI/DeviceVolumePanel.h` — `#include`, `Strip` ★ badge + `setIsDefault`, panel combo members + helpers.
- **Modify** `Source/UI/DeviceVolumePanel.cpp` — star badge wiring, combo row (ctor / rebuild / resized), OS↔UI sync, onChange.

Frameworks: CoreAudio + AudioToolbox are already linked to `dcorerouter`
(CMakeLists.txt:106-107) — no link changes needed. `SystemAudioDevices` is used only
by the app target (not the test targets).

---

## Task 0 — Branch

- [ ] **Step 1: Create a feature branch**

```bash
git checkout -b feat/system-default-device-picker
```

---

## Task 1 — `SystemAudioDevices` CoreAudio module

**Files:**
- Create: `Source/Engine/SystemAudioDevices.h`
- Create: `Source/Engine/SystemAudioDevices.cpp`
- Modify: `CMakeLists.txt` (after `Source/Engine/DeviceVolume.cpp`, line ~39)

- [ ] **Step 1: Write the header**

Create `Source/Engine/SystemAudioDevices.h`:

```cpp
#pragma once

#include <juce_core/juce_core.h>

namespace dcr
{

    // A device the OS can use as the system default on a given direction, paired
    // with its CoreAudio AudioDeviceID.  deviceID is kept as unsigned int so this
    // header stays CoreAudio-free (AudioDeviceID is a UInt32; 0 == kAudioObjectUnknown).
    struct AudioDeviceRef
    {
        juce::String name;
        unsigned int deviceID = 0;
    };

    // Read / set the macOS *default* output or input device -- the same setting as
    // System Settings -> Sound -> Output/Input and the menu-bar sound picker.  This
    // is a SYSTEM-WIDE pointer deciding where ordinary apps send/take audio; it is
    // INDEPENDENT of D-Router's own routing (the engine opens devices by explicit
    // ID, so changing this never reroutes the matrix).
    //
    // MESSAGE THREAD ONLY: every call is IPC to coreaudiod and may block briefly.
    // Never call from the matrix thread or a CoreAudio IO callback.
    class SystemAudioDevices
    {
    public:
        enum class Scope { Input,
            Output };

        // All devices carrying streams on this direction (i.e. eligible to be the
        // system default output/input).  Order is the HAL's own device order.
        static juce::Array<AudioDeviceRef> list (Scope);

        // The current system default device for this direction (deviceID 0 / empty
        // name if none, or on failure).
        static AudioDeviceRef getDefault (Scope);

        // Make the device with this AudioDeviceID the system default.  true on
        // success; no-op + false if id is 0 or the HAL rejects it.
        static bool setDefault (Scope, unsigned int deviceID);
    };

} // namespace dcr
```

- [ ] **Step 2: Write the implementation**

Create `Source/Engine/SystemAudioDevices.cpp`:

```cpp
#include "Engine/SystemAudioDevices.h"

#include <CoreAudio/CoreAudio.h>

#include <vector>

namespace dcr
{

    namespace
    {

        AudioObjectPropertyScope caScope (SystemAudioDevices::Scope s) noexcept
        {
            return s == SystemAudioDevices::Scope::Input ? kAudioDevicePropertyScopeInput
                                                         : kAudioDevicePropertyScopeOutput;
        }

        // The system-object selector carrying the default device for this direction.
        AudioObjectPropertySelector defaultSelector (SystemAudioDevices::Scope s) noexcept
        {
            return s == SystemAudioDevices::Scope::Input ? kAudioHardwarePropertyDefaultInputDevice
                                                         : kAudioHardwarePropertyDefaultOutputDevice;
        }

        // Read a device's CoreAudio name (kAudioObjectPropertyName) as a juce::String.
        // (Local copy of the same helper in DeviceVolume.cpp -- kept here so this
        // module is self-contained and the proven DeviceVolume.cpp is untouched.)
        juce::String deviceNameOf (AudioObjectID dev)
        {
            CFStringRef cf = nullptr;
            UInt32 sz = sizeof (cf);
            AudioObjectPropertyAddress addr { kAudioObjectPropertyName,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain };
            if (AudioObjectGetPropertyData (dev, &addr, 0, nullptr, &sz, &cf) != noErr || cf == nullptr)
                return {};
            auto s = juce::String::fromCFString (cf);
            CFRelease (cf);
            return s;
        }

        // Does this device carry at least one stream on the given scope?
        bool hasStreams (AudioObjectID dev, AudioObjectPropertyScope scope)
        {
            AudioObjectPropertyAddress addr { kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain };
            UInt32 sz = 0;
            return AudioObjectGetPropertyDataSize (dev, &addr, 0, nullptr, &sz) == noErr && sz > 0;
        }

        // Every AudioObjectID the HAL knows about (empty on failure).
        std::vector<AudioObjectID> allDeviceIDs()
        {
            AudioObjectPropertyAddress addr { kAudioHardwarePropertyDevices,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain };
            UInt32 sz = 0;
            if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &sz) != noErr || sz == 0)
                return {};
            std::vector<AudioObjectID> ids (sz / sizeof (AudioObjectID));
            if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &sz, ids.data()) != noErr)
                return {};
            return ids;
        }

    } // namespace

    juce::Array<AudioDeviceRef> SystemAudioDevices::list (Scope scope)
    {
        const auto sc = caScope (scope);
        juce::Array<AudioDeviceRef> out;
        for (auto id : allDeviceIDs())
            if (hasStreams (id, sc))
                out.add ({ deviceNameOf (id), (unsigned int) id });
        return out;
    }

    AudioDeviceRef SystemAudioDevices::getDefault (Scope scope)
    {
        AudioObjectPropertyAddress addr { defaultSelector (scope),
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        AudioDeviceID dev = kAudioObjectUnknown;
        UInt32 sz = sizeof (dev);
        if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &dev) != noErr
            || dev == kAudioObjectUnknown)
            return {};
        return { deviceNameOf (dev), (unsigned int) dev };
    }

    bool SystemAudioDevices::setDefault (Scope scope, unsigned int deviceID)
    {
        if (deviceID == 0)
            return false;
        AudioObjectPropertyAddress addr { defaultSelector (scope),
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        AudioDeviceID dev = (AudioDeviceID) deviceID;
        return AudioObjectSetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, sizeof (dev), &dev) == noErr;
    }

} // namespace dcr
```

- [ ] **Step 3: Add the source to CMake**

In `CMakeLists.txt`, in the `target_sources(dcorerouter PRIVATE …)` block, add the
new file right after the `DeviceVolume.cpp` line:

```cmake
    Source/Engine/DeviceVolume.cpp
    Source/Engine/SystemAudioDevices.cpp
    Source/Engine/ReconfigurationController.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build build -j`
Expected: compiles clean (module is unused so far — no warnings about it).

- [ ] **Step 5: Commit**

```bash
git add Source/Engine/SystemAudioDevices.h Source/Engine/SystemAudioDevices.cpp CMakeLists.txt
git commit -m "feat: add SystemAudioDevices CoreAudio helper (default device get/set/list)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2 — `Strip` ★ default badge

**Files:**
- Modify: `Source/UI/DeviceVolumePanel.h` (the `Strip` struct)
- Modify: `Source/UI/DeviceVolumePanel.cpp` (Strip ctor + `resized`)

- [ ] **Step 1: Declare the badge + setter on `Strip`**

In `Source/UI/DeviceVolumePanel.h`, inside `struct Strip`, add the method after
`void applyEnabledLook();` and a member after `juce::Label naLabel;`:

```cpp
            Strip (const juce::String& deviceName, DeviceVolume::Scope scope);
            void resized() override;
            void pull(); // OS -> UI (skips the fader while dragging)
            void applyEnabledLook();
            void setIsDefault (bool isDefault); // show/hide the system-default ★

            DeviceVolume vol;
            juce::Label nameLabel;
            juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
            juce::Label dbLabel; // dB readout (matches Audio MIDI Setup), 2 dp
            juce::TextButton mute { "M" };
            juce::Label naLabel; // "N/A" overlay when no controllable volume
            juce::Label starLabel; // "★" badge: this device is the system default
            bool dragging = false;
            bool lastMuted = false;
```

- [ ] **Step 2: Create the badge in the `Strip` ctor**

In `Source/UI/DeviceVolumePanel.cpp`, in the `Strip` constructor, after the
`naLabel` block (just before `applyEnabledLook();`), add:

```cpp
        naLabel.setText ("N/A", juce::dontSendNotification);
        naLabel.setJustificationType (juce::Justification::centred);
        naLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (140, 140, 145));
        naLabel.setTooltip ("macOS exposes no volume control for this device");
        addChildComponent (naLabel);

        // Gold ★ in the top-left corner marking the current system default device on
        // this direction.  Hidden until the panel calls setIsDefault(true); purely
        // indicative, so it never intercepts clicks.
        starLabel.setText (juce::String::fromUTF8 ("\xe2\x98\x85"), juce::dontSendNotification);
        starLabel.setJustificationType (juce::Justification::centred);
        starLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (240, 200, 90));
        starLabel.setFont (juce::Font (juce::FontOptions (14.0f)));
        starLabel.setInterceptsMouseClicks (false, false);
        starLabel.setTooltip (scope == DeviceVolume::Scope::Output ? "System default output device"
                                                                   : "System default input device");
        addChildComponent (starLabel); // hidden until setIsDefault(true)

        applyEnabledLook();
        pull();
```

- [ ] **Step 3: Implement `setIsDefault`**

In `Source/UI/DeviceVolumePanel.cpp`, after the `Strip::applyEnabledLook()` method,
add:

```cpp
    void DeviceVolumePanel::Strip::setIsDefault (bool isDefault)
    {
        starLabel.setVisible (isDefault);
    }
```

- [ ] **Step 4: Position the badge in `Strip::resized`**

In `Source/UI/DeviceVolumePanel.cpp`, replace the body of `Strip::resized()`:

```cpp
    void DeviceVolumePanel::Strip::resized()
    {
        auto r = getLocalBounds().reduced (4);
        starLabel.setBounds (r.getX(), r.getY(), 16, 16); // top-left corner badge
        nameLabel.setBounds (r.removeFromTop (30));
        mute.setBounds (r.removeFromBottom (22).reduced (10, 0));
        r.removeFromBottom (2);
        dbLabel.setBounds (r.removeFromBottom (15));
        r.removeFromBottom (2);
        fader.setBounds (r);
        naLabel.setBounds (r); // overlay centred on the (disabled) fader area
    }
```

- [ ] **Step 5: Build**

Run: `cmake --build build -j`
Expected: compiles clean (badge created but never shown yet).

- [ ] **Step 6: Commit**

```bash
git add Source/UI/DeviceVolumePanel.h Source/UI/DeviceVolumePanel.cpp
git commit -m "feat: add system-default star badge to device strips

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3 — Panel combo: populate + track OS (read-only)

**Files:**
- Modify: `Source/UI/DeviceVolumePanel.h` (include + panel members/helpers)
- Modify: `Source/UI/DeviceVolumePanel.cpp` (ctor, rebuild, resized, sync helpers, timer)

- [ ] **Step 1: Include the helper + declare panel members/methods**

In `Source/UI/DeviceVolumePanel.h`, add the include near the top (after the
`DeviceVolume.h` include):

```cpp
#include "Engine/AudioEngine.h"
#include "Engine/DeviceVolume.h"
#include "Engine/SystemAudioDevices.h"
```

Then in the panel's `private:` section, after `void timerCallback() override;`, add
the helper declarations:

```cpp
        void timerCallback() override;

        // Direction -> SystemAudioDevices::Scope.
        SystemAudioDevices::Scope defaultScope() const noexcept;
        // OS -> UI: set the combo selection (by AudioDeviceID) and the strip ★ from
        // the current system default.  Skips the combo while its popup is open.
        void syncDefaultToOS();
        // UI -> OS: apply the combo's selection as the new system default.
        void applyDefaultSelection();
```

And after the `juce::Label title;` member, add the combo row members:

```cpp
        juce::Label title;
        juce::Label defaultLabel; // "System Output" / "System Input"
        juce::ComboBox defaultCombo; // pick the macOS default device for this direction
        juce::Array<AudioDeviceRef> defaultDevices; // parallel to combo items (id = index + 1)
```

- [ ] **Step 2: Wire the combo row in the panel ctor**

In `Source/UI/DeviceVolumePanel.cpp`, in the `DeviceVolumePanel` constructor, after
the `title` setup block (`addAndMakeVisible (title);`) and before the `viewport`
setup, add:

```cpp
        addAndMakeVisible (title);

        // System-default device picker for this direction: lists ALL devices on this
        // direction (not just the routed strips below) and sets the macOS default
        // when changed -- a stand-in for System Settings > Sound.  Independent of
        // D-Router's own routing.
        defaultLabel.setText (dir == Direction::Inputs ? "System Input" : "System Output",
            juce::dontSendNotification);
        defaultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        defaultLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
        addAndMakeVisible (defaultLabel);

        defaultCombo.setTextWhenNothingSelected (juce::String::fromUTF8 ("\xe2\x80\x94")); // em dash
        defaultCombo.setTextWhenNoChoicesAvailable ("No devices");
        defaultCombo.setTooltip (dir == Direction::Inputs
                ? "Set the macOS default input device (System Settings > Sound > Input)"
                : "Set the macOS default output device (System Settings > Sound > Output)");
        defaultCombo.onChange = [this] { applyDefaultSelection(); };
        addAndMakeVisible (defaultCombo);
```

- [ ] **Step 3: Add the scope helper + sync (read-only) + populate in rebuild**

In `Source/UI/DeviceVolumePanel.cpp`, implement the scope helper just before
`DeviceVolumePanel::rebuild()`:

```cpp
    SystemAudioDevices::Scope DeviceVolumePanel::defaultScope() const noexcept
    {
        return direction == Direction::Inputs ? SystemAudioDevices::Scope::Input
                                              : SystemAudioDevices::Scope::Output;
    }
```

Then at the end of `DeviceVolumePanel::rebuild()`, after `emptyLabel.setVisible (...)`
and before `resized();`, populate the combo:

```cpp
        emptyLabel.setVisible (strips.isEmpty());

        // (Re)populate the system-default picker with ALL devices on this direction.
        // Done only here (tab open / device change) -- the timer never rebuilds the
        // list, only the selection + ★.
        defaultDevices = SystemAudioDevices::list (defaultScope());
        defaultCombo.clear (juce::dontSendNotification);
        for (int i = 0; i < defaultDevices.size(); ++i)
            defaultCombo.addItem (defaultDevices.getReference (i).name, i + 1); // item-id 0 is reserved
        syncDefaultToOS();

        resized();
```

Then implement `syncDefaultToOS()` after `rebuild()`:

```cpp
    void DeviceVolumePanel::syncDefaultToOS()
    {
        const auto def = SystemAudioDevices::getDefault (defaultScope());

        // Reflect the OS default in the combo (matched by AudioDeviceID, not name, so
        // same-named devices don't collide).  Don't fight an open popup.
        if (! defaultCombo.isPopupActive())
        {
            int wantId = 0; // 0 == nothing selected
            if (def.deviceID != 0)
                for (int i = 0; i < defaultDevices.size(); ++i)
                    if (defaultDevices.getReference (i).deviceID == def.deviceID)
                    {
                        wantId = i + 1;
                        break;
                    }
            if (defaultCombo.getSelectedId() != wantId)
                defaultCombo.setSelectedId (wantId, juce::dontSendNotification);
        }

        // Light the ★ on whichever routed strip is the default device (by name --
        // consistent with D-Router's name-based device model; if the default isn't a
        // routed device, no strip lights and the combo alone shows it).
        for (auto* s : strips)
            s->setIsDefault (def.deviceID != 0 && s->vol.getDeviceName() == def.name);
    }
```

- [ ] **Step 4: Stub `applyDefaultSelection` (wired for real in Task 4)**

In `Source/UI/DeviceVolumePanel.cpp`, after `syncDefaultToOS()`, add a temporary
no-op so the ctor's `onChange` reference links (replaced in Task 4):

```cpp
    void DeviceVolumePanel::applyDefaultSelection()
    {
        // Wired in Task 4.
    }
```

- [ ] **Step 5: Call sync from the timer**

In `Source/UI/DeviceVolumePanel.cpp`, update `timerCallback()`:

```cpp
    void DeviceVolumePanel::timerCallback()
    {
        for (auto* s : strips)
            s->pull();
        syncDefaultToOS(); // track external default-device changes + keep ★ correct
    }
```

- [ ] **Step 6: Lay out the combo row in `resized`**

In `Source/UI/DeviceVolumePanel.cpp`, in `DeviceVolumePanel::resized()`, replace the
top of the method (the title + the lines setting `emptyLabel`/`viewport` bounds) with
a version that carves out a combo row between the title and the viewport:

```cpp
    void DeviceVolumePanel::resized()
    {
        auto r = getLocalBounds().reduced (8);
        title.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);

        // System-default device picker row: label on the left, combo filling the rest.
        auto comboRow = r.removeFromTop (24);
        defaultLabel.setBounds (comboRow.removeFromLeft (90));
        comboRow.removeFromLeft (4);
        defaultCombo.setBounds (comboRow.removeFromLeft (juce::jmin (260, comboRow.getWidth())));
        r.removeFromTop (6);

        emptyLabel.setBounds (r);
        viewport.setBounds (r);

        const int h = viewport.getMaximumVisibleHeight();
        int x = 0;
        for (auto* s : strips)
        {
            s->setBounds (x, 0, kStripWidth, h);
            x += kStripWidth + kStripGap;
        }
        stripsHolder.setSize (juce::jmax (x, r.getWidth()), h);
    }
```

- [ ] **Step 7: Build**

Run: `cmake --build build -j`
Expected: compiles clean.

- [ ] **Step 8: Commit**

```bash
git add Source/UI/DeviceVolumePanel.h Source/UI/DeviceVolumePanel.cpp
git commit -m "feat: show system-default device picker in Audio Setup (read-only, tracks OS)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4 — Make the combo set the default

**Files:**
- Modify: `Source/UI/DeviceVolumePanel.cpp` (`applyDefaultSelection`)

- [ ] **Step 1: Implement `applyDefaultSelection`**

In `Source/UI/DeviceVolumePanel.cpp`, replace the Task-3 stub body of
`applyDefaultSelection()`:

```cpp
    void DeviceVolumePanel::applyDefaultSelection()
    {
        const int id = defaultCombo.getSelectedId();
        if (id <= 0 || id > defaultDevices.size())
            return; // programmatic clear / nothing selected

        const auto& dev = defaultDevices.getReference (id - 1);
        // setDefault failing (device just vanished?) leaves syncDefaultToOS to snap
        // the selection + ★ back to the OS's actual default on the next pass.
        SystemAudioDevices::setDefault (defaultScope(), dev.deviceID);
        syncDefaultToOS();
    }
```

Note: programmatic selection changes in `syncDefaultToOS()` use
`juce::dontSendNotification`, so `onChange` (→ `applyDefaultSelection`) fires **only**
on real user interaction — no feedback loop.

- [ ] **Step 2: Build**

Run: `cmake --build build -j`
Expected: compiles clean.

- [ ] **Step 3: Commit**

```bash
git add Source/UI/DeviceVolumePanel.cpp
git commit -m "feat: set macOS default device from the Audio Setup picker

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5 — Format, full build, real-device verification

**Files:** none (verification + formatting only)

- [ ] **Step 1: clang-format the touched files (CI fails otherwise)**

Run:
```bash
clang-format -i Source/Engine/SystemAudioDevices.h Source/Engine/SystemAudioDevices.cpp \
  Source/UI/DeviceVolumePanel.h Source/UI/DeviceVolumePanel.cpp
git diff --name-only   # confirm whether formatting changed anything
```
If anything changed, rebuild and commit:
```bash
cmake --build build -j
git commit -am "style: clang-format system default device picker

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 2: Clean build**

Run: `cmake --build build -j`
Expected: links `build/dcorerouter_artefacts/Release/D-Router.app`, no warnings on
the new code.

- [ ] **Step 3: Run the app**

Run: `./run.sh`
Then open the **AUDIO SETUP** tab.

- [ ] **Step 4: Manual verification (real devices — record results honestly)**

Verify and report PASS/FAIL for each:
1. **Output picker present & correct default:** "System Output" dropdown lists your
   Mac's output devices; the currently-selected one matches System Settings → Sound →
   Output; if that device is also a routed strip, it shows the ★.
2. **Set output:** pick a different output device → the macOS menu-bar/Sound setting
   follows and system audio actually moves to it; the ★ moves (or disappears if the
   new default isn't a routed strip).
3. **External change tracked:** change the output from the menu bar → within ~⅛ s the
   dropdown selection + ★ update to match.
4. **Input mirror:** repeat 1–3 with the "System Input" dropdown.
5. **Routing independence:** confirm changing the system default does **not** change
   D-Router's own matrix routing / which devices the engine has open.
6. **Open-popup not stolen:** open the dropdown and hover a moment — the timer must
   not close or reset it.

- [ ] **Step 5: Open a PR**

```bash
git push -u origin feat/system-default-device-picker
gh pr create --title "Audio Setup: pick the macOS default output/input device" \
  --body "Adds a per-direction 'System Output'/'System Input' dropdown to the Audio Setup tab that lists all system devices and sets the macOS default device, plus a ★ on the current-default strip. Tracks external changes via the existing poll timer. Independent of D-Router's own routing. macOS/CoreAudio; no headless-testable logic so verified on real devices.

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

---

## Self-review

- **Spec coverage:** output + input dropdowns (T3 ctor, dir-based label/scope) ✓;
  lists ALL devices on direction (`SystemAudioDevices::list`, T1) ✓; sets macOS
  default (`setDefault`, T1/T4) ✓; ★ on current-default strip (T2 + `syncDefaultToOS`
  T3) ✓; ★ absent when default not routed (name match in `syncDefaultToOS`) ✓; tracks
  external changes via 8 Hz timer (T3 step 5) ✓; popup not stolen (`isPopupActive`
  guard, T3) ✓; list rebuilt only in `rebuild()` (T3) ✓; routing independence (comment
  in header T1 + verify T5.5) ✓; message-thread-only / no RT (header doc T1) ✓;
  edge cases — set fail revert (T4), nothing-selected guard (T4), no devices
  (`setTextWhenNoChoicesAvailable` T3) ✓; clang-format (T5) ✓.
- **Placeholder scan:** the Task-3 `applyDefaultSelection` no-op is explicitly a
  temporary stub replaced in Task 4 (stated) — not a gap. No TBD/TODO/"handle errors"
  hand-waves remain.
- **Type consistency:** `AudioDeviceRef{name, deviceID}` and
  `SystemAudioDevices::{Scope, list, getDefault, setDefault}` (T1) are used with those
  exact names/signatures in T3/T4; `defaultScope()`, `syncDefaultToOS()`,
  `applyDefaultSelection()`, `setIsDefault()` declared (T2/T3 headers) match their
  definitions; combo item-id convention (index+1) consistent across populate (T3) and
  read (T3 sync / T4 apply).
```
