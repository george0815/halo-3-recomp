---
title: Dumping your disc
nav_order: 8
---

# Dumping Halo 3

This project ships no game data. You need your **own** copy of Halo 3 for
Xbox 360, copied ("dumped") into a disc image. Downloading ISOs you don't
own is piracy, and pirated dumps are a bad base for this work anyway: you
can't trust where they came from or what was changed in them.

The installer takes the disc image (`.iso`) as it is: there's nothing to
extract or decrypt yourself.

## Option A: a modded Xbox 360 (most common)

Consoles with an RGH or JTAG mod can read the disc and copy it to a PC.

1. Put the disc in the console.
2. Rip it to an ISO with a homebrew dashboard (Aurora, FSD) or a file
   manager like XeXMenu.
3. Move the ISO to your PC over FTP (Aurora has an FTP server built in) or
   with a USB drive.

RGH works on almost every 360 motherboard revision. It means modifying the
console's hardware, so look up a current guide for your board first.

## Option B: a PC DVD drive with custom firmware

360 discs use the XGD2 format, which normal PC drives can't fully read.
Certain older Lite-On, Samsung and Hitachi drives can rip them after being
flashed with Kreon-style firmware; the 360 preservation community documents
which models work and with which tools. A good dump comes with checksums
you can compare with known-good records (Redump has records for retail
discs).

## Which version you have

Only the original disc release is supported for now. The installer checks
this for you (its first step hashes the game's executable) and tells you if
your disc is a different version. Title updates aren't supported yet.

| Field      | Value                                      |
|------------|--------------------------------------------|
| SHA-1      | `927a8c49062a55179d84116e33f8a5a0b3db13c1` (`default.xex`) |
| Title ID   | `4D5307E6`                                 |
| Media ID   | `699E0227`                                 |
| Version    | `0.0.0.42` (disc, no title update)         |
| Image      | base `0x82000000`, size `0x012A0000`       |
| Entry      | `0x82582A98`                               |

## For developers: the files in `private/`

Building from source ([Building](building.html)) needs the disc's files in
`private/game/` and the executable in `private/`:

```sh
python3 tools/xdvdfs_extract.py Halo3.iso private/game
cp private/game/default.xex private/
sha1sum private/default.xex    # should match the table above
```

`tools/xdvdfs_extract.py` reads full XGD2 images (video and game
partitions) and plain XISOs; `--list` prints the file tree without
extracting. `extract-xiso` and `xdvdfs` work too. You don't need to decrypt
or decompress the XEX yourself: XenonRecomp does that when it loads it.

Every address in the recompiler's config (`H3RecompLib/config/halo3.toml`)
is tied to this one executable, which is why other pressings and title
updates need work of their own. Title updates (`.xexp` patches inside a TU
package) can be added later: a modded console that has downloaded the update
keeps it in its cache partition, and tools like Velocity can open the
package. The `patch_file_path` lines in `halo3.toml` are there for it.
