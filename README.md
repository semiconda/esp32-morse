# esp32-morse

Type a message on your phone; an ESP32-S3 sends it as Morse code on its LED.

The board serves the app itself over its own WiFi network — no internet, nothing
to install on the phone. The phone sets the message and the speed; the board does
the timing.

- `firmware/` — PlatformIO project (Arduino, ESP32-S3)
- `app/` — the app: the page the board serves to the phone
- `index.html` — the project website, at the repository root (not written yet)

## What you need

- A **Jinhua ESP32-S3 N16R8** dev board (16 MB flash, 8 MB octal PSRAM). Other
  ESP32-S3 DevKitC-style boards with the RGB LED on GPIO 48 should work; the
  memory settings in `firmware/platformio.ini` are for the N16R8.
- A USB-C **data** cable, plugged into the port marked **USB** (native USB, not UART).
- [PlatformIO](https://platformio.org/) — the VS Code extension, or the `pio` command line.

**Nothing is wired to the board.** The only pin used is GPIO 48, which drives the
WS2812 LED already soldered on the dev board.

## Build and flash

```sh
git clone https://github.com/semiconda/esp32-morse.git
cd esp32-morse/firmware
pio run -t upload        # first time: downloads the toolchain, then builds and flashes
pio device monitor       # optional: watch it boot
```

The app (`app/index.html`) is compiled into the firmware during the build, so there
is one upload step and no filesystem image to upload separately.

**On Windows, clone into a short path.** The libraries PlatformIO downloads sit
deep inside `firmware/.pio/`, and once a full path passes 260 characters the
compiler cannot open the file: the build fails with *No such file or directory*
for a header that is plainly there (`AsyncTCPVersion.h`, `Adafruit_NeoPixel.h`).
A short clone path such as `C:\src\esp32-morse` avoids it.

**The first flash of a factory-fresh board fails once, by design.** If the upload
says *"No serial data received"* (the board shows up as USB ID `303A:4001`): hold
**BOOT**, tap **RST**, release BOOT, and flash again. The board comes back on a
**new COM port** (`pio device list` shows it) as `303A:1001`. After that, every
flash resets the board on its own. The port also disappears and reappears at
every reset; that is normal.

## Use it

1. On the phone, join the WiFi **`esp32-morse`**, password **`instrument`**.
2. Open **http://192.168.4.1** — type the `http://`. If the phone warns that the
   network has no internet, stay connected.
3. Type a message, pick a speed (5–40 words per minute), press **Send**.

The password is **published on purpose, not a secret**: it is in this README and in
the firmware. It only stops a passer-by joining by accident and keeps the phone
from complaining about an open network. Nothing is protected by it.

There is **no friendly name** for the board, only the address. A one-name DNS
server does not work reliably on modern phones (Android's Private DNS and Chrome's
DNS-over-HTTPS bypass it), and an address that always works beats a name that
works sometimes. Up to four phones can be connected at once; they all see the same
transmission, and any of them can stop it.

What the app does:

- **Allowed characters:** A–Z (lowercase is fine), 0–9, space, and
  `. , ? ' ! / ( ) & : ; = + - _ " $ @`. Anything else is highlighted in place and
  Send stays disabled until it is gone. Leading and trailing spaces are dropped and
  runs of spaces become one; tabs, line breaks and non-breaking spaces count as
  unsupported characters. Up to 255 characters.
- **Send** transmits once. The message stays in the box, so pressing Send again
  repeats it, at whatever speed is set by then. **Stop** ends it within one element.
- **The live display** shows the message with the current word, the current
  character, and its dots and dashes with the one being sent lit. At high speed
  it lags the LED slightly — the WiFi link and the browser take tens of
  milliseconds — and says so.
- When a transmission completes, the board reports **how long it took by its own
  clock**, next to the predicted duration.
- **The LED panel** sets the colour and brightness, previewing the LED for two
  seconds. Between transmissions the LED is dark. Warnings appear when a setting
  will be hard to see or hard to film. **Filming preset** sets green at 20%, chosen
  for slow-motion filming (one colour, dim enough not to bloom); the board also
  starts with this setting.

## Morse timing

Morse timing is defined by one unit, the dot. A dash is 3 dots, the gap inside a
letter is 1, between letters 3, between words 7. The reference word PARIS is
exactly 50 units long, so speed in words per minute gives

    dot (ms) = 1200 / WPM

At 20 WPM a dot must be 60 ms and a dash 180 ms. Those numbers are predicted on
paper and then measured against what the board actually emits.

The board times each element against absolute microsecond deadlines in a
dedicated task, so small delays never add up over a long message.

## Talking to the board from a computer

The board also accepts commands as JSON lines over its USB serial port (115200
baud), the same commands the app sends over its WebSocket and handled by the same
code. This is how you can check that the **firmware itself** refuses bad input,
with no app in the way. For example, in `pio device monitor`:

```
{"cmd":"send","id":1,"text":"HELLO #WORLD","wpm":20}
{"cmd":"send","id":2,"text":"HELLO","wpm":0}
{"cmd":"send","id":3,"text":"HELLO","wpm":20}
{"cmd":"stop","id":4}
```

Each reply prints as `[reply] {...}`: the first two are refused (an unsupported
character at position 6; a speed outside 5–40), the third is sent, the fourth
stops it. Every 10 seconds the board prints a `[status]` line with the number of
phones connected; each transmission prints its predicted duration and, at the
end, `[done] elapsed_ms=...`.

From a phone or computer browser that has a developer console, the app offers the
same thing: `morse.raw({cmd:'send', text:'HELLO', wpm:0})` sends any object and
logs the board's reply.

The other commands are `check` (`text`) and `led` (`r`, `g`, `b` 0–255,
`brightness` 0–100).

## Tests

The Morse table, the message rules and the timing arithmetic are tested on the
computer, with no board:

```sh
cd firmware
pio test -e native -v
```

This needs a C++ compiler on the PATH, which PlatformIO does not include. On
Windows: `winget install BrechtSanders.WinLibs.POSIX.UCRT`, then open a new
terminal. (If the build compiles but the link fails with *cannot find -lmsvcrt*,
the install was left incomplete; reinstalling it fixes that.) On macOS:
`xcode-select --install`. On Linux: your distribution's `g++`.
