# Third-party notices

## Parsec VDD protocol reference

The virtual-display client in `src/vdd/` interoperates with the Parsec Virtual Display Driver.
Its device identifiers, IOCTL codes, watchdog timing, and removal payload were implemented from
the BSD-2-Clause-licensed `nomi-san/parsec-vdd` reference project:

- Copyright (c) 2023, Nguyen Duy
- <https://github.com/nomi-san/parsec-vdd>

No Parsec driver binaries are distributed by this repository. The signed driver remains a separate
user-installed system component.

## nlohmann/json

Layout and control-protocol JSON use `nlohmann/json`, supplied through the vcpkg manifest under the
MIT License: <https://github.com/nlohmann/json>.
