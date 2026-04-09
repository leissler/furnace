#include "engine.h"
#include "../ta-log.h"
#include "../utfutils.h"
#include "song.h"
#include "dispatch.h"

SafeWriter* DivEngine::saveXGM(bool loop) {
  stop();
  repeatPattern = false;
  setOrder(0);
  BUSY_BEGIN_SOFT;
  
  calcSongTimestamps();
  int loopOrder = curSubSong->ts.loopStart.order;
  int loopRow = curSubSong->ts.loopStart.row;
  logI("XGM loop point: %d %d", loopOrder, loopRow);
  warnings = "";

  curOrder = 0;
  freelance = false;
  playing = false;
  extValuePresent = false;
  remainingLoops = loop ? 1 : 0;

  SafeWriter* w = new SafeWriter;
  w->init();

  w->write("XGM ", 4);

  // Collect samples up to 63
  std::vector<int> xgmSamples;
  int xgmSampleIdTable[65536]; // song.sampleLen can be large
  memset(xgmSampleIdTable, -1, sizeof(xgmSampleIdTable));
  
  for (int i=0; i<song.sampleLen && xgmSamples.size() < 63; i++) {
    if (song.sample[i] && song.sample[i]->depth == DIV_SAMPLE_DEPTH_8BIT) {
      xgmSampleIdTable[i] = xgmSamples.size();
      xgmSamples.push_back(i);
    }
  }
  
  // Sample ID table (252 bytes)
  int sampleIdTablePos = w->size();
  for (int i=0; i<63; i++) {
    w->writeI(0); // placeholder
  }

  int sampleDataBlocSizePos = w->size();
  w->writeS(0);

  w->writeC(1); // Version
  w->writeC((fmod(curSubSong->hz,1.0) < 0.00001 && curSubSong->hz == 50.0)?1:0); // NTSC / PAL

  int sampleDataStart = w->size();
  unsigned int xgmSampleOff[64];
  unsigned int xgmSampleSize[64];
  int currentSampleOffset = 0;

  for (size_t i=0; i<63; i++) {
    if (i < xgmSamples.size()) {
       DivSample* s = song.sample[xgmSamples[i]];
       int len = s->length8;
       int padLen = (len + 255) & ~255;
       xgmSampleOff[i] = currentSampleOffset;
       xgmSampleSize[i] = padLen;
       w->write(s->data8, len);
       for(int j=0; j<padLen - len; j++) w->writeC(0);
       currentSampleOffset += padLen;
    } else {
       xgmSampleOff[i] = 0xFFFF;
       xgmSampleSize[i] = 256;
    }
  }

  // Rewrite sample ID table
  int currentPos = w->size();
  if (currentSampleOffset > 0) {
     w->seek(sampleIdTablePos, SEEK_SET);
     for (size_t i=0; i<63; i++) {
       w->writeS(xgmSampleOff[i] == 0xFFFF ? 0xFFFF : xgmSampleOff[i] / 256);
       w->writeS(xgmSampleSize[i] / 256);
     }
     w->seek(sampleDataBlocSizePos, SEEK_SET);
     w->writeS(currentSampleOffset / 256);
     w->seek(currentPos, SEEK_SET);
  }

  int musicDataSizePos = w->size();
  w->writeI(0); // placeholder for music data block size
  int musicDataStart = w->size();

  bool willExport[DIV_MAX_CHIPS];
  int pcmChAlloc = 0;
  int pcmChMask[DIV_MAX_CHIPS];
  
  int hasOPN2 = 0;
  int hasSN = 0;

  for (int i=0; i<DIV_MAX_CHIPS; i++) {
    willExport[i] = false;
    pcmChMask[i] = -1;
  }

  for (int i=0; i<song.systemLen; i++) {
    if (song.system[i] == DIV_SYSTEM_YM2612 || song.system[i] == DIV_SYSTEM_YM2612_EXT || song.system[i] == DIV_SYSTEM_YM2612_CSM) {
      if (!hasOPN2) {
        hasOPN2 = 1;
        willExport[i] = true;
        pcmChMask[i] = pcmChAlloc++;
      }
    } else if (song.system[i] == DIV_SYSTEM_SMS) {
      if (!hasSN) {
        hasSN = 1;
        willExport[i] = true;
      }
    } else if (song.system[i] == DIV_SYSTEM_PCM_DAC && pcmChAlloc < 4) {
      willExport[i] = true;
      pcmChMask[i] = pcmChAlloc++;
    } else if (song.system[i] == DIV_SYSTEM_YM2612_DUALPCM || song.system[i] == DIV_SYSTEM_YM2612_DUALPCM_EXT) {
      if (!hasOPN2) {
        hasOPN2 = 1;
        willExport[i] = true;
        pcmChMask[i] = pcmChAlloc;
        pcmChAlloc += 2; // Uses two PCM channels
      }
    }
  }

  int loopPos = -1;
  bool done = false;

  auto processWritesXGM = [&](int chipIndex, std::vector<DivRegWrite>& writes) {
    if (song.system[chipIndex] == DIV_SYSTEM_YM2612 || song.system[chipIndex] == DIV_SYSTEM_YM2612_EXT || song.system[chipIndex] == DIV_SYSTEM_YM2612_DUALPCM || song.system[chipIndex] == DIV_SYSTEM_YM2612_DUALPCM_EXT || song.system[chipIndex] == DIV_SYSTEM_YM2612_CSM) {
      int regCount0 = 0;
      int regCount1 = 0;
      int r0[256];
      int v0[256];
      int r1[256];
      int v1[256];
      
      for (size_t wi=0; wi<writes.size(); wi++) {
        DivRegWrite& write = writes[wi];
        if ((write.addr & 0xffff00ff) == 0xffff0000) { // Play sample
          int subCh = (write.addr & 0xff00) >> 8;
          int sId = xgmSampleIdTable[write.val];
          if (sId >= 0 && sId < 63) {
            int pcmChannel = (pcmChMask[chipIndex] == -1 ? 0 : pcmChMask[chipIndex]) + subCh;
            w->writeC(0x50 | (0x0C) | (pcmChannel & 3));
            w->writeC((sId + 1));
          }
        } else if ((write.addr & 0xffff00ff) == 0xffff0002) { // Stop PCM
          int subCh = (write.addr & 0xff00) >> 8;
          int pcmChannel = (pcmChMask[chipIndex] == -1 ? 0 : pcmChMask[chipIndex]) + subCh;
          w->writeC(0x50 | (0x0C) | (pcmChannel & 3));
          w->writeC(0);
        } else if (write.addr < 0x100) {
          r0[regCount0] = write.addr & 0xff;
          v0[regCount0] = write.val & 0xff;
          regCount0++;
          if (regCount0 == 16) {
             w->writeC(0x20 | (regCount0 - 1));
             for(int j=0; j<regCount0; j++) { w->writeC(r0[j]); w->writeC(v0[j]); }
             regCount0 = 0;
          }
        } else if ((write.addr & 0x300) == 0x100) {
          r1[regCount1] = write.addr & 0xff;
          v1[regCount1] = write.val & 0xff;
          regCount1++;
          if (regCount1 == 16) {
             w->writeC(0x30 | (regCount1 - 1));
             for(int j=0; j<regCount1; j++) { w->writeC(r1[j]); w->writeC(v1[j]); }
             regCount1 = 0;
          }
        }
      }
      if (regCount0 > 0) {
         w->writeC(0x20 | (regCount0 - 1));
         for(int j=0; j<regCount0; j++) { w->writeC(r0[j]); w->writeC(v0[j]); }
      }
      if (regCount1 > 0) {
         w->writeC(0x30 | (regCount1 - 1));
         for(int j=0; j<regCount1; j++) { w->writeC(r1[j]); w->writeC(v1[j]); }
      }
    } else if (song.system[chipIndex] == DIV_SYSTEM_SMS) {
      int regCount = 0;
      int v[256];
      for (size_t wi=0; wi<writes.size(); wi++) {
        DivRegWrite& write = writes[wi];
        if (write.addr == 0) {
           v[regCount++] = write.val & 0xff;
           if (regCount == 16) {
             w->writeC(0x10 | (regCount - 1));
             for(int j=0; j<regCount; j++) { w->writeC(v[j]); }
             regCount = 0;
           }
        }
      }
      if (regCount > 0) {
         w->writeC(0x10 | (regCount - 1));
         for(int j=0; j<regCount; j++) { w->writeC(v[j]); }
      }
    } else if (song.system[chipIndex] == DIV_SYSTEM_PCM_DAC) {
      for (size_t wi=0; wi<writes.size(); wi++) {
        DivRegWrite& write = writes[wi];
        if ((write.addr & 0xffff00ff) == 0xffff0000) { // Play sample
          int subCh = (write.addr & 0xff00) >> 8;
          int sId = xgmSampleIdTable[write.val];
          if (sId >= 0 && sId < 63) {
            int pcmChannel = (pcmChMask[chipIndex] == -1 ? 0 : pcmChMask[chipIndex]) + subCh;
            w->writeC(0x50 | (0x0C) | (pcmChannel & 3));
            w->writeC((sId + 1));
          }
        } else if ((write.addr & 0xffff00ff) == 0xffff0002) { // Stop PCM
          int subCh = (write.addr & 0xff00) >> 8;
          int pcmChannel = (pcmChMask[chipIndex] == -1 ? 0 : pcmChMask[chipIndex]) + subCh;
          w->writeC(0x50 | (0x0C) | (pcmChannel & 3));
          w->writeC(0);
        }
      }
    }
  };

  playSub(false);
  
  // To avoid evaluating thousands of times for 1 XGM frame (1/60th second) when engine hz is different,
  // we do the same "fractional Wait" logic VGM exporter has, but since we are locking to 60Hz,
  // it's simpler to just advance nextTick() and count the engine time until we reach 1/60s.
  // Actually, DivEngine::nextTick() steps by 1 Furnace tick.
  // We can just step nextTick() until end of song.
  double engineTime = 0.0;
  double xgmFrameTicks = curSubSong->hz / ((fmod(curSubSong->hz,1.0) < 0.00001 && curSubSong->hz == 50.0) ? 50.0 : 60.0);
  double expectedEngineTime = xgmFrameTicks;

  for (int i=0; i<song.systemLen; i++) {
    disCont[i].dispatch->toggleRegisterDump(true);
    disCont[i].dispatch->setSkipRegisterWrites(false);
    disCont[i].dispatch->getRegisterWrites().clear();
  }

  while (!done) {
    if (loopPos == -1 && curOrder == loopOrder && curRow == loopRow) {
      loopPos = w->size();
    }
    
    // Evaluate 1 furnace tick
    done = nextTick();
    
    // After nextTick, get register writes
    for (int i=0; i<song.systemLen; i++) {
       if (!willExport[i]) continue;
       std::vector<DivRegWrite>& writes = disCont[i].dispatch->getRegisterWrites();
       if (!writes.empty()) {
          processWritesXGM(i, writes);
          writes.clear();
       }
    }
    
    engineTime += 1.0;
    while (engineTime >= expectedEngineTime) {
       w->writeC(0x00); // write 1 XGM wait frame
       expectedEngineTime += xgmFrameTicks;
    }
    
    if (!playing) done = true;
  }

  if (loop && loopPos != -1) {
    w->writeC(0x7e);
    int loopOffset = loopPos - musicDataStart;
    w->writeC(loopOffset & 0xff);
    w->writeC((loopOffset >> 8) & 0xff);
    w->writeC((loopOffset >> 16) & 0xff);
  }
  
  w->writeC(0x7f); // End of music data
  
  int endPos = w->size();
  w->seek(musicDataSizePos, SEEK_SET);
  w->writeI(endPos - musicDataStart);
  w->seek(endPos, SEEK_SET);

  remainingLoops = -1;
  playing = false;
  freelance = false;
  extValuePresent = false;
  
  for (int i=0; i<song.systemLen; i++) {
    disCont[i].dispatch->toggleRegisterDump(false);
    disCont[i].dispatch->getRegisterWrites().clear();
  }
  
  BUSY_END;

  return w;
}
