6502-PICOCALC
=============

The idea behind this build is to port the emulators for my 6502 homebrew computer family to the ClockworkPi PicoCalc device. 

Why? The PicoCalc has all the hardware we need to emulate the 6502 family of computers on one portable device: Built-in keyboard, 4:3 or square aspect ratio LCD monitor, speakers, serial output (UART pins on side of device; USB?)
and the heart of it is a Raspberry Pi Pico 1 or 2 (I can put either one in the device) with enough processing power and RAM to emulate much of the 6502 system hardware.

I would like to make a 
multiphase plan and write that plan to PLAN.md in this workspace. This will be a full replace of the PicoCalc's firmware for the Pi. The project built here should produce a .uf2 file for the Pi and have full device control.

Here are the necessary resources:

- https://github.com/clockworkpi/PicoCalc - The default Github repo for the device.
- https://github.com/madcock/PicoMiteAllVersions - PicoMite software repo

- /Users/acwright/Developer/Assembly/6502-BIOS - The source of truth and heart of the 6502 family of computers and also the ROM software the emulation will run (built-in).
- /Users/acwright/Developer/NodeJS/6502-EMULATOR - The existing Typescript based emulator for the 6502 family.
- /Users/acwright/Developer/Kicad/6502-DEV - The Teensy based emulator for the 6502 family. The only other emulator running on hardware that is not an x86 based PC.

Open questions:

- How much of IO 1 & 2 can we support? BASIC only uses one bank BANK 0 (256K)
- How do we support Cartridges / CF / DOS? Maybe we don't? VCS in 6502 family doesn't have CF support for example but does support carts. It does however need some way of loading programs. If serial XMODEM is only option then we got with that for now.
- RTC? Again, maybe we don't support that on this device...