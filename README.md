# Quarry Spatial Audio

A mod for **The Quarry** (PC, Steam) that makes the game output **surround
sound** and **Windows spatial sound** — Dolby Atmos for Headphones, Windows
Sonic and other spatial audio formats — instead of plain stereo.

Out of the box, The Quarry on PC only outputs stereo, even when Dolby Atmos or
Windows Sonic is switched on. With this mod:

- **With spatial sound switched on in Windows,** the game sends Windows a 7.1.4
  mix (the usual seven speakers plus four overhead) and individually
  positioned sounds, so Atmos or Sonic can place each sound around you,
  including behind you and above you.
- **With surround speakers,** or when Windows mixes your device wider than its
  hardware, the game outputs full surround and pans sounds into it.
- **On plain stereo headphones or speakers,** nothing changes unless you ask
  for headphone panning.

It was made with blind and low-vision players in mind: hearing where a sound
comes from — in front, beside or behind — matters a great deal when you
cannot see the screen. It works alongside UE4SS and the QuarryAccess
accessibility mod.

## Requirements

- The Quarry, Steam version for Windows (tested September 2026).
- Windows 10 or 11.
- For spatial sound: spatial sound switched on for your output device in
  Windows (Settings, System, Sound, your device, Spatial sound). Windows
  Sonic for Headphones is free; Dolby Atmos for Headphones needs the Dolby
  Access app.

## Installing

1. Download `X3DAudio1_7.dll` and `QuarrySpatial.ini` from the latest release.
2. Open the game folder, then `SMG026\Binaries\Win64` — the folder that
   contains `TheQuarry-Win64-Shipping.exe`. In Steam: right-click The Quarry,
   Manage, Browse local files.
3. Copy both files there.
4. Start the game as usual.

## Settings

Open `QuarrySpatial.ini` in a text editor. Restart the game after changing it.

There are two settings.

`Output=` sets how the game's sound is laid out:

- `auto` (the default) — 3D spatial sound if Windows spatial sound is switched
  on for the device; otherwise the channel layout Windows mixes the device at,
  when that is wider than what the device reports as its hardware (7.1 with
  Atmos or Sonic on); otherwise the game's own output.
- `spatial` — 3D spatial sound: a 7.1.4 mix plus individually positioned
  sounds. This covers every Windows spatial format — Dolby Atmos for
  Headphones, Dolby Atmos for home theater, Windows Sonic, DTS — because
  Windows, not the game, turns the sound into what your headphones or receiver
  needs. Falls back to what `auto` would choose if spatial sound is off.
- `7.1`, `5.1` or `quad` — that speaker layout. `5.1` means front left and
  right, centre, LFE and two side speakers, as most receivers use. With Atmos
  or Sonic on, `7.1` gives you Atmos or Sonic rendering a 7.1 channel mix
  instead of positioned sounds.
- `stereo` — stereo, even on a surround device.
- `headphones` — 3D spatial sound if it is switched on; otherwise stereo with
  the game's sound engine set to headphone panning.
- `mono` — the game mixes everything to one channel, played on both sides, so
  no sound is lost to one ear. Unlike Windows' own Mono audio setting, it
  affects only the game, not your screen reader or other programs.

Before using a layout it has made up (`7.1`, `5.1`, `quad` or `stereo`), the
mod asks Windows whether it accepts that layout on your device. If Windows says
no, the mod says so in the log and uses the next best choice, so a wrong
setting cannot leave the game silent.

`Device=` sets which output the game plays on. Leave it empty for the Windows
default. Otherwise write part of the device's name, for example `iD4` or
`Headphones`; capitals do not matter. This lets you keep the game and your
screen reader on different devices. If no device matches, the log lists the
names of the available devices. The game may start on the default device for
a moment before it switches.

## Checking that it works

Each time the game starts, the mod writes `QuarrySpatial.log` next to the DLL.
It is a few lines of plain text:

- the device it used, how many channels its hardware has, what Windows mixes
  it at, and whether Windows spatial sound is on for it;
- the output it chose;
- whether spatial sound was enabled in the game's sound engine;
- what the game's sound engine (Wwise) ended up mixing to, read back from the
  engine itself — for example `Wwise is mixing to: 7.1.4.` in spatial mode or
  `7.1.` in surround mode.

## Uninstalling

Delete `X3DAudio1_7.dll`, `QuarrySpatial.ini` and `QuarrySpatial.log` from
`SMG026\Binaries\Win64`. The mod changes no game files; everything it does
happens in memory while the game runs.

## Known issues

- During testing, the game showed an "has crashed and will close" message when
  quitting. The crash is in the game's own shutdown code; whether the mod is
  involved has not been established yet.
- The mod reads the audio device when the game starts. If you change your
  default output device or its spatial sound setting, restart the game.
- A game update may change the code the spatial switch depends on. The mod
  checks the exact bytes first; if they differ, it leaves the game alone,
  says so in the log, and surround still works.

## How it works

For modders and the curious.

- **Loading.** The game imports `X3DAudio1_7.dll`, a small DirectX library
  with two functions. A DLL of that name in the game folder is loaded before
  any game code runs. The mod passes both functions through to the real copy
  in Windows, so the game's own 3D calculations are unchanged.
- **Why the game outputs stereo.** The game's sound engine, Audiokinetic
  Wwise, sizes its output from the audio device's *hardware* format
  (`PKEY_AudioEngine_DeviceFormat`), not from the format Windows mixes at. A
  stereo USB interface or headset therefore gets stereo even when Atmos has
  Windows mixing at 7.1.
- **Surround.** When the game itself reads that property, the mod answers with
  the format Windows mixes at. Wwise then opens that layout and pans 3D sounds
  into it. Reads made by Windows' own audio code on the game's behalf still
  get the true answer: the mod only changes a read whose immediate caller —
  after the Steam overlay, which sits in front of the same functions — is
  the game.
- **Spatial.** Wwise's Windows spatial sound support is compiled into the
  game but gated on an "Allow 3D Audio" setting the game ships switched off.
  The gate is a single conditional jump in the sound engine's output setup.
  The mod checks the 18 bytes around it against the known Steam build and,
  only if they match exactly, replaces the jump with two no-op instructions.
  Wwise then opens a Windows spatial audio stream with a 7.1.4 bed and up to
  128 positioned objects (measured with Dolby Atmos for Headphones).
- **Mono and device choice.** Windows refuses a one-channel output stream on
  most devices, so for mono the output itself stays as it is. Instead the mod
  calls Wwise's exported `ReplaceOutput` to rebuild the main output with a
  one-channel layout; Wwise mixes to one channel and spreads it over the
  device's real channels. The same call, with the device's Wwise id from the
  exported `AK::GetDeviceID`, moves the output to the chosen device. The
  widened format is only given for that device: the mod checks the device's
  endpoint id inside the property read.
- **Reading the result back.** The game exports Wwise's API by name; the mod
  calls `GetSpeakerConfiguration` to log what Wwise is actually mixing to,
  and `SetPanningRule` for the headphones option.

## Building

With MinGW-w64 GCC (for example WinLibs) on `PATH`, or its path in the
`QSA_GCC` environment variable:

    .\build.ps1          # builds build\X3DAudio1_7.dll
    .\build.ps1 -Test    # also builds and runs the offline tests

The tests stand in for the game: the test program exports stand-ins for the
Wwise functions under the game's names, and checks the pass-through, every
output decision, the byte check before the spatial patch (including refusing
a one-byte difference), that only the game's own device-format reads are
changed, a speaker layout set by hand (both accepted and refused by Windows),
mono, headphone panning, and the device choice (using another active output
on the test machine, and a name that matches nothing).

## License

GPL-3.0-or-later. See `LICENSE`.
