#pragma once
void assertSharedBus();
class SPIClass {
 public:
  void begin(int,int,int,int) { assertSharedBus(); }
};
extern SPIClass SPI;
