# Cartridge mapper tests

`src/machine/cart.c` is the one part of this port whose contract is written down somewhere
else. **6502-VCS `PLAN.md` §§1–3** is the normative copy — the naming scheme, the mapper
and the flash command set — because three emulators and one programmer have to agree on
them bit for bit.

So this runs on a desktop, where a Pico is not:

```
./run.sh
```

`shim/` is `__not_in_flash_func` and the three numbers `machine/media.h` wants from the SDK.
The real `media.h` compiles, layout arithmetic and static assertions included; `test_cart.c`
supplies the two functions `cart.c` calls into flash with, over a plain array. Nothing else
in the firmware is shimmed and nothing else should be: this exists for the modules that have
an external contract. The other one is the CompactFlash card, in
[`test/storage/`](../storage/README.md).

## The fixture

`fixtures/banked-128K.crt` is a copy of the one in
`6502-VCS Firmware/FH Programmer/host/test/fixtures/`, the shared oracle. 6502-EMULATOR's
tests, the DB Emulator's, `6502-flash`'s own and these all check against **that file**
rather than against each other, which is the only way four implementations of one contract
end up agreeing rather than sharing a bug.

Sixteen 8 KB banks, every byte of a bank holding that bank's own number, with a 16-byte
ASCII tag at each end. CRC-32 `e0e4f83c`, quoted in `test_cart.c` so a stale copy fails
rather than passing against the wrong bytes. **If it moves in 6502-VCS, this copy has to be
told.**
