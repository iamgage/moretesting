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

## Leaderboard switch arrow
When a map is ranked on Hitbloq, a `Hitbloq >` arrow appears above the leaderboard (its position is found at runtime). Press it to show the Hitbloq panel over the leaderboard, and `< Leaderboard` to go back. If it floats too high or low, change `arrowUp` (metres) in `Hitbloq.json`. The panel has Pool / Global-Near / Refresh / Playlist buttons.

## Playlists
- **Mods menu (left of the main menu) -> Hitbloq Playlists**: lists every Hitbloq pool, 6 per page. Tap 1-6 to save that pool's playlist, `<` `>` change page.
- **Playlist** button on the panel: saves the playlist of the currently selected pool.
Playlists are saved to PlaylistManager's folder as `Hitbloq_<pool>.bplist`. Restart the game, then use PlaylistManager's download icon on the playlist to fetch the missing songs.
