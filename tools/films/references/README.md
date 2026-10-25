# Film references

A snapshot of `private/films/` (2026-10-25): the reference checksums of
the nine campaign films in `library.txt`, all at 60 ticks per second, with
the masks they were learned with. `learn_film.sh` and `run_library.sh`
keep working on `private/films/`; copy these there to restore them:

    cp tools/films/references/* private/films/

- `<name>.hash.txt`: one checksum per game tick of a full replay (the
  reference).
- `<name>.mask`: the cosmetic state left out of the checksum.
- `<name>.rules.log`: the rules each learning round added.
- `library.txt`: the films' Theater indexes and lengths.

The checksums are only good for the films they were made from, which stay
local with the rest of the game data (`private/films_data/hdd.pristine`,
Theater's saved films): anyone else needs their own films and references.
