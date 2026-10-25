# PitchRide

**DJ back to back with a friend, online.**

PitchRide is a free DJ app for Mac and Windows. Open a room, send a friend
its code, and you're both on the decks of the same booth from wherever you
are: each of you hears the whole mix, live, and either of you can take the
next track.

**Get it at [pitchride.co.uk/download](https://pitchride.co.uk/download).**

## What it does

- **Rooms.** Host a public room anyone can see, or a private one only people
  with its code can join. Chat with whoever's in it.
- **Both on the decks.** Each of you plays your own two decks, or one of you
  plays all four while the other takes over when it's their turn.
- **Your own music.** If your friend loads a track you don't have, it's sent
  to you automatically, with its title, key and beat grid.
- **Controllers.** Plug in a DJ controller: popular ones are set up for you,
  and a mapping wizard sets up the rest by asking you to press each control.
- **Private by design.** The two apps talk to each other directly, and
  everything between them is encrypted. When a direct connection isn't
  possible, it goes through our relay, still encrypted end to end.
- **Updates itself.** When there's a new version, it downloads it and asks
  before installing.

## Built on Mixxx

PitchRide is built on [Mixxx](https://mixxx.org), the free and open-source DJ
software, and keeps its licence: the GNU General Public License, version 2 or
later. Thank you to everyone who has made Mixxx. Mixxx's full credits are in
the app under **? → About PitchRide**.

## This repository

This is the source code of each released version of PitchRide. We develop
in private and publish each version here when it's released, as one commit.

The online part (rooms, the connection between the apps, track sharing) is
in **[pitchride-core](https://github.com/b2b-sessions/pitchride-core)**.

We don't take pull requests here yet. Questions, bugs or ideas: use
**? → Report a bug or feedback** in the app, or email
[pitchrideenquiries@gmail.com](mailto:pitchrideenquiries@gmail.com).

## Building it

PitchRide builds like Mixxx 2.5: see Mixxx's
[build instructions](https://github.com/mixxxdj/mixxx/wiki/Compiling-on-Linux)
for your system. In addition:

1. Put pitchride-core in `lib/b2b-core`:

   ```bash
   git clone https://github.com/b2b-sessions/pitchride-core.git lib/b2b-core
   ```

2. PitchRide's skin artwork is licensed to us separately and isn't in this
   repository. Point `-DB2B_SKINS_DIR` at a folder of skins (one folder per
   skin) to build with your own.

## Licence

PitchRide is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License, version 2 or (at your option) any
later version. See [LICENSE](LICENSE) and [COPYING](COPYING).

PitchRide is not made by or affiliated with the Mixxx team. "PitchRide" and
its logo are PitchRide's own; please don't use them for other software.
