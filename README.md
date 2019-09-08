# Arduboy emulator

This is an Arduboy emulator written in C++, using libsimavr and SDL2 to do the
heavy lifting.

## Building

You need to have meson and the development files for libsimavr and SDL2
installed. On a Debian-based system, the simplest way to do this is to run the
following command:

```
sudo apt install meson libsimavr-dev libsdl2-dev
```

To build simarduboy, run the following commands:

```
meson build
ninja -C build
```

The name of the binary will be `build/src/simarduboy`.

## Running

Just give the name of an Arduboy firmware file, which can be either in ELF or
Intel HEX format, as a parameter. For example:

```
./simarduboy game.hex
```
