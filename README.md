# OpenPS2BIOS

An experimental replacement PlayStation 2 BIOS by Tamariba Devs, developed for
emulators and tested in PCSX2. It includes an original boot screen and the EE and
IOP kernels needed to start PS2 software. No Sony firmware is included.

Current version: 0.2

> [!WARNING]
> **For emulator use only. Do not flash or install this BIOS on a real PS2.**
> Real console hardware is unsupported and untested. Installing an experimental
> BIOS could leave your console unable to boot.
>
> OpenPS2BIOS does not initialise the console's memory controller or other
> hardware that emulators set up on their own. On a real PS2 it will not boot.

## Compatibility

See the [game compatibility table](COMPATIBILITY.md) for individual results.
The listed games reach their main menus. Gameplay, saving, sound and controller
support may still have problems; reaching a menu does not mean a game is fully
playable.

OpenPS2BIOS is still in development. Results can change between BIOS builds and
emulator versions.

## Known limitations

- The boot screen only starts PS2 discs. There is no system menu, memory card
  browser or clock and language settings.
- PlayStation (PS1) discs, audio CDs and DVD video are not supported.
- When a game reboots the IOP, the modules it requests are not loaded. Built-in
  replacements provide the services instead. Encrypted (KELF) modules are not
  supported.
- A game stops when it uses a system call that is not implemented yet, so that
  missing features show up as a clear failure, not as a crash later on.

## Downloads

Prebuilt ROM images are attached to each release on the
[Releases page](../../releases). To build it yourself, see below.

## Building

Requirements:

- Clang with the MIPS backend, `ld.lld` and `llvm-objcopy`
- GNU Make
- Python 3.10 or newer

The PS2SDK is not required. The build is tested with LLVM 23. Clang reports
"MIPS-I support is experimental" for the IOP sources; this warning is expected.

```sh
make
```

This produces `build/OpenPS2BIOS.bin`, a 4 MiB ROM image. In your emulator,
select it as the BIOS.

## Contributing

Bug reports, compatibility reports and pull requests are welcome. Please check
existing issues before opening a new one, and keep each report focused on one
problem or game.

For bugs and compatibility reports, open an issue with:

- The game title, region, serial and disc number. Mention any patches or mods.
- The OpenPS2BIOS build and emulator version.
- Your operating system and any relevant emulator settings.
- Steps to reproduce the result, what you expected and what happened.
- How far the game gets: boot, main menu, gameplay or completion.
- Logs or screenshots, if they help show the problem.

Reports about working games are useful too. Please say what you tested beyond
the menu, including controller input, audio and saving where possible. Do not
attach game images, Sony BIOS files or other copyrighted game data.

Pull requests for documentation and compatibility updates are welcome. Describe
what changed and include the testing details behind any compatibility update.
For larger changes, open an issue first so we can discuss the approach.

## Acknowledgements

The [PS2SDK](https://github.com/ps2dev/ps2sdk) project served as a reference for
system call numbers, module interfaces and data structures. OpenPS2BIOS contains
no PS2SDK code.

## License

OpenPS2BIOS is licensed under the [MIT License](LICENSE).

Copyright (c) 2026 Tamariba Devs.

OpenPS2BIOS is an independent project and is not affiliated with Sony Interactive
Entertainment.
