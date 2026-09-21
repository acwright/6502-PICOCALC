// The one thing machine/cart.c wants from the SDK. On a desktop every function
// is in the same place.
#ifndef PICO_SHIM_H
#define PICO_SHIM_H
#define __not_in_flash_func(f) f
#endif
