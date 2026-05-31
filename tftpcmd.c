#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>

#include "ttftp.h"

static void
show_help()
{
  printf("tftp -a ADDR [-h] [-d FILE] [-p FILE]\n");
}

int main(int argc, char** argv)
{
  int opt;
  int put = 0, dl = 0;

  char file[PATH_MAX] = {};
  char addr[128] = {};
  char out[PATH_MAX] = {};
  while ((opt = getopt(argc, argv, "a:p:d:ho:")) != -1) {
    switch (opt) {
    case 'a':
      strcpy(addr, optarg);
      break;
    case 'p':
      put = 1;
      strcpy(file, optarg);
      break;
    case 'o':
      strcpy(out, optarg);
      break;
    case 'd':
      dl = 1;
      strcpy(file, optarg);
      break;
    case 'h':
    default:
      show_help();
      return -1;
    }
  }
  
  if (!*addr) {
    show_help();
    return -1;
  }
  
  if (!put && !dl) {
    printf("Must specify one operation\n");
    show_help();
    return -1;
  }
  
  if (!*file) {
    printf("No file\n");
    show_help();
    return -1;
  }
  
  struct tftpc* tc = tftpc_open(addr);
  if (!tc) {
    printf("Could not connect!!!\n");
    return -1;
  }
  
  struct stat st;
  if (put && stat(file, &st) < 0) {
    perror("stat");
    return -1;
  }
  
  int fd = open(file, O_RDONLY);
  char* buf = malloc(st.st_size);
  read(fd, buf, st.st_size);
  close(fd);
  
  if (put) {
    int r;
    if ((r=tftpc_put(tc, file, buf, st.st_size)) < 0) {
      printf("Put failed: %s (%d)\n", strerror(-r), r);
      return -1;
    }
  }
  
  if (dl) {
    void* data;
    size_t size;
    int r;
    if ((r = tftpc_get(tc, file, &data, &size)) < 0) {
      printf("Get failed: %s (%d)\n", strerror(-r), r);
      return -1;
    }
    
    FILE* fp = stdout;
    if (*out) {
      fp = fopen(out, "wb");
    }
    
    if (!fp) {
      printf("Could not open %s for writing\n", out);
      return -1;
    }
    
    fwrite(data, size, 1, fp);
    if (fp != stdout)
      fclose(fp);
    return 0;
  }
}