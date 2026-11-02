#include "sd_video_format.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using SdVideoFormat::Error;

static void put16(uint8_t* p,uint16_t value) { p[0]=value; p[1]=value>>8; }
static void put32(uint8_t* p,uint32_t value) {
  for(int i=0;i<4;++i) p[i]=uint8_t(value>>(8*i));
}
static void seal(std::vector<uint8_t>& bytes) {
  const size_t checksum=bytes.size()-4;
  put32(bytes.data()+checksum,SdVideoFormat::crc32(bytes.data(),checksum));
}
static std::vector<uint8_t> makeHeader() {
  std::vector<uint8_t> bytes(512,0);
  memcpy(bytes.data(),"M5RGB332",8);
  put16(bytes.data()+8,1); put16(bytes.data()+10,512);
  put16(bytes.data()+12,256); put16(bytes.data()+14,240);
  put16(bytes.data()+16,15); put16(bytes.data()+18,1);
  put32(bytes.data()+20,444); put32(bytes.data()+24,61440);
  memcpy(bytes.data()+32,"Test card",9); seal(bytes);
  return bytes;
}

int main(int argc,char** argv) {
  assert(SdVideoFormat::crc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926);
  const auto good=makeHeader();
  SdVideoFormat::Header decoded;
  const uint64_t size=SdVideoFormat::fileBytes(444);
  assert(size==27279872 && SdVideoFormat::decode(good.data(),512,size,decoded)==Error::None);
  assert(decoded.frameCount==444 && decoded.fpsNum==15 && decoded.fpsDen==1);
  assert(decoded.version==1 && decoded.headerSize==512 && !decoded.isIndexed && decoded.paletteCrc==0);
  assert(!strcmp(decoded.title,"Test card"));
  assert(SdVideoFormat::decode(nullptr,0,size,decoded)==Error::ShortHeader);
  assert(SdVideoFormat::decode(good.data(),511,size,decoded)==Error::ShortHeader);
  assert(SdVideoFormat::decode(good.data(),512,size-1,decoded)==Error::FileSize);
  assert(SdVideoFormat::decode(good.data(),512,size+1,decoded)==Error::FileSize);
  const auto invalid=[&](size_t offset,uint8_t value,Error expected) {
    auto bytes=good; bytes[offset]=value; seal(bytes);
    assert(SdVideoFormat::decode(bytes.data(),512,size,decoded)==expected);
  };
  invalid(0,'X',Error::Magic);
  invalid(8,2,Error::Version);
  invalid(10,0xff,Error::HeaderSize);
  invalid(12,0xff,Error::Dimensions);
  invalid(14,0xef,Error::Dimensions);
  invalid(16,0,Error::FrameRate);
  invalid(16,31,Error::FrameRate);
  invalid(18,0,Error::FrameRate);
  invalid(24,1,Error::FrameSize);
  invalid(28,1,Error::Flags);
  invalid(96,1,Error::Reserved);
  auto badCrc=good; badCrc[508]^=1;
  assert(SdVideoFormat::decode(badCrc.data(),512,size,decoded)==Error::Checksum);
  auto zeroFrames=good; put32(zeroFrames.data()+20,0); seal(zeroFrames);
  assert(SdVideoFormat::decode(zeroFrames.data(),512,512,decoded)==Error::FrameCount);
  auto noTerminator=good; memset(noTerminator.data()+32,'x',64); seal(noTerminator);
  assert(SdVideoFormat::decode(noTerminator.data(),512,size,decoded)==Error::Title);
  auto badTitlePadding=good; badTitlePadding[50]='x'; seal(badTitlePadding);
  assert(SdVideoFormat::decode(badTitlePadding.data(),512,size,decoded)==Error::Title);
  auto utf8=good; memset(utf8.data()+32,0,64); memcpy(utf8.data()+32,"Caf\xc3\xa9",5); seal(utf8);
  assert(SdVideoFormat::decode(utf8.data(),512,size,decoded)==Error::None);
  utf8[32]=0xc0; utf8[33]=0x80; seal(utf8); // Invalid overlong NUL.
  assert(SdVideoFormat::decode(utf8.data(),512,size,decoded)==Error::Title);
  auto fractional=good; put16(fractional.data()+16,30000); put16(fractional.data()+18,1001); seal(fractional);
  assert(SdVideoFormat::decode(fractional.data(),512,size,decoded)==Error::None);
  auto huge=good; put32(huge.data()+20,0xffffffffu); seal(huge);
  const uint64_t hugeSize=SdVideoFormat::fileBytes(0xffffffffu);
  assert(hugeSize>0xffffffffu);
  assert(SdVideoFormat::decode(huge.data(),512,hugeSize,decoded)==Error::None);
  assert(decoded.frameCount==0xffffffffu);

  // v2 keeps the base fields but carries a complete RGB888 palette in its
  // 1024-byte header. RGB332 remains byte-for-byte compatible with v1.
  std::vector<uint8_t> indexed(1024,0);
  memcpy(indexed.data(),good.data(),96);
  memcpy(indexed.data(),"M5PAL256",8);
  put16(indexed.data()+8,2); put16(indexed.data()+10,1024);
  for(size_t i=0;i<256;++i) {
    indexed[96+i*3]=uint8_t(i);
    indexed[97+i*3]=uint8_t(255-i);
    indexed[98+i*3]=uint8_t(i*3);
  }
  seal(indexed);
  const uint64_t indexedSize=SdVideoFormat::fileBytes(444,1024);
  uint16_t declared=0;
  assert(SdVideoFormat::inspectPrefix(indexed.data(),16,declared)==Error::None && declared==1024);
  assert(SdVideoFormat::inspectPrefix(good.data(),16,declared)==Error::None && declared==512);
  assert(SdVideoFormat::inspectPrefix(indexed.data(),15,declared)==Error::ShortHeader && declared==0);
  assert(SdVideoFormat::decode(indexed.data(),1024,indexedSize,decoded)==Error::None);
  assert(decoded.version==2 && decoded.headerSize==1024 && decoded.isIndexed);
  assert(!memcmp(decoded.palette,indexed.data()+96,768));
  const uint32_t firstPaletteCrc=decoded.paletteCrc;
  assert(firstPaletteCrc==SdVideoFormat::crc32(indexed.data()+96,768));
  assert(SdVideoFormat::decode(indexed.data(),512,indexedSize,decoded)==Error::ShortHeader);
  assert(SdVideoFormat::decode(indexed.data(),1024,indexedSize-1,decoded)==Error::FileSize);
  assert(SdVideoFormat::decode(indexed.data(),1024,size,decoded)==Error::FileSize);
  auto wrongVersion=indexed; put16(wrongVersion.data()+8,1); seal(wrongVersion);
  assert(SdVideoFormat::decode(wrongVersion.data(),1024,indexedSize,decoded)==Error::Version);
  auto wrongSize=indexed; put16(wrongSize.data()+10,512); seal(wrongSize);
  assert(SdVideoFormat::decode(wrongSize.data(),1024,indexedSize,decoded)==Error::HeaderSize);
  auto badReserved=indexed; badReserved[864]=1; seal(badReserved);
  assert(SdVideoFormat::decode(badReserved.data(),1024,indexedSize,decoded)==Error::Reserved);
  auto indexedTitlePadding=indexed; indexedTitlePadding[95]='x'; seal(indexedTitlePadding);
  assert(SdVideoFormat::decode(indexedTitlePadding.data(),1024,indexedSize,decoded)==Error::Title);
  auto badPalette=indexed; badPalette[96]^=1;
  assert(SdVideoFormat::decode(badPalette.data(),1024,indexedSize,decoded)==Error::Checksum);
  seal(badPalette);
  assert(SdVideoFormat::decode(badPalette.data(),1024,indexedSize,decoded)==Error::None);
  assert(decoded.paletteCrc!=firstPaletteCrc);
  auto badIndexedCrc=indexed; badIndexedCrc[1020]^=1;
  assert(SdVideoFormat::decode(badIndexedCrc.data(),1024,indexedSize,decoded)==Error::Checksum);

  SdVideoFormat::FrameClock clock;
  clock.start(100,15,1);
  for(int i=0;i<15;++i) clock.advance();
  assert(clock.dueUs==1000100);
  clock.start(0,30000,1001);
  for(int i=0;i<30000;++i) clock.advance();
  assert(clock.dueUs==1001000000ull);

  for(int argument=1;argument<argc;++argument) {
    std::ifstream file(argv[argument],std::ios::binary|std::ios::ate);
    assert(file.good());
    const uint64_t actual=uint64_t(file.tellg());
    file.seekg(0);
    uint8_t bytes[SdVideoFormat::maxHeaderBytes];
    file.read(reinterpret_cast<char*>(bytes),16); assert(file.gcount()==16);
    assert(SdVideoFormat::inspectPrefix(bytes,16,declared)==Error::None);
    file.read(reinterpret_cast<char*>(bytes)+16,declared-16); assert(file.gcount()==declared-16);
    assert(SdVideoFormat::decode(bytes,declared,actual,decoded)==Error::None);
    printf("Actual converted file validated: v%u, %llu bytes, %lu frames, %u/%ufps, CRC32=%lu (%s)\n",
           decoded.version,
           static_cast<unsigned long long>(actual),static_cast<unsigned long>(decoded.frameCount),
           decoded.fpsNum,decoded.fpsDen,
           static_cast<unsigned long>(SdVideoFormat::read32(bytes+declared-4)),argv[argument]);
    if(decoded.isIndexed) printf("Indexed palette CRC32=%lu, title=%s\n",
                                 static_cast<unsigned long>(decoded.paletteCrc),decoded.title);
  }
  puts("M5V v1/v2 format tests passed: strict header/palette/payload/CRC/UTF8, 64-bit sizing, rational frame scheduling");
}
