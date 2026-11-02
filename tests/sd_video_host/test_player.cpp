#include "sd_video_player.h"
#include "sd_video_format.h"
#include "composite_video.h"
#include <SD.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <functional>
#include <algorithm>

MockConsole Serial;
SPIClass SPI;
MockSd SD;
SemaphoreHandle_t sharedBus;
void assertSharedBus() {
  assert(sharedBus && sharedBus->owner==std::this_thread::get_id());
}

size_t File::read(uint8_t* output,size_t wanted) {
  assertSharedBus();
  if(wanted>SD.maxReadSize) SD.maxReadSize=wanted;
  std::this_thread::sleep_for(std::chrono::milliseconds(SD.delayPerReadMs));
  if(!record_ || (SD.failPayloadRead && position_>=512)) return 0;
  const size_t received=std::min(wanted,record_->bytes.size()-position_);
  memcpy(output,record_->bytes.data()+position_,received); position_+=received;
  return received;
}
bool File::seek(uint32_t position) {
  assertSharedBus(); ++SD.seekCalls;
  SD.lastSeekPosition=position;
  if(!record_ || position>record_->bytes.size()) return false;
  position_=position; return true;
}
File File::openNextFile() {
  assertSharedBus(); ++SD.directoryCalls;
  return directoryIndex_<SD.files.size()?File(SD.files[directoryIndex_++]):File{};
}
File MockSd::open(const char* path,const char*) {
  assertSharedBus();
  if(!strcmp(path,"/")) {
    auto root=std::make_shared<MockFileRecord>(); root->directory=true; root->path="/"; return File(root);
  }
  for(const auto& file:files) if(file->path==path) return File(file);
  return {};
}

namespace CompositeVideo {
static uint8_t buffers[2][width*height] = {};
static int back=1;
static bool pending=false;
static std::chrono::steady_clock::time_point flipAt;
static std::mutex videoMutex;
static std::vector<uint8_t> presented;
static std::vector<uint32_t> presentedPalettes;
static std::vector<bool> presentedIndexed;
static uint32_t bankPaletteCrc[2] = {};
static bool bankIndexed[2] = {};
uint8_t* backbuffer() {
  std::lock_guard<std::mutex> guard(videoMutex);
  return pending?nullptr:buffers[back];
}
bool present() {
  std::lock_guard<std::mutex> guard(videoMutex);
  if(pending) return false;
  pending=true; flipAt=std::chrono::steady_clock::now()+std::chrono::milliseconds(1); return true;
}
bool setBackPalette(const uint8_t* palette) {
  std::lock_guard<std::mutex> guard(videoMutex);
  if(pending) return false;
  bankIndexed[back]=palette!=nullptr;
  bankPaletteCrc[back]=palette?SdVideoFormat::crc32(palette,768):0;
  return true;
}
bool presentPending() {
  std::lock_guard<std::mutex> guard(videoMutex);
  if(pending && std::chrono::steady_clock::now()>=flipAt) {
    assert(buffers[back][0]==buffers[back][width*height-1]);
    presented.push_back(buffers[back][0]);
    presentedPalettes.push_back(bankPaletteCrc[back]);
    presentedIndexed.push_back(bankIndexed[back]);
    back=1-back; pending=false;
  }
  return pending;
}
uint32_t presentedFrames() {
  std::lock_guard<std::mutex> guard(videoMutex); return uint32_t(presented.size());
}
}

static void put16(uint8_t* p,uint16_t value) { p[0]=value; p[1]=value>>8; }
static void put32(uint8_t* p,uint32_t value) { for(int i=0;i<4;++i) p[i]=uint8_t(value>>(8*i)); }
static std::shared_ptr<MockFileRecord> video(const char* path,uint8_t base=0x10,bool indexed=false,uint8_t paletteSeed=7) {
  auto file=std::make_shared<MockFileRecord>(); file->path=path;
  const uint16_t headerSize=indexed?1024:512;
  file->bytes.resize(headerSize+2*SdVideoFormat::frameBytes,0);
  uint8_t* h=file->bytes.data(); memcpy(h,indexed?"M5PAL256":"M5RGB332",8);
  put16(h+8,indexed?2:1); put16(h+10,headerSize); put16(h+12,256); put16(h+14,240);
  put16(h+16,15); put16(h+18,1); put32(h+20,2); put32(h+24,61440);
  memcpy(h+32,"Synthetic playback",18);
  if(indexed) for(size_t i=0;i<768;++i) h[96+i]=uint8_t(i+paletteSeed);
  put32(h+headerSize-4,SdVideoFormat::crc32(h,headerSize-4));
  memset(h+headerSize,base,SdVideoFormat::frameBytes);
  memset(h+headerSize+SdVideoFormat::frameBytes,base+1,SdVideoFormat::frameBytes);
  return file;
}
static void waitFor(const std::function<bool()>& condition,unsigned timeoutMs=3000) {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
  while(!condition()) {
    assert(std::chrono::steady_clock::now()<deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

int main() {
  sharedBus=xSemaphoreCreateMutex();
  // Firmware owns the player for the process lifetime, as does this host test.
  auto* player=new SdVideoPlayer;
  assert(!player->begin(nullptr));
  auto alpha=video("/alpha.M5V"), zed=video("/Zed.m5v",0x30);
  auto corrupt=video("/bad_crc.m5v"); corrupt->bytes[508]^=1;
  auto truncated=video("/truncated.m5v"); truncated->bytes.pop_back();
  auto wrongDimensions=video("/dimensions.m5v"); wrongDimensions->bytes[12]=1;
  auto tooFast=video("/rate.m5v"); tooFast->bytes[16]=31;
  auto unrelated=video("/ignore.bin");
  SD.files={zed,corrupt,truncated,alpha,wrongDimensions,tooFast,unrelated};
  assert(player->begin(sharedBus));
  waitFor([&]{return !player->status().scanning;});
  assert(player->count()==2 && player->status().mounted);
  assert(player->status().rejectedFiles==4 && player->status().sdErrors==0);
  assert(!strcmp(player->entry(0).name,"alpha.M5V") && !strcmp(player->entry(1).name,"Zed.m5v"));
  assert(!player->entry(2).path[0] && !player->play(2));
  assert(!player->status().playing && CompositeVideo::presentedFrames()==0);
  const unsigned directoryCalls=SD.directoryCalls;
  assert(player->play(0));
  waitFor([&]{return player->status().framesPresented>=4;});
  player->stop();
  assert(!player->status().playing && !player->status().loading);
  assert(SD.seekCalls>0 && SD.directoryCalls==directoryCalls);
  assert(SD.maxReadSize<=8192);
  {
    std::lock_guard<std::mutex> guard(CompositeVideo::videoMutex);
    const auto& frames=CompositeVideo::presented;
    assert(frames.size()>=4 && frames[0]==0x10 && frames[1]==0x11 && frames[2]==0x10 && frames[3]==0x11);
  }
  // Once stop acknowledges, a queued/stale SD frame cannot replace a built-in.
  uint8_t* builtin=CompositeVideo::backbuffer(); assert(builtin);
  assert(CompositeVideo::setBackPalette(nullptr));
  memset(builtin,0xaa,61440); assert(CompositeVideo::present());
  waitFor([]{return !CompositeVideo::presentPending();});
  const uint32_t stoppedFrames=CompositeVideo::presentedFrames();
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  assert(CompositeVideo::presentedFrames()==stoppedFrames);

  // Switching playback cancels incomplete reads before changing files.
  assert(player->play(0));
  waitFor([&]{return player->status().framesPresented>=1;});
  assert(player->play(1));
  waitFor([&]{return player->status().framesPresented>=2;});
  player->stop();
  {
    std::lock_guard<std::mutex> guard(CompositeVideo::videoMutex);
    const auto& frames=CompositeVideo::presented;
    assert(frames.back()==0x31 && frames[frames.size()-2]==0x30);
  }

  // A slow card repeats the previous front buffer and records an underrun;
  // failing reads never present a partial frame.
  SD.delayPerReadMs=12;
  assert(player->play(0));
  waitFor([&]{return player->status().framesPresented>=3;});
  player->stop(); assert(player->status().underruns>=1);
  SD.delayPerReadMs=1; SD.failPayloadRead=true;
  const auto beforeFailure=CompositeVideo::presentedFrames();
  assert(player->play(0));
  waitFor([&]{return !player->status().loading && player->status().selectedIndex<0;});
  assert(player->status().sdErrors==1 && CompositeVideo::presentedFrames()==beforeFailure);
  SD.failPayloadRead=false;

  // Replacement is caught by playback-time revalidation without an automatic
  // directory rescan or playback restart.
  alpha->bytes[0]='X';
  assert(player->play(0));
  waitFor([&]{return !player->status().loading && player->status().selectedIndex<0;});
  assert(player->status().sdErrors==2 && SD.directoryCalls==directoryCalls);
  alpha->bytes[0]='M';

  // No card retries occur until explicitly requested by the user.
  SD.mountAllowed=false;
  assert(player->rescan()); waitFor([&]{return !player->status().scanning;});
  assert(!player->status().mounted && player->count()==0);
  const unsigned attempts=SD.beginCalls;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(SD.beginCalls==attempts);
  SD.mountAllowed=true;
  SD.files={zed,alpha};
  assert(player->rescan()); waitFor([&]{return !player->status().scanning;});
  assert(player->count()==2 && !player->status().playing);
  player->stop();

  // Palette-indexed v2 files coexist with unchanged v1 files. The catalog
  // stores palette identity rather than allocating 768 bytes for every entry.
  static_assert(sizeof(SdVideoPlayer::Entry)<512,"Catalog must not copy full palettes");
  auto indexed=video("/palette.m5v",0x50,true);
  auto corruptIndexed=video("/bad_palette.m5v",0x50,true); corruptIndexed->bytes[96]^=1;
  auto truncatedIndexed=video("/short_palette.m5v",0x50,true); truncatedIndexed->bytes.resize(512);
  auto reservedIndexed=video("/reserved_palette.m5v",0x50,true);
  reservedIndexed->bytes[864]=1;
  put32(reservedIndexed->bytes.data()+1020,SdVideoFormat::crc32(reservedIndexed->bytes.data(),1020));
  SD.files={zed,indexed,alpha,corruptIndexed,truncatedIndexed,reservedIndexed};
  assert(player->rescan()); waitFor([&]{return !player->status().scanning;});
  assert(player->count()==3 && player->status().rejectedFiles==3);
  const auto paletteEntry=player->entry(1);
  assert(!strcmp(paletteEntry.name,"palette.m5v"));
  assert(paletteEntry.version==2 && paletteEntry.headerSize==1024);
  assert(paletteEntry.paletteCrc==SdVideoFormat::crc32(indexed->bytes.data()+96,768));
  assert(player->entry(0).version==1 && player->entry(0).headerSize==512 && player->entry(0).paletteCrc==0);
  const uint32_t firstIndexedFrame=CompositeVideo::presentedFrames();
  assert(player->play(1));
  waitFor([&]{return player->status().framesPresented>=4;}); player->stop();
  assert(player->status().version==2 && player->status().headerSize==1024);
  assert(SD.lastSeekPosition==1024);
  {
    std::lock_guard<std::mutex> guard(CompositeVideo::videoMutex);
    for(size_t i=firstIndexedFrame;i<firstIndexedFrame+4;++i) {
      assert(CompositeVideo::presented[i]==uint8_t(0x50+(i-firstIndexedFrame)%2));
      assert(CompositeVideo::presentedIndexed[i]);
      assert(CompositeVideo::presentedPalettes[i]==paletteEntry.paletteCrc);
    }
  }
  // v2 -> v1 clears each bank's palette before presenting the RGB332 frame.
  assert(player->play(0)); waitFor([&]{return player->status().framesPresented>=2;}); player->stop();
  {
    std::lock_guard<std::mutex> guard(CompositeVideo::videoMutex);
    const size_t last=CompositeVideo::presented.size()-1;
    assert(!CompositeVideo::presentedIndexed[last] && !CompositeVideo::presentedIndexed[last-1]);
    assert(!CompositeVideo::presentedPalettes[last] && !CompositeVideo::presentedPalettes[last-1]);
  }
  assert(SD.lastSeekPosition==512);
  // A palette may change while retaining a valid header CRC and payload size.
  // The cached identity must reject it until an explicit rescan occurs.
  const uint32_t beforePaletteChange=CompositeVideo::presentedFrames();
  const uint32_t errorsBeforePaletteChange=player->status().sdErrors;
  indexed->bytes[96]^=1;
  put32(indexed->bytes.data()+1020,SdVideoFormat::crc32(indexed->bytes.data(),1020));
  assert(player->play(1));
  waitFor([&]{return !player->status().loading && player->status().selectedIndex<0;});
  assert(player->status().sdErrors==errorsBeforePaletteChange+1);
  assert(CompositeVideo::presentedFrames()==beforePaletteChange);
  assert(player->rescan()); waitFor([&]{return !player->status().scanning;});
  assert(player->entry(1).paletteCrc!=paletteEntry.paletteCrc);
  assert(player->play(1)); waitFor([&]{return player->status().framesPresented>=1;}); player->stop();
  builtin=CompositeVideo::backbuffer(); assert(builtin);
  assert(CompositeVideo::setBackPalette(nullptr));
  memset(builtin,0xaa,61440); assert(CompositeVideo::present());
  waitFor([]{return !CompositeVideo::presentPending();});
  {
    std::lock_guard<std::mutex> guard(CompositeVideo::videoMutex);
    assert(!CompositeVideo::presentedIndexed.back() && CompositeVideo::presentedPalettes.back()==0);
  }
  puts("SD player v1/v2 tests passed: shared SPI, strict catalog/palette revalidation, correct loop offsets, palette/frame swaps, stop/switch, underruns/errors/rescan");
}
