---
title: "User Manual"
description: "Reference for the current toggle switch and per-mode knob behavior in the firmware."
draft: false
---

# User Manual

## Toggle Switch Functions

This page describes the current behavior implemented in the firmware.

### Toggle Switch 1

This switch controls how the live input is routed relative to the loop and effect chain.

| Position | Behavior |
| --- | --- |
| **Left** | The live input is routed through the looper and downstream effects. The dry passthrough signal is muted at the mixer output. |
| **Center** | The live input stays dry and is mixed back in after the loop/effect processing. |
| **Right** | Currently behaves the same as **Center** in the firmware. |

> Note: only one side of this toggle has unique behavior in the current code. The opposite outer position is not assigned a separate function yet.

### Toggle Switch 2

This switch controls the pitched harmony voice inside the looper.

| Position | Behavior |
| --- | --- |
| **Left** | The harmony voice plays in reverse. |
| **Center** | The harmony voice plays forward. |
| **Right** | Enables randomized speed variation on the harmony voice. |

The main loop itself continues to run forward at normal speed. This switch is most audible when the harmony voice is mixed in.

### Toggle Switch 3

This switch selects the main operating mode.

| Position | Behavior |
| --- | --- |
| **Left** | **Sampler mode**. The micro-looper switches to sampler behavior. |
| **Center** | **Looper mode**. The micro-looper runs in overdub looper mode. |
| **Right** | **Reverb mode**. The micro-looper is disabled, and both the reverb and pitch shifter are enabled. |

## SAMPLER Mode

In this mode the micro-looper runs in sampler mode. The dedicated reverb and pitch shifter are off, but the signal still passes through the downstream delay, distortion, crusher, and mixer stages.

| Knob | Function | Notes |
| --- | --- | --- |
| **Knob 1** | Sampler input sensitivity | Higher settings make the sampler auto-start more easily from incoming signal level. The overdub fading mapping on this knob is inactive in this mode. |
| **Knob 2** | Stretch attack / fade-in | Sets the fade-in time when stretched sampler playback comes in. The slice control is fixed in sampler mode, so this knob does **not** change loop length here. |
| **Knob 3** | Harmony pitch interval | Controls the looper harmony voice from one octave down, through unison, up to one octave up. This is most useful when the harmony voice is mixed in with Knob 4. |
| **Knob 4** | Harmony mix | Blends the original sampler playback with the harmony voice. Lower settings favor the original playback; higher settings favor the harmony voice. |
| **Knob 5** | Tape-style delay modulation depth | Fully counterclockwise disables the delay block. Any position above minimum turns the modulated delay on and increases the modulation depth. |
| **Knob 6** | Split crusher / distortion control | Left of center lowers the crusher sample rate for more bitcrushing. Right of center increases distortion gain. Around center, both effects are at their mildest settings. |

### Bypass Footswitch

| Gesture | Behavior |
| --- | --- |
| **Single press** | Toggles sampler playback on or off. It does **not** directly start recording in this mode. Recording is triggered by the sampler auto-start logic, which depends on the incoming signal level and the sensitivity set by **Knob 1**. |
| **Double press** | Stops playback. If the sampler is currently recording, the recording is finalized and playback is stopped. |
| **Long press** | No dedicated action in sampler mode in the current firmware. |

### Alternate Footswitch

| Gesture | Behavior |
| --- | --- |
| **Single press** | Toggles stretched sampler playback when a stretched buffer is available. In the current firmware it also toggles the reverb freeze state in the background, even though reverb is not audible in this mode. |
| **Release** | No dedicated release action is routed for the alternate footswitch. |
| **Double press** | Stops stretched playback. The second tap also toggles the hidden reverb freeze state once, and it does **not** also trigger the normal single-press action on tap two. |
| **Long press** | Clears the stretched playback state and stops stretch playback. No dedicated reverb action is routed for the hold gesture. |

## OVERDUB Mode

In this mode the micro-looper runs as an overdub looper. The dedicated reverb and pitch shifter are off, and the downstream delay, distortion, crusher, and mixer stages remain active.

| Knob | Function | Notes |
| --- | --- | --- |
| **Knob 1** | Overdub fading / loop persistence | Controls how much of the existing loop remains when you overdub. Lower settings replace older material more aggressively; higher settings preserve more of the previous loop while layering new audio on top. |
| **Knob 2** | Loop slice length and stretch fade-in | Its main job is setting the micro-loop slice length. The same knob also sets the fade-in time when stretched playback is started from the alternate footswitch. |
| **Knob 3** | Harmony pitch interval | Controls the looper harmony voice from one octave down, through unison, up to one octave up. |
| **Knob 4** | Harmony mix | Blends the original loop playback with the harmony voice. Lower settings favor the original loop; higher settings favor the harmony voice. |
| **Knob 5** | Tape-style delay modulation depth | Fully counterclockwise disables the delay block. Any position above minimum turns the modulated delay on and increases the modulation depth. |
| **Knob 6** | Split crusher / distortion control | Left of center lowers the crusher sample rate for more bitcrushing. Right of center increases distortion gain. Around center, both effects are at their mildest settings. |

### Bypass Footswitch

| Gesture | Behavior |
| --- | --- |
| **Single press** | Controls the overdub record cycle. If the looper is idle, the press arms recording. If a loop is already recording, the press requests a stop. When MIDI clock is active, the actual record start can wait for the next clock beat instead of starting immediately. |
| **Double press** | Stops loop playback immediately. If recording is active, the current recording is finalized and committed. The second tap does not also trigger the normal single-press action. |
| **Long press** | Clears the overdub looper state for a fresh restart. Recording and playback are stopped, loop buffers are cleared, and the looper is re-armed to an empty state. |

### Alternate Footswitch

| Gesture | Behavior |
| --- | --- |
| **Single press** | Starts or enables stretched playback. If no stretched layer exists yet and enough loop material is available, the press also kicks off stretch generation. In the current firmware it also toggles the reverb freeze state in the background. |
| **Release** | No dedicated release action is routed for the alternate footswitch. |
| **Double press** | Stops stretched playback. The second tap also toggles the hidden reverb freeze state once, and it does **not** also trigger the normal single-press action on tap two. |
| **Long press** | Re-runs stretch generation for the current loop, when enough loop material exists, and enables stretched playback. No dedicated reverb action is routed for the hold gesture. |

## REVERB Mode

In this mode the micro-looper is disabled. The signal goes through the reverb first, then the pitch shifter, followed by the downstream delay, distortion, crusher, and mixer stages.

| Knob | Function | Notes |
| --- | --- | --- |
| **Knob 1** | Reverb time | Controls the reverb decay length. The micro-looper sensitivity and fading mappings tied to this knob are inactive in this mode. |
| **Knob 2** | Unused in the current firmware | This knob still points at micro-looper attack and slice parameters, but those have no audible effect while the micro-looper is disabled. |
| **Knob 3** | Pitch-shift amount and direction | Center is unison. Turning left shifts down, and turning right shifts up, with a continuous range up to one octave in either direction. |
| **Knob 4** | Pitch-shifter mix | Blends the unshifted reverb signal with the pitch-shifted reverb signal. Lower settings favor the unshifted reverb; higher settings favor the shifted signal. |
| **Knob 5** | Tape-style delay modulation depth | Fully counterclockwise disables the delay block. Any position above minimum turns the modulated delay on and increases the modulation depth. |
| **Knob 6** | Split crusher / distortion control | Left of center lowers the crusher sample rate for more bitcrushing. Right of center increases distortion gain. Around center, both effects are at their mildest settings. |

### Bypass Footswitch

In the current firmware, the **bypass** footswitch does **not** bypass the reverb path. Instead, it still drives the hidden micro-looper state in the background while you remain in **REVERB** mode.

| Gesture | Behavior |
| --- | --- |
| **Single press** | Arms hidden overdub recording if the looper is idle, or requests a stop if the hidden looper is already recording. |
| **Double press** | Stops the hidden looper playback and finalizes the hidden recording if one is active. The second tap does not also trigger the normal single-press action. |
| **Long press** | Clears the hidden overdub looper state for a fresh restart. |

This means the footswitch can change looper state while the audible signal remains in **REVERB** mode.

### Alternate Footswitch

| Gesture | Behavior |
| --- | --- |
| **Single press** | Toggles the audible reverb freeze state. At the same time, it also sends an alternate-footswitch press to the hidden looper, which attempts to start or enable stretched playback in the background. |
| **Release** | No dedicated release action is routed for the alternate footswitch. |
| **Double press** | Toggles the audible reverb freeze state once and stops hidden stretched looper playback. The second tap does **not** also trigger the normal single-press action on tap two. |
| **Long press** | Has no direct reverb action. Instead, it re-runs or starts hidden stretched playback in the background looper when enough loop material exists. |
