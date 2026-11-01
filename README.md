# esp32-morse

Type a message on your phone; an ESP32-S3 sends it as Morse code on its LED
(and on a buzzer, if one is fitted).

The board serves the page itself over its own WiFi network — no internet, no app
to install. The phone sets the message and the speed; the board does the timing.

- `firmware/` — PlatformIO project (Arduino, ESP32-S3)
- `index.html` — the project website (at the repository root)

## The number this project is checked against

Morse timing is defined by one unit, the dot. A dash is 3 dots, the gap inside a
letter is 1, between letters 3, between words 7. The reference word PARIS is
exactly 50 units long, so speed in words per minute gives

    dot (ms) = 1200 / WPM

At 20 WPM a dot must be 60 ms and a dash 180 ms. Those numbers are predicted on
paper and then measured against what the board actually emits.

Built for *Basic Skills for Experimentalists* (TIGP 2026), Project 2.
