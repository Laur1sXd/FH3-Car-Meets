# FH3 Car Meets

Car Meets adds eight offline meeting locations to the Xbox One version of Forza
Horizon 3 running in Azimuth. It uses FH3's native lineup, camera and car selection
menus, with location-specific entrances and exits.

## Locations

- Surfers Paradise
- Byron Bay
- Rainforest
- Twelve Apostles
- Parkes Satellite Array
- Kiewa Valley
- Redstone Gold Mine
- Silver Sands Shipwrecks

The original Airfield Car Meet retains its native progression unlock. All meets,
including Airfield, retain the time and sky present when you enter. Night entry
stays night; the native clock resumes when you leave.

## Install

Use Azimuth with Xbox One FH3 build **1.0.125.0** and **FH3 Mod Platform 1.3.0**.
Put `CarMeets.dll` in `fh3-mods` beside `azimuth.exe`.

## Optional Underglow integration

The mod publishes `CarMeets.ParkedCars` v1 for compatible Underglow builds. Other
parked cars can have stable random colors and effects, including live music
reactions. Your own car keeps its equipped appearance, including no glow when
none is fitted. Turning Underglow off or removing it leaves ordinary car shadows.

## Build

Use an x64 Native Tools Command Prompt for Visual Studio with C++20 support:

```bat
Build.cmd
```

Or use llvm-mingw from PowerShell:

```powershell
.\Build.ps1 -ToolchainBin C:\path\to\llvm-mingw\bin -SmokeTest
```

The generated payload is included. To change locations, edit `locations.json` and
regenerate it using your own supported FH3 installation before rebuilding:

```powershell
python tools/generate_payload.py --game "D:\FH3\Mount"
```

The smoke test checks native platform registration, update metadata, the eight
meets, labels and embedded overrides. The parked-car regression covers all twelve
bays at each meet. Runtime camera, terrain and lighting changes need in-game tests.

## Credits and license

FH3 and its native Car Meet assets: **Playground Games, Turn 10 Studios and Microsoft**.

[FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform): **Levilente** and
contributors. The included SDK retains its MIT license and original notices.

The mod's code is provided under **GPL-3.0-only**, as recorded in `LICENSE`.
Original game assets and third-party code retain their respective ownership and
license terms. See `Credits.txt` and `sdk/LICENSE` for the full notices.
