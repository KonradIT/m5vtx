# VTXControl_ESP32 dependency

`platformio.ini` fetches the requested upstream repository directly:

```ini
lib_deps =
    https://github.com/igorka48/VTXControl_ESP32.git#54c21169305499667b9039022282288177ef1699
```

The library source lives in PlatformIO's ignored dependency cache, normally
`.pio/libdeps/m5stack-basic-v27/VTXControl_ESP32/`.

## Scope of use

The production `src/vtx_manager.cpp` includes the dependency's actual
`VTX_SmartAudio.h` and `VTX_Tramp.h`. It uses their command/response definitions,
frame layouts, `sa_CRC8`, and Tramp frame initialization/checksum/finalization.
These are compiled into the firmware through the upstream headers. This is
header-only use of the dependency's protocol code; the upstream `VTXControl`
controller class is not instantiated.

The pinned repository is a PlatformIO application layout containing both
`src/VTXControl.cpp` and `src/VTX_Test/VTX_Test.cpp`. Its current controller uses
`HardwareSerial`, despite the README's SoftwareSerial description. Its
constructor ignores the requested pin and starts UART2 with default routing;
it does not provide the GPIO release required by our single-wire connection.
Its blocking retries, fixed transmitter power tables and status handling also
do not preserve this project's confirmed settings/readback behavior.

`tools/vtx_dependency.py` uses PlatformIO's supported PRE build middleware to
exclude compiled sources only inside that dependency. It leaves the upstream
headers and every other library untouched. This excludes the standalone test
application as well as the unused blocking controller without modifying them.
The repository's own example video dependency is not installed as a project
requirement; our existing video engine remains the only owner of I2S0/APLL.

Our nonblocking `VtxManager` therefore remains the transport/status adapter:

- Hardware UART2 on the selected GPIO16 or GPIO17, releasing the line for RX.
- SmartAudio/Tramp discovery limited to the selected protocol, with timeouts.
- Reply CRC/checksum validation and rejection of transmitted request echoes.
- Settings readback rather than treating a transmitted command as confirmation.
- Floating receive and leading/trailing dummy zeros for the connected DarwinFPV.
- Its verified one-based SA2 GET power normalization, without guessing mW.
- Existing frequency/channel/power/pit controls and protocol selection.

The two changed upstream header configuration macros do not change this
adapter: it always checks reply CRCs and chooses dummy framing from its own
verified transport profile. The CRC and frame-building algorithms are reused
directly from the external dependency.

## Alternative considered

[igorka48/BetaVTXControl](https://github.com/igorka48/BetaVTXControl), reviewed at
`ae80809f6f23d2fe0239ca8c93ed17709466e2cd`, is packaged as a library and uses
hardware UART. Its TX-only API does not replace the required discovery and
validated readback, so it is not added as another dependency.

## Verification

Build once to install the pinned dependency, then run the VTX host suite:

```sh
pio run -e m5stack-basic-v27
make vtx-host-test
```

`tools/run_vtx_host_tests.ps1` does the same on Windows after checking the
installed dependency's Git root, origin and commit. The fixtures cover CRC/echo
rejection, SmartAudio versions and power normalization, Tramp settings/limits,
confirmed setters, protocol and pin switching, and bounded probing. They do
not establish a live VTX connection; that needs the M5Stack and a VTX.

Sources: [pinned controller](https://github.com/igorka48/VTXControl_ESP32/blob/54c21169305499667b9039022282288177ef1699/src/VTXControl.cpp),
[pinned SmartAudio definitions](https://github.com/igorka48/VTXControl_ESP32/blob/54c21169305499667b9039022282288177ef1699/src/VTX_SmartAudio.h),
and [PlatformIO build middleware](https://docs.platformio.org/en/latest/scripting/middlewares.html).
