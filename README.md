# Forza Horizon 2 - Extra Options
A quality-of-life mod for the Xbox One version of Forza Horizon 2 that adds several tweaks to the game.

> **ForzaInfinity.** This `fh2online` branch is the hook of [ForzaInfinity](https://github.com/anarqz/ForzaInfinity),
> which brings Forza Horizon 2's online services back on **Azimuth 0.9.10** (recommended) or **0.9.9**:
> online free roam and races, friends, car clubs, rivals and ghosts, leaderboards, drivatars, the
> storefront and a cloud save, with HARDCORE and NOOB profiles. It keeps every Extra Options feature
> below. Players install it from the [ForzaInfinity releases](https://github.com/anarqz/ForzaInfinity/releases/latest),
> not from this repository; how it works is in `adapters/` and [BUILD.md](BUILD.md).

# Features
 - Change your car anywhere.
 - Autodrive (as seen in Forza Horizon 6).
 - Drift Camera (as seen in Forza Horizon 4/5/6).
 - FOV sliders for every camera type.
 - Roof toggle for supported cars during free-roam.
 - Lights and turn-signals toggles for all cars.
 - New rim style category with all stock rims.
 - Custom spawn locations.
 - "mediapc" modding folder support.
 
# Enhancements
 - Intro skip.
 - All DLC packs properly unlocked.
 - VIP status & "Treasure Map" unlocks.
 - Toggle for 4K liveries w/ enhanced sampling.
 - Unrestricted Photo Mode.
 - "Return to Garage" button in the pause menu.
 - Aero tuning without equipping Forza Aero parts.
 - New rim style category with all aftermarket rims.
 - Toggle for having the minimap follow the orbit camera.
 - Bypass for Azimuth's save data checks.
 - Custom Gamerpic support.

# Information

Most of the features can now be toggled and tweaked inside the game itself through the native menus.

### Treasure Map
The treasure map is disabled by default. Open the INI and set "UnlockTreasureMap" to 1 if you wish to have it.

### Autodrive
Autodrive is engaged by pressing D-Pad Down on a controller or F2 on a keyboard.

### Vehicle Lights
Lights can be toggled by clicking the right-stick.

Turn signals can be engaged by holding D-Pad Left/Right for 1 second.

They will de-engage once a turn is made and the steering is centered.

Note: Some cars have glitched turn signals. This is not a bug with the mod, just an issue with how the assets were authored.
Cars that have been confirmed to have glitched turn signals: Focus RS '09, Corvette ZR1 '09, Carrera GT '03, Cayman GTS '15.

### Roofs
Roofs can be toggled by pressing and holding the right-stick for 3 seconds while stationary.

Note: The 3 cars with proper roof animations (BMW Z4 iDrive, Mazda MX-5 (2013), and Ferrari California) will look broken if you are using vanilla car model quality. Set "Car Detail" to High or Ultra in Azimuth.
The models will still look broken in car meets as the LOD1 is forced there. I will report this to the Azimuth developer instead of having the mod mess with that.

Note #2: This is a rather experimental feature, so it doesn't look perfect in some cars due to the way the ForzaVista animations were authored.
The Lotus Exige, for example, will have its roof floating above the car. It's beyond the view of the camera but you will spot its shadow. This will be looked into in future updates.

# Installation

The installation is different depending on the translation layer you're using.

Azimuth:

 - Extract all the 3 files from the "Azimuth" folder in this ZIP to Azimuth's main folder where "Azimuth.exe" is located.

Mezcal:

 - Extract all the 3 files from the "Mezcal" folder in this ZIP to "Forza Horizon 2\Mount" which is where "forza_durango_release_final.exe" is located.

Note: Add this command if you're running Linux and using Wine: `WINEDLLOVERRIDES="winmm.dll=n,b" %command%`

# Custom gamerpic 
 - You can edit the "Gamerpic.png" image located where you extracted the mod files to with any image you like and it will be used as your avatar in-game.
 - The maximum dimensions supported are 2048x2048 and the image must weight less than 16 megabytes.

# AI Usage Disclosure
Codex was used in this project to create the DLL. The actual research was done manually, mostly based on things I already toyed with in the other titles from the series, but I don't yet have the C language knowledge to code something like this fully from scratch (I was initially editing the raw executable). Leaving this here as I believe people have the right to know.
