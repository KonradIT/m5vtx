The test compiles the production `src/vtx_manager.cpp` using small host shims for
Arduino and the ESP32 UART/GPIO. It verifies wire parsing and state transitions;
it does not validate physical GPIO timing or a transmitter's actual behavior.

Coverage includes CRC and request-echo rejection, SmartAudio versions and the
verified DarwinFPV power-index offset, Tramp limits/settings/channel/power/pit
commands and race lock, mandatory settings readback, forced protocol switching,
and bounded discovery restricted to the selected protocol. The Tramp cases use
wire fixtures; the connected bench transmitter uses SmartAudio.

From the repository root (a C++11 compiler is sufficient):

```powershell
g++ -std=c++11 -I tests/vtx_host/stubs -I include -I lib/VTXControl/src tests/vtx_host/test_vtx.cpp src/vtx_manager.cpp -o tests/vtx_host/test_vtx.exe
./tests/vtx_host/test_vtx.exe
```
