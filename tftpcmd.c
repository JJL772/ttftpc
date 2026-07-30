//-----------------------------------------------------------------------------------------
// Copyright (C) 2026, Jeremy Lorelli
//-----------------------------------------------------------------------------------------
// Purpose: TFTP test
//-----------------------------------------------------------------------------------------
// This file is part of 'ttftp'. It is subject to the license terms in the
// LICENSE file found in the top-level directory of this distribution.
// No part of 'ttftp', including this file, may be copied, modified, propagated,
// or otherwise distributed except according to the terms contained in the LICENSE file.
//
// SPDX-License-Identifier: BSD-3-Clause
//-----------------------------------------------------------------------------------------
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include "ttftp.h"

static void
show_help()
{
  printf("tftp -a ADDR [-h] [-d FILE] [-p FILE]\n");
}

int main(int argc, char** argv)
{
  int opt;
  int put = 0, dl = 0, verbose = 0;

  char file[PATH_MAX] = {};
  char addr[128] = {};
  char out[PATH_MAX] = {};
  while ((opt = getopt(argc, argv, "va:p:d:ho:")) != -1) {
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
    case 'v':
      verbose = 1;
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
  
  if (verbose)
    tc->log_level = TFTP_LOG_DEBUG;
  
  struct stat st;
  if (put && stat(file, &st) < 0) {
    perror("stat");
    return -1;
  }
  
  int fd = open(file, O_RDONLY);
  char* buf = malloc(st.st_size);
  read(fd, buf, st.st_size);
  close(fd);
  
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  ssize_t nbytes = -1;

  if (put) {
    int r;
    if ((r=tftpc_put(tc, file, buf, st.st_size)) < 0) {
      printf("Put failed: %s (%d)\n", strerror(-r), r);
      return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    nbytes = st.st_size;
  }
  
  if (dl) {
    void* data;
    size_t size;
    int r;
    if ((r = tftpc_get(tc, file, &data, &size)) < 0) {
      printf("Get failed: %s (%d)\n", strerror(-r), r);
      return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    nbytes = size;

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
    
    float elapsed = (end.tv_sec + end.tv_nsec / 1e9) - (start.tv_sec + start.tv_nsec / 1e9);
    printf("Elapsed: %.2f seconds (%.2f MB/s)\n", elapsed, (nbytes / elapsed) / 1e6);
    return 0;
  }
}