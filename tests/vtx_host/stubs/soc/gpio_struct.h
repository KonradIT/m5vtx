#pragma once
struct MockGpioRegisters { unsigned enable = 0; struct { unsigned data = 0; } enable1; };
extern MockGpioRegisters GPIO;
