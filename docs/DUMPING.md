# Dumping Halo 3

This project ships no game data. You need your **own** copy of Halo 3 for
Xbox 360. Downloading ISOs you don't own is piracy, and pirated dumps are also
a bad base for this work: you can't trust where they came from or what was
changed in them.

The file the recompiler needs is `default.xex`, the game's main executable,
from the root of the disc. The rest of the disc (the `maps/` folder, etc.) is
needed later, once the runtime starts loading content.

## Option A: a modded Xbox 360 (most common)

Consoles with an RGH or JTAG mod can read the disc and copy it to a PC.

1. Put the disc in the console.
2. In a homebrew dashboard (Aurora, FSD) or a file manager like XeXMenu, copy
   the disc contents to the hard drive. You can also rip it to an ISO.
3. Move the files to your PC over FTP (Aurora has an FTP server built in) or
   with a USB drive.

RGH works on almost every 360 motherboard revision. It means modifying the
console's hardware, so look up a current guide for your board first.

## Option B: a PC DVD drive with custom firmware

360 discs use the XGD2 format, which normal PC drives can't fully read.
Certain older Lite-On/Samsung/Hitachi drives can rip them after being flashed
with Kreon-style firmware. Which models work, and with which tools, is
documented in the 360 preservation community. A good dump comes with
checksums you can verify against known-good records (Redump has hash records
for retail discs).

## Getting `default.xex` out of an ISO

```sh
python3 tools/xdvdfs_extract.py Halo3.iso private/game
cp private/game/default.xex private/
```

`tools/xdvdfs_extract.py` reads full XGD2 images (video + game partition) and
plain XISOs. Pass `--list` to print the file tree without extracting.
`extract-xiso` and `xdvdfs` also work.

You don't need to decrypt or decompress the XEX yourself. XenonRecomp does
that when it loads the file.

## Record which executable you have

There are several retail pressings, and every title update changes the code.
Every address in our config is tied to one exact executable:

```sh
sha1sum private/default.xex
```

Write the hash in the project notes, so everyone working on the project knows
they're on the same build.

### Reference build

The addresses in `H3RecompLib/config/halo3.toml` are for this executable:

| Field      | Value                                      |
|------------|--------------------------------------------|
| SHA-1      | `927a8c49062a55179d84116e33f8a5a0b3db13c1` |
| Title ID   | `4D5307E6`                                 |
| Media ID   | `699E0227`                                 |
| Version    | `0.0.0.42` (disc, no title update)         |
| Image      | base `0x82000000`, size `0x012A0000`       |
| Entry      | `0x82582A98`                               |

## Title updates (later)

Start with the disc executable without updates. It's one fewer moving part.
Title updates (`.xexp` patches inside a TU package) can be added later. A
modded console that has downloaded the update keeps it in its cache
partition, and tools like Velocity can open the package. Then uncomment the
`patch_file_path` lines in `H3RecompLib/config/halo3.toml`.
