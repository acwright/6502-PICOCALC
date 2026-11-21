# CompactFlash card tests

`src/machine/storage.c` is the emulated CompactFlash card in slot 3, and it has to answer as a
real card does. The reference is the SD controller firmware that answers on the bus as a card
(6502-COB `Firmware/SCP Controller/core/ata.c` and `identify.c`); 6502-EMULATOR's
`src/core/IO/Storage.ts` and the DB Emulator's `StorageCard.cpp` in 6502-DEV follow the same
card, and their tests check the same things.

So this runs on a desktop:

```
./run.sh
```

`shim/ff.h` is the part of FatFs that `storage.c` uses, over a sparse in-memory image: only
the sectors written hold memory, so a test can reach the card's last sector without a 256 MB
file. Nothing else is shimmed.

`test_storage.c` checks what a real card does in the places the model once differed
(6502-PICOCALC#1): IDENTIFY's word 0 and its byte-swapped strings, ready reading `$50`, a count
of 0 moving 256 sectors, a command written in the middle of a transfer ending it, Erase
Sectors erasing its count, the data register ignored outside a transfer, and a transfer
stopping at the end of the card.
