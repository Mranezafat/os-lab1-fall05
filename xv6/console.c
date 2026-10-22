// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}
//PAGEBREAK: 50

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
  int i, c, locking;
  uint *argp;
  char *s;

  locking = cons.locking;
  if(locking)
    acquire(&cons.lock);

  if (fmt == 0)
    panic("null fmt");

  argp = (uint*)(void*)(&fmt + 1);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(*argp++, 10, 1);
      break;
    case 'x':
    case 'p':
      printint(*argp++, 16, 0);
      break;
    case 's':
      if((s = (char*)*argp++) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&cons.lock);
}

void
panic(char *s)
{
  int i;
  uint pcs[10];

  cli();
  cons.locking = 0;
  // use lapiccpunum so that we can call panic from mycpu()
  cprintf("lapicid %d: panic: ", lapicid());
  cprintf(s);
  cprintf("\n");
  getcallerpcs(&s, pcs);
  for(i=0; i<10; i++)
    cprintf(" %p", pcs[i]);
  panicked = 1; // freeze other CPU
  for(;;)
    ;
}

//PAGEBREAK: 50
#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
  int pos;

  // Cursor position: col + 80*row.
  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);

  if(c == '\n')
    pos += 80 - pos%80;
  else if(c == BACKSPACE){
    if(pos > 0) --pos;
  } else
    crt[pos++] = (c&0xff) | 0x0700;  // black on white

  if(pos < 0 || pos > 25*80)
    panic("pos under/overflow");

  if((pos/80) >= 24){  // Scroll up.
    memmove(crt, crt+80, sizeof(crt[0])*23*80);
    pos -= 80;
    memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
  }

  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos>>8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
  crt[pos] = ' ' | 0x0700;
}

static void
move_cursor(int forward)
{
  int pos;

  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);

  if(forward){
    if(pos >= 25*80-1)
      return;
    pos++;
  } else {
    if(pos <= 0)
      return;
    pos--;
  }

  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos >> 8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
}

void
consputc(int c)
{
  if(panicked){
    cli();
    for(;;)
      ;
  }

  if(c == BACKSPACE){
    uartputc('\b'); uartputc(' '); uartputc('\b');
  } else
    uartputc(c);
  cgaputc(c);
}

#define INPUT_BUF 128
struct {
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
  uint cursor;
  uint selection_anchor;
  int selecting;
  uint line_start;
  uint rendered_len;
} input;

static int
get_cursor_pos(void)
{
  int pos;

  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);
  return pos;
}

static void
set_cursor_pos(int pos)
{
  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos >> 8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
}

static void
render_input(void)
{
  uint lo, hi, i;

  if(input.cursor < input.selection_anchor){
    lo = input.cursor;
    hi = input.selection_anchor;
  } else {
    lo = input.selection_anchor;
    hi = input.cursor;
  }

  for(i = 0; i < input.rendered_len; i++)
    if(input.line_start + i < 25 * 80)
      crt[input.line_start + i] = ' ' | 0x0700;

  for(i = input.w; i < input.e; i++){
    uchar attr = (input.selecting && i >= lo && i < hi) ? 0x70 : 0x07;
    if(input.line_start + (i - input.w) < 25 * 80)
      crt[input.line_start + (i - input.w)] =
        input.buf[i % INPUT_BUF] | (attr << 8);
  }
  input.rendered_len = input.e - input.w;
  set_cursor_pos(input.line_start + (input.cursor - input.w));
}

static char clipboard[INPUT_BUF];
static uint clipboard_len;

static int
selection_bounds(uint *lo, uint *hi)
{
  if(!input.selecting || input.cursor == input.selection_anchor)
    return 0;
  if(input.cursor < input.selection_anchor){
    *lo = input.cursor;
    *hi = input.selection_anchor;
  } else {
    *lo = input.selection_anchor;
    *hi = input.cursor;
  }
  return 1;
}

static void
copy_selection(void)
{
  uint lo, hi, i;

  if(!selection_bounds(&lo, &hi))
    return;
  clipboard_len = hi - lo;
  for(i = 0; i < clipboard_len; i++)
    clipboard[i] = input.buf[(lo + i) % INPUT_BUF];
}

static void
delete_selection(void)
{
  uint lo, hi, i, len;

  if(!selection_bounds(&lo, &hi))
    return;
  len = hi - lo;
  for(i = lo; i < input.e - len; i++)
    input.buf[i % INPUT_BUF] = input.buf[(i + len) % INPUT_BUF];
  input.e -= len;
  input.cursor = lo;
  input.selecting = 0;
  render_input();
}

static void
paste_clipboard(void)
{
  uint i;

  if(clipboard_len == 0)
    return;
  if(input.selecting)
    delete_selection();
  if(input.e - input.r + clipboard_len > INPUT_BUF)
    return;

  for(i = input.e; i > input.cursor; i--)
    input.buf[(i + clipboard_len - 1) % INPUT_BUF] =
      input.buf[(i - 1) % INPUT_BUF];
  for(i = 0; i < clipboard_len; i++)
    input.buf[(input.cursor + i) % INPUT_BUF] = clipboard[i];
  input.e += clipboard_len;
  input.cursor += clipboard_len;
  render_input();
}

#define C(x)  ((x)-'@')  // Control-x
#define KEY_LF 0xE4
#define KEY_RT 0xE5
#define KEY_DEL 0xE9
#define KEY_SHIFT_LF 0xEA
#define KEY_SHIFT_RT 0xEB

enum delete_mode {
  DELETE_BACKSPACE,
  DELETE_FORWARD
};

static void
delete_char(enum delete_mode mode)
{
  uint i;
  uint delete_index;
  if(input.selecting && input.cursor != input.selection_anchor){
    delete_selection();
    return;
  }
  if(mode == DELETE_BACKSPACE){
    if(input.cursor <= input.w)
      return;
    delete_index = input.cursor - 1;
    input.cursor--;
    move_cursor(0);
  } else {
    if(input.cursor >= input.e)
      return;
    delete_index = input.cursor;
  }
  for(i = delete_index; i < input.e - 1; i++)
    input.buf[i % INPUT_BUF] =
      input.buf[(i + 1) % INPUT_BUF];
  input.e--;
  render_input();
}

void
consoleintr(int (*getc)(void))
{
  int c, doprocdump = 0;

  acquire(&cons.lock);
  while((c = getc()) >= 0){
    switch(c){
    case C('P'):  // Process listing.
      // procdump() locks cons.lock indirectly; invoke later
      doprocdump = 1;
      break;
    case C('C'):
      copy_selection();
      break;
    case C('V'):
      paste_clipboard();
      break;
    case C('U'):  // Kill line.
      while(input.e != input.w &&
            input.buf[(input.e-1) % INPUT_BUF] != '\n'){
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    case C('H'): case '\x7f':  // Backspace
      delete_char(DELETE_BACKSPACE);
      break;
    case KEY_DEL:
      delete_char(DELETE_FORWARD);
      break;
    case KEY_LF:
      if(input.cursor > input.w){
        input.selecting = 0;
        input.cursor--;
        move_cursor(0);
        render_input();
      }
      break;

    case KEY_RT:
      if(input.cursor < input.e){
        input.selecting = 0;
        input.cursor++;
        move_cursor(1);
        render_input();
      }
      break;

    case KEY_SHIFT_LF: {
      if(!input.selecting){
        input.selection_anchor = input.cursor;
        input.selecting = 1;
      }
      if(input.cursor > input.w)
        input.cursor--;
      if(input.cursor == input.selection_anchor)
        input.selecting = 0;
      render_input();
      break;
    }

    case KEY_SHIFT_RT: {
      if(!input.selecting){
        input.selection_anchor = input.cursor;
        input.selecting = 1;
      }
      if(input.cursor < input.e)
        input.cursor++;
      if(input.cursor == input.selection_anchor)
        input.selecting = 0;
      render_input();
      break;
    }
    default:
      if(c != 0 && input.e-input.r < INPUT_BUF){
        uint i;

        if(input.selecting && input.cursor != input.selection_anchor)
          delete_selection();
        if(input.e == input.w && input.cursor == input.e){
          input.line_start = get_cursor_pos();
          input.rendered_len = 0;
        }
        c = (c == '\r') ? '\n' : c;

        if(c == '\n' || c == C('D')){
          input.buf[input.e++ % INPUT_BUF] = c;
          input.cursor = input.e;
          consputc(c);
        } else {
          for(i = input.e; i > input.cursor; i--)
            input.buf[i % INPUT_BUF] =
              input.buf[(i - 1) % INPUT_BUF];

          input.buf[input.cursor % INPUT_BUF] = c;
          input.e++;
          input.cursor++;

          render_input();
        }

        if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
          input.w = input.e;
          input.rendered_len = 0;
          wakeup(&input.r);
        }
      }
      break;
    }
  }
  release(&cons.lock);
  if(doprocdump) {
    procdump();  // now call procdump() wo. cons.lock held
  }
}

int
consoleread(struct inode *ip, char *dst, int n)
{
  uint target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&cons.lock);
  while(n > 0){
    while(input.r == input.w){
      if(myproc()->killed){
        release(&cons.lock);
        ilock(ip);
        return -1;
      }
      sleep(&input.r, &cons.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    if(c == C('D')){  // EOF
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        input.r--;
      }
      break;
    }
    *dst++ = c;
    --n;
    if(c == '\n')
      break;
  }
  release(&cons.lock);
  ilock(ip);

  return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for(i = 0; i < n; i++)
    consputc(buf[i] & 0xff);
  release(&cons.lock);
  ilock(ip);

  return n;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "console");

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  ioapicenable(IRQ_KBD, 0);
}

