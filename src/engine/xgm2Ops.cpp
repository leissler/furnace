#include "engine.h"
#include "../ta-log.h"
#include "../utfutils.h"
#include "song.h"
#include "dispatch.h"

SafeWriter* DivEngine::saveXGM2(bool loop) {
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

  SafeWriter* fm = new SafeWriter;
  fm->init();
  SafeWriter* psg = new SafeWriter;
  psg->init();

  w->write("XGM2", 4);

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
  for (int i=0; i<124; i++) {
    w->writeI(0); // placeholder
  }

  int sampleDataBlocSizePos = w->size();
  w->writeS(0);

  w->writeC(1); // Version
  w->writeC((fmod(curSubSong->hz,1.0) < 0.00001 && curSubSong->hz == 50.0)?1:0); // NTSC / PAL

  unsigned int xgmSampleOff[124];
  unsigned int xgmSampleSize[124];
  int currentSampleOffset = 0;

  for (size_t i=0; i<124; i++) {
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
       xgmSampleSize[i] = 0;
    }
  }

  // Rewrite sample ID table
  int currentPos = w->size();
  if (currentSampleOffset > 0) {
     w->seek(sampleIdTablePos, SEEK_SET);
     for (size_t i=0; i<124; i++) {
       w->writeS(xgmSampleOff[i] == 0xFFFF ? 0xFFFF : xgmSampleOff[i] / 256);
     }
     w->seek(sampleDataBlocSizePos, SEEK_SET);
     w->writeS((currentSampleOffset + 255) / 256);
     w->seek(currentPos, SEEK_SET);
  }

  int fmlenPos = w->size();
  w->writeS(0); // placeholder for fmlen
  int psglenPos = w->size();
  w->writeS(0); // placeholder for psglen

  // Note: we jump straight to data block writing, which must be aligned
  int startPadding = w->size() % 256;
  if(startPadding) for(int i=0;i<256-startPadding;i++) w->writeC(0);  bool willExport[DIV_MAX_CHIPS];
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

  int psgTone[4] = {0,0,0,0};
  int psgLcdc = 0;

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
        if ((write.addr & 0xffff00ff) == 0xffff0000 || (write.addr & 0xffff00ff) == 0xffff0001) { // Play sample
          int subCh = (write.addr & 0xff00) >> 8;
          int sId = xgmSampleIdTable[write.val];
          if (sId >= 0 && sId < 124) {
            int pcmChannel = (pcmChMask[chipIndex] == -1 ? 0 : pcmChMask[chipIndex]) + subCh;
            // Map freq \> 14kHz to Full speed (b2=0), < 14kHz to Half speed (b2=1)
            // By default, just set to full speed.
            int speedBit = 0;
            if ((write.addr & 0xffff00ff) == 0xffff0001) {
               // Simple freq mapping: If < 10kHz, half speed, else full
               int realFreq = (write.val*44100)/2790; // Approx rate mapping
               if (realFreq < 10000) speedBit = 4; // Set bit 2
            }
            fm->writeC(0x10 | (1 << 3) | speedBit | (pcmChannel % 3));
            fm->writeC((sId + 1));
          }
        } else if ((write.addr & 0xffff00ff) == 0xffff0002) { // Stop PCM
          int subCh = (write.addr & 0xff00) >> 8;
          int pcmChannel = (pcmChMask[chipIndex] == -1 ? 0 : pcmChMask[chipIndex]) + subCh;
          fm->writeC(0x10 | (1 << 3) | (pcmChannel % 3));
          fm->writeC(0);
        } else if (write.addr < 0x100) {
          r0[regCount0] = write.addr & 0xff;
          v0[regCount0] = write.val & 0xff;
          regCount0++;
          if (regCount0 == 8) {
             fm->writeC(0xE0 | (0 << 3) | (regCount0 - 1));
             for(int j=0; j<regCount0; j++) { fm->writeC(r0[j]); fm->writeC(v0[j]); }
             regCount0 = 0;
          }
        } else if ((write.addr & 0x300) == 0x100) {
          r1[regCount1] = write.addr & 0xff;
          v1[regCount1] = write.val & 0xff;
          regCount1++;
          if (regCount1 == 8) {
             fm->writeC(0xE0 | (1 << 3) | (regCount1 - 1));
             for(int j=0; j<regCount1; j++) { fm->writeC(r1[j]); fm->writeC(v1[j]); }
             regCount1 = 0;
          }
        }
      }
      if (regCount0 > 0) {
         fm->writeC(0xE0 | (0 << 3) | (regCount0 - 1));
         for(int j=0; j<regCount0; j++) { fm->writeC(r0[j]); fm->writeC(v0[j]); }
      }
      if (regCount1 > 0) {
         fm->writeC(0xE0 | (1 << 3) | (regCount1 - 1));
         for(int j=0; j<regCount1; j++) { fm->writeC(r1[j]); fm->writeC(v1[j]); }
      }
    } else if (song.system[chipIndex] == DIV_SYSTEM_SMS) {
      for (size_t wi=0; wi<writes.size(); wi++) {
        DivRegWrite& write = writes[wi];
        if (write.addr == 0) {
           int val = write.val & 0xff;
           if (val & 0x80) { // Latch
               int chan = (val >> 5) & 3;
               int type = (val >> 4) & 1; // 0=tone, 1=vol
               int data = val & 0x0F;
               psgLcdc = val & 0x70;
               if (type == 1) { // Vol
                   psg->writeC(((0x8 + chan) << 4) | data);
               } else { // Tone low
                   psgTone[chan] = (psgTone[chan] & ~0x0f) | data;
                   if (chan == 3) { // Noise is single byte write
                       psg->writeC(0x20 | 0x0C);
                       psg->writeC(data);
                   }
               }
           } else { // Data
               int chan = (psgLcdc >> 5) & 3;
               int type = (psgLcdc >> 4) & 1;
               if (type == 0 && chan < 3) { // Tone upper
                   int data = val & 0x3F;
                   int tone = psgTone[chan] & 0x0F;
                   tone |= (data << 4);
                   psgTone[chan] = tone; // 10-bit tone
                   int x = (chan << 2) | ((tone >> 8) & 3);
                   psg->writeC(0x20 | x);
                   psg->writeC(tone & 0xff);
               }
           }
        }
      }
    } else if (song.system[chipIndex] == DIV_SYSTEM_PCM_DAC) {
      for (size_t wi=0; wi<writes.size(); wi++) {
        DivRegWrite& write = writes[wi];
        if ((write.addr & 0xffff00ff) == 0xffff0000 || (write.addr & 0xffff00ff) == 0xffff0001) { // Play sample
          int subCh = (write.addr & 0xff00) >> 8;
          int sId = xgmSampleIdTable[write.val];
          if (sId >= 0 && sId < 124) {
            int pcmChannel = (pcmChMask[chipIndex] == -1 ? 0 : pcmChMask[chipIndex]) + subCh;
            fm->writeC(0x10 | (1 << 3) | (pcmChannel % 3));
            fm->writeC((sId + 1));
          }
        } else if ((write.addr & 0xffff00ff) == 0xffff0002) { // Stop PCM
          int subCh = (write.addr & 0xff00) >> 8;
          int pcmChannel = (pcmChMask[chipIndex] == -1 ? 0 : pcmChMask[chipIndex]) + subCh;
          fm->writeC(0x10 | (1 << 3) | (pcmChannel % 3));
          fm->writeC(0);
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
  double fmEngineTime = 0.0;
  double psgEngineTime = 0.0;
  double xgmFrameTicks = curSubSong->hz / ((fmod(curSubSong->hz,1.0) < 0.00001 && curSubSong->hz == 50.0) ? 50.0 : 60.0);
  double expectedFmTime = xgmFrameTicks;
  double expectedPsgTime = xgmFrameTicks;

  for (int i=0; i<song.systemLen; i++) {
    disCont[i].dispatch->toggleRegisterDump(true);
    disCont[i].dispatch->setSkipRegisterWrites(false);
    disCont[i].dispatch->getRegisterWrites().clear();
  }

  while (!done) {
    if (loopPos == -1 && curOrder == loopOrder && curRow == loopRow) {
      loopPos = fm->size();
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
    
    fmEngineTime += 1.0;
    psgEngineTime += 1.0;
    
    int fmWaitFrames = 0;
    while (fmEngineTime >= expectedFmTime) {
       fmWaitFrames++;
       expectedFmTime += xgmFrameTicks;
    }
    while (fmWaitFrames > 0) {
       if (fmWaitFrames <= 15) {
           fm->writeC(0x00 | (fmWaitFrames - 1));
           fmWaitFrames = 0;
       } else {
           int chunk = fmWaitFrames;
           if (chunk > 271) chunk = 271;
           fm->writeC(0x0F);
           fm->writeC(chunk - 16);
           fmWaitFrames -= chunk;
       }
    }
    
    int psgWaitFrames = 0;
    while (psgEngineTime >= expectedPsgTime) {
       psgWaitFrames++;
       expectedPsgTime += xgmFrameTicks;
    }
    while (psgWaitFrames > 0) {
       if (psgWaitFrames <= 14) {
           psg->writeC(0x00 | (psgWaitFrames - 1));
           psgWaitFrames = 0;
       } else {
           int chunk = psgWaitFrames;
           if (chunk > 270) chunk = 270;
           psg->writeC(0x0E);
           psg->writeC(chunk - 15);
           psgWaitFrames -= chunk;
       }
    }
    
    if (!playing) done = true;
  }

  if (loop && loopPos != -1) {
    fm->writeC(0xFF);
    int loopOffset = loopPos; // wait: loopPos is relative to fmdataplz
    fm->writeC(loopOffset & 0xff);
    fm->writeC((loopOffset >> 8) & 0xff);
    fm->writeC((loopOffset >> 16) & 0xff);
    
    psg->writeC(0x0F);
    psg->writeC(0);
    psg->writeC(0);
    psg->writeC(0); // If we wanted PSG looping, we need PSG loopPos
  } else {
    fm->writeC(0xFF);
    fm->writeC(0xff); fm->writeC(0xff); fm->writeC(0xff);
    psg->writeC(0x0F);
    psg->writeC(0xff); psg->writeC(0xff); psg->writeC(0xff);
  }
  
  // Pad FMDAT:
  int fmPad = fm->size() % 256;
  if(fmPad) for(int i=0;i<256-fmPad;i++) fm->writeC(0);
  
  // Pad PSGDAT:
  int psgPad = psg->size() % 256;
  if(psgPad) for(int i=0;i<256-psgPad;i++) psg->writeC(0);
  
  w->write(fm->getFinalBuf(), fm->size());
  w->write(psg->getFinalBuf(), psg->size());

  w->seek(fmlenPos, SEEK_SET);
  w->writeS(fm->size() / 256);
  w->seek(psglenPos, SEEK_SET);
  w->writeS(psg->size() / 256);

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
