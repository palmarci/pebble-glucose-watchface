# Morten's Glucose Watchface

[Get it on Pebble Appstore](https://apps.repebble.com/b785c22984c843299e95bba0)

This is my blood glucose watchface. It can work with any glucose data source.

<img width="720" height="320" alt="Image" src="https://github.com/user-attachments/assets/096b87a2-90ee-45ba-b960-69c5d54e950f" />

## Companion app

This watchface uses a generic [Pebble Glucose Protocol](https://github.com/mortenfyhn/pebble-glucose-protocol), and can therefore be made to work with any data source. You need a companion app or other source that sends glucose data using the this protocol. Each companion app must be added to [`package.json`](package.json).

Options:
* [MiniMed Pebble Bridge](https://github.com/mortenfyhn/minimed-pebble-bridge) - an experimental MiniMed pump companion app
* xDrip - hopefully one day
* [Direct MiniMed pump connection](https://github.com/mortenfyhn/PebbleOS) - this is what I use
* Make your own app implementing the same [protocol](https://github.com/mortenfyhn/pebble-glucose-protocol)

## Development

Can run in emulator with test data, see the [`justfile`](justfile).
