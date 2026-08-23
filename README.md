# Morten's Glucose Watchface

This is my blood glucose watchface. It looks like this:

<img width="144" height="168" alt="Image" src="https://github.com/user-attachments/assets/ed52c276-f736-465b-a529-b09f5383ff79" />

It has:

* 2 hour glucose graph
* Glucose trend projection
* Insulin-on-board
* Pump/sensor status (such as "SUSPENDED")
* Age of reading (if older than 6 min)

Forks and PRs welcome.

## Target platforms

Mainly built for `flint` (Pebble 2 Duo) but I intend to make it look good on all Pebbles.

## Companion app

This watchface uses a generic [Pebble Glucose Protocol](https://github.com/mortenfyhn/pebble-glucose-protocol), so it can be made to work with any data source. You need a companion app to send glucose data to the watch, via the Pebble app. Each companion app must be added to [`package.json`](package.json).

Options:
* [MiniMed Pebble Bridge](https://github.com/mortenfyhn/minimed-pebble-bridge) - an experimental MiniMed pump companion app
* xDrip - hopefully one day
* [Direct MiniMed pump connection](https://github.com/mortenfyhn/PebbleOS) - this is what I use
* Make your own app implementing the same [protocol](https://github.com/mortenfyhn/pebble-glucose-protocol)

## Development

Can run in emulator with test data, see the [`justfile`](justfile).
