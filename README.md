<div align="center">

# Pacific Drive Co-Op

**Two players, one station wagon, the Olympic Exclusion Zone.**

An unofficial co-op mod for [Pacific Drive](https://store.steampowered.com/app/1458140/Pacific_Drive/).
One of you drives, the other rides in the back seat, hops out to loot and keeps their own inventory.

[**Download**](https://github.com/NiceTryJony/PacificDrive_Co-Op-mode/releases/latest) ·
[Install](#install) ·
[Playing](#playing-together) ·
[Troubleshooting](#when-something-goes-wrong) ·
[Report a bug](https://github.com/NiceTryJony/PacificDrive_Co-Op-mode/issues)

![Status](https://img.shields.io/badge/status-early%20but%20playable-orange)
![Players](https://img.shields.io/badge/players-2-blue)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey)
![Built on UE4SS](https://img.shields.io/badge/built%20on-UE4SS-6e40c9)

</div>

---

> [!WARNING]
> This is early work. It plays, but you will run into bugs and the odd crash.
> **Back up your saves before your first run.** The launcher can do it for you in one click.

## What it is

Pacific Drive was made for one player. This mod lets a friend come along for the ride.

- **Host** is the driver. Their game runs the world, the car and the zone.
- **Client** is the passenger. They sit in the back, get out whenever they want, walk around and carry their own stuff.
- If the two games ever disagree, the host is right.

You play over LAN or a virtual LAN like [Radmin VPN](https://www.radmin-vpn.com/). There is no server and no matchmaking: you just connect to your friend.

## What you need

- Pacific Drive on Windows, **the same game version on both PCs**
- **The same mod version on both PCs** (just grab the latest release both times)
- A shared network: same LAN, or both in one Radmin VPN network
- Only the host needs a save. The client gets the world from the host.

## Install

1. Open [**Releases**](https://github.com/NiceTryJony/PacificDrive_Co-Op-mode/releases/latest) and download the newest launcher (`PacificDriveCoop-Launcher-*.exe`). Older releases are there only for history, always use the latest one.
2. Close the game and run the launcher. Both players do this.
3. The launcher finds the game on its own. If it can't, point it at the game folder.
4. It installs the mod loader (UE4SS) and the mod. Nothing in the game files is overwritten without a backup.
5. Pick who you are:
   - **Host:** copy your address and room code and send them to your friend.
   - **Client:** paste the host's invite (or type in the address and room code).
6. Start the game from the launcher.

> [!TIP]
> Windows SmartScreen may complain because the exe isn't signed. Click **More info → Run anyway**.
> When Windows asks about network access, allow it. The mod uses **UDP port 47777**.

## Playing together

1. **Host** loads their save and plays as usual. When you enter a zone, the world is saved and sent to the client automatically.
2. **Client** starts the game and waits in the main menu. A *"Joining the driver's world"* card shows up at the top, and the world loads by itself.
3. Walk up to the car and press **Ctrl+E** to take a back seat. Press it again to get out.

While you're seated, you can only use the doors and the map screen. That keeps you from grabbing the wheel by accident. You can turn this off in the launcher settings.

### Hotkeys

| Keys | Who | What it does |
|---|---|---|
| `Ctrl+E` | Client | Sit in the back seat / get out |
| `Ctrl+H` | Both | Change the size of the co-op HUD |
| `Ctrl+F7` | Both | Drop a "something's wrong here" mark (handy for bug reports) |
| `Ctrl+F10` | Host | Save the zone and send it again by hand |
| `Ctrl+F11` | Client | Load the zone you received by hand |

## What works

<details open>
<summary><b>Synced between both players</b></summary>

- Car position, wheels and speed
- Back seat on either side, getting in and out, staying seated through zone changes
- Seeing each other walk around
- Doors, hood, trunk, ignition, lights and fuel
- Car parts, paint and effects, both ways
- Trunk: the passenger takes and puts items in it
- Garage doors and the part regeneration station
- Quirks: fix one on either side and it's gone for both
- Car and garage radio play the same track, and the host can share their music files
- Weather events, rain and fog
- Anchors: take one and it's gone for both, the anomalies it spawns show up for both
- Exit portal, radiation protection for the passenger inside the car
- Map marks
- Items the client drops appear for the host

</details>

<details>
<summary><b>Being tested</b></summary>

- Garage storage containers
- Items the host drops appearing for the client
- The garage exit gate opening for both, no matter who picked the destination

</details>

<details>
<summary><b>Planned</b></summary>

- Either player picks the destination at the map table
- Seeing what the other player holds in their hand
- Letting the client drive (needs research: car physics only run on the host)
- Anomalies and random zone events matching for both
- The Gobbler, the "fork" and the anchor radiation zones
- Containers and trunks of donor cars in the zone
- Less network and CPU use

</details>

### Known issues

- The zone isn't 100% identical on both sides yet (a few leeches, some plants).
- The instability countdown can drift apart by up to a minute and a half.
- The passenger camera can feel a bit off.
- The radio sometimes crackles for the client inside a zone.

## When something goes wrong

Start with the launcher: open **Troubleshooting** and press **Run checks**. It tells you what's missing and usually how to fix it.

<details>
<summary><b>The client can't connect</b></summary>

- Both of you are in the same Radmin network and the host's address is the `26.x.x.x` one from Radmin.
- The room code matches on both sides.
- The firewall lets the game through on UDP port 47777. Try turning the firewall off for a minute to check.
- The host is already in game, not in the main menu.

</details>

<details>
<summary><b>The mod doesn't seem to load</b></summary>

- Run checks in the launcher. If it says the mod is installed but not switched on, reinstall from the launcher.
- Open `...\PenDriverPro\Binaries\Win64\ue4ss\UE4SS.log` and look for `[PacificDriveCoop] loaded`. No line means the mod didn't start.
- Antivirus sometimes eats `UE4SS.dll`. Add the game folder to its exceptions and reinstall.

</details>

<details>
<summary><b>The world doesn't load for the client</b></summary>

- Wait a bit, a big zone takes time to send.
- The host can press `Ctrl+F10` to send it again, the client then presses `Ctrl+F11`.
- Make sure you both run the same game and mod versions.

</details>

<details>
<summary><b>The game crashes on start</b></summary>

- Overlays like RTSS / MSI Afterburner can clash with the mod's on-screen text. Close them or turn the overlay off in the launcher settings.
- Update to the latest release and reinstall from the launcher.

</details>

<details>
<summary><b>My save looks wrong</b></summary>

Restore it from the launcher (**Saves → Restore**). That's what the backups are for.

</details>

### Reporting a bug

1. In the launcher, go to **Troubleshooting → Make report**. It packs the logs into one file.
2. [Open an issue](https://github.com/NiceTryJony/PacificDrive_Co-Op-mode/issues) and attach the report.
3. Tell us who you were (host or client), what you did right before it broke, and whether it happens again.

If you pressed `Ctrl+F7` at the moment things went weird, mention it. The mark makes the logs much easier to read.

## Uninstall

Remove the mod from the launcher, or just delete the `Win64\ue4ss\Mods\PacificDriveCoop` folder.
If the installer replaced `UE4SS.dll` or `dwmapi.dll`, the originals are in `Win64\ue4ss_backup_before_coop`.

## FAQ

**Does it work over the internet without a VPN?**
Not directly. Use Radmin VPN, ZeroTier or anything that puts you in one virtual LAN.

**Can more than two people play?**
No. It's built around one driver and one passenger.

**Does the client need their own save?**
No. The client's inventory is kept separately, the world comes from the host.

**Will it break my single-player save?**
It shouldn't, but it's early software. Back up first.

**Can I play on Steam Deck / Linux?**
Not tested. Windows only for now.

## Building from source

You need CMake and Visual Studio 2022 (C++ workload).

```bash
git clone --recursive https://github.com/NiceTryJony/PacificDrive_Co-Op-mode.git
cmake -B build
cmake --build build --config Release
```

- `RE-UE4SS/` is pinned to the same commit as the UE4SS the mod installs into the game. Don't bump it on its own.
- The launcher is built with `tools/CoopLauncher/build.bat`.

| Folder | What's inside |
|---|---|
| `CppMod/` | The mod itself: networking (ENet) and all the sync code |
| `tools/CoopLauncher/` | The launcher |
| `tools/CoopConsole/` | A small status window you can keep next to the game |
| `tools/PDCoopRecon/` | Lua scripts used to study the game |

## Credits

Built on [UE4SS](https://github.com/UE4SS/RE-UE4SS) and [ENet](http://enet.bespin.org/).

Pacific Drive belongs to Ironwood Studios and Kepler Interactive. This is a fan project, not affiliated with or endorsed by them. You need your own copy of the game.
