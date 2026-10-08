WitchTime 1.0 - bullet time after a perfect dodge for CONTROL Resonant
======================================================================

A perfect dodge slows time for a moment in the game: 0.6 seconds, and Dylan slows with
everything else. With this mod that moment plays as it is, then the world stays slow while
Dylan runs, jumps, dodges and attacks at normal speed: 4 seconds in all by default, the world
at 20% of its speed. Combat Abilities you cast in it start at Dylan's speed; what they throw
moves with the slowed world. A second perfect dodge inside it starts it again.
Length, the world's speed and Dylan's speed can be set.

REQUIRES
  crloader (the winmm.dll mod loader) in the game folder. If you run other DLL mods, you have it.
  Optional: Mod Settings Menu, to change the settings in game under Options > MODS.

INSTALL
  1. Extract this archive into the game folder (next to CONTROLResonant.exe). Everything goes
     into one folder:
       crmods\WitchTime\
  2. Start the game.
  Checked on game versions 1.4.0 and 1.4.1. After a later game update the mod still runs if
  the code it needs is unchanged, and shows no message. If an update stops it from working, a
  message at start says so and the game runs unmodified.
  Works next to other mods that hook the same game functions.
  CombatRebalance 0.13 or later works together with it: attack speed, recovery and charge speed
  apply inside a witch time too. Until 0.12 the bullet time was part of CombatRebalance; update
  it, or switch its bullet time off.

UNINSTALL
  Delete the crmods\WitchTime folder. Nothing is stored in your saves.

SETTINGS
  crmods\WitchTime\witchtime_config.ini, commented, reloaded while the game runs.
  With Mod Settings Menu, the same settings are under Options > MODS > WitchTime, with an
  Enable switch at the top. A value changed in the menu wins over the ini.
    Duration     seconds in all, the game's own 0.6 s included: 1 to 20 (4)
    WorldSpeed   the world's speed once the game's own slow motion is over, in percent (20)
    PlayerSpeed  Dylan's speed, in percent (100)

LOG
  crmods\WitchTime\witchtime.log, rewritten every start. It says what was found in the game's
  code, the settings, and for the first perfect dodge what happened step by step. If a game
  update or another mod stops the mod, the log has the message it showed.
