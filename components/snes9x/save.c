/* This file is part of Snes9x. See LICENSE file. */

#include <stdio.h>
#include <string.h>
#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "cpuexec.h"
#include "apu.h"
#include "dma.h"
#include "display.h"
#include "srtc.h"
#include "soundux.h"
#include "sa1.h"
#include <stdlib.h>
#include "esp_heap_caps.h"

/* Version 004: bumped after the WRAM/VRAM/SRAM allocation-placement change
 * (Phase 50). States written by an older build serialized APU/CPU pointers
 * relative to the OLD IAPU.RAM/buffer addresses; the load-time rebase doesn't
 * fully cover the new layout, so loading an old state crashed on resume.
 * Bumping the version makes S9xLoadState reject incompatible states cleanly
 * (header mismatch → return false → fresh boot) instead of crashing. */
static const char header[16] = "SNES9X_000000004";

/* Write helper — returns false on error */
static bool wr(FILE *f, const void *data, size_t len)
{
   return fwrite(data, 1, len, f) == len;
}

/* Read helper — returns false on error */
static bool rd(FILE *f, void *data, size_t len)
{
   return fread(data, 1, len, f) == len;
}

bool S9xSaveState(const char *filename)
{
   FILE *fp = fopen(filename, "wb");
   if (!fp)
      return false;

   bool ok = true;

   /* Header */
   ok = ok && wr(fp, header, sizeof(header));

   /* CPU + ICPU */
   ok = ok && wr(fp, &CPU, sizeof(CPU));
   ok = ok && wr(fp, &ICPU, sizeof(ICPU));

   /* PPU + DMA */
   ok = ok && wr(fp, &PPU, sizeof(PPU));
   ok = ok && wr(fp, DMA, sizeof(SDMA) * 8);

   /* Memory blocks */
   ok = ok && wr(fp, Memory.VRAM, VRAM_SIZE);
   ok = ok && wr(fp, Memory.RAM, RAM_SIZE);
   ok = ok && wr(fp, Memory.SRAM, SRAM_SIZE);
   ok = ok && wr(fp, Memory.FillRAM, FILLRAM_SIZE);

   /* APU state (classic, non-blargg) */
   ok = ok && wr(fp, &APU, sizeof(APU));
   ok = ok && wr(fp, &IAPU, sizeof(IAPU));
   ok = ok && wr(fp, IAPU.RAM, 0x10000);

   /* SoundData */
   ok = ok && wr(fp, &SoundData, sizeof(SoundData));

   /* SA-1 carts only, so states of other games keep the same layout */
   if (Settings.SA1)
   {
      SA1.Registers.PC = SA1.PC - SA1.PCBase;
      S9xSA1PackStatus();
      ok = ok && wr(fp, &SA1, sizeof(SA1));
   }

   fclose(fp);

   if (ok)
      printf("SNES save state written: %s\n", filename);
   else
      printf("SNES save state FAILED: %s\n", filename);

   return ok;
}

bool S9xLoadState(const char *filename)
{
   FILE *fp = fopen(filename, "rb");
   if (!fp)
      return false;

   /* Verify header */
   char buf[16];
   if (!rd(fp, buf, sizeof(buf)) || memcmp(buf, header, sizeof(header)) != 0) {
      printf("SNES load state: wrong header\n");
      fclose(fp);
      return false;
   }

   /* Save IAPU.RAM pointer before overwriting — we need to fix up after load */
   uint8_t *IAPU_RAM = IAPU.RAM;

   S9xReset();

   bool ok = true;

   ok = ok && rd(fp, &CPU, sizeof(CPU));
   ok = ok && rd(fp, &ICPU, sizeof(ICPU));
   ok = ok && rd(fp, &PPU, sizeof(PPU));
   ok = ok && rd(fp, DMA, sizeof(SDMA) * 8);

   ok = ok && rd(fp, Memory.VRAM, VRAM_SIZE);
   ok = ok && rd(fp, Memory.RAM, RAM_SIZE);
   ok = ok && rd(fp, Memory.SRAM, SRAM_SIZE);
   ok = ok && rd(fp, Memory.FillRAM, FILLRAM_SIZE);

   ok = ok && rd(fp, &APU, sizeof(APU));
   ok = ok && rd(fp, &IAPU, sizeof(IAPU));
   ok = ok && rd(fp, IAPU_RAM, 0x10000);

   ok = ok && rd(fp, &SoundData, sizeof(SoundData));

   if (ok && Settings.SA1)
   {
      /* Only plain values are taken from the file; the pointers in SSA1
       * are rebuilt from the registers by S9xFixSA1AfterSnapshotLoad(). */
      SSA1* sa1_buf = (SSA1*) heap_caps_malloc(sizeof(SSA1), MALLOC_CAP_SPIRAM);  /* 32 KB */
      ok = sa1_buf && rd(fp, sa1_buf, sizeof(SSA1));
      if (ok)
      {
         const SSA1* sa1_state = sa1_buf;
         int i;
         SA1.Flags               = sa1_state->Flags;
         SA1.NMIActive           = sa1_state->NMIActive;
         SA1.IRQActive           = sa1_state->IRQActive;
         SA1.WaitingForInterrupt = sa1_state->WaitingForInterrupt;
         SA1.op1                 = sa1_state->op1;
         SA1.op2                 = sa1_state->op2;
         SA1.arithmetic_op       = sa1_state->arithmetic_op;
         SA1.sum                 = sa1_state->sum;
         SA1.overflow            = sa1_state->overflow;
         SA1.in_char_dma         = sa1_state->in_char_dma;
         SA1.variable_bit_pos    = sa1_state->variable_bit_pos;
         memcpy(&SA1.Registers, &sa1_state->Registers, sizeof(SA1.Registers));
         for (i = 0; i < 4; i++)
            S9xSetSA1MemMap(i, Memory.FillRAM [0x2220 + i]);
         S9xFixSA1AfterSnapshotLoad();
      }
      free(sa1_buf);
   }

   fclose(fp);

   if (!ok) {
      printf("SNES load state: read error\n");
      return false;
   }

   /* Fix up IAPU pointers — they were serialized as absolute addresses
    * from the save-time IAPU.RAM base. Rebase them to our current allocation. */
   IAPU.PC          = IAPU.PC          - IAPU.RAM + IAPU_RAM;
   IAPU.DirectPage  = IAPU.DirectPage  - IAPU.RAM + IAPU_RAM;
   IAPU.WaitAddress1 = IAPU.WaitAddress1 - IAPU.RAM + IAPU_RAM;
   IAPU.WaitAddress2 = IAPU.WaitAddress2 - IAPU.RAM + IAPU_RAM;
   IAPU.RAM = IAPU_RAM;

   /* Fix up derived state */
   FixROMSpeed();
   IPPU.ColorsChanged = true;
   IPPU.OBJChanged = true;
   CPU.InDMA = false;
   S9xFixColourBrightness();
   S9xAPUUnpackStatus();
   S9xFixSoundAfterSnapshotLoad();
   ICPU.ShiftedPB = ICPU.Registers.PB << 16;
   ICPU.ShiftedDB = ICPU.Registers.DB << 16;
   S9xSetPCBase(ICPU.ShiftedPB + ICPU.Registers.PC);
   S9xUnpackStatus();
   S9xFixCycles();
   S9xReschedule();

   printf("SNES save state loaded: %s\n", filename);
   return true;
}
