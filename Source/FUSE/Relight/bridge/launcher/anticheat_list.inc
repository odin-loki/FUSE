// FUSE Relight RL-2.3: known anti-cheat markers the launcher refuses to inject next to.
// Copyright (c) 2026 FUSE contributors (MIT). New code (upstream NvRemixLauncher has no such check).
//
// Policy (plan §0.2, non-goals): FUSE Relight does not circumvent anti-cheat or DRM and is not for
// online multiplayer titles. fuse_relight_launcher refuses to start a game (exit code 9100, nothing
// is created or injected) when ANY of the following holds:
//   Process    a process with this image name is running (anti-cheat services and clients stay
//              resident while a protected game runs; kernel anti-cheats also flag injection into
//              unrelated processes, so the refusal is system-wide);
//   Directory  the game's executable directory or its parent contains this sub-directory
//              (anti-cheat runtimes ship inside the game install);
//   File       the game's executable directory or its parent contains this file ('/' separated
//              relative paths are allowed, e.g. "pb/pbcl.dll");
//   ExeName    the executable itself matches (anti-cheat bootstrappers that launch the real game).
// Matching is case-insensitive; '*' matches any run of characters. There is deliberately no
// switch to override a match. Adding an entry: one row, with the product and a short note on what
// the marker is. False positives are preferred over false negatives.
//
// Not detectable here, and therefore not covered: anti-cheats that live inside another process
// (Valve Anti-Cheat runs inside the Steam client; server-side-only anti-cheats). Do not use Relight
// on VAC-secured or other online servers.
//
// Columns: product, kind, pattern, note.
RL_ANTICHEAT(EasyAntiCheat, Directory, "EasyAntiCheat", "EAC runtime folder shipped with protected games")
RL_ANTICHEAT(EasyAntiCheat, Directory, "EasyAntiCheat_EOS", "EAC (Epic Online Services) runtime folder")
RL_ANTICHEAT(EasyAntiCheat, File, "EasyAntiCheat_x64.dll", "EAC client module")
RL_ANTICHEAT(EasyAntiCheat, File, "EasyAntiCheat_x86.dll", "EAC client module (32-bit)")
RL_ANTICHEAT(EasyAntiCheat, File, "EasyAntiCheat_EOS_Setup.exe", "EAC (EOS) service installer")
RL_ANTICHEAT(EasyAntiCheat, ExeName, "start_protected_game.exe", "EAC bootstrapper that starts the game")
RL_ANTICHEAT(EasyAntiCheat, Process, "EasyAntiCheat.exe", "EAC service")
RL_ANTICHEAT(EasyAntiCheat, Process, "EasyAntiCheat_EOS.exe", "EAC (EOS) service")
RL_ANTICHEAT(BattlEye, Directory, "BattlEye", "BattlEye runtime folder")
RL_ANTICHEAT(BattlEye, File, "BEClient.dll", "BattlEye client module")
RL_ANTICHEAT(BattlEye, File, "BEClient_x64.dll", "BattlEye client module (64-bit)")
RL_ANTICHEAT(BattlEye, ExeName, "*_BE.exe", "BattlEye launcher naming convention (e.g. Game_BE.exe)")
RL_ANTICHEAT(BattlEye, Process, "BEService.exe", "BattlEye service")
RL_ANTICHEAT(BattlEye, Process, "BEService_x64.exe", "BattlEye service (64-bit)")
RL_ANTICHEAT(Riot Vanguard, Process, "vgc.exe", "Vanguard user-mode service")
RL_ANTICHEAT(Riot Vanguard, Process, "vgtray.exe", "Vanguard tray client")
RL_ANTICHEAT(FACEIT, Process, "faceit.exe", "FACEIT client")
RL_ANTICHEAT(FACEIT, Process, "FACEITService.exe", "FACEIT anti-cheat service")
RL_ANTICHEAT(FACEIT, Process, "faceitclient.exe", "FACEIT anti-cheat client")
RL_ANTICHEAT(ESEA, Process, "ESEAClient.exe", "ESEA anti-cheat client")
RL_ANTICHEAT(PunkBuster, File, "pb/pbcl.dll", "PunkBuster client module")
RL_ANTICHEAT(PunkBuster, File, "pb/pbsv.dll", "PunkBuster server module")
RL_ANTICHEAT(PunkBuster, File, "pb/pbag.dll", "PunkBuster agent module")
RL_ANTICHEAT(PunkBuster, Process, "PnkBstrA.exe", "PunkBuster service A")
RL_ANTICHEAT(PunkBuster, Process, "PnkBstrB.exe", "PunkBuster service B")
RL_ANTICHEAT(nProtect GameGuard, Directory, "GameGuard", "GameGuard runtime folder")
RL_ANTICHEAT(nProtect GameGuard, File, "GameGuard.des", "GameGuard loader")
RL_ANTICHEAT(nProtect GameGuard, Process, "GameMon.des", "GameGuard monitor")
RL_ANTICHEAT(nProtect GameGuard, Process, "GameMon64.des", "GameGuard monitor (64-bit)")
RL_ANTICHEAT(XIGNCODE3, Directory, "XIGNCODE", "XIGNCODE3 runtime folder")
RL_ANTICHEAT(XIGNCODE3, File, "x3.xem", "XIGNCODE3 module")
RL_ANTICHEAT(AhnLab HackShield, Directory, "HShield", "HackShield runtime folder")
RL_ANTICHEAT(AhnLab HackShield, File, "EhSvc.dll", "HackShield module")
RL_ANTICHEAT(EA Anticheat, Directory, "EAAntiCheat", "EA Anticheat runtime folder")
RL_ANTICHEAT(EA Anticheat, ExeName, "EAAntiCheat.GameServiceLauncher.exe", "EA Anticheat bootstrapper")
RL_ANTICHEAT(EA Anticheat, Process, "EAAntiCheat.GameService.exe", "EA Anticheat service")
RL_ANTICHEAT(Anti-Cheat Expert, Process, "SGuard64.exe", "Tencent ACE guard")
RL_ANTICHEAT(Anti-Cheat Expert, Process, "SGuardSvc64.exe", "Tencent ACE service")
RL_ANTICHEAT(mhyprot, File, "mhyprot2.sys", "miHoYo protection driver shipped with the game")
