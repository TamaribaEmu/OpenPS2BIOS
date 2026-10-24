# OpenPS2BIOS

An experimental replacement PlayStation 2 BIOS by Tamariba Devs, developed for
emulators and tested in PCSX2. It includes an original boot screen and the EE and
IOP kernels needed to start PS2 software. No Sony firmware is included.

Current version: 0.4

> [!WARNING]
> **For emulator use only. Do not flash or install this BIOS on a real PS2.**
> Real console hardware is unsupported and untested. Installing an experimental
> BIOS could leave your console unable to boot.
>
> OpenPS2BIOS does not initialise the console's memory controller or other
> hardware that emulators set up on their own. On a real PS2 it will not boot.

## Compatibility

See the [game compatibility table](COMPATIBILITY.md) for individual results.
Testing is still limited. Reaching a title screen or menu does not establish
that gameplay, saving, sound or controller input works correctly.

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

## How it was developed

OpenPS2BIOS is an independent reimplementation, but calling it “clean-room”
would overstate the process used.

We use public hardware documentation, PS2SDK and PCSX2 source, and tests
that compare our behavior with a retail BIOS. We have consulted existing
implementations to understand how things work, including PS2SDK’s interfaces
and module-loading behavior. We have not used a separate team to turn those
findings into a specification for the people writing the code.

No Sony firmware is included. PS2SDK and PCSX2 are references; their
implementation code has not been copied into this project.

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

### Before submitting code

Please follow the same approach:

- Tell us which sources you used. Link the relevant documentation or code
  in your pull request.
- You can consult public documentation and projects such as PS2SDK and
  PCSX2. Use them to understand behavior, rather than copying or translating
  their implementations.
- Do not use leaked Sony SDK materials, Sony BIOS source, or disassembled
  or decompiled Sony firmware to write contributions.
- For retail BIOS tests, report what you tested and what happened. Do not
  upload BIOS files or game data.
- If you want to include third-party code, discuss it first. Identify its
  source and license, and keep any required notices.
- Explain how you tested your change. If you are unsure where some code
  came from or whether we can use it, say so before submitting it.

We will hold off on merging code until questions about its origins are
resolved.

## License

OpenPS2BIOS is licensed under the [MIT License](LICENSE).

Copyright (c) 2026 Tamariba Devs.

OpenPS2BIOS is an independent project and is not affiliated with Sony Interactive
Entertainment.
