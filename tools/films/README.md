# Film tests

Saved Theater films replay a game from its recorded inputs, so a film is a
test: play it, checksum the game state every tick, and compare against the
checksums of the recompiled original. This is the oracle for decompilation
(docs/ROADMAP.md 6.1): a replaced function that changes any tick's checksum
fails at that tick.

- `run_film.sh <out dir> [--category n] [--index n] [--mask file]
  [--record] [--seconds n]`: plays one film from the Theater lobby and
  writes `hash.txt` (one checksum per game tick). Run it without arguments
  for the details.
- `compare_hashes.py <reference> <run>`: compares two `hash.txt` over the
  film's level; exits 1 and prints the first differing tick if any differs.
- `learn_mask.py learn|check|dumps`: learns which cosmetic state to leave
  out of the checksum from recorded replays (below), or from raw state
  dumps (`run_film.sh --dump`).
- `learn_film.sh <name> <index> <film seconds>`: "Adding a film" below,
  automated for full-length films: two full replays, then records just the
  ticks around the first difference (`H3_STATE_RECORD_TICKS`), learns, and
  repeats until the replays match. When the records all agree (recording
  slows a replay, and some races only go both ways at full speed), it
  dumps raw states in those stretches from unrecorded replays and learns
  from the dumps instead. Keeps `private/films/<name>.mask`,
  `<name>.hash.txt` (the reference) and `<name>.rules.log` (what each round
  added, to read). `--jobs 2` runs the full pair and the records two at a
  time, each in its own data directory (`H3_FILM_DATA_SLOTS`, default
  `private/films_data:private/films_data2`; the game writes to `hdd/` and
  `shadercache/`, so they can't be shared). A parallel pair that matches is
  confirmed by one more replay run alone at full speed; dumps always run
  alone.
- `run_library.sh <out dir> [--native]`: every film in
  `private/films/library.txt` against its reference (and with
  `H3_CHECK_NATIVE`): the regression test for a changed build.

A snapshot of the references and masks is kept in `references/`.

Test runs use `H3_FILM_DATA` (`private/films_data`: links to the game files
plus a copy of `hdd/`), so they never write to the real save data. A run
stops by itself when the film is over (the tick stops advancing).

## Why replays need help to match

Out of the box, two replays of one film differ in about 700 of the game
state's million words on every tick, without the game itself diverging
(the same objects, positions and AI throughout):

- The game state sits at a random offset in its memory (anti-tamper
  placement seeded from the clock), so every pointer into it differs.
  `H3_RANDOM_SEED` fixes the seeds (hooks/random_seed.cpp).
- Cosmetic randomness (effects, particles) draws from a "local" seed that
  the main thread steps per rendered frame and a particle thread steps
  concurrently, and the game runs a varying number of ticks per frame.
  `H3_RANDOM_SEED` also gives the particle thread its own seed and resets
  the local one when a level starts; `H3_LOCKSTEP` runs exactly one tick per
  frame (hooks/tick_rate.cpp).
- What's left depends on when asynchronous loads finish: effects, the HUD,
  screen effects, and a few fields of some objects (probably sound-driven,
  such as a biped's lip sync). That is masked out of the checksum by
  meaning (data array names, object definition tag + field offset), not by
  address, so it holds for every tick of the film.

`run_film.sh` always sets `H3_RANDOM_SEED` and `H3_LOCKSTEP`.

## Adding a film

Save the film to the library in Theater (Recent Films only keeps the last
few), copy `hdd/` into `private/films_data/`, add the film to
`private/films/library.txt` (indexes are newest first: saving a film moves
the others down) and run `learn_film.sh`. By hand, per film:

```sh
F=private/films/run          # any scratch directory
OPTS="--category 3 --index 0 --seconds 400"   # where the film is, how long it runs
# 1. Record a few replays (each record.bin is ~90 KB per tick: ~300 MB
#    for a one-minute film; delete them once learned).
for r in 1 2 3 4 5 6; do tools/films/run_film.sh $F/rec$r $OPTS --record; done
# 2. Learn the mask, each replay against the first.
tools/films/learn_mask.py learn private/films/<name>.mask $F/rec*/record.bin
# 3. Check it: two fresh replays must match on every tick.
for r in a b; do tools/films/run_film.sh $F/chk$r $OPTS --mask private/films/<name>.mask; done
tools/films/compare_hashes.py $F/chka/hash.txt $F/chkb/hash.txt
```

Read the learned rules before trusting a mask: a rule hides whatever it
names in every tick. Everything learned so far is cosmetic, but a rule in
an array or object field that gameplay uses means the replays really
diverged, which needs fixing rather than masking. If step 3 still finds
differing ticks, record two more replays and `learn ... --known <mask>`.

Keep the reference `hash.txt` of the recompiled build next to the mask:
that's what a changed build is compared against.

## Limits

- Navigation is scripted by frame number from the main menu with the
  cursor on THEATER (`H3_FILM_PREFS`: a `preferences.dat` that leaves it
  there). Recent Films (category 0) and Campaign (category 3, films saved
  to the library) have been checked; the others should work the same way.
- Learned on Sierra 117 only. Data array names and object tags come from
  the level, so each film needs its own mask.
- Physics (Havok) keeps its own state outside the game state; it shows up
  in the checksum through object positions, which are copied from it every
  tick.
