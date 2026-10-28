// Exercise the production parser/state machine against wire frames without RF.
#include <deque>
#include <vector>
#include <cassert>
#include <cstring>
#include <cstdio>
#define private public
#include "vtx_manager.h"
#undef private
#include <VTX_SmartAudio.h>
#include <VTX_Tramp.h>
#include <soc/gpio_struct.h>
#include <driver/gpio.h>

uint32_t mockMillis = 0;
MockConsole Serial;
MockGpioRegisters GPIO;
MockGpioTrace mockGpioTrace;

static void inject(VtxManager& vtx, const std::vector<uint8_t>& bytes) {
  for (uint8_t byte : bytes) vtx.uart_.incoming.push_back(byte);
  vtx.receive();
}
static std::vector<uint8_t> sa(uint8_t code, std::initializer_list<uint8_t> payload) {
  std::vector<uint8_t> out{0xAA,0x55,code,uint8_t(payload.size())};
  out.insert(out.end(), payload);
  out.push_back(sa_CRC8(out.data()+2,out.size()-2));
  return out;
}
static std::vector<uint8_t> tramp(uint8_t command,std::initializer_list<uint8_t> payload) {
  std::vector<uint8_t> out(16,0);
  out[0]=0x0f; out[1]=command;
  std::copy(payload.begin(),payload.end(),out.begin()+2);
  out[14]=trampCrc(out.data());
  return out;
}

static void testProtocolSelectionAndTramp() {
  VtxManager selected;
  assert(selected.selectedProtocol()==VtxProtocol::Unknown);
  assert(selected.setProtocol(VtxProtocol::Tramp));
  assert(!selected.uartStarted_ && selected.selectedProtocol()==VtxProtocol::Tramp);
  assert(!selected.setProtocol(static_cast<VtxProtocol>(99)));
  assert(selected.selectedProtocol()==VtxProtocol::Tramp);
  selected.begin(16);
  assert(selected.activeProtocol_==VtxProtocol::Tramp && selected.status().probeCount==3);
  assert(selected.status().baud==9600 && selected.status().stopBits==1);
  assert(selected.status().protocol==VtxProtocol::Unknown && !selected.status().connected);
  // Even directly invoking the other family's parser cannot accept a reply.
  const auto wrongFamily=sa(9,{36,3,0,0x16,0xae});
  assert(!selected.parseSmartAudio(wrongFamily.data(),wrongFamily.size(),5));
  inject(selected,wrongFamily);
  assert(selected.status().validResponses==0 && !selected.status().connected);
  mockMillis+=51; selected.update();
  assert(selected.uart_.outgoing[1]=='r');
  inject(selected,selected.uart_.outgoing);
  assert(!selected.status().connected && selected.status().echoFrames==1);
  auto limits=tramp('r',{0x25,0x16,0x39,0x17,0x20,0x03}); // 5669..5945MHz,800mW.
  auto badLimits=limits; badLimits[14]^=0x80;
  inject(selected,badLimits);
  assert(selected.status().validResponses==0 && selected.status().crcErrors>0);
  inject(selected,limits);
  assert(selected.status().protocol==VtxProtocol::Tramp && !selected.status().connected);
  assert(!selected.setPower(400) && !selected.setChannel(36) && !selected.setPit(true));
  selected.update(); mockMillis+=201; selected.update();
  assert(selected.uart_.outgoing[1]=='v');
  inject(selected,tramp('v',{0xad,0x16,25,0,0,0})); selected.update();
  assert(selected.status().connected && selected.status().frequencyMHz==5805);
  assert(selected.status().powerMilliwatts==25 && !selected.status().pitMode);
  assert(!selected.setFrequency(5660) && !selected.setFrequency(5950));
  assert(!selected.setChannel(32)); // R1 lies below the advertised lower limit.
  assert(!selected.setPower(0) && !selected.setPower(801));
  assert(!selected.setPowerIndex(1)); // SA index commands do not apply to Tramp.

  const auto completeWrite=[&](uint8_t command,uint16_t argument,const std::vector<uint8_t>& reply) {
    const uint32_t before=selected.status().validResponses;
    selected.update();
    const auto& bytes=selected.uart_.outgoing;
    assert(bytes.size()==16 && bytes[0]==0x0f && bytes[1]==command);
    assert(bytes[2]==uint8_t(argument) && bytes[3]==uint8_t(argument>>8));
    assert(bytes[14]==trampCrc(bytes.data()) && bytes[15]==0);
    inject(selected,bytes); // SET echo/ACK must never stand in for a GET.
    assert(selected.status().validResponses==before);
    mockMillis+=701; selected.update(); mockMillis+=201; selected.update();
    assert(selected.uart_.outgoing[1]=='v');
    inject(selected,reply); selected.update();
    assert(selected.status().validResponses==before+1);
  };
  assert(selected.setPower(400));
  assert(selected.status().powerMilliwatts==25);
  completeWrite('P',400,tramp('v',{0xad,0x16,0x90,1,0,0}));
  assert(selected.status().powerMilliwatts==400);
  assert(selected.setChannel(36));
  completeWrite('F',5806,tramp('v',{0xae,0x16,0x90,1,0,0}));
  assert(selected.status().channelIndex==36 && selected.status().frequencyMHz==5806);
  assert(selected.setPit(true));
  completeWrite('I',0,tramp('v',{0xae,0x16,0x90,1,0,1}));
  assert(selected.status().pitMode);
  assert(selected.setPit(false));
  completeWrite('I',1,tramp('v',{0xae,0x16,0x90,1,0,0}));
  assert(!selected.status().pitMode);
  inject(selected,tramp('v',{0xae,0x16,0x90,1,1,0}));
  assert(selected.status().raceLocked);
  assert(!selected.setPower(25) && !selected.setChannel(37) && !selected.setPit(true));

  // Switching clears old settings and queued writes, and retains the new family
  // over reprobe/begin/pin changes without falling through to another protocol.
  assert(selected.setProtocol(VtxProtocol::SmartAudio));
  assert(selected.status().probeCount==34 && selected.status().probeIndex==0);
  assert(selected.activeProtocol_==VtxProtocol::SmartAudio);
  assert(selected.status().protocol==VtxProtocol::Unknown && !selected.status().connected);
  assert(!selected.parseTramp(limits.data()));
  inject(selected,limits);
  assert(selected.status().validResponses==0 && selected.status().powerMilliwatts==0);
  inject(selected,sa(9,{36,3,0,0x16,0xae}));
  assert(selected.status().connected && selected.setChannel(37));
  assert(selected.pendingLength_>0);
  assert(selected.setProtocol(VtxProtocol::Tramp));
  assert(selected.pendingLength_==0 && !selected.verifyPending_);
  assert(selected.status().frequencyMHz==0 && !selected.status().connected);
  assert(selected.setPin(17) && selected.selectedProtocol()==VtxProtocol::Tramp);
  assert(selected.activeProtocol_==VtxProtocol::Tramp && selected.status().probeCount==3);
  selected.reprobe();
  assert(selected.activeProtocol_==VtxProtocol::Tramp && selected.status().probeCount==3);
  selected.begin(16);
  assert(selected.activeProtocol_==VtxProtocol::Tramp && selected.selectedProtocol()==VtxProtocol::Tramp);

  // Exhaust each forced family. All emitted discovery frames remain read-only,
  // candidate progress stays within the filtered list, and no fallback crosses
  // into the other family even when the transmitter is silent.
  for(VtxProtocol family : {VtxProtocol::Tramp,VtxProtocol::SmartAudio}) {
    VtxManager silent;
    assert(silent.setProtocol(family)); silent.begin(16);
    const uint8_t count=silent.status().probeCount;
    for(int i=0;i<1000 && silent.status().probing;++i) {
      mockMillis+=800; silent.update();
      assert(silent.status().probeIndex<count);
      if(silent.uart_.outgoing.empty()) continue;
      const auto& bytes=silent.uart_.outgoing;
      if(family==VtxProtocol::Tramp) {
        assert(bytes.size()==16 && bytes[0]==0x0f);
        assert(bytes[1]=='r' || bytes[1]=='v');
        for(size_t j=2;j<14;++j) assert(bytes[j]==0);
      } else {
        size_t header=0; while(header<bytes.size() && bytes[header]!=0xaa) ++header;
        assert(header+4<bytes.size() && bytes[header+1]==0x55);
        assert(bytes[header+2]==3 && bytes[header+3]==0);
      }
    }
    assert(!silent.status().probing && !silent.status().connected);
    assert(silent.status().queries==count*2);
    assert(silent.selectedProtocol()==family && silent.status().probeIndex==count-1);
    assert(silent.probeCandidate(count)==nullptr);
    assert(silent.setProtocol(VtxProtocol::Unknown));
    assert(silent.selectedProtocol()==VtxProtocol::Unknown && silent.status().probeCount==37);
  }
}

int main() {
  assert(sa_CRC8("\xAA\x55\x03\x00",4)==0x9f);
  VtxManager vtx;
  vtx.begin(17);
  assert(vtx.status().probing && !vtx.status().connected);
  assert(vtx.activeProtocol_==VtxProtocol::SmartAudio && vtx.status().transportProfile==4);
  vtx.configure({VtxProtocol::Tramp,9600,false});
  mockMillis+=51; vtx.update();
  assert(vtx.uart_.outgoing[1]=='r');
  VtxManager::onRxEdge(&vtx); assert(vtx.responseEdges_==0); // Settling ignored.
  mockMillis+=2; VtxManager::onRxEdge(&vtx); assert(vtx.responseEdges_==1);
  inject(vtx,vtx.uart_.outgoing); // Half duplex request echo must not lock.
  assert(vtx.status().echoFrames==1 && !vtx.status().connected);
  const auto caps=tramp('r',{0x25,0x16,0x39,0x17,0x20,0x03}); // 5669..5945, 800mW
  inject(vtx,caps);
  assert(vtx.status().protocol==VtxProtocol::Tramp);
  assert(!vtx.status().connected && vtx.status().maxPowerMilliwatts==800);
  assert(vtx.status().minFrequencyMHz==5669);
  vtx.update(); mockMillis+=201; vtx.update();
  assert(vtx.uart_.outgoing[1]=='v');
  auto settings=tramp('v',{0xad,0x16,0x19,0,0,0,0x19,0}); // 5805MHz,25mW
  auto bad=settings; bad[4]^=1; inject(vtx,bad);
  assert(!vtx.status().connected && vtx.status().crcErrors>0);
  inject(vtx,settings);
  assert(vtx.status().connected && !vtx.status().probing);
  assert(vtx.status().frequencyMHz==5805 && vtx.status().powerMilliwatts==25);
  assert(vtx.status().channelIndex==3);
  assert(!vtx.setPower(1600) && !vtx.setFrequency(6100));
  vtx.update();
  assert(vtx.setFrequency(5840));
  assert(vtx.status().frequencyMHz==5805); // Queueing is not readback.
  vtx.update();
  assert(vtx.uart_.outgoing[1]=='F' && vtx.uart_.outgoing[2]==0xd0 && vtx.uart_.outgoing[3]==0x16);
  mockMillis+=800; vtx.update(); mockMillis+=201; vtx.update();
  assert(vtx.uart_.outgoing[1]=='v'); // Mandatory GET after write.
  inject(vtx,tramp('v',{0xd0,0x16,0x19,0,0,0}));
  assert(vtx.status().frequencyMHz==5840);

  vtx.reprobe();
  vtx.configure({VtxProtocol::SmartAudio,4800,false});
  vtx.sendQuery();
  inject(vtx,vtx.uart_.outgoing);
  assert(!vtx.status().connected && vtx.status().echoFrames==1);
  // SmartAudio v2, 5805MHz, raw power index2, pit bit1 (not bit2).
  auto saSettings=sa(9,{3,2,2,0x16,0xad});
  auto corrupt=saSettings; corrupt.back()^=0x40;
  inject(vtx,corrupt); assert(!vtx.status().connected);
  // Feed split/noisy packets to test stream assembly and resynchronization.
  inject(vtx,{0xff,0x00,saSettings[0],saSettings[1],saSettings[2]});
  inject(vtx,std::vector<uint8_t>(saSettings.begin()+3,saSettings.end()));
  assert(vtx.status().connected && vtx.status().saVersion==2);
  assert(vtx.status().pitMode && vtx.status().frequencyMHz==5805);
  assert(vtx.status().powerIndex==2 && vtx.status().powerMilliwatts==0);
  assert(vtx.status().powerCount==3);
  assert(!vtx.setPower(800) && !vtx.setPit(true));
  assert(vtx.setPowerIndex(1));
  inject(vtx,sa(9,{3,0,0,0x16,0xad}));
  assert(vtx.status().powerIndex==0 && vtx.status().powerCount==3);

  vtx.reprobe(); vtx.configure({VtxProtocol::SmartAudio,4900,false});
  // v2.1 advertises 0dBm pit,14dBm/20dBm/26dBm levels.
  inject(vtx,sa(0x11,{3,1,0,0x16,0xad,20,3,0,14,20,26}));
  assert(vtx.status().saVersion==21 && vtx.status().powerCount==3);
  assert(vtx.status().powerLevelsDbm[0]==14 && vtx.status().powerIndex==1);
  assert(vtx.status().powerMilliwatts==100);
  assert(vtx.setPower(25));
  assert(vtx.pending_[5]==(0x80|14));
  vtx.pendingLength_=0;
  assert(vtx.setPit(true)); assert(vtx.pending_[5]==0x80);

  // Published TBS rev09 vectors (response length includes CRC).
  vtx.reprobe(); vtx.configure({VtxProtocol::SmartAudio,4800,false});
  inject(vtx,{0xaa,0x55,0x01,0x06,0x00,0x00,0x01,0x16,0xe9,0x4d});
  assert(vtx.status().saVersion==1 && vtx.status().frequencyMHz==5865);
  vtx.reprobe(); vtx.configure({VtxProtocol::SmartAudio,4800,false});
  inject(vtx,{0xaa,0x55,0x11,0x0c,0,0,0,0x16,0xe9,0x0e,3,0,0x0e,0x14,0x1a,1});
  assert(vtx.status().saVersion==21 && vtx.status().frequencyMHz==5865);
  assert(vtx.status().powerCount==3 && vtx.status().powerLevelsDbm[2]==26);

  for(uint8_t profile=1; profile<=3; ++profile) {
    vtx.reprobe(); vtx.configure({VtxProtocol::SmartAudio,4800,false,profile});
    vtx.sendQuery(); inject(vtx,vtx.uart_.outgoing);
    assert(vtx.status().echoFrames==1 && !vtx.status().connected);
    inject(vtx,sa(9,{3,2,0,0x16,0xad}));
    assert(vtx.setChannel(1));
    assert(vtx.pendingLength_==(profile==2 ? 6 : 7));
    assert(vtx.pending_[profile==2 ? 0 : 1]==0xaa);
  }
  vtx.reprobe(); vtx.configure({VtxProtocol::SmartAudio,4800,false,4});
  vtx.sendQuery();
  assert(vtx.lastTxLength_==7 && vtx.lastTx_[0]==0 && vtx.lastTx_[6]==0);
  assert(mockGpioTrace.pulls[17]==GPIO_FLOATING);
  inject(vtx,sa(9,{36,3,0,0x16,0xae}));
  assert(vtx.setPowerIndex(2));
  assert(vtx.pendingLength_==8 && vtx.pending_[0]==0 && vtx.pending_[7]==0);

  // Clone format with length including the CRC still needs a valid CRC.
  vtx.reprobe(); vtx.configure({VtxProtocol::SmartAudio,4800,false});
  auto clone=sa(9,{3,2,0,0x16,0xad});
  ++clone[3]; clone.back()=sa_CRC8(clone.data()+2,clone.size()-3);
  inject(vtx,clone);
  assert(vtx.status().connected && vtx.status().frequencyMHz==5805);

  // Four failed status queries mark a formerly working link disconnected.
  vtx.waiting_=false; vtx.gotResponse_=false; vtx.nextAction_=mockMillis;
  for(int i=0;i<16 && vtx.status().connected;++i) { mockMillis+=500; vtx.update(); }
  assert(!vtx.status().connected);

  // A complete discovery run with no incoming bytes stops boundedly.
  VtxManager absent; absent.begin(17);
  for(int i=0;i<1000 && absent.status().probing;++i) { mockMillis+=500; absent.update(); }
  assert(!absent.status().probing && !absent.status().connected);
  assert(absent.status().queries==absent.status().probeCount*2);
  // A runtime pin switch must release the previous wire and clear old state.
  assert(absent.pin()==17);
  assert(absent.setPin(16));
  assert(absent.pin()==16 && absent.status().probing);
  assert(mockGpioTrace.directions[17]==GPIO_MODE_INPUT);
  assert(mockGpioTrace.pulls[17]==GPIO_FLOATING);
  assert(absent.status().validResponses==0 && absent.responseEdges_==0);
  assert(!absent.setPin(26) && absent.pin()==16);
  absent.begin(17);
  assert(absent.pin()==17);
  assert(mockGpioTrace.directions[16]==GPIO_MODE_INPUT);
  assert(mockGpioTrace.pulls[16]==GPIO_FLOATING);
  VtxManager remembered;
  assert(remembered.setKnownPowerCount(4));
  assert(!remembered.setKnownPowerCount(0) && !remembered.setKnownPowerCount(9));
  remembered.begin(16);
  inject(remembered,sa(9,{36,0,0,0x16,0xae}));
  assert(remembered.status().powerCount==4 && remembered.setPowerIndex(3));
  remembered.reprobe();
  inject(remembered,sa(9,{36,0,0,0x16,0xae}));
  assert(remembered.status().powerCount==4);

  // A real response arriving at500ms remains inside the extended reply window.
  VtxManager delayed;
  delayed.begin(16);
  mockMillis+=60; delayed.update();
  mockMillis+=500; delayed.update();
  assert(delayed.waiting_ && delayed.misses_==0);
  inject(delayed,sa(9,{36,3,0,0x16,0xae}));
  delayed.update();
  assert(delayed.status().connected);
  assert(delayed.setChannel(37)); delayed.update();
  assert(delayed.verifyPending_ && delayed.waiting_);
  const uint32_t beforeAck=delayed.status().validResponses;
  inject(delayed,sa(3,{37,1})); // Matching SET_CHANNEL ACK, not full settings.
  assert(delayed.gotResponse_);
  assert(delayed.status().channelIndex==36 && delayed.status().validResponses==beforeAck);
  delayed.update();
  mockMillis+=201; delayed.update();
  assert(!delayed.verifyPending_ && delayed.lastTx_[3]==3); // GET_SETTINGS wire code.
  inject(delayed,sa(9,{37,3,0,0x16,0xd3})); delayed.update();
  assert(delayed.status().frequencyMHz==5843 && delayed.status().channelIndex==37);

  // An optional SET ACK missing must not count toward status-query failures.
  assert(delayed.setChannel(36)); delayed.update();
  mockMillis+=701; delayed.update();
  assert(delayed.misses_==0 && delayed.status().connected);

  // Connected DarwinFPV: SET is zero-based, but GET reports one-based power.
  VtxManager darwin;
  assert(darwin.setKnownPowerCount(4));
  assert(darwin.setSa2PowerOffset(1));
  assert(darwin.sa2KnownCount_==0);
  assert(!darwin.setSa2PowerOffset(2));
  assert(darwin.setKnownPowerCount(3));
  darwin.begin(16);
  inject(darwin,sa(9,{36,3,0,0x16,0xae}));
  assert(darwin.status().rawPowerIndex==3 && darwin.status().powerIndex==2);
  assert(darwin.status().powerCount==3 && !darwin.setPowerIndex(3));
  assert(darwin.setPowerIndex(1));
  assert(darwin.pending_[5]==1); // SET wire value stays zero-based.
  darwin.pendingLength_=0;
  inject(darwin,sa(9,{36,2,0,0x16,0xae}));
  assert(darwin.status().rawPowerIndex==2 && darwin.status().powerIndex==1);
  assert(darwin.status().powerCount==3);
  inject(darwin,sa(9,{36,0,0,0x16,0xae})); // Invalid below configured GET offset.
  assert(darwin.status().rawPowerIndex==2 && darwin.status().powerIndex==1);
  assert(darwin.setPowerIndex(2));
  assert(darwin.pending_[5]==2);
  testProtocolSelectionAndTramp();
  puts("VTX host tests passed: CRC/echo, SA versions, Tramp limits/settings/pit/lock, readback, forced protocol switching, bounded filtered discovery");
}
