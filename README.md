# sdi-probe

A single-binary command-line probe for **Blackmagic DeckLink** capture cards on Linux.

When an SDI feed "doesn't work", the hard part is finding *where* the chain breaks: driver,
device enumeration, cable, video format, or the capture itself. `sdi-probe` walks that chain in
order, prints a verdict at each step, and can record a few seconds of raw video so you can look
at the picture yourself.

No ffmpeg, no encoder application, no desktop session - copy the binary onto a headless server
and run it. `ffmpeg -f decklink -list_devices 1 -i dummy` is the usual advice, but distro ffmpeg
builds almost never enable DeckLink (it is nonfree), and vendor GUIs need a display.

It also separates two things that are easy to conflate: what the card *supports* (listed by any
tool, even with no cable attached) versus whether a signal is *present*
(`bmdDeckLinkStatusVideoInputSignalLocked`, which is what this checks).

## Build

Requires the Blackmagic DeckLink SDK, which is proprietary and **not** included here. Download it
from <https://www.blackmagicdesign.com/developer/> and unpack it into `sdk/`:

```
sdk/
  ACTIVE                 <- one line: the folder name to build against
  DeckLink_SDK_12_4_1/   <- DeckLinkAPI.h, DeckLinkAPIDispatch.cpp, ...
  DeckLink_SDK_11_5_1/   <- keep as many versions side by side as you like
```

`sdk/ACTIVE` sets the default version and is the one file to edit to switch. `DeckLink_SDK_*`
folders are gitignored, so an unpacked SDK cannot be committed by accident.

```bash
./build.sh                        # version named in sdk/ACTIVE
SDK_VERSION=11_5_1 ./build.sh     # another folder in sdk/, by suffix
SDK=/opt/decklink-sdk ./build.sh  # an SDK from anywhere
OUTPUT=/tmp/sdi-probe ./build.sh  # different output path
```

The build records what it used, so a build log always says what the binary was built against:

```
SDK      : ./sdk/DeckLink_SDK_12_4_1
version  : 12.4.1   (selected via ACTIVE file)
compiler : g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
to change: edit ./sdk/ACTIVE, or run  SDK_VERSION=<suffix> ./build.sh  |  SDK=<path> ./build.sh
```

## Usage

```
sdi-probe info [-v]        driver, SDK, devices, signal lock  (default command)
sdi-probe capture [opts]   actually read frames and prove it works
sdi-probe --help
```

| Capture option | Meaning |
|---|---|
| `--device <N>` | device index, as reported by `info` (default 0) |
| `--seconds <N>` | capture duration (default 5) |
| `--out <file>` | write raw frames to a file playable with ffmpeg |
| `--mode <code>` | force a display mode (e.g. `Hi50`); omit to auto-detect |
| `--audio` | also count incoming audio samples |

Exit codes: **0** signal locked / frames captured, **1** devices exist but no signal, **2** driver
missing or capture could not start. Usable directly in monitoring checks and CI.

```bash
$ sdi-probe info
Driver (running Desktop Video API) : 16.3
SDK (this tool was built against)  : 12.4.1

== device_id=0  DeckLink Duo (1)
   model         : DeckLink Duo 2 Mini
   signal locked : YES
   detected mode : Hi50
   input modes   : 16 (use -v to list)

Summary: 4 device(s), 4 input-capable, 2 with a locked signal.

$ sdi-probe capture --device 0 --seconds 5 --out /tmp/sdi.raw
   [format changed] -> 1080i50 (1920x1080)
frames captured  : 125
frames w/o signal: 0

RESULT: OK - signal is being read.
wrote 618.0 MB to /tmp/sdi.raw

Play it back with:
  ffplay -f rawvideo -pixel_format uyvy422 -video_size 1920x1080 -framerate 25.00 /tmp/sdi.raw
```

Raw video carries no header, so playback needs pixel format, resolution and frame rate spelled
out. The tool prints the exact command for what it actually captured.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `cannot load the DeckLink API` | driver not installed or not loaded - `dpkg -l \| grep desktopvideo`, `lsmod \| grep blackmagic`, `ls -l /dev/blackmagic*` |
| devices listed, `signal locked : no` | cable unplugged, or the source is not transmitting |
| frames captured but playback is garbage | wrong `--mode` - take the code from `info -v` |
| `No device at index N` | indices span **all** cards in the machine: a 4-channel card is 0-3, a second card continues at 4 |
| driver older than SDK | ffmpeg-based apps warn `Installed DeckLink drivers are too old`. A driver newer than the SDK is fine |

Two things worth knowing: a DeckLink Duo 2 is one physical card exposing **4** independent SDI
channels (`/dev/blackmagic/io0..io3`), and each channel is switchable input/output - one
configured as output reports as not input-capable. The Desktop Video driver is also a DKMS
module, rebuilt on every kernel upgrade and able to fail silently, so re-run `sdi-probe info`
after any kernel change.

## License

MIT for the tool source. The DeckLink SDK it builds against is proprietary Blackmagic Design
software, governed by its own EULA, and is not distributed here.
