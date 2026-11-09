# KCtest — x64 farmbot (Metin2_windows-x64.exe)

Port of KeFuckingNoobMini to the pure x64 client, rebuilt as a single injected DLL —
no kernel driver, all reads/writes are in-process.

- **Target:** `Metin2_windows-x64.exe` (PE32+, ImageBase 0x140000000)
- **Build:** `g++ -Wall -O2 -s -shared -static -o KCtest10.dll KCtest10.cpp -lgdi32 -luser32 -lwinmm`
  (MinGW-w64 g++ 13.2.0, UCRT)
- **Use:** inject `KCtest10.dll` into the game process. A small Win32 panel (`frmMain`)
  appears; config is read from `kctest.txt` next to the build (`on / mob / metin / fixed / dist`).
- **Hot-swap:** `kswap.ps1 -Dll KCtest10.dll` unloads any previous KCtest DLL and injects the
  new one — no game restart. Run it from a PowerShell prompt in this folder.

## Layout (mirrors KeFuckingNoob-master)

    KCtest\
      README.md
      turkmmo-post.txt
      KCtestMini\
        KCtest10.cpp    <- full source, single file
        KCtest10.dll    <- ready to inject
        kctest.txt      <- config (on, mob, metin, fixed, dist)
        kswap.ps1       <- hot unload/reload helper
        inject.exe      <- raw injector used by kswap
        unload.exe      <- raw unloader used by kswap

## How it works

- Actor list: the client's `std::map` is walked from the manager global
  (base+0xE059B8; node layout +0x19 isnil, +0x20 VID, +0x28 instance).
- Attack: the real `CPythonPlayer::PressActor` (RVA 0x23EE00, fastcall
  `rcx=player, rdx=mainActor, r8d=vid, r9d=1`) is called from a 167-byte code
  cave patched into `CPythonApplication::Update` (RVA 0x16BB49), so every
  attack runs on the game's main thread — no foreign-thread map mutation.
- Death detection: this client has no readable dead flag (`+0x838` never sets,
  `+0x290` is useless on both). `getIsDead` uses the animation frame counter
  `+0x488` (alive loops, max seen 217; dead climbs past 320 and freezes,
  threshold 250) plus an instant path: if `+0x320` or `+0x7D0` ever carried our
  player VID (we hit it / it hit us) and `+0x7D0` drops to 0, the mob is dead
  on the spot.


