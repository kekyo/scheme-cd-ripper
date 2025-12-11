# Scheme CD music/sound ripper

Scheme CD Ripper is a linux CLI tool that rips audio CDs to FLAC.

[![Project Status: Active – The project has reached a stable, usable state and is being actively developed.](https://www.repostatus.org/badges/latest/active.svg)](https://www.repostatus.org/#active)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Packages are available in (Debian/Ubuntu): https://github.com/kekyo/scheme-cd-ripper/releases

----

[(Japanese language is here/日本語はこちら)](./README_ja.md)

## What is this?

Scheme CD Ripper is a linux CLI tool that rips audio CDs to FLAC, automatic fetches metadata from multiple CDDB servers and inserts tags into FLAC file.

This workflow is designed for processing large numbers of CDs continuously, for archiving usage.

### Features

- Encode and save audio tracks as FLAC while performing music stream integrity checks (with `cd-paranoia`).
- Reads the disc TOC, queries multiple CDDB servers and MusicBrainz. Merges all matches, and prompts you to pick a candidate.
- Inserts Vorbis comments (ID3 like tags in FLAC format) automatically. Furthermore, if a cover art image exists, it can be automatically embedded.
- File names and directories can be automated using your specified format with tags.
- And since it uses GNOME GIO (GVfs) for file output, you can output directly to a NAS or similar device using a URL.
- Supports continuous mode for efficient operation of multiple CDs.

### Example session

```bash
$ cdrip

Scheme CD music/sound ripper
Copyright (c) Kouji Matsui (@kekyo@mi.kekyo.net)
https://github.com/kekyo/cd-ripper
Licence: Under MIT.

Detected CD drives:
  [1] /dev/cdrom (media: present)
  [2] /dev/sr1 (media: none)
Select device [1-2] (default first with media, otherwise 1): 

Using device: /dev/cdrom (media: present)
Checking /dev/cdrom for cdrom...
                CDROM sensed: PIONEER  BD-RW   BDR-XD05 3.10 SCSI CD-ROM

Verifying drive can read CDDA...
        Expected command set reads OK.

Attempting to determine drive endianness from data........
        Data appears to be coming back Little Endian.
        certainty: 100%

Options:
  device      : "/dev/cdrom"
  format      : "{album}/{tracknumber:02d}_{safetitle}.flac"
  compression : 8 (auto)
  mode        : default (best - full integrity checks)

CDDB disc id: 1403e605
Fetcing from CDDB servers ...

[1] BarlowGirl - For the Beauty of the Earth (Studio Series) (via freedb (japan))
[2] Bomani "D'mite" Armah - Read a Book Single (via freedb (japan))
[3] Stellar Kart - Angel In Chorus (Studio Series) (via freedb (japan))
[4] Disney - Shanna (via dbpoweramp)
[5] Ladina - Verbotene Liebe (via dbpoweramp)
[6] Across The Sky - Found By You [Studio Series]  (2003) (via dbpoweramp)
[7] Bomani "D'mite" Armah - Read a Book Single (via dbpoweramp)
[8] Cuba Libre - Sierra Madre (via dbpoweramp)
[9] Big Daddy Weave - You're Worthy Of My Praise(Studio Series) (via dbpoweramp)
[10] BarlowGirl - For the Beauty of the Earth (Studio Series) (via dbpoweramp)
[11] Crossroads - Unknown (via dbpoweramp)
[12] Stellar Kart - Angel In Chorus (Studio Series) (via dbpoweramp)
[13] Tigertown - Wandering Eyes EP (via dbpoweramp)
[14] Jerry Smith - Twinkle Tracks (via dbpoweramp)
[15] DONALDO 22 - DONALDO22 (via dbpoweramp)
[0] (Ignore all, not use these tags)

Select match [0-15] (default 1): 3

Start ripping...

Track 1/5 [ETA: 13:19]: "Angel In Chorus (LP Version)" [==============================]
Track 2/5 [ETA: 09:59]: "Angel In Chorus (original key performance with background vocals)" [==============================]
Track 3/5 [ETA: 06:39]: "Angel In Chorus (low key performance without background vocals)" [==============================]
Track 4/5 [ETA: 03:19]: "Angel In Chorus (medium key without background vocals (original key)" [==============================]
Track 5/5 [ETA: 00:00]: "Angel In Chorus (high key without background vocals)" [==============================]

Done.
```

-----

## Installation

For Debian (bookworm) / Ubuntu (noble, jammy), [prebuilt binaries are available here](https://github.com/kekyo/scheme-cd-ripper/releases).
There are two packages available (`cdrip.deb`, `libcdrip-dev.deb`), but if you only need to use `cdrip` command, installing just the first one is sufficient.
The second one is an API library for C language when you want to use this feature.

For environments other than those listed above, you can build it yourself. In that case, please refer to [Self Building](https://github.com/kekyo/scheme-cd-ripper#self-building).

## CLI Usage

The default options are configured for easy use of cdrip.
Of course, you can adjust them to your preferences as follows:

```bash
cdrip [-d device] [-f format] [-m mode] [-c compression] [-s] [-r] [-n] [-a] [-i config] [-u file|dir ...]
```

- `-d`, `--device`: CD device path (`/dev/cdrom` or others). If not specified, it will automatically detect available CD devices and list them.
- `-f`, `--format`: FLAC destination path format. using tag names inside `{}`, tags are case-insensitive. (default: `{album}/{tracknumber:02d}_{safetitle}.flac`)
- `-m`, `--mode`: Integrity check mode: `best` (full integrity checks, default), `fast` (disabled any checks)
- `-c`, `--compression`: FLAC compression level (default: `auto` (best --> `8`, fast --> `1`))
- `-s`, `--sort`: Sort CDDB results by album name on the prompt.
- `-r`, `--repeat`: Prompt for next disc after finishing.
- `-n`, `--no-eject`: Keep disc in the drive after ripping finishes.
- `-a`, `--auto`: Enable fully automatic mode (without any prompts).
  It picks the first drive that already has media, chooses the top CDDB match an entry with cover art are prioritized, and loops in repeat mode without prompts.
- `-i`, `--input`: cdrip config file path (default search: `./cdrip.conf` --> `~/.cdrip.conf`)
- `-u`, `--update <file|dir> [more ...]`: Update existing FLAC tags from CDDB using embedded tags (other options ignored)

All command-line options (except `-u` and `-i`) can override the contents of the config file specified with `-i`.

TIPS: If you want to import a large number of CDs continuously with MusicBrainz tagging, you can do so by specifying the `cdrip -a -r` option.

## Vorbis comments

The following Vorbis comments (ID3 like tags in FLAC) are automatically inserted into FLAC file:

|Key|Description|Source|
|:----|:----|:----|
|`title`|Music/song/sound title|CDDB|
|`artist`|Artist name(s)|CDDB|
|`album`|Album name|CDDB|
|`genre`|Genre|CDDB|
|`date`|Date (Non-formal format)|CDDB|
|`tracknumber`|Track number|internal|
|`tracktotal`|Number of tracks per this disc|internal|
|`albumartist`|Album artist|MusicBrainz|
|`discnumber`|Disc number (position)|MusicBrainz|
|`disctotal`|Total discs in release|MusicBrainz|
|`media`|Medium format (CD etc.)|MusicBrainz|
|`releasecountry`|Release country code|MusicBrainz|
|`releasestatus`|Release status|MusicBrainz|
|`label`|Label name(s)|MusicBrainz|
|`catalognumber`|Catalog number(s)|MusicBrainz|
|`isrc`|ISRC (if present)|MusicBrainz|
|`cddb`|Fetched CDDB server name|internal|
|`cddb_date`|CDDB fetched timestamp (ISO form)|internal|
|`cddb_discid`|CDDB disc ID (Required for re-fetching from CDDB server)|internal|
|`cddb_offsets`|Track start offsets (Required for re-fetching from CDDB server)|internal|
|`cddb_total_seconds`|Disc length in seconds (Required for re-fetching from CDDB server)|internal|
|`musicbrainz_release`|Release MBID (Primary key for MusicBrainz)|MusicBrainz|
|`musicbrainz_medium`|Medium MBID (Primary key for MusicBrainz)|MusicBrainz|
|`musicbrainz_releasegroupid`|Release group MBID|MusicBrainz|
|`musicbrainz_trackid`|Track MBID|MusicBrainz|
|`musicbrainz_recordingid`|Recording MBID|MusicBrainz|
|`musicbrainz_discid`|MusicBrainz disc ID (Partial use, will remove when fetch succeed)|internal|
|`musicbrainz_leadout`|MusicBrainz CD leadout time (Partial use, will remove when fetch succeed)|internal|

When obtaining information from CDDB or MusicBrainz, not all of this tag information may be available.

Note: There's no need to worry. While Vorbis comments are typically written in uppercase, this document simply uses lowercase.

## About MusicBrainz and tags

- [MusicBrainz](https://musicbrainz.org/) is a community-maintained music database that provides structured IDs, credits, genres, and release metadata.
- CDDB servers primarily return text fields like track titles, while MusicBrainz returns precise release-level metadata and stable IDs, improving tagging accuracy.
- In Scheme CD ripper, simply include `musicbrainz` in the `[cddb]` `servers` list to enable it; it is already included in the default server list.
- Fetching and embedding cover art [(via Cover Art Archive)](https://coverartarchive.org/) is only possible when a MusicBrainz match is used; other CDDB servers do not supply cover art.

## Filename formatting

The filename format is a template for any path, including directory names, that uses curly braces to automatically and flexibly determine the path using Vorbis comment key names.

For example:

- `"{album}/{tracknumber:02d}_{safetitle}.flac"`: This is the default definition and should be appropriate in most cases. It creates subdirectories named after the album and places FLAC files within them, each named with the track number and track title.
- `"store/to/{safetitle}.flac"`: Of course, you can also add a base path and always store it within that.
- `"smb://nas.yourhome.localdomain/smbshare/music/{safetitle}.flac"`: Scheme CD ripper supports GNOME GIO, so you can also specify a URL to save directly to a remote host (Required GVfs configuration.)

In addition to Vorbis comment keys, the following dedicated keys can also be used in filename formatting:

|Key|Description|
|:----|:----|
|`safetitle`|Truncate at newline, trim trailing and replace unsafe characters|

Note: These are not stored in the FLAC file and can only be used in the filename format.

Additionally, it includes the following features:

- You can zero-pad two digit numbers with `:02d` etc.
  This is similar to C language's `printf` format specifiers, but it only supports this format.
  e.g. `"{tracknumber:02d}.flac"`.
- If the format contains directories, they will be created automatically.
- The `.flac` extension is appended automatically if you omit it.

### Update existing FLACs using embedded CDDB tags

`-u`/`--update` lets you refresh metadata on ripped FLACs without the original CD:

```bash
# Single file
cdrip -u album/01_track.flac

# Multiple paths (files or directories; directories are searched recursively for *.flac)
cdrip -u album1 album2/track03.flac /path/to/archive
```

Requirements: FLAC files must contain these tags (These tags are automatically inserted if you rip using the Scheme CD ripper):

- Re-fetches from CDDB server: `cddb_discid`, `cddb_offsets` and `cddb_total_seconds`.
- Re-fetches from MusicBrainz (first time): `musicbrainz_discid`, `cddb_offsets` and `musicbrainz_leadout`.
- Re-fetches from MusicBrainz (not first time): `musicbrainz_release` and `musicbrainz_medium`.

CDDB candidates are fetched the same way as during ripping; you still select the desired match interactively (except auto mode.)

## Config file format

Scheme CD ripper will refer config file. It is INI-like format.

`cdrip.conf` (current directory) --> `~/.cdrip.conf`: The first file found in this order is loaded.
You can also explicitly specify the file using the `-i`/`--input` option.

Example config file:

```ini
[cdrip]
device=/dev/cdrom
format={album}/{tracknumber:02d}_{safetitle}.flac
compression=auto     # auto, 0-8
mode=best            # best / fast / default
repeat=false
sort=false
auto=false

[cddb]
servers=musicbrainz,freedb_japan,gnudb,dbpoweramp   # Comma separated labels

[cddb.gnudb]
label=gnudb
host=gnudb.gnudb.org
port=80
path=/~cddb/cddb.cgi

[cddb.dbpoweramp]
label=dbpoweramp
host=freedb.dbpoweramp.com
port=80
path=/~cddb/cddb.cgi

[cddb.freedb_japan]
label=freedb (japan)
host=freedbtest.dyndns.org
port=80
path=/~cddb/cddb.cgi
```

Server IDs are defined in the `[cddb.<server_id>]` section, and queries are made in the order specified under `servers` in the `[cddb]` section.
If no servers are configured, the built-in three servers (musicbrainz, gnudb, and dbpoweramp) are used.

A special server id `musicbrainz` is not required `[cddb.musicbrainz]` section definitions.

-----

## Self Building

### Dependencies

- libcdio-paranoia
- libcddb
- libFLAC++
- GNOME GIO
- libsoup 3.0
- json-glib
- CMake and a C++17 compiler
- dpkg-dev (for `dpkg-shlibdeps` when building packages)
- Node.js and [screw-up](https://github.com/kekyo/screw-up) (Automated-versioning tool)
- cowbuilder (deb package building)

### Build

In Ubuntu 22.04/24.04:

```bash
sudo apt-get install build-essential cmake dpkg-dev nodejs \
  libcdio-paranoia-dev libcddb2-dev libflac++-dev libglib2.0-dev libsoup-3.0-dev libjson-glib-dev
npm install -g screw-up

./build.sh
```

### Build packages

`build_package.sh` runs `build.sh` inside a cowbuilder chroot with qemu-user-static to target one distro/arch per call. Run it repeatedly for all combinations you need.

Prerequisites:

```bash
sudo apt-get install cowbuilder qemu-user-static debootstrap systemd-container
```

Build examples:

```bash
# Ubuntu noble / amd64
./build_package.sh --distro ubuntu --release noble --arch x86_64

# Debian bookworm / arm64
./build_package.sh --distro debian --release bookworm --arch arm64
```

Notes:
- Arch aliases: `x86_64|amd64`, `i686|i386`, `armv7|armhf`, `aarch64|arm64`
- Debug build: add `--debug` (passes `-d` to `build.sh`)
- Refresh chroot: add `--refresh-base`
- Outputs: `artifacts/<distro>-<release>-<arch>/*.deb`

Batch build for all predefined combos:

```bash
# ubuntu noble/jammy × amd64/i386/armhf/arm64
# debian bookworm × amd64/i386/armhf/arm64
./build_package_all.sh            # reuse existing bases
./build_package_all.sh --refresh-base  # rebuild bases then build all
```

-----

## Note

In this software, "CDDB" does not refer to the terminology of a specific product, but rather to "the CD metadata database."

## License

Under MIT.
