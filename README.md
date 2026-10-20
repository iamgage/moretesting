# Hitbloq for Quest (Beat Saber 1.40.8)

Standalone C++ port of the core of the PC Hitbloq mod.

## Build
Push this folder to a GitHub repo -> Actions -> "Build Hitbloq qmod" -> download the `Hitbloq.qmod` artifact.
Or locally: `qpm restore && qpm s qmod`.

## Config
`/sdcard/ModData/com.beatgames.beatsaber/Configs/Hitbloq.json` (created on first launch):
- `playerId`: your BeatLeader/ScoreSaber id. Leave empty to auto-detect from the Meta account.
- `accountType`: `auto` (default, same as the PC mod: ScoreSaber if its mod is installed, otherwise BeatLeader; the other one is used as a fallback), `scoresaber`, or `beatleader`. Only matters for registering a new Hitbloq account.

- `posX`, `posY`, `posZ`, `rotY`: fallback position used only if the real leaderboard can't be found. `arrowUp`: height of the switch arrow above the leaderboard.
- `playlistDir`: where playlists are saved (default is PlaylistManager's folder).
- `autoOpen` (default `true`): show the Hitbloq leaderboard automatically on Hitbloq-ranked maps. The `<` arrow switches back to the normal leaderboard and the choice is remembered, like the PC mod. Set to `false` to only open it with the `>` arrow.

## Use
Select a map. The Hitbloq panel appears (grab the handle to move it). Buttons: Pool / Global-Around Me / Refresh.
After a cleared run the score is refreshed automatically (BeatLeader or ScoreSaber must upload it first).

## Leaderboard switch arrows (like the PC mod)
On a Hitbloq-ranked map a `>` arrow appears on the right of the leaderboard. It shows the Hitbloq panel over the leaderboard; a `<` arrow on the left switches back. The leaderboard's position is found at runtime. If the arrows float too high/low/wide, change `arrowUp` / `arrowSide` (metres) in `Hitbloq.json`.

## Pool browser (Mods menu)
Left of the main menu -> **Hitbloq**. Pick a pool with 1-6 (`<` `>` change page), `Sort` cycles Popularity / Name / Ranked maps, and **GO TO PLAYLIST!** saves the selected pool's playlist to PlaylistManager's folder as `Hitbloq_<pool>.bplist`. Restart the game, then use PlaylistManager's download icon to fetch the missing songs. The **Playlist** button on the panel does the same for the pool of the selected map.

## Source tabs (0.4.0)
The panel has three tabs on top: **Hitbloq / BeatLeader / ScoreSaber**. BeatLeader and ScoreSaber show the global top 10 for any custom map (no account needed); Hitbloq only exists for Hitbloq-ranked maps. The `>` arrow now appears on every custom map, the panel opens by itself only on Hitbloq-ranked maps (`autoOpen`). Pool / Global-Near / Refresh apply to the Hitbloq tab.

## Changes in 0.3.1
- The global leaderboard no longer waits for the Hitbloq account check, so it shows even when the player id could not be detected (only the rank line, Near Me and auto-refresh need an id).
- Opens automatically on ranked maps (`autoOpen`), and re-finds the real leaderboard position whenever the view changes instead of only once.
- Column header row, and clearer error messages. More log lines: `map info: http .. pools`, `board loaded for pool ..`.

## Troubleshooting
Logs are written by Paper to `/sdcard/ModData/com.beatgames.beatsaber/logs2/` (or use MBF's Logs button). Expected lines on startup: `late_load start`, `Mods menu entry registered`, `Hitbloq loaded`. When you pick a map: `level detail refreshed`, `panel created`, `leaderboard at ...`.
