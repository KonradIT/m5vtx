`test_vtx_table.cpp` compiles `src/vtx_table.cpp`, the Betaflight vtxtable CLI,
alone: it parses a DarwinFPV flight controller's dump and GEPRC's RAD Mini file,
prints them back verbatim, and checks lookups and every error. `make host-tests`
runs it; it needs no dependency.

`test_vtx.cpp` compiles production `src/vtx_manager.cpp` using small host shims for
Arduino and the ESP32 UART/GPIO. Protocol definitions and codec helpers come from
the installed `igorka48/VTXControl_ESP32` Git dependency pinned at
`54c21169305499667b9039022282288177ef1699`. The tests verify wire parsing and state
transitions; physical GPIO timing and transmitter behavior require hardware tests.

Coverage includes CRC and request-echo rejection, SmartAudio versions and the
verified DarwinFPV power-index offset, Tramp limits/settings/channel/power/pit
commands and race lock, mandatory settings readback, forced protocol switching,
and bounded discovery restricted to the selected protocol. The Tramp cases use
wire fixtures; the connected bench transmitter uses SmartAudio.

Install the project's dependencies through the normal PlatformIO build first.
Then `make vtx-host-test` from the repository root compiles and runs the test
against the installed headers. The PowerShell helper does the same after
checking the dependency's provenance, using a C++11 compiler and Git:

```powershell
./tools/run_vtx_host_tests.ps1
```

The helper resolves `.pio/libdeps/m5stack-basic-v27/VTXControl_ESP32/src`, verifies
the installed Git root, origin, and exact commit, and requires an unchanged source
tree. It also rejects local headers that would shadow the pinned protocol headers.
PlatformIO's generated package metadata outside `src` is allowed. The compile
uses the installed protocol headers and our nonblocking UART adapter; upstream
high-level application sources are excluded, matching the firmware integration.
It does not use vendored headers, review clones, PlatformIO commands, or hardware.

The helper finds common MSYS2 compilers or a compiler on PATH. Explicit executable
paths and a different installed PlatformIO environment can be supplied:

```powershell
./tools/run_vtx_host_tests.ps1 -EnvironmentName m5stack-basic-v27 -Compiler 'C:/msys64/mingw64/bin/g++.exe' -GitExecutable 'C:/Program Files/Git/cmd/git.exe'
./tools/run_vtx_host_tests.ps1 -VerifyDependencyOnly
```

The test executable is rebuilt at `tests/vtx_host/test_vtx.exe`; compiler or test
failures stop the helper. When intentionally upgrading the dependency, update the
pin in both `platformio.ini` and the helper, then rerun the existing wire fixtures.
