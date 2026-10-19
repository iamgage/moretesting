# Hitbloq for Quest (Beat Saber 1.40.8)

Standalone C++ port of the core of the PC Hitbloq mod.

## Build
Push this folder to a GitHub repo -> Actions -> "Build Hitbloq qmod" -> download the `Hitbloq.qmod` artifact.
Or locally: `qpm restore && qpm s qmod`.

## Config
`/sdcard/ModData/com.beatgames.beatsaber/Configs/Hitbloq.json` (created on first launch):
- `playerId`: your BeatLeader/ScoreSaber id. Leave empty to auto-detect from the Meta account.
- `accountType`: `auto` (default, same as the PC mod: ScoreSaber if its mod is installed, otherwise BeatLeader; the other one is used as a fallback), `scoresaber`, or `beatleader`. Only matters for registering a new Hitbloq account.

- `posX`, `posY`, `posZ`, `rotY`: where the panel appears (defaults put it on the right, where the vanilla leaderboard sits). You can also grab its handle in game.
- `playlistDir`: where playlists are saved (default is PlaylistManager's folder).

## Use
Select a map. The Hitbloq panel appears (grab the handle to move it). Buttons: Pool / Global-Around Me / Refresh.
After a cleared run the score is refreshed automatically (BeatLeader or ScoreSaber must upload it first).

## Playlists
- **Playlist** button: saves the playlist of the currently selected pool.
- **Browse** button: lists every Hitbloq pool (6 per page). Tap 1-6 to save that pool's playlist, `<` `>` change page.
Playlists are saved to PlaylistManager's folder as `Hitbloq_<pool>.bplist`. Restart the game, then use PlaylistManager's download icon on the playlist to fetch the missing songs.
