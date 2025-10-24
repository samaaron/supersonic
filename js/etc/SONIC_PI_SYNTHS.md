# Sonic Pi Synth Documentation for SuperSonic

Complete parameter documentation for 8 Sonic Pi synths available in SuperSonic.

---

## 1. :beep (Sine Wave)

**File:** /home/sam/Development/sonic-pi/app/gui/help/synths_item_2749.html

**Description:** A simple pure sine wave. The sine wave is the simplest, purest sound there is and is the fundamental building block of all noise. The mathematician Fourier demonstrated that any sound could be built out of a number of sine waves (the more complex the sound, the more sine waves needed). Have a play combining a number of sine waves to design your own sounds!

**Introduced:** v2.0

### Parameters

| Parameter | Default | Description | Constraints | Slidable | BPM Scaled |
|-----------|---------|-------------|-------------|----------|------------|
| **note** | 52 | Note to play. Either a MIDI number or a symbol representing a note. For example: `30`, `52`, `:C`, `:C2`, `:Eb4`, or `:Ds3` | Must be zero or greater | Yes | No |
| **amp** | 1 | The amplitude of the sound. Typically a value between 0 and 1. Higher amplitudes may be used, but won't make the sound louder, they will just reduce the quality of all the sounds currently being played (due to compression.) | Must be zero or greater | Yes | No |
| **pan** | 0 | Position of sound in stereo. With headphones on, this means how much of the sound is in the left ear, and how much is in the right ear. With a value of -1, the sound is completely in the left ear, a value of 0 puts the sound equally in both ears and a value of 1 puts the sound in the right ear. Values in between -1 and 1 move the sound accordingly. | Must be between -1 and 1 inclusively | Yes | No |
| **attack** | 0 | Amount of time (in beats) for sound to reach full amplitude (attack_level). A short attack (i.e. 0.01) makes the initial part of the sound very percussive like a sharp tap. A longer attack (i.e 1) fades the sound in gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **decay** | 0 | Amount of time (in beats) for the sound to move from full amplitude (attack_level) to the sustain amplitude (sustain_level). | Must be zero or greater | No | Yes |
| **sustain** | 0 | Amount of time (in beats) for sound to remain at sustain level amplitude. Longer sustain values result in longer sounds. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **release** | 1 | Amount of time (in beats) for sound to move from sustain level amplitude to silent. A short release (i.e. 0.01) makes the final part of the sound very percussive (potentially resulting in a click). A longer release (i.e 1) fades the sound out gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **attack_level** | 1 | Amplitude level reached after attack phase and immediately before decay phase | Must be zero or greater | No | No |
| **decay_level** | sustain_level | Amplitude level reached after decay phase and immediately before sustain phase. Defaults to sustain_level unless explicitly set | Must be zero or greater | No | No |
| **sustain_level** | 1 | Amplitude level reached after decay phase and immediately before release phase. | Must be zero or greater | No | No |
| **env_curve** | 2 | Select the shape of the curve between levels in the envelope. 1=linear, 2=exponential, 3=sine, 4=welch, 6=squared, 7=cubed | Must be one of: [1, 2, 3, 4, 6, 7] | No | No |

---

## 2. :dsaw (Detuned Saw Wave)

**File:** /home/sam/Development/sonic-pi/app/gui/help/synths_item_2758.html

**Description:** A pair of detuned saw waves passed through a low pass filter. Two saw waves with slightly different frequencies generates a nice thick sound which is the basis for a lot of famous synth sounds. Thicken the sound by increasing the detune value, or create an octave-playing synth by choosing a detune of 12 (12 MIDI notes is an octave).

**Introduced:** v2.0

### Parameters

| Parameter | Default | Description | Constraints | Slidable | BPM Scaled |
|-----------|---------|-------------|-------------|----------|------------|
| **note** | 52 | Note to play. Either a MIDI number or a symbol representing a note. For example: `30`, `52`, `:C`, `:C2`, `:Eb4`, or `:Ds3` | Must be zero or greater | Yes | No |
| **amp** | 1 | The amplitude of the sound. Typically a value between 0 and 1. Higher amplitudes may be used, but won't make the sound louder, they will just reduce the quality of all the sounds currently being played (due to compression.) | Must be zero or greater | Yes | No |
| **pan** | 0 | Position of sound in stereo. With headphones on, this means how much of the sound is in the left ear, and how much is in the right ear. With a value of -1, the sound is completely in the left ear, a value of 0 puts the sound equally in both ears and a value of 1 puts the sound in the right ear. Values in between -1 and 1 move the sound accordingly. | Must be between -1 and 1 inclusively | Yes | No |
| **attack** | 0 | Amount of time (in beats) for sound to reach full amplitude (attack_level). A short attack (i.e. 0.01) makes the initial part of the sound very percussive like a sharp tap. A longer attack (i.e 1) fades the sound in gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **decay** | 0 | Amount of time (in beats) for the sound to move from full amplitude (attack_level) to the sustain amplitude (sustain_level). | Must be zero or greater | No | Yes |
| **sustain** | 0 | Amount of time (in beats) for sound to remain at sustain level amplitude. Longer sustain values result in longer sounds. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **release** | 1 | Amount of time (in beats) for sound to move from sustain level amplitude to silent. A short release (i.e. 0.01) makes the final part of the sound very percussive (potentially resulting in a click). A longer release (i.e 1) fades the sound out gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **attack_level** | 1 | Amplitude level reached after attack phase and immediately before decay phase | Must be zero or greater | No | No |
| **decay_level** | sustain_level | Amplitude level reached after decay phase and immediately before sustain phase. Defaults to sustain_level unless explicitly set | Must be zero or greater | No | No |
| **sustain_level** | 1 | Amplitude level reached after decay phase and immediately before release phase. | Must be zero or greater | No | No |
| **env_curve** | 2 | Select the shape of the curve between levels in the envelope. 1=linear, 2=exponential, 3=sine, 4=welch, 6=squared, 7=cubed | Must be one of: [1, 2, 3, 4, 6, 7] | No | No |
| **cutoff** | 100 | MIDI note representing the highest frequencies allowed to be present in the sound. A low value like 30 makes the sound round and dull, a high value like 100 makes the sound buzzy and crispy. | Must be zero or greater, must be less than 131 | Yes | No |
| **detune** | 0.1 | Distance (in MIDI notes) between components of sound. Affects thickness, sense of tuning and harmony. Tiny values such as 0.1 create a thick sound. Larger values such as 0.5 make the tuning sound strange. Even bigger values such as 5 create chord-like sounds. | No constraints specified | Yes | No |

---

## 3. :dpulse (Detuned Pulse Wave)

**File:** /home/sam/Development/sonic-pi/app/gui/help/synths_item_2757.html

**Description:** A pair of detuned pulse waves passed through a low pass filter. Two pulse waves with slightly different frequencies generates a nice thick sound which can be used as a basis for some nice bass sounds. Thicken the sound by increasing the detune value, or create an octave-playing synth by choosing a detune of 12 (12 MIDI notes is an octave). Each pulse wave can also have individual widths (although the default is for the detuned pulse to mirror the width of the main pulse).

**Introduced:** v2.8

### Parameters

| Parameter | Default | Description | Constraints | Slidable | BPM Scaled |
|-----------|---------|-------------|-------------|----------|------------|
| **note** | 52 | Note to play. Either a MIDI number or a symbol representing a note. For example: `30`, `52`, `:C`, `:C2`, `:Eb4`, or `:Ds3` | Must be zero or greater | Yes | No |
| **amp** | 1 | The amplitude of the sound. Typically a value between 0 and 1. Higher amplitudes may be used, but won't make the sound louder, they will just reduce the quality of all the sounds currently being played (due to compression.) | Must be zero or greater | Yes | No |
| **pan** | 0 | Position of sound in stereo. With headphones on, this means how much of the sound is in the left ear, and how much is in the right ear. With a value of -1, the sound is completely in the left ear, a value of 0 puts the sound equally in both ears and a value of 1 puts the sound in the right ear. Values in between -1 and 1 move the sound accordingly. | Must be between -1 and 1 inclusively | Yes | No |
| **attack** | 0 | Amount of time (in beats) for sound to reach full amplitude (attack_level). A short attack (i.e. 0.01) makes the initial part of the sound very percussive like a sharp tap. A longer attack (i.e 1) fades the sound in gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **decay** | 0 | Amount of time (in beats) for the sound to move from full amplitude (attack_level) to the sustain amplitude (sustain_level). | Must be zero or greater | No | Yes |
| **sustain** | 0 | Amount of time (in beats) for sound to remain at sustain level amplitude. Longer sustain values result in longer sounds. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **release** | 1 | Amount of time (in beats) for sound to move from sustain level amplitude to silent. A short release (i.e. 0.01) makes the final part of the sound very percussive (potentially resulting in a click). A longer release (i.e 1) fades the sound out gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **attack_level** | 1 | Amplitude level reached after attack phase and immediately before decay phase | Must be zero or greater | No | No |
| **decay_level** | sustain_level | Amplitude level reached after decay phase and immediately before sustain phase. Defaults to sustain_level unless explicitly set | Must be zero or greater | No | No |
| **sustain_level** | 1 | Amplitude level reached after decay phase and immediately before release phase. | Must be zero or greater | No | No |
| **env_curve** | 2 | Select the shape of the curve between levels in the envelope. 1=linear, 2=exponential, 3=sine, 4=welch, 6=squared, 7=cubed | Must be one of: [1, 2, 3, 4, 6, 7] | No | No |
| **cutoff** | 100 | MIDI note representing the highest frequencies allowed to be present in the sound. A low value like 30 makes the sound round and dull, a high value like 100 makes the sound buzzy and crispy. | Must be zero or greater, must be less than 131 | Yes | No |
| **detune** | 0.1 | Distance (in MIDI notes) between components of sound. Affects thickness, sense of tuning and harmony. Tiny values such as 0.1 create a thick sound. Larger values such as 0.5 make the tuning sound strange. Even bigger values such as 5 create chord-like sounds. | No constraints specified | Yes | No |
| **pulse_width** | 0.5 | The width of the pulse wave as a value between 0 and 1. A width of 0.5 will produce a square wave. Different values will change the timbre of the sound. Only valid if wave is type pulse. | Must be between 0 and 1 exclusively | Yes | No |
| **dpulse_width** | pulse_width | The width of the second detuned pulse wave as a value between 0 and 1. A width of 0.5 will produce a square wave. Different values will change the timbre of the sound. Only valid if wave is type pulse. | Must be between 0 and 1 exclusively | Yes | No |

---

## 4. :bnoise (Brown Noise)

**File:** /home/sam/Development/sonic-pi/app/gui/help/synths_item_2751.html

**Description:** Noise whose spectrum falls off in power by 6 dB per octave. Useful for generating percussive sounds such as snares and hand claps. Also useful for simulating wind or sea effects.

**Introduced:** v2.0

### Parameters

| Parameter | Default | Description | Constraints | Slidable | BPM Scaled |
|-----------|---------|-------------|-------------|----------|------------|
| **amp** | 1 | The amplitude of the sound. Typically a value between 0 and 1. Higher amplitudes may be used, but won't make the sound louder, they will just reduce the quality of all the sounds currently being played (due to compression.) | Must be zero or greater | Yes | No |
| **pan** | 0 | Position of sound in stereo. With headphones on, this means how much of the sound is in the left ear, and how much is in the right ear. With a value of -1, the sound is completely in the left ear, a value of 0 puts the sound equally in both ears and a value of 1 puts the sound in the right ear. Values in between -1 and 1 move the sound accordingly. | Must be between -1 and 1 inclusively | Yes | No |
| **attack** | 0 | Amount of time (in beats) for sound to reach full amplitude (attack_level). A short attack (i.e. 0.01) makes the initial part of the sound very percussive like a sharp tap. A longer attack (i.e 1) fades the sound in gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **decay** | 0 | Amount of time (in beats) for the sound to move from full amplitude (attack_level) to the sustain amplitude (sustain_level). | Must be zero or greater | No | Yes |
| **sustain** | 0 | Amount of time (in beats) for sound to remain at sustain level amplitude. Longer sustain values result in longer sounds. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **release** | 1 | Amount of time (in beats) for sound to move from sustain level amplitude to silent. A short release (i.e. 0.01) makes the final part of the sound very percussive (potentially resulting in a click). A longer release (i.e 1) fades the sound out gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **attack_level** | 1 | Amplitude level reached after attack phase and immediately before decay phase | Must be zero or greater | No | No |
| **decay_level** | sustain_level | Amplitude level reached after decay phase and immediately before sustain phase. Defaults to sustain_level unless explicitly set | Must be zero or greater | No | No |
| **sustain_level** | 1 | Amplitude level reached after decay phase and immediately before release phase. | Must be zero or greater | No | No |
| **env_curve** | 2 | Select the shape of the curve between levels in the envelope. 1=linear, 2=exponential, 3=sine, 4=welch, 6=squared, 7=cubed | Must be one of: [1, 2, 3, 4, 6, 7] | No | No |
| **cutoff** | 110 | MIDI note representing the highest frequencies allowed to be present in the sound. A low value like 30 makes the sound round and dull, a high value like 100 makes the sound buzzy and crispy. | Must be zero or greater, must be less than 131 | Yes | No |
| **res** | 0 | Filter resonance as a value between 0 and 1. Large amounts of resonance (a res: near 1) can create a whistling sound around the cutoff frequency. Smaller values produce less resonance. | Must be zero or greater, must be less than 1 | Yes | No |

**Note:** This synth does NOT have a `note` parameter since it's noise-based.

---

## 5. :prophet (The Prophet)

**File:** /home/sam/Development/sonic-pi/app/gui/help/synths_item_2781.html

**Description:** Dark and swirly, this synth uses Pulse Width Modulation (PWM) to create a timbre which continually moves around. This effect is created using the pulse ugen which produces a variable width square wave. We then control the width of the pulses using a variety of LFOs - sin-osc and lf-tri in this case. We use a number of these LFO modulated pulse ugens with varying LFO type and rate (and phase in some cases) to provide the LFO with a different starting point. We then mix all these pulses together to create a thick sound and then feed it through a resonant low pass filter (rlpf). For extra bass, one of the pulses is an octave lower (half the frequency) and its LFO has a little bit of randomisation thrown into its frequency component for that extra bit of variety.

**Design Note:** Synth design adapted from: The Prophet Speaks (page 2), Steal This Sound, Mitchell Sigman

**Introduced:** v2.0

### Parameters

| Parameter | Default | Description | Constraints | Slidable | BPM Scaled |
|-----------|---------|-------------|-------------|----------|------------|
| **note** | 52 | Note to play. Either a MIDI number or a symbol representing a note. For example: `30`, `52`, `:C`, `:C2`, `:Eb4`, or `:Ds3` | Must be zero or greater | Yes | No |
| **amp** | 1 | The amplitude of the sound. Typically a value between 0 and 1. Higher amplitudes may be used, but won't make the sound louder, they will just reduce the quality of all the sounds currently being played (due to compression.) | Must be zero or greater | Yes | No |
| **pan** | 0 | Position of sound in stereo. With headphones on, this means how much of the sound is in the left ear, and how much is in the right ear. With a value of -1, the sound is completely in the left ear, a value of 0 puts the sound equally in both ears and a value of 1 puts the sound in the right ear. Values in between -1 and 1 move the sound accordingly. | Must be between -1 and 1 inclusively | Yes | No |
| **attack** | 0 | Amount of time (in beats) for sound to reach full amplitude (attack_level). A short attack (i.e. 0.01) makes the initial part of the sound very percussive like a sharp tap. A longer attack (i.e 1) fades the sound in gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **decay** | 0 | Amount of time (in beats) for the sound to move from full amplitude (attack_level) to the sustain amplitude (sustain_level). | Must be zero or greater | No | Yes |
| **sustain** | 0 | Amount of time (in beats) for sound to remain at sustain level amplitude. Longer sustain values result in longer sounds. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **release** | 1 | Amount of time (in beats) for sound to move from sustain level amplitude to silent. A short release (i.e. 0.01) makes the final part of the sound very percussive (potentially resulting in a click). A longer release (i.e 1) fades the sound out gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **attack_level** | 1 | Amplitude level reached after attack phase and immediately before decay phase | Must be zero or greater | No | No |
| **decay_level** | sustain_level | Amplitude level reached after decay phase and immediately before sustain phase. Defaults to sustain_level unless explicitly set | Must be zero or greater | No | No |
| **sustain_level** | 1 | Amplitude level reached after decay phase and immediately before release phase. | Must be zero or greater | No | No |
| **env_curve** | 2 | Select the shape of the curve between levels in the envelope. 1=linear, 2=exponential, 3=sine, 4=welch, 6=squared, 7=cubed | Must be one of: [1, 2, 3, 4, 6, 7] | No | No |
| **cutoff** | 110 | MIDI note representing the highest frequencies allowed to be present in the sound. A low value like 30 makes the sound round and dull, a high value like 100 makes the sound buzzy and crispy. | Must be zero or greater, must be less than 131 | Yes | No |
| **res** | 0.7 | Filter resonance as a value between 0 and 1. Large amounts of resonance (a res: near 1) can create a whistling sound around the cutoff frequency. Smaller values produce less resonance. | Must be zero or greater, must be less than 1 | Yes | No |

---

## 6. :tb303 (TB-303 Emulation)

**File:** /home/sam/Development/sonic-pi/app/gui/help/synths_item_2808.html

**Description:** Emulation of the classic Roland TB-303 Bass Line synthesiser. Overdrive the res (i.e. use very large values) for that classic late 80s acid sound.

**Introduced:** v2.0

### Parameters

| Parameter | Default | Description | Constraints | Slidable | BPM Scaled |
|-----------|---------|-------------|-------------|----------|------------|
| **note** | 52 | Note to play. Either a MIDI number or a symbol representing a note. For example: `30`, `52`, `:C`, `:C2`, `:Eb4`, or `:Ds3` | Must be zero or greater | Yes | No |
| **amp** | 1 | The amplitude of the sound. Typically a value between 0 and 1. Higher amplitudes may be used, but won't make the sound louder, they will just reduce the quality of all the sounds currently being played (due to compression.) | Must be zero or greater | Yes | No |
| **pan** | 0 | Position of sound in stereo. With headphones on, this means how much of the sound is in the left ear, and how much is in the right ear. With a value of -1, the sound is completely in the left ear, a value of 0 puts the sound equally in both ears and a value of 1 puts the sound in the right ear. Values in between -1 and 1 move the sound accordingly. | Must be between -1 and 1 inclusively | Yes | No |
| **attack** | 0 | Amount of time (in beats) for sound to reach full amplitude (attack_level). A short attack (i.e. 0.01) makes the initial part of the sound very percussive like a sharp tap. A longer attack (i.e 1) fades the sound in gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **decay** | 0 | Amount of time (in beats) for the sound to move from full amplitude (attack_level) to the sustain amplitude (sustain_level). | Must be zero or greater | No | Yes |
| **sustain** | 0 | Amount of time (in beats) for sound to remain at sustain level amplitude. Longer sustain values result in longer sounds. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **release** | 1 | Amount of time (in beats) for sound to move from sustain level amplitude to silent. A short release (i.e. 0.01) makes the final part of the sound very percussive (potentially resulting in a click). A longer release (i.e 1) fades the sound out gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **attack_level** | 1 | Amplitude level reached after attack phase and immediately before decay phase | Must be zero or greater | No | No |
| **decay_level** | sustain_level | Amplitude level reached after decay phase and immediately before sustain phase. Defaults to sustain_level unless explicitly set | Must be zero or greater | No | No |
| **sustain_level** | 1 | Amplitude level reached after decay phase and immediately before release phase. | Must be zero or greater | No | No |
| **env_curve** | 2 | Select the shape of the curve between levels in the envelope. 1=linear, 2=exponential, 3=sine, 4=welch, 6=squared, 7=cubed | Must be one of: [1, 2, 3, 4, 6, 7] | No | No |
| **cutoff** | 120 | The maximum cutoff value as a MIDI note | Must be less than or equal to 130 | Yes | No |
| **cutoff_min** | 30 | The minimum cutoff value. | Must be less than or equal to 130 | Yes | No |
| **cutoff_attack** | attack | Attack time for cutoff filter. Amount of time (in beats) for sound to reach full cutoff value. Default value is set to match amp envelope's attack value. | Must be zero or greater | No | Yes |
| **cutoff_decay** | decay | Decay time for cutoff filter. Amount of time (in beats) for sound to move from full cutoff value (cutoff attack level) to the cutoff sustain level. Default value is set to match amp envelope's decay value. | Must be zero or greater | No | Yes |
| **cutoff_sustain** | sustain | Amount of time for cutoff value to remain at sustain level in beats. Default value is set to match amp envelope's sustain value. | Must be zero or greater | No | Yes |
| **cutoff_release** | release | Amount of time (in beats) for sound to move from cutoff sustain value to cutoff min value. Default value is set to match amp envelope's release value. | Must be zero or greater | No | Yes |
| **cutoff_attack_level** | 1 | The peak cutoff (value of cutoff at peak of attack) as a value between 0 and 1 where 0 is the :cutoff_min and 1 is the :cutoff value | Must be between 0 and 1 inclusively | No | No |
| **cutoff_decay_level** | cutoff_sustain_level | The level of cutoff after the decay phase as a value between 0 and 1 where 0 is the :cutoff_min and 1 is the :cutoff value | Must be between 0 and 1 inclusively | No | No |
| **cutoff_sustain_level** | 1 | The sustain cutoff (value of cutoff at sustain time) as a value between 0 and 1 where 0 is the :cutoff_min and 1 is the :cutoff value. | Must be between 0 and 1 inclusively | No | No |
| **res** | 0.9 | Filter resonance as a value between 0 and 1. Large amounts of resonance (a res: near 1) can create a whistling sound around the cutoff frequency. Smaller values produce less resonance. | Must be zero or greater, must be less than 1 | Yes | No |
| **wave** | 0 | Wave type - 0 saw, 1 pulse, 2 triangle. Different waves will produce different sounds. | Must be one of: [0, 1, 2] | Yes | No |
| **pulse_width** | 0.5 | The width of the pulse wave as a value between 0 and 1. A width of 0.5 will produce a square wave. Different values will change the timbre of the sound. Only valid if wave is type pulse. | Must be between 0 and 1 exclusively | Yes | No |

---

## 7. :chiplead (Chip Lead)

**File:** /home/sam/Development/sonic-pi/app/gui/help/synths_item_2753.html

**Description:** A slightly clipped square (pulse) wave with phases of 12.5%, 25% or 50% modelled after the 2A03 chip found in voices 1 and 2 of the NES games console. This can be used for retro sounding leads and harmonised lines. This also adds an opt 'note_resolution' which locks the note slide to certain pitches which are multiples of the step size. This allows for emulation of the sweep setting on the 2A03.

**Introduced:** v2.10

### Parameters

| Parameter | Default | Description | Constraints | Slidable | BPM Scaled |
|-----------|---------|-------------|-------------|----------|------------|
| **note** | 60 | Note to play. Either a MIDI number or a symbol representing a note. For example: `30`, `52`, `:C`, `:C2`, `:Eb4`, or `:Ds3` | Must be zero or greater | Yes | No |
| **note_resolution** | 0.1 | Locks down the note resolution to be multiples of this (MIDI) number. For example, a `note_resolution:` of 1 will only allow semitones to be played. When used in conjunction with `note_slide:` produces a staircase of notes rather than a continuous line which is how things were on the NES. Set to 0 to disable. This wasn't a feature of this triangle (bass) channel on the original chip but some emulators have added it in since. | Must be zero or greater | Yes (parameter can be changed) | No |
| **amp** | 1 | The amplitude of the sound. Typically a value between 0 and 1. Higher amplitudes may be used, but won't make the sound louder, they will just reduce the quality of all the sounds currently being played (due to compression.) | Must be zero or greater | Yes | No |
| **pan** | 0 | Position of sound in stereo. With headphones on, this means how much of the sound is in the left ear, and how much is in the right ear. With a value of -1, the sound is completely in the left ear, a value of 0 puts the sound equally in both ears and a value of 1 puts the sound in the right ear. Values in between -1 and 1 move the sound accordingly. | Must be between -1 and 1 inclusively | Yes | No |
| **attack** | 0 | Amount of time (in beats) for sound to reach full amplitude (attack_level). A short attack (i.e. 0.01) makes the initial part of the sound very percussive like a sharp tap. A longer attack (i.e 1) fades the sound in gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **decay** | 0 | Amount of time (in beats) for the sound to move from full amplitude (attack_level) to the sustain amplitude (sustain_level). | Must be zero or greater | No | Yes |
| **sustain** | 0 | Amount of time (in beats) for sound to remain at sustain level amplitude. Longer sustain values result in longer sounds. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **release** | 1 | Amount of time (in beats) for sound to move from sustain level amplitude to silent. A short release (i.e. 0.01) makes the final part of the sound very percussive (potentially resulting in a click). A longer release (i.e 1) fades the sound out gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **attack_level** | 1 | Amplitude level reached after attack phase and immediately before decay phase | Must be zero or greater | No | No |
| **decay_level** | sustain_level | Amplitude level reached after decay phase and immediately before sustain phase. Defaults to sustain_level unless explicitly set | Must be zero or greater | No | No |
| **sustain_level** | 1 | Amplitude level reached after decay phase and immediately before release phase. | Must be zero or greater | No | No |
| **env_curve** | 2 | Select the shape of the curve between levels in the envelope. 1=linear, 2=exponential, 3=sine, 4=welch, 6=squared, 7=cubed | Must be one of: [1, 2, 3, 4, 6, 7] | No | No |
| **width** | 0 | Which of the three pulse_widths to use - 0 => 12.5%, 1 => 25%, 2 => 50% | Must be one of: [0, 1, 2] | Yes | No |

---

## 8. :fm (Basic FM Synthesis)

**File:** /home/sam/Development/sonic-pi/app/gui/help/synths_item_2761.html

**Description:** A sine wave with a fundamental frequency which is modulated at audio rate by another sine wave with a specific modulation, division and depth. Useful for generating a wide range of sounds by playing with the divisor and depth params. Great for deep powerful bass and fun 70s sci-fi sounds.

**Introduced:** v2.0

### Parameters

| Parameter | Default | Description | Constraints | Slidable | BPM Scaled |
|-----------|---------|-------------|-------------|----------|------------|
| **note** | 52 | Note to play. Either a MIDI number or a symbol representing a note. For example: `30`, `52`, `:C`, `:C2`, `:Eb4`, or `:Ds3` | Must be zero or greater | Yes | No |
| **amp** | 1 | The amplitude of the sound. Typically a value between 0 and 1. Higher amplitudes may be used, but won't make the sound louder, they will just reduce the quality of all the sounds currently being played (due to compression.) | Must be zero or greater | Yes | No |
| **pan** | 0 | Position of sound in stereo. With headphones on, this means how much of the sound is in the left ear, and how much is in the right ear. With a value of -1, the sound is completely in the left ear, a value of 0 puts the sound equally in both ears and a value of 1 puts the sound in the right ear. Values in between -1 and 1 move the sound accordingly. | Must be between -1 and 1 inclusively | Yes | No |
| **attack** | 0 | Amount of time (in beats) for sound to reach full amplitude (attack_level). A short attack (i.e. 0.01) makes the initial part of the sound very percussive like a sharp tap. A longer attack (i.e 1) fades the sound in gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **decay** | 0 | Amount of time (in beats) for the sound to move from full amplitude (attack_level) to the sustain amplitude (sustain_level). | Must be zero or greater | No | Yes |
| **sustain** | 0 | Amount of time (in beats) for sound to remain at sustain level amplitude. Longer sustain values result in longer sounds. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **release** | 1 | Amount of time (in beats) for sound to move from sustain level amplitude to silent. A short release (i.e. 0.01) makes the final part of the sound very percussive (potentially resulting in a click). A longer release (i.e 1) fades the sound out gently. Full length of sound is attack + decay + sustain + release. | Must be zero or greater | No | Yes |
| **attack_level** | 1 | Amplitude level reached after attack phase and immediately before decay phase | Must be zero or greater | No | No |
| **decay_level** | sustain_level | Amplitude level reached after decay phase and immediately before sustain phase. Defaults to sustain_level unless explicitly set | Must be zero or greater | No | No |
| **sustain_level** | 1 | Amplitude level reached after decay phase and immediately before release phase. | Must be zero or greater | No | No |
| **env_curve** | 2 | Select the shape of the curve between levels in the envelope. 1=linear, 2=exponential, 3=sine, 4=welch, 6=squared, 7=cubed | Must be one of: [1, 2, 3, 4, 6, 7] | No | No |
| **cutoff** | 100 | MIDI note representing the highest frequencies allowed to be present in the sound. A low value like 30 makes the sound round and dull, a high value like 100 makes the sound buzzy and crispy. | Must be zero or greater, must be less than 131 | Yes | No |
| **divisor** | 2 | Modifies the frequency of the modulator oscillator relative to the carrier. Don't worry too much about what this means - just try different numbers out! | No constraints specified | Yes | No |
| **depth** | 1 | Modifies the depth of the carrier wave used to modify fundamental frequency. Don't worry too much about what this means - just try different numbers out! | No constraints specified | Yes | No |

---

## Slide Options (Common to All Synths)

Any parameter that is slidable has three additional options named `_slide`, `_slide_curve`, and `_slide_shape`. For example, if `amp` is slidable, you can also set `amp_slide`, `amp_slide_curve`, and `amp_slide_shape` with the following effects:

| Option | Default | Description |
|--------|---------|-------------|
| **_slide** | 0 | Amount of time (in beats) for the parameter value to change. A long parameter_slide value means that the parameter takes a long time to slide from the previous value to the new value. A parameter_slide of 0 means that the parameter instantly changes to the new value. |
| **_slide_shape** | 5 | Shape of curve. 0: step, 1: linear, 3: sine, 4: welch, 5: custom (use *_slide_curve: opt e.g. amp_slide_curve:), 6: squared, 7: cubed. |
| **_slide_curve** | 0 | Shape of the slide curve (only honoured if slide shape is 5). 0 means linear and positive and negative numbers curve the segment up and down respectively. |

---

## Summary

This document contains complete parameter documentation for the 8 Sonic Pi synths available in SuperSonic, extracted from Sonic Pi's HTML tutorial files. Each synth includes:

- Full description and usage notes
- Complete parameter tables with defaults, descriptions, constraints, and behavior
- Information about slidable parameters and BPM scaling
- Version introduced
- Special notes about synth design (where applicable)

The synths documented are:
1. `:beep` - Pure sine wave
2. `:dsaw` - Detuned saw waves
3. `:dpulse` - Detuned pulse waves
4. `:bnoise` - Brown noise (no note parameter)
5. `:prophet` - PWM-based synth
6. `:tb303` - Roland TB-303 emulation with extensive cutoff envelope
7. `:chiplead` - NES 2A03 chip emulation
8. `:fm` - Basic FM synthesis
