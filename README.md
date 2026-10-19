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

## Use
Select a map. The Hitbloq panel appears (grab the handle to move it). Buttons: Pool / Global-Around Me / Refresh.
After a cleared run the score is refreshed automatically (BeatLeader or ScoreSaber must upload it first).

## Leaderboard switch arrows (like the PC mod)
On a Hitbloq-ranked map a `>` arrow appears on the right of the leaderboard. It shows the Hitbloq panel over the leaderboard; a `<` arrow on the left switches back. The leaderboard's position is found at runtime. If the arrows float too high/low/wide, change `arrowUp` / `arrowSide` (metres) in `Hitbloq.json`.

## Pool browser (Mods menu)
Left of the main menu -> **Hitbloq**. Pick a pool with 1-6 (`<` `>` change page), `Sort` cycles Popularity / Name / Ranked maps, and **GO TO PLAYLIST!** saves the selected pool's playlist to PlaylistManager's folder as `Hitbloq_<pool>.bplist`. Restart the game, then use PlaylistManager's download icon to fetch the missing songs. The **Playlist** button on the panel does the same for the pool of the selected map.

## Troubleshooting
Logs are written by Paper to `/sdcard/ModData/com.beatgames.beatsaber/logs2/` (or use MBF's Logs button). Expected lines on startup: `late_load start`, `Mods menu entry registered`, `Hitbloq loaded`. When you pick a map: `level detail refreshed`, `panel created`, `leaderboard at ...`.
