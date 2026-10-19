# Hitbloq for Quest (Beat Saber 1.40.8)

Standalone C++ port of the core of the PC Hitbloq mod.

## Build
Push this folder to a GitHub repo -> Actions -> "Build Hitbloq qmod" -> download the `Hitbloq.qmod` artifact.
Or locally: `qpm restore && qpm s qmod`.

## Config
`/sdcard/ModData/com.beatgames.beatsaber/Configs/Hitbloq.json` (created on first launch):
- `playerId`: your BeatLeader/ScoreSaber id. Leave empty to auto-detect from the Meta account.
- `accountType`: `auto` (default, same as the PC mod: ScoreSaber if its mod is installed, otherwise BeatLeader; the other one is used as a fallback), `scoresaber`, or `beatleader`. Only matters for registering a new Hitbloq account.

## Use
Select a map. The Hitbloq panel appears (grab the handle to move it). Buttons: Pool / Global-Around Me / Refresh.
After a cleared run the score is refreshed automatically (BeatLeader or ScoreSaber must upload it first).
