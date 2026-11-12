# Pacific Drive Co-Op

A small co-op mod for Pacific Drive for two players. One player drives, the other rides in the back seat, walks around and keeps their own inventory.

It is made with UE4SS (a C++ mod plus Lua for research). It is early work: it plays, but expect bugs and crashes. Back up your saves before you try it.

## How it works

- **Host** is the driver. The host's game simulates the world and the car.
- **Client** is the passenger. They sit in the back seat, can get out, and have their own inventory.
- LAN or VPN only (Radmin works fine). No dedicated server.
- Everything the client sees comes from the host. If the two disagree, the host wins.

## Install

1. Download `PacificDriveCoop-Launcher-0.17.19.exe` from [Releases](https://github.com/NiceTryJony/PacificDrive_Co-Op-mode/releases).
2. Both players run it. It finds the game, installs UE4SS and the mod, and sets up the connection.
3. The host starts first, the client joins.

Both players need the same version of the mod and the game.

## What works now

- Car position, wheels and speed on the client
- Passenger sits down and gets out of the car (left and right back seats), and returns to the seat after a zone change
- Both players see each other walking
- Car doors, hood, trunk, ignition, lights, fuel
- Car parts, paint and effects, both ways
- Car trunk: the passenger takes and puts items through the host
- Garage doors and the part regeneration station
- Quirks: fixing one on either side clears it on the other
- Car radio and garage radio, shared track, and the host can send its music files to the client
- Weather event and levels (rain, fog) follow the host
- Anchors: taking one removes it for both players, and the anomalies it spawns appear for both
- Exit portal, passenger radiation protection in the car
- Map marks (Ctrl+F7), both ways
- Items dropped from the client to the host

## In testing

- Garage storage containers (written, not checked on two PCs yet)
- Items dropped by the host showing up for the client
- Gate sync: the garage exit gate opens for both players, whoever picked the destination (in the latest build, not checked on two PCs yet)

## Planned

- Map table: either player picks the destination and both can drive out without choosing it again
- See the item the other player holds in their hand
- Client at the wheel (needs research first, the car physics only run on the host)
- Anomalies and other random zone events the same for both players
- Gobbler and the "fork", the anchor radiation zones
- Containers and trunks of donor cars in the zone
- Less network and CPU use: send the main data in the first seconds after the client joins, then sync less often

## Known problems

- The zone is not fully the same on both sides yet (a few leeches, some plants)
- Instability countdown can differ between players by up to a minute and a half
- The passenger camera feels off
- Radio can crackle for the client in a zone

## Building

Needs CMake and a C++ compiler. `RE-UE4SS` is pinned to the same commit as the one installed in the game. The launcher is built with `tools/CoopLauncher/build.bat`.

## Credits

Built on [UE4SS](https://github.com/UE4SS/RE-UE4SS) and [ENet](http://enet.bespin.org/).

Pacific Drive belongs to Ironwood Studios and Kepler Interactive. This is an unofficial fan project and is not affiliated with them.
