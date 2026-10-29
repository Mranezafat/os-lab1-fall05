#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define BUFFER_SIZE 512
#define MAX_RUN_COUNT 1000000

struct output_buffer {
  int fd;
  char data[BUFFER_SIZE];
  int used;
};

struct input_buffer {
  int fd;
  char data[BUFFER_SIZE];
  int position;
  int size;
};

static int
write_data(int fd, char *buf, int n)
{
  int written;
  int total;

  total = 0;
  while(total < n){
    written = write(fd, buf + total, n - total);
    if(written <= 0)
      return -1;
    total += written;
  }
  return 0;
}

static int
read_character(struct input_buffer *input, char *character)
{
  if(input->position == input->size){
    input->size = read(input->fd, input->data, BUFFER_SIZE);
    input->position = 0;
    if(input->size <= 0)
      return input->size;
  }

  *character = input->data[input->position];
  input->position++;
  return 1;
}

static int
flush_output(struct output_buffer *output)
{
  if(output->used == 0)
    return 0;
  if(write_data(output->fd, output->data, output->used) < 0)
    return -1;
  output->used = 0;
  return 0;
}

static int
write_character(struct output_buffer *output, char character)
{
  if(output->used == BUFFER_SIZE && flush_output(output) < 0)
    return -1;
  output->data[output->used] = character;
  output->used++;
  return 0;
}

static int
write_number(struct output_buffer *output, int number)
{
  char digits[16];
  int count;
  int i;

  count = 0;
  do {
    digits[count] = '0' + number % 10;
    count++;
    number /= 10;
  } while(number > 0);

  for(i = count - 1; i >= 0; i--)
    if(write_character(output, digits[i]) < 0)
      return -1;

  return 0;
}

static int
write_run(struct output_buffer *output, char character, int count)
{
  if(write_character(output, character) < 0)
    return -1;
  if(write_character(output, ',') < 0)
    return -1;
  if(write_number(output, count) < 0)
    return -1;
  if(write_character(output, ';') < 0)
    return -1;
  return 0;
}

static int
encode(int input_fd, int output_fd)
{
  char buf[BUFFER_SIZE];
  char current;
  struct output_buffer output;
  int count;
  int have_current;
  int i;
  int n;

  count = 0;
  have_current = 0;
  output.fd = output_fd;
  output.used = 0;

  while((n = read(input_fd, buf, sizeof(buf))) > 0){
    for(i = 0; i < n; i++){
      if(!have_current){
        current = buf[i];
        count = 1;
        have_current = 1;
      } else if(buf[i] == current){
        count++;
      } else {
        if(write_run(&output, current, count) < 0)
          return -1;
        current = buf[i];
        count = 1;
      }
    }
  }

  if(n < 0)
    return -1;

  if(have_current && write_run(&output, current, count) < 0)
    return -1;

  return flush_output(&output);
}

static int
write_repeated(struct output_buffer *output, char character, int count)
{
  while(count > 0){
    if(write_character(output, character) < 0)
      return -1;
    count--;
  }

  return 0;
}

static int
decode(int input_fd, int output_fd)
{
  char character;
  char current;
  struct input_buffer input;
  struct output_buffer output;
  int count;
  int digits;
  int n;

  input.fd = input_fd;
  input.position = 0;
  input.size = 0;
  output.fd = output_fd;
  output.used = 0;

  while((n = read_character(&input, &character)) == 1){
    if(read_character(&input, &current) != 1 || current != ',')
      return -1;

    count = 0;
    digits = 0;

    while((n = read_character(&input, &current)) == 1 && current != ';'){
      if(current < '0' || current > '9')
        return -1;
      if(count > MAX_RUN_COUNT / 10)
        return -1;
      count = count * 10 + current - '0';
      if(count > MAX_RUN_COUNT)
        return -1;
      digits++;
    }

    if(n != 1 || digits == 0 || count <= 0)
      return -1;

    if(write_repeated(&output, character, count) < 0)
      return -1;
  }

  if(n < 0)
    return -1;

  return flush_output(&output);
}

int
main(int argc, char *argv[])
{
  char *output_name;
  int input_fd;
  int output_fd;
  int result;

  if(argc != 3){
    printf(2, "usage: rle encode|decode input-file\n");
    exit();
  }

  if(strcmp(argv[1], "encode") == 0)
    output_name = "encoded.txt";
  else if(strcmp(argv[1], "decode") == 0)
    output_name = "decoded.txt";
  else {
    printf(2, "rle: mode must be encode or decode\n");
    exit();
  }

  input_fd = open(argv[2], O_RDONLY);
  if(input_fd < 0){
    printf(2, "rle: cannot open %s\n", argv[2]);
    exit();
  }

  unlink(output_name);
  output_fd = open(output_name, O_CREATE | O_WRONLY);
  if(output_fd < 0){
    printf(2, "rle: cannot create %s\n", output_name);
    close(input_fd);
    exit();
  }

  if(strcmp(argv[1], "encode") == 0)
    result = encode(input_fd, output_fd);
  else
    result = decode(input_fd, output_fd);

  close(input_fd);
  close(output_fd);

  if(result < 0){
    printf(2, "rle: invalid input or file error\n");
    unlink(output_name);
    exit();
  }

  exit();
}
