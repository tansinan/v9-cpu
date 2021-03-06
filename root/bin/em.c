// em -- cpu emulator
//
// Usage:  em [-v] [-m memsize] [-f filesys] file
//
// Description:
//
// Written by Robert Swierczek

#include <u.h>
#include <libc.h>
#include <libm.h>
#include <dir.h>

enum {
  MEM_SZ = 128*1024*1024, // default memory size of virtual machine (128M)
  TB_SZ  =     1024*1024, // page translation buffer array size (4G / pagesize)
  FS_SZ  =   4*1024*1024, // ram file system size (4M)
  TPAGES = 4096,          // maximum cached page translations
};

enum {           // page table entry flags
  PTE_P = 0x001, // Present
  PTE_W = 0x002, // Writeable
  PTE_U = 0x004, // User
  PTE_A = 0x020, // Accessed
  PTE_D = 0x040, // Dirty
};

enum {           // processor fault codes (some can be masked together)
  FMEM,          // bad physical address
  FTIMER,        // timer interrupt
  FKEYBD,        // keyboard interrupt
  FPRIV,         // privileged instruction
  FINST,         // illegal instruction
  FSYS,          // software trap
  FARITH,        // arithmetic trap
  FIPAGE,        // page fault on opcode fetch
  FWPAGE,        // page fault on write
  FRPAGE,        // page fault on read
  USER = 16      // user mode exception
};

uint verbose,    // chatty option -v
  memory, memorySize,    // physical memory
  user,          // user mode
  iena,          // interrupt enable
  ipend,         // interrupt pending
  trap,          // fault code
  ivec,          // interrupt vector
  vadr,          // bad virtual address
  virtualMemoryEnabled,        // virtual memory enabled
  pageDirectory,          // page directory
  tpage[TPAGES], // valid page translations
  tpages,        // number of cached page translations
  *kernelReadPageTable, *kernelWritePageTable,    // kernel read/write page transation tables
  *userReadPageTable, *userWritePageTable,    // user read/write page transation tables
  *currentReadPageTable,  *currentWritePageTable;     // current read/write page transation tables

char *cmd;       // command name

static int dbg;  // debugger enable flag
static char dbgbuf[0x200];

void *new(int size)
{
  void *p;
  if ((p = sbrk((size + 7) & -8)) == (void *)-1) { dprintf(2,"%s : fatal: unable to sbrk(%d)\n", cmd, size); exit(-1); }
  return (void *)(((int)p + 7) & -8);
}

void flush()
{
  uint v;
//  static int xx; if (tpages >= xx) { xx = tpages; dprintf(2,"****** flush(%d)\n",tpages); }
//  if (verbose) printf("F(%d)",tpages);
  while (tpages) {
    v = tpage[--tpages];
    kernelReadPageTable[v] = kernelWritePageTable[v] = userReadPageTable[v] = userWritePageTable[v] = 0;
  }
}

uint setpage(uint v, uint p, uint writable, uint userable)
{
  if (p >= memorySize) { trap = FMEM; vadr = v; return 0; }
  p = ((v ^ (memory + p)) & -4096) + 1;
  if (!kernelReadPageTable[v >>= 12]) {
    if (tpages >= TPAGES) flush();
    tpage[tpages++] = v;
  }
//  if (verbose) printf(".");
  kernelReadPageTable[v] = p;
  kernelWritePageTable[v] = writable ? p : 0;
  userReadPageTable[v] = userable ? p : 0;
  userWritePageTable[v] = (userable && writable) ? p : 0;
  return p;
}

uint rlook(uint v)
{
  uint pde, *ppde, pte, *ppte, q, userable;
//  dprintf(2,"rlook(%08x)\n",v);
  if (!virtualMemoryEnabled) return setpage(v, v, 1, 1);
  pde = *(ppde = (uint *)(pageDirectory + (v>>22<<2))); // page directory entry
  if (pde & PTE_P) {
    if (!(pde & PTE_A)) *ppde = pde | PTE_A;
    if (pde >= memorySize) { trap = FMEM; vadr = v; return 0; }
    pte = *(ppte = (uint *)(memory + (pde & -4096) + ((v >> 10) & 0xffc))); // page table entry
    if ((pte & PTE_P) && ((userable = (q = pte & pde) & PTE_U) || !user)) {
      if (!(pte & PTE_A)) *ppte = pte | PTE_A;
      return setpage(v, pte, (pte & PTE_D) && (q & PTE_W), userable); // set writable after first write so dirty gets set
    }
  }
  trap = FRPAGE;
  vadr = v;
  return 0;
}

uint wlook(uint v)
{
  uint pde, *ppde, pte, *ppte, q, userable;
//  dprintf(2,"wlook(%08x)\n",v);
  if (!virtualMemoryEnabled) return setpage(v, v, 1, 1);
  pde = *(ppde = (uint *)(pageDirectory + (v>>22<<2))); // page directory entry
  if (pde & PTE_P) {
    if (!(pde & PTE_A)) *ppde = pde | PTE_A;
    if (pde >= memorySize) { trap = FMEM; vadr = v; return 0; }
    pte = *(ppte = (uint *)(memory + (pde & -4096) + ((v >> 10) & 0xffc)));  // page table entry
    if ((pte & PTE_P) && (((userable = (q = pte & pde) & PTE_U) || !user) && (q & PTE_W))) {
      if ((pte & (PTE_D | PTE_A)) != (PTE_D | PTE_A)) *ppte = pte | (PTE_D | PTE_A);
      return setpage(v, pte, q & PTE_W, userable);
    }
  }
  trap = FWPAGE;
  vadr = v;
  return 0;
}

static char dbg_getcmd(char *buf)
{
  char c;
  char *pos = buf;

  printf("\ndbg => ");
  fflush(stdout);
  fflush(stdin);
  fflush(stderr);

  do {
    c = getchar();
    putchar(c);
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != EOF)
      *pos++ = c;
  } while(c != EOF && c != '\n' && c != '\r');

    return buf[0];
}

static char *DBG_HELP_STRING = "\n"
	"h:\tprint help commands.\n"
	"q:\tquit.\n"
	"c:\tcontinue.\n"
	"s:\tsingle step for one instruction.\n"
	"i:\tdisplay registers.\n"
	"x:\tdisplay memory, the input address is hex number (e.g x 10000)\n";

static char *DBG_REG_CONTEX = "\n"
	"ra:\t%x\n"
	"rb:\t%x\n"
	"rc:\t%x\n"
	"rd:\t%8.8x\t[cur sp]\n"
	"re:\t%8.8x\t[next pc]\n"
	"ff:\t%f\n"
	"fg:\t%f\n\n"
	"tsp:\t%8.8x\t[top sp]\n"
        "user:\t%x\t\t[user mode or not]\n"
        "iena:\t%x\t\t[interrupt flag]\n"
        "trap:\t%x\t\t[current trap]\n"
        "vmem:\t%x\t\t[virtual memory enabled or not]\n\n"
        "ipend:\t%8.8x\t[interrupted pending or not]\n\n";

void cpu(uint pc, uint sp)
{
  uint integerRegisterFile[3];
  const int A = 0;
  const int B = 1;
  const int C = 2;

  uint a, b, c, ssp, usp, t, p, v, u, delta, cycle, xcycle, timer, timeout, fpc, tpc, xsp, tsp, fsp;
  double f, g;
  int immediate, *xpc, kbchar;
  char ch;
  struct pollfd pfd;

  static char rbuf[4096]; // XXX

  a = b = c = timer = timeout = fpc = tsp = fsp = 0;
  cycle = delta = 4096;
  xcycle = delta * 4;
  kbchar = -1;
  xpc = 0;
  tpc = -pc;
  xsp = sp;
  goto fixpc;

fixsp:
  if ((p = currentWritePageTable[(v = xsp - tsp) >> 12])) {
    tsp = (xsp = v ^ (p-1)) - v;
    fsp = (4096 - (xsp & 4095)) << 8;
  }

  for (;;) {
    if ((uint)xpc == fpc) {
fixpc:
      if (!(p = currentReadPageTable[(v = (uint)xpc - tpc) >> 12]) && !(p = rlook(v))) { trap = FIPAGE; goto exception; }
      xcycle -= tpc;
      xcycle += (tpc = (uint)(xpc = (int *)(v ^ (p-1))) - v);
      fpc = ((uint)xpc + 4096) & -4096;
next:
      if ((uint)xpc > xcycle) {
        cycle += delta;
        xcycle += delta * 4;
        if (iena || !(ipend & FKEYBD)) { // XXX dont do this, use a small queue instead
          pfd.fd = 0;
          pfd.events = POLLIN;
          if (poll(&pfd, 1, 0) == 1 && read(0, &ch, 1) == 1) {
            kbchar = ch;
            if (kbchar == '`') { dprintf(2,"ungraceful exit. cycle = %u\n", cycle + (int)((uint)xpc - xcycle)/4); return; }
            if (iena) { trap = FKEYBD; iena = 0; goto interrupt; }
            ipend |= FKEYBD;
          }
        }
        if (timeout) {
          timer += delta;
          if (timer >= timeout) { // XXX  // any interrupt actually!
//          dprintf(2,"timeout! timer=%d, timeout=%d\n",timer,timeout);
            timer = 0;
            if (iena) { trap = FTIMER; iena = 0; goto interrupt; }
            ipend |= FTIMER;
          }
        }
      }
    }

    immediate = *xpc++;

    if (dbg) {
    again:
      switch(dbg_getcmd(dbgbuf)) {
      case 'c':
        dbg = 0;
        break;
      case 's':
        printf("[%8.8x] %lx\n", (uint)xpc - tpc, *xpc);
        break;
      case 'q':
        exit(0);
      case 'i':
        printf(DBG_REG_CONTEX,
               a, b, c, xsp - tsp, (uint)xpc - tpc, f, g,
               (user ? usp : ssp) - tsp, user, iena, trap, virtualMemoryEnabled, ipend);
        goto again;
      case 'x':
        if((sscanf(dbgbuf + 1, "%x", &u) != 1)
           || (!(t = currentReadPageTable[u >> 12]) && !(t = rlook(u))))
          printf("\ninvalid address: %s.\n", dbgbuf + 1);
        else
          printf("\n[%8.8x]: %2.2x\n", u, *((unsigned char *)(u ^ (t & -2))));
        goto again;
      case 'h':
      default:
        printf(DBG_HELP_STRING);
        goto again;
      }
    }

    switch ((uchar)immediate) {
    case HALT: if (user || verbose) dprintf(2,"halt(%d) cycle = %u\n", a, cycle + (int)((uint)xpc - xcycle)/4); return; // XXX should be supervisor!
    case IDLE: if (user) { trap = FPRIV; break; }
      if (!iena) { trap = FINST; break; } // XXX this will be fatal !!!
      for (;;) {
        pfd.fd = 0;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 0) == 1 && read(0, &ch, 1) == 1) {
          kbchar = ch;
          if (kbchar == '`') { dprintf(2,"ungraceful exit. cycle = %u\n", cycle + (int)((uint)xpc - xcycle)/4); return; }
          trap = FKEYBD;
          iena = 0;
          goto interrupt;
        }
        cycle += delta;
        if (timeout) {
          timer += delta;
          if (timer >= timeout) { // XXX  // any interrupt actually!
//        dprintf(2,"IDLE timeout! timer=%d, timeout=%d\n",timer,timeout);
            timer = 0;
            trap = FTIMER;
            iena = 0;
            goto interrupt;
          }
        }
      }

    // memory -- designed to be restartable/continuable after exception/interrupt
    case MCPY: // while (c) { *a = *b; a++; b++; c--; }
      while (c) {
        if (!(t = currentReadPageTable[b >> 12]) && !(t = rlook(b))) goto exception;
        if (!(p = currentWritePageTable[a >> 12]) && !(p = wlook(a))) goto exception;
        if ((v = 4096 - (a & 4095)) > c) v = c;
        if ((u = 4096 - (b & 4095)) > v) u = v;
        memcpy((char *)(a ^ (p & -2)), (char *)(b ^ (t & -2)), u);
        a += u; b += u; c -= u;
//        if (!(++cycle % DELTA)) { pc -= 4; break; } XXX
      }
      continue;

    case MCMP: // for (;;) { if (!c) { a = 0; break; } if (*b != *a) { a = *b - *a; b += c; c = 0; break; } a++; b++; c--; }
      for (;;) {
        if (!c) { a = 0; break; }
        if (!(t = currentReadPageTable[b >> 12]) && !(t = rlook(b))) goto exception;
        if (!(p = currentReadPageTable[a >> 12]) && !(p = rlook(a))) goto exception;
        if ((v = 4096 - (a & 4095)) > c) v = c;
        if ((u = 4096 - (b & 4095)) > v) u = v;
        if ((t = memcmp((char *)(a ^ (p & -2)), (char *)(b ^ (t & -2)), u))) { a = t; b += c; c = 0; break; }
        a += u; b += u; c -= u;
//        if (!(++cycle % DELTA)) { pc -= 4; break; } XXX
      }
      continue;

    case MCHR: // for (;;) { if (!c) { a = 0; break; } if (*a == b) { c = 0; break; } a++; c--; }
      for (;;) {
        if (!c) { a = 0; break; }
        if (!(p = currentReadPageTable[a >> 12]) && !(p = rlook(a))) goto exception;
        if ((u = 4096 - (a & 4095)) > c) u = c;
        if ((t = (uint)memchr((char *)(v = a ^ (p & -2)), b, u))) { a += t - v; c = 0; break; }
        a += u; c -= u;
//        if (!(++cycle % DELTA)) { pc -= 4; break; } XXX
      }
      continue;

    case MSET: // while (c) { *a = b; a++; c--; }
      while (c) {
        if (!(p = currentWritePageTable[a >> 12]) && !(p = wlook(a))) goto exception;
        if ((u = 4096 - (a & 4095)) > c) u = c;
        memset((char *)(a ^ (p & -2)), b, u);
        a += u; c -= u;
//        if (!(++cycle % DELTA)) { pc -= 4; break; } XXX
      }
      continue;

    // math
    case POW:  f = pow(f,g); continue;
    case ATN2: f = atan2(f,g); continue;
    case FABS: f = fabs(f); continue;
    case ATAN: f = atan(f); continue;
    case LOG:  if (f) f = log(f); continue; // XXX others?
    case LOGT: if (f) f = log10(f); continue; // XXX
    case EXP:  f = exp(f); continue;
    case FLOR: f = floor(f); continue;
    case CEIL: f = ceil(f); continue;
    case HYPO: f = hypot(f,g); continue;
    case SIN:  f = sin(f); continue;
    case COS:  f = cos(f); continue;
    case TAN:  f = tan(f); continue;
    case ASIN: f = asin(f); continue;
    case ACOS: f = acos(f); continue;
    case SINH: f = sinh(f); continue;
    case COSH: f = cosh(f); continue;
    case TANH: f = tanh(f); continue;
    case SQRT: f = sqrt(f); continue;
    case FMOD: f = fmod(f,g); continue;

    case ENT:  if (fsp && (fsp -= immediate & -256) > 4096<<8) fsp = 0; xsp += immediate>>8; if (fsp) continue; goto fixsp;
    case LEV:  if (immediate < fsp) { t = *(uint *)(xsp + (immediate>>8)) + tpc; fsp -= (immediate + 0x800) & -256; } // XXX revisit this mess
               else { if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; t = *(uint *)((v ^ p) & -8) + tpc; fsp = 0; }
               xsp += (immediate>>8) + 8; xcycle += t - (uint)xpc; if ((uint)(xpc = (int *)t) - fpc < -4096) goto fixpc; goto next;

    // jump
    case JMP:  xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next;
    case JMPI: if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8) + (a<<2)) >> 12]) && !(p = rlook(v))) break;
               xcycle += (t = *(uint *)((v ^ p) & -4)); if ((uint)(xpc = (int *)((uint)xpc + t)) - fpc < -4096) goto fixpc; goto next;
    case JSR:  if (fsp & (4095<<8)) { xsp -= 8; fsp += 8<<8; *(uint *)xsp = (uint)xpc - tpc; }
               else { if (!(p = currentWritePageTable[(v = xsp - tsp - 8) >> 12]) && !(p = wlook(v))) break; *(uint *)((v ^ p) & -8) = (uint)xpc - tpc; fsp = 0; xsp -= 8; }
               xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next;
    case JSRA: if (fsp & (4095<<8)) { xsp -= 8; fsp += 8<<8; *(uint *)xsp = (uint)xpc - tpc; }
               else { if (!(p = currentWritePageTable[(v = xsp - tsp - 8) >> 12]) && !(p = wlook(v))) break; *(uint *)((v ^ p) & -8) = (uint)xpc - tpc; fsp = 0; xsp -= 8; }
               xcycle += a + tpc - (uint)xpc; if ((uint)(xpc = (int *)(a + tpc)) - fpc < -4096) goto fixpc; goto next;

    // stack
    case PSHA: if (fsp & (4095<<8)) { xsp -= 8; fsp += 8<<8; *(uint *)xsp = a; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp - 8) >> 12]) && !(p = wlook(v))) break; *(uint *)   ((v ^ p) & -8) = a;     xsp -= 8; fsp = 0; goto fixsp;
    case PSHB: if (fsp & (4095<<8)) { xsp -= 8; fsp += 8<<8; *(uint *)xsp = b; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp - 8) >> 12]) && !(p = wlook(v))) break; *(uint *)   ((v ^ p) & -8) = b;     xsp -= 8; fsp = 0; goto fixsp;
    case PSHC: if (fsp & (4095<<8)) { xsp -= 8; fsp += 8<<8; *(uint *)xsp = c; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp - 8) >> 12]) && !(p = wlook(v))) break; *(uint *)   ((v ^ p) & -8) = c;     xsp -= 8; fsp = 0; goto fixsp;
    case PSHF: if (fsp & (4095<<8)) { xsp -= 8; fsp += 8<<8; *(double *)xsp = f; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp - 8) >> 12]) && !(p = wlook(v))) break; *(double *) ((v ^ p) & -8) = f;     xsp -= 8; fsp = 0; goto fixsp;
    case PSHG: if (fsp & (4095<<8)) { xsp -= 8; fsp += 8<<8; *(double *)xsp = g; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp - 8) >> 12]) && !(p = wlook(v))) break; *(double *) ((v ^ p) & -8) = g;     xsp -= 8; fsp = 0; goto fixsp;
    case PSHI: if (fsp & (4095<<8)) { xsp -= 8; fsp += 8<<8; *(int *)xsp = immediate>>8; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp - 8) >> 12]) && !(p = wlook(v))) break; *(int *)    ((v ^ p) & -8) = immediate>>8; xsp -= 8; fsp = 0; goto fixsp;

    case POPA: if (fsp) { a = *(uint *)xsp; xsp += 8; fsp -= 8<<8; continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp) >> 12]) && !(p = rlook(v))) break; a = *(uint *)   ((v ^ p) & -8); xsp += 8; goto fixsp;
    case POPB: if (fsp) { b = *(uint *)xsp; xsp += 8; fsp -= 8<<8; continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp) >> 12]) && !(p = rlook(v))) break; b = *(uint *)   ((v ^ p) & -8); xsp += 8; goto fixsp;
    case POPC: if (fsp) { c = *(uint *)xsp; xsp += 8; fsp -= 8<<8; continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp) >> 12]) && !(p = rlook(v))) break; c = *(uint *)   ((v ^ p) & -8); xsp += 8; goto fixsp;
    case POPF: if (fsp) { f = *(double *)xsp; xsp += 8; fsp -= 8<<8; continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp) >> 12]) && !(p = rlook(v))) break; f = *(double *) ((v ^ p) & -8); xsp += 8; goto fixsp;
    case POPG: if (fsp) { g = *(double *)xsp; xsp += 8; fsp -= 8<<8; continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp) >> 12]) && !(p = rlook(v))) break; g = *(double *) ((v ^ p) & -8); xsp += 8; goto fixsp;

    // load effective address
    case LEA:  a = xsp - tsp + (immediate>>8); continue;
    case LEAG: a = (uint)xpc - tpc + (immediate>>8); continue;

    // load a local
    case LL:   if (immediate < fsp) { a = *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(uint *) ((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LLS:  if (immediate < fsp) { a = *(short *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(short *) ((v ^ p) & -2);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LLH:  if (immediate < fsp) { a = *(ushort *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(ushort *) ((v ^ p) & -2);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LLC:  if (immediate < fsp) { a = *(char *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(char *) (v ^ p & -2);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LLB:  if (immediate < fsp) { a = *(uchar *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(uchar *) (v ^ p & -2);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LLD:  if (immediate < fsp) { f = *(double *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; f = *(double *) ((v ^ p) & -8);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LLF:  if (immediate < fsp) { f = *(float *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; f = *(float *)  ((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    // load a global
    case LG:   if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(uint *)   ((v ^ p) & -4); continue;
    case LGS:  if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(short *)  ((v ^ p) & -2); continue;
    case LGH:  if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(ushort *) ((v ^ p) & -2); continue;
    case LGC:  if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(char *)   (v ^ p & -2); continue;
    case LGB:  if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(uchar *)  (v ^ p & -2); continue;
    case LGD:  if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; f = *(double *) ((v ^ p) & -8); continue;
    case LGF:  if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; f = *(float *)  ((v ^ p) & -4); continue;

    // load a indexed
    case LX:   if (!(p = currentReadPageTable[(v = a + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(uint *)   ((v ^ p) & -4); continue;
    case LXS:  if (!(p = currentReadPageTable[(v = a + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(short *)  ((v ^ p) & -2); continue;
    case LXH:  if (!(p = currentReadPageTable[(v = a + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(ushort *) ((v ^ p) & -2); continue;
    case LXC:  if (!(p = currentReadPageTable[(v = a + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(char *)   (v ^ p & -2); continue;
    case LXB:  if (!(p = currentReadPageTable[(v = a + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = *(uchar *)  (v ^ p & -2); continue;
    case LXD:  if (!(p = currentReadPageTable[(v = a + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; f = *(double *) ((v ^ p) & -8); continue;
    case LXF:  if (!(p = currentReadPageTable[(v = a + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; f = *(float *)  ((v ^ p) & -4); continue;

    // load a immediate
    case LI:   a = immediate>>8; continue;
    case LHI:  a = a<<24 | (uint)immediate>>8; continue;
    case LIF:  f = (immediate>>8)/256.0; continue;

    // load b local
    case LBL:  if (immediate < fsp) { b = *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(uint *) ((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LBLS: if (immediate < fsp) { b = *(short *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(short *)  ((v ^ p) & -2);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LBLH: if (immediate < fsp) { b = *(ushort *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(ushort *) ((v ^ p) & -2);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LBLC: if (immediate < fsp) { b = *(char *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(char *)  (v ^ p & -2);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LBLB: if (immediate < fsp) { b = *(uchar *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(uchar *)  (v ^ p & -2);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LBLD: if (immediate < fsp) { g = *(double *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; g = *(double *) ((v ^ p) & -8);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case LBLF: if (immediate < fsp) { g = *(float *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; g = *(float *)  ((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    // load b global
    case LBG:  if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(uint *)   ((v ^ p) & -4); continue;
    case LBGS: if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(short *)  ((v ^ p) & -2); continue;
    case LBGH: if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(ushort *) ((v ^ p) & -2); continue;
    case LBGC: if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(char *)   (v ^ p & -2); continue;
    case LBGB: if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(uchar *)  (v ^ p & -2); continue;
    case LBGD: if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; g = *(double *) ((v ^ p) & -8); continue;
    case LBGF: if (!(p = currentReadPageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; g = *(float *)  ((v ^ p) & -4); continue;

    // load b indexed
    case LBX:  if (!(p = currentReadPageTable[(v = b + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(uint *)   ((v ^ p) & -4); continue;
    case LBXS: if (!(p = currentReadPageTable[(v = b + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(short *)  ((v ^ p) & -2); continue;
    case LBXH: if (!(p = currentReadPageTable[(v = b + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(ushort *) ((v ^ p) & -2); continue;
    case LBXC: if (!(p = currentReadPageTable[(v = b + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(char *)   (v ^ p & -2); continue;
    case LBXB: if (!(p = currentReadPageTable[(v = b + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; b = *(uchar *)  (v ^ p & -2); continue;
    case LBXD: if (!(p = currentReadPageTable[(v = b + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; g = *(double *) ((v ^ p) & -8); continue;
    case LBXF: if (!(p = currentReadPageTable[(v = b + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; g = *(float *)  ((v ^ p) & -4); continue;

    // load b immediate
    case LBI:  b = immediate>>8; continue;
    case LBHI: b = b<<24 | (uint)immediate>>8; continue;
    case LBIF: g = (immediate>>8)/256.0; continue;

    // misc transfer
    case LCL:  if (immediate < fsp) { c = *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; c = *(uint *) ((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case LBA:  b = a; continue;  // XXX need LAB, LAC to improve k.c  // or maybe a = a * imm + b ?  or b = b * imm + a ?
    case LCA:  c = a; continue;
    case LBAD: g = f; continue;

    // store a local
    case SL:   if (immediate < fsp) { *(uint *)(xsp + (immediate>>8)) = a; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(uint *) ((v ^ p) & -4) = a;
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case SLH:  if (immediate < fsp) { *(ushort *)(xsp + (immediate>>8)) = a; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(ushort *) ((v ^ p) & -2) = a;
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case SLB:  if (immediate < fsp) { *(uchar *)(xsp + (immediate>>8)) = a; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(uchar *) (v ^ p & -2) = a;
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case SLD:  if (immediate < fsp) { *(double *)(xsp + (immediate>>8)) = f; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(double *) ((v ^ p) & -8) = f;
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;
    case SLF:  if (immediate < fsp) { *(float *)(xsp + (immediate>>8)) = f; continue; }
               if (!(p = currentWritePageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(float *) ((v ^ p) & -4) = f;
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    // store a global
    case SG:   if (!(p = currentWritePageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(uint *)   ((v ^ p) & -4) = a; continue;
    case SGH:  if (!(p = currentWritePageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(ushort *) ((v ^ p) & -2) = a; continue;
    case SGB:  if (!(p = currentWritePageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(uchar *)  (v ^ p & -2)   = a; continue;
    case SGD:  if (!(p = currentWritePageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(double *) ((v ^ p) & -8) = f; continue;
    case SGF:  if (!(p = currentWritePageTable[(v = (uint)xpc - tpc + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(float *)  ((v ^ p) & -4) = f; continue;

    // store a indexed
    case SX:   if (!(p = currentWritePageTable[(v = b + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(uint *)   ((v ^ p) & -4) = a; continue;
    case SXH:  if (!(p = currentWritePageTable[(v = b + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(ushort *) ((v ^ p) & -2) = a; continue;
    case SXB:  if (!(p = currentWritePageTable[(v = b + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(uchar *)  (v ^ p & -2)   = a; continue;
    case SXD:  if (!(p = currentWritePageTable[(v = b + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(double *) ((v ^ p) & -8) = f; continue;
    case SXF:  if (!(p = currentWritePageTable[(v = b + (immediate>>8)) >> 12]) && !(p = wlook(v))) break; *(float *)  ((v ^ p) & -4) = f; continue;

    // arithmetic
    case ADDF: f += g; continue;
    case SUBF: f -= g; continue;
    case MULF: f *= g; continue;
    case DIVF: if (g == 0.0) { trap = FARITH; break; } f /= g; continue; // XXX

    case ADD:  a += b; continue;
    case ADDI: a += immediate>>8; continue;
    case ADDL: if (immediate < fsp) { a += *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a += *(uint *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case SUB:  a -= b; continue;
    case SUBI: a -= immediate>>8; continue;
    case SUBL: if (immediate < fsp) { a -= *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a -= *(uint *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case MUL:  a = (int)a * (int)b; continue; // XXX MLU ???
    case MULI: a = (int)a * (immediate>>8); continue;
    case MULL: if (immediate < fsp) { a = (int)a * *(int *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = (int)a * *(int *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case DIV:  if (!b) { trap = FARITH; break; } a = (int)a / (int)b; continue;
    case DIVI: if (!(t = immediate>>8)) { trap = FARITH; break; } a = (int)a / (int)t; continue;
    case DIVL: if (immediate < fsp) { if (!(t = *(uint *)(xsp + (immediate>>8)))) { trap = FARITH; break; } a = (int)a / (int)t; continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; if (!(t = *(uint *)((v ^ p) & -4))) { trap = FARITH; break; } a = (int)a / (int)t;
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case DVU:  if (!b) { trap = FARITH; break; } a /= b; continue;
    case DVUI: if (!(t = immediate>>8)) { trap = FARITH; break; } a /= t; continue;
    case DVUL: if (immediate < fsp) { if (!(t = *(int *)(xsp + (immediate>>8)))) { trap = FARITH; break; } a /= t; continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; if (!(t = *(uint *)((v ^ p) & -4))) { trap = FARITH; break; } a /= t;
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case MOD:  a = (int)a % (int)b; continue;
    case MODI: a = (int)a % (immediate>>8); continue;
    case MODL: if (immediate < fsp) { a = (int)a % *(int *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = (int)a % *(int *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case MDU:  a %= b; continue;
    case MDUI: a %= (immediate>>8); continue;
    case MDUL: if (immediate < fsp) { a %= *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a %= *(uint *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case AND:  a &= b; continue;
    case ANDI: a &= immediate>>8; continue;
    case ANDL: if (immediate < fsp) { a &= *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a &= *(uint *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case OR:   a |= b; continue;
    case ORI:  a |= immediate>>8; continue;
    case ORL:  if (immediate < fsp) { a |= *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a |= *(uint *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case XOR:  a ^= b; continue;
    case XORI: a ^= immediate>>8; continue;
    case XORL: if (immediate < fsp) { a ^= *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a ^= *(uint *)((v ^ p) & -4);
               if ((fsp || (v ^ (xsp - tsp)) & -4096)) continue; goto fixsp;

    case SHL:  a <<= b; continue;
    case SHLI: a <<= immediate>>8; continue;
    case SHLL: if (immediate < fsp) { a <<= *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a <<= *(uint *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case SHR:  a = (int)a >> (int)b; continue;
    case SHRI: a = (int)a >> (immediate>>8); continue;
    case SHRL: if (immediate < fsp) { a = (int)a >> *(int *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a = (int)a >> *(int *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    case SRU:  a >>= b; continue;
    case SRUI: a >>= immediate>>8; continue;
    case SRUL: if (immediate < fsp) { a >>= *(uint *)(xsp + (immediate>>8)); continue; }
               if (!(p = currentReadPageTable[(v = xsp - tsp + (immediate>>8)) >> 12]) && !(p = rlook(v))) break; a >>= *(uint *)((v ^ p) & -4);
               if (fsp || (v ^ (xsp - tsp)) & -4096) continue; goto fixsp;

    // logical
    case EQ:   a = a == b; continue;
    case EQF:  a = f == g; continue;
    case NE:   a = a != b; continue;
    case NEF:  a = f != g; continue;
    case LT:   a = (int)a < (int)b; continue;
    case LTU:  a = a < b; continue;
    case LTF:  a = f < g; continue;
    case GE:   a = (int)a >= (int)b; continue;
    case GEU:  a = a >= b; continue;
    case GEF:  a = f >= g; continue;

    // branch
    case BZ:   if (!a)               { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BZF:  if (!f)               { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BNZ:  if (a)                { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BNZF: if (f)                { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BE:   if (a == b)           { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BEF:  if (f == g)           { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BNE:  if (a != b)           { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BNEF: if (f != g)           { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BLT:  if ((int)a < (int)b)  { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BLTU: if (a < b)            { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BLTF: if (f <  g)           { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BGE:  if ((int)a >= (int)b) { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BGEU: if (a >= b)           { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;
    case BGEF: if (f >= g)           { xcycle += immediate>>8; if ((uint)(xpc += immediate>>10) - fpc < -4096) goto fixpc; goto next; } continue;

    // conversion
    case CID:  f = (int)a; continue;
    case CUD:  f = a; continue;
    case CDI:  a = (int)f; continue;
    case CDU:  a = f; continue;

    // misc
    case BIN:  if (user) { trap = FPRIV; break; } a = kbchar; kbchar = -1; continue;  // XXX
    case BOUT: if (user) { trap = FPRIV; break; } if (a != 1) { dprintf(2,"bad write a=%d\n",a); return; } ch = b; a = write(a, &ch, 1); continue;
    case SSP:  xsp = a; tsp = fsp = 0; goto fixsp;

    case NOP:  continue;
    case CYC:  a = cycle + (int)((uint)xpc - xcycle)/4; continue; // XXX protected?  XXX also need wall clock time instruction
    case MSIZ: if (user) { trap = FPRIV; break; } a = memorySize; continue;

    case CLI:  if (user) { trap = FPRIV; break; } a = iena; iena = 0; continue;
    case STI:  if (user) { trap = FPRIV; break; } if (ipend) { trap = ipend & -ipend; ipend ^= trap; iena = 0; goto interrupt; } iena = 1; continue;

    case RTI:
      if (user) { trap = FPRIV; break; }
      xsp -= tsp; tsp = fsp = 0;
      if (!(p = currentReadPageTable[xsp >> 12]) && !(p = rlook(xsp))) { dprintf(2,"RTI kstack fault\n"); goto fatal; }
      t = *(uint *)((xsp ^ p) & -8); xsp += 8;
      if (!(p = currentReadPageTable[xsp >> 12]) && !(p = rlook(xsp))) { dprintf(2,"RTI kstack fault\n"); goto fatal; }
      xcycle += (pc = *(uint *)((xsp ^ p) & -8) + tpc) - (uint)xpc; xsp += 8;
      xpc = (int *)pc;
      if (t & USER) { ssp = xsp; xsp = usp; user = 1; currentReadPageTable = userReadPageTable; currentWritePageTable = userWritePageTable; }
      if (!iena) { if (ipend) { trap = ipend & -ipend; ipend ^= trap; goto interrupt; } iena = 1; }
      goto fixpc; // page may be invalid

    case IVEC: if (user) { trap = FPRIV; break; } ivec = a; continue;
    case PDIR: if (user) { trap = FPRIV; break; } if (a > memorySize) { trap = FMEM; break; } pageDirectory = (memory + a) & -4096; flush(); fsp = 0; goto fixpc; // set page directory
    case SPAG: if (user) { trap = FPRIV; break; } if (a && !pageDirectory) { trap = FMEM; break; } virtualMemoryEnabled = a; flush(); fsp = 0; goto fixpc; // enable paging

    case TIME: if (user) { trap = FPRIV; break; }
       if (immediate>>8) { dprintf(2,"timer%d=%u timeout=%u\n", immediate>>8, timer, timeout); continue; }    // XXX undocumented feature!
       timeout = a; continue; // XXX cancel pending interrupts if disabled?

    // XXX need some sort of user mode thread locking functions to support user mode semaphores, etc.  atomic test/set?

    case LVAD: if (user) { trap = FPRIV; break; } a = vadr; continue;

    case TRAP: trap = FSYS; break;

    case LUSP: if (user) { trap = FPRIV; break; } a = usp; continue;
    case SUSP: if (user) { trap = FPRIV; break; } usp = a; continue;

    default: trap = FINST; break;
    }
exception:
    if (!iena) { dprintf(2,"exception in interrupt handler\n"); goto fatal; }
interrupt:
    xsp -= tsp; tsp = fsp = 0;
    if (user) { usp = xsp; xsp = ssp; user = 0; currentReadPageTable = kernelReadPageTable; currentWritePageTable = kernelWritePageTable; trap |= USER; }
    xsp -= 8; if (!(p = currentWritePageTable[xsp >> 12]) && !(p = wlook(xsp))) { dprintf(2,"kstack fault!\n"); goto fatal; }
    *(uint *)((xsp ^ p) & -8) = (uint)xpc - tpc;
    xsp -= 8; if (!(p = currentWritePageTable[xsp >> 12]) && !(p = wlook(xsp))) { dprintf(2,"kstack fault\n"); goto fatal; }
    *(uint *)((xsp ^ p) & -8) = trap;
    xcycle += ivec + tpc - (uint)xpc;
    xpc = (int *)(ivec + tpc);
    goto fixpc;
  }
fatal:
  dprintf(2,"processor halted! cycle = %u pc = %08x ir = %08x sp = %08x a = %d b = %d c = %d trap = %u\n", cycle + (int)((uint)xpc - xcycle)/4, (uint)xpc - tpc, immediate, xsp - tsp, a, b, c, trap);
}

void usage()
{
  dprintf(2,"%s : usage: %s [-g] [-v] [-m memsize] [-f filesys] file\n", cmd, cmd);
  exit(-1);
}

int main(int argc, char *argv[])
{
  int i, f;
  struct { uint magic, bss, entry, flags; } hdr;
  char *file, *fs;
  struct stat st;

  cmd = *argv++;
  if (argc < 2) usage();
  file = *argv;
  memorySize = MEM_SZ;
  fs = 0;
  dbg = 0;
  verbose = 0;
  while (--argc && *file == '-') {
    switch(file[1]) {
    case 'g': dbg = 1; break;
    case 'v': verbose = 1; break;
    case 'm': memorySize = atoi(*++argv) * (1024 * 1024); argc--; break;
    case 'f': fs = *++argv; argc--; break;
    default: usage();
    }
    file = *++argv;
  }

  if (dbg) dprintf(2,"in debuger mode\n");
  if (verbose) dprintf(2,"mem size = %u\n",memorySize);
  memory = (((int) new(memorySize + 4096)) + 4095) & -4096;

  if (fs) {
    if (verbose) dprintf(2,"%s : loading ram file system %s\n", cmd, fs);
    if ((f = open(fs, O_RDONLY)) < 0) { dprintf(2,"%s : couldn't open file system %s\n", cmd, fs); return -1; }
    if (fstat(f, &st)) { dprintf(2,"%s : couldn't stat file system %s\n", cmd, fs); return -1; }
    if ((i = read(f, (void*)(memory + memorySize - FS_SZ), st.st_size)) != st.st_size) { dprintf(2,"%s : failed to read filesystem size %d returned %d\n", cmd, st.st_size, i); return -1; }
    close(f);
  }

  if ((f = open(file, O_RDONLY)) < 0) { dprintf(2,"%s : couldn't open %s\n", cmd, file); return -1; }
  if (fstat(f, &st)) { dprintf(2,"%s : couldn't stat file %s\n", cmd, file); return -1; }

  read(f, &hdr, sizeof(hdr));
  if (hdr.magic != 0xC0DEF00D) { dprintf(2,"%s : bad hdr.magic\n", cmd); return -1; }

  if (read(f, (void*)memory, st.st_size - sizeof(hdr)) != st.st_size - sizeof(hdr)) { dprintf(2,"%s : failed to read file %sn", cmd, file); return -1; }
  close(f);

//  if (verbose) dprintf(2,"entry = %u text = %u data = %u bss = %u\n", hdr.entry, hdr.text, hdr.data, hdr.bss);

  // setup virtual memory
  kernelReadPageTable = (uint *) new(TB_SZ * sizeof(uint)); // kernel read table
  kernelWritePageTable = (uint *) new(TB_SZ * sizeof(uint)); // kernel write table
  userReadPageTable = (uint *) new(TB_SZ * sizeof(uint)); // user read table
  userWritePageTable = (uint *) new(TB_SZ * sizeof(uint)); // user write table
  currentReadPageTable = kernelReadPageTable;
  currentWritePageTable = kernelWritePageTable;

  if (verbose) dprintf(2,"%s : emulating %s\n", cmd, file);
  cpu(hdr.entry, memorySize - FS_SZ);
  return 0;
}
