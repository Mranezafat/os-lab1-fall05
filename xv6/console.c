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
#define SELECT_NONE 0
#define SELECT_MARKING 1
#define SELECT_DONE 2

struct {
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
  uint cursor;
  uint display_cursor;
  uint rendered_len;
  uint line_start;
  uint selection_anchor;
  int selection_state;
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
  uint attr, cursor_offset, hi, i, len, lo, total;

  len = input.e - input.w;
  cursor_offset = input.cursor - input.w;

  if(len == 0 && input.rendered_len == 0)
    input.line_start = get_cursor_pos();

  for(i = 0; i < input.display_cursor; i++)
    uartputc('\b');

  for(i = input.w; i < input.e; i++)
    uartputc(input.buf[i % INPUT_BUF]);

  total = len;
  if(input.rendered_len > len){
    total = input.rendered_len;
    for(i = len; i < input.rendered_len; i++)
      uartputc(' ');
  }

  for(i = cursor_offset; i < total; i++)
    uartputc('\b');

  while(input.line_start + total >= 24*80){
    memmove(crt, crt+80, sizeof(crt[0])*23*80);
    memset(crt+23*80, 0, sizeof(crt[0])*80);
    if(input.line_start >= 80)
      input.line_start -= 80;
    else
      input.line_start = 0;
  }

  for(i = 0; i < input.rendered_len; i++)
    crt[input.line_start + i] = ' ' | 0x0700;

  lo = hi = 0;
  if(input.selection_state == SELECT_DONE){
    if(input.cursor < input.selection_anchor){
      lo = input.cursor;
      hi = input.selection_anchor;
    } else {
      lo = input.selection_anchor;
      hi = input.cursor;
    }
  }

  for(i = input.w; i < input.e; i++){
    attr = 0x0700;
    if(input.selection_state == SELECT_DONE && i >= lo && i < hi)
      attr = 0x7000;
    crt[input.line_start + i - input.w] =
      (input.buf[i % INPUT_BUF] & 0xff) | attr;
  }

  set_cursor_pos(input.line_start + cursor_offset);

  input.display_cursor = cursor_offset;
  input.rendered_len = len;
}

static char clipboard[INPUT_BUF];
static uint clipboard_len;

static int
selection_bounds(uint *lo, uint *hi)
{
  if(input.selection_state != SELECT_DONE ||
     input.cursor == input.selection_anchor)
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

static int
delete_selection(void)
{
  uint lo, hi, i, len;

  if(!selection_bounds(&lo, &hi))
    return 0;
  len = hi - lo;
  for(i = lo; i < input.e - len; i++)
    input.buf[i % INPUT_BUF] = input.buf[(i + len) % INPUT_BUF];
  input.e -= len;
  input.cursor = lo;
  input.selection_state = SELECT_NONE;
  return 1;
}

static void
insert_characters(char *text, uint len)
{
  uint i;

  for(i = input.e; i > input.cursor; i--)
    input.buf[(i + len - 1) % INPUT_BUF] =
      input.buf[(i - 1) % INPUT_BUF];

  for(i = 0; i < len; i++)
    input.buf[(input.cursor + i) % INPUT_BUF] = text[i];

  input.e += len;
  input.cursor += len;
}

static void
paste_clipboard(void)
{
  uint hi, lo, selected_len;

  if(clipboard_len == 0)
    return;

  selected_len = 0;
  if(selection_bounds(&lo, &hi))
    selected_len = hi - lo;

  if(input.e - input.r - selected_len + clipboard_len > INPUT_BUF)
    return;

  delete_selection();
  input.selection_state = SELECT_NONE;
  insert_characters(clipboard, clipboard_len);
  render_input();
}

#define C(x)  ((x)-'@')  // Control-x
#define KEY_LF 0xE4
#define KEY_RT 0xE5
#define KEY_DEL 0xE9

enum delete_mode {
  DELETE_BACKSPACE,
  DELETE_FORWARD
};

static void
delete_char(enum delete_mode mode)
{
  uint i;
  uint delete_index;

  if(input.selection_state == SELECT_MARKING)
    return;

  if(delete_selection()){
    render_input();
    return;
  }

  input.selection_state = SELECT_NONE;
  if(mode == DELETE_BACKSPACE){
    if(input.cursor <= input.w)
      return;
    delete_index = input.cursor - 1;
    input.cursor--;
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

static void
move_input_cursor(int direction)
{
  if(input.selection_state == SELECT_DONE)
    input.selection_state = SELECT_NONE;

  if(direction < 0){
    if(input.cursor > input.w)
      input.cursor--;
  } else {
    if(input.cursor < input.e)
      input.cursor++;
  }
  render_input();
}

static void
move_to_line_end(int to_end)
{
  if(input.selection_state == SELECT_DONE)
    input.selection_state = SELECT_NONE;

  if(to_end)
    input.cursor = input.e;
  else
    input.cursor = input.w;
  render_input();
}

static void
toggle_selection(void)
{
  if(input.selection_state == SELECT_MARKING){
    if(input.cursor == input.selection_anchor)
      input.selection_state = SELECT_NONE;
    else
      input.selection_state = SELECT_DONE;
  } else {
    input.selection_anchor = input.cursor;
    input.selection_state = SELECT_MARKING;
  }
  render_input();
}

static void
finish_input(int c)
{
  if(input.selection_state == SELECT_DONE)
    delete_selection();
  input.selection_state = SELECT_NONE;

  input.cursor = input.e;
  render_input();

  if(input.e - input.r >= INPUT_BUF)
    return;

  input.buf[input.e++ % INPUT_BUF] = c;
  input.cursor = input.e;
  consputc(c);
  input.w = input.e;
  input.display_cursor = 0;
  input.rendered_len = 0;
  wakeup(&input.r);
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
      if(input.selection_state != SELECT_MARKING)
        paste_clipboard();
      break;
    case C('I'):
      toggle_selection();
      break;
    case C('Q'):
      move_to_line_end(0);
      break;
    case C('E'):
      move_to_line_end(1);
      break;
    case C('U'):  // Kill line.
      input.e = input.w;
      input.cursor = input.w;
      input.selection_state = SELECT_NONE;
      render_input();
      break;
    case C('H'): case '\x7f':  // Backspace
      delete_char(DELETE_BACKSPACE);
      break;
    case KEY_DEL:
      delete_char(DELETE_FORWARD);
      break;
    case KEY_LF:
      move_input_cursor(-1);
      break;
    case KEY_RT:
      move_input_cursor(1);
      break;
    default:
      if(c == 0)
        break;

      c = (c == '\r') ? '\n' : c;
      if(c == '\n' || c == C('D')){
        finish_input(c);
        break;
      }

      if(input.selection_state == SELECT_MARKING)
        break;

      {
        uint hi, lo, selected_len;
        char character;

        selected_len = 0;
        if(selection_bounds(&lo, &hi))
          selected_len = hi - lo;
        if(input.e - input.r - selected_len >= INPUT_BUF)
          break;

        delete_selection();
        input.selection_state = SELECT_NONE;
        character = c;
        insert_characters(&character, 1);
        render_input();

        if(input.e == input.r + INPUT_BUF){
          input.w = input.e;
          input.display_cursor = 0;
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

