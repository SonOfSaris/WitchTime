# WitchTime

Bullet time after a perfect dodge for **CONTROL Resonant**. The game slows time for 0.6 seconds
after a perfect dodge, Dylan included. WitchTime keeps the world slow after that moment while
Dylan moves at normal speed: 4 seconds in all by default, the world at 20% of its speed.

- The game's own moment of slow motion plays as it is; the mod takes over from its end.
- Jump, dodge, Rush, attacks, Combat Abilities and the camera run at Dylan's speed; what you throw
  moves with the slowed world.
- A second perfect dodge inside it starts it again.
- Length, the world's speed and Dylan's speed are settings. With Mod Settings Menu installed they
  are under Options > MODS, with an Enable switch.

## Install

Grab `WitchTime.zip` from the [releases](../../releases) and extract it into the game folder
(next to `CONTROLResonant.exe`). Everything lands in `crmods\WitchTime\`. It needs crloader, the
`winmm.dll` mod loader, which you already have if you run other DLL mods. Full details are in
[README-WitchTime.txt](README-WitchTime.txt).

Checked on game versions 1.4.0 and 1.4.1. After a later update the mod still runs if the code it
needs is unchanged. It shows a message only when an update, or another mod, stops it from working:
the message names the mod and game versions and the game function that was not found. It works
next to other mods that hook the same game functions. CombatRebalance 0.13 or later works together
with it (the bullet time was part of CombatRebalance until 0.12).

## Build

Visual Studio 2022 (MSVC, x64). From a terminal:

```bat
build.bat
```

It compiles `witchtime.cpp` with the vendored [MinHook](https://github.com/TsudaKageyu/minhook)
into `out\witchtime.dll`. `package.py` makes the release zip from it and the package files; by
hand, copy the DLL, `witchtime_config.ini`, `witchtime.menu.json` and `README-WitchTime.txt` into
`crmods\WitchTime\` in the game folder.

Tests: `tests\build_tests.bat` builds `tests\out\codecheck.exe`, which maps the game's exe as data,
replays the mod's patterns and runs the game's own functions (the time step updates, the timescale
curve request and update, the engine's clock) on test data next to the mod's code, and
`tests\out\fakegame.exe`, a stand-in game that loads the DLL and plays perfect dodges through its
hooks. The rig needs the bytes of the game's functions: `python tests\gen_bytes.py <path to
CONTROLResonant.exe>` writes `tests\game_bytes.inc` from your installed game (the file is the
game's code and is not in the repository). `tests\check_exe.py` replays the patterns over an exe
after a game update.

## How it works

A perfect dodge asks the game's timescale curve database for its 0.6 s curve. The mod hooks that
request and asks for the same curve once more, as long as the witch time: the game keeps the entry
in its list and counts its seconds, and after the game's own update of the running curves the mod
writes the entry's value itself (the world's speed of the setting, eased back to 1 at the end). From
the end of the game's own curve Dylan's clock comes free of the world's: the player's own time step
(`LocalTimeScale`, which animation, movement and attacks read) is multiplied back up right after
the systems that write it, and the 59 player systems that take the game's clock as a parameter
(jump, dodge, Rush, input, camera ...) are handed a copy of that clock with the full step in it,
by swapping one pointer in their list of environments while the clock is free and putting the
game's back afterwards. Hook points are found by byte pattern in the game executable at start, and
the list of systems is read once from the scheduler's table. If a game update moves anything, the
mod stays inactive and says so.

For other mods: a mod that scales the player's time step as well can hand its factor to the
exported `WitchTime_PlayerFactor` while `WitchTime_Hooked` is 1; WitchTime multiplies the step by
it together with its own, and `WitchTime_Written` is the step it wrote last. CombatRebalance does
this for its attack speed, so the two never multiply the step twice.

## Credits

Built with [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD-2-Clause,
vendored; see `minhook/LICENSE.txt`, the notices are also embedded in the DLL). Loaded by crloader.
