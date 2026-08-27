# Morten's Glucose Watchface

[Get it on Pebble Appstore](https://apps.repebble.com/b785c22984c843299e95bba0)

This is my blood glucose watchface. It looks like this:

<img width="822" height="402" alt="Image" src="https://github.com/user-attachments/assets/4fe3f03a-25f9-4095-9803-990e25f91f4f" />

Forks and PRs welcome.

## Companion app

This watchface uses a generic [Pebble Glucose Protocol](https://github.com/mortenfyhn/pebble-glucose-protocol), so it can be made to work with any data source. You need a companion app to send glucose data to the watch, via the Pebble app. Each companion app must be added to [`package.json`](package.json).

Options:
* [MiniMed Pebble Bridge](https://github.com/mortenfyhn/minimed-pebble-bridge) - an experimental MiniMed pump companion app
* xDrip - hopefully one day
* [Direct MiniMed pump connection](https://github.com/mortenfyhn/PebbleOS) - this is what I use
* Make your own app implementing the same [protocol](https://github.com/mortenfyhn/pebble-glucose-protocol)

## Development

Can run in emulator with test data, see the [`justfile`](justfile).
