//-----------------------------------------------------------------------------------------
// Copyright (C) 2026, Jeremy Lorelli
//-----------------------------------------------------------------------------------------
// Purpose: Header-only tiny TFTP client
//-----------------------------------------------------------------------------------------
// This file is part of 'ttftp'. It is subject to the license terms in the
// LICENSE file found in the top-level directory of this distribution.
// No part of 'ttftp', including this file, may be copied, modified, propagated,
// or otherwise distributed except according to the terms contained in the LICENSE file.
//
// SPDX-License-Identifier: BSD-3-Clause
//-----------------------------------------------------------------------------------------
#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tftpc
{
  int port;
  struct in_addr addr;
  int fd;
  int retries;
  
  struct sockaddr_in fromaddr;
};

enum tftp_opcode
{
  TFTP_RRQ   = 0x1,
  TFTP_WRQ   = 0x2,
  TFTP_DATA  = 0x3,
  TFTP_ACK   = 0x4,
  TFTP_ERROR = 0x5,
};

#define TFTPC__SOCKADDR(_c, _sa) \
  (_sa).sin_addr = (_c)->addr; \
  (_sa).sin_family = AF_INET; \
  (_sa).sin_port = htons((_c)->port);

/**
 * @brief Opens a new TFTP connection to a remote server. Thus must be passed to tftpc_close when you're done.
 * Part of the @b PUBLIC API.
 * @param addr IP address of the server, possibly including port name.
 *   The IP address must be resolved before calling this.
 * @returns New TFTP client context, or NULL on failure.
 */
static struct tftpc*
tftpc_open(const char* addr)
{
  int port = 0, fd = -1;

  char a[128] = {0};
  strncpy(a, addr, sizeof(a)-1);

  /* split out port decl */
  char* p = strpbrk(a, ":");
  if (p) {
    *p = 0, ++p;
    port = atoi(p);
  }
  
  if (port <= 0)
    port = 69;
  
  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    perror("socket");
    return NULL;
  }
  
  struct tftpc* tc = (struct tftpc*)calloc(1, sizeof(struct tftpc));
  tc->fd = fd;
  tc->addr.s_addr = inet_addr(addr);
  tc->port = port;
  tc->retries = 10;
  return tc;
}

/**
 * @brief Closes a TFTP connection to a server, and frees it.
 * Part of the @b PUBLIC API.
 * @param c Context to be closed and freed. May be null.
 */
static void
tftpc_close(struct tftpc* c)
{
  if (!c)
    return;
  close(c->fd);
}

static inline int
tftpc__putstr(uint8_t** p, uint8_t* end, const char* str)
{
  const size_t sl = strlen(str);
  if ((*p) + sl >= end)
    return -1;
  strcpy((char*)*p, str);
  *p += sl + 1;
  return 0;
}

static inline int
tftpc__puts(uint8_t** p, uint8_t* end, uint16_t n)
{
  if ((*p) + 2 > end)
    return -1;
  *(uint16_t*)(*p) = htons(n);
  (*p) += 2;
  return 0;
}

static inline int
tftpc__putbuf(uint8_t** p, uint8_t* end, const void* buf, size_t l)
{
  if ((*p) + l > end)
    return -1;
  memcpy(*p, buf, l);
  (*p) += l;
  return 0;
}

/**
 * @brief Gets a short from the buffer, in host byte order
 * @param off Offset in bytes
 */
static inline uint16_t
tftpc__gets(const void* packet, int off)
{
  return ntohs(*(uint16_t*)((uint8_t*)(packet) + off));
}

static inline int
tftpc__is_loopback(struct in_addr a)
{
  return a.s_addr == 0 || a.s_addr == 0x7F000001;
}

/**
 * @brief Checks if the addresses match between the two.
 */
static inline int
tftpc__match_addr(const struct sockaddr_in a, const struct sockaddr_in b)
{
  /* sometimes 0.0.0.0 comes back as 127.0.0.1 */
  if (tftpc__is_loopback(a.sin_addr) && tftpc__is_loopback(b.sin_addr))
    return 1;
  return a.sin_addr.s_addr == b.sin_addr.s_addr;
}

/**
 * @brief SEND data packet to the server, waits for an ACK and resends until it gets one.
 */
static int
tftpc__do_data(struct tftpc* c, uint16_t block, const void* data, size_t ds)
{
  ssize_t nr;
  int retries = 10;

  /* max payload will always be 512 bytes */
  if (ds > 512)
    return -ERANGE;
  
  uint8_t packet[512 + 4]; /* 512=max payload, plus 4 bytes header */
  uint8_t* p = packet, *end = packet + sizeof(packet);

  /*  2 bytes     2 bytes      n bytes
   * ----------------------------------
   *| Opcode |   Block #  |   Data     |
   * ----------------------------------*/
  tftpc__puts(&p, end, TFTP_DATA);
  tftpc__puts(&p, end, block);
  tftpc__putbuf(&p, end, data, ds);

sendagain:
  struct sockaddr_in sa = c->fromaddr;

  nr = sendto(c->fd, packet, p - packet, 0, (struct sockaddr*)&sa, sizeof(sa));
  if (nr < 0) {
    return -errno;
  }

recvagain:

  /* we expect an ACK now */
  socklen_t sl = sizeof(struct sockaddr_in);
  nr = recvfrom(c->fd, packet, sizeof(packet), 0, (struct sockaddr*)&sa, &sl);
  if (nr < 0) {
    if (errno == ETIMEDOUT && --retries >= 0)
      goto sendagain;
    return -errno;
  }

  /* reject bogus hosts */
  if (tftpc__match_addr(sa, c->fromaddr))
    goto recvagain;
  c->fromaddr.sin_port = sa.sin_port;

  /*  2 bytes     2 bytes
   *  ---------------------
   * | Opcode |   Block #  |
   *  ---------------------
   */

  /* check opcode */
  if (TFTP_ACK != tftpc__gets(packet, 0))
    goto recvagain; /* this does not count as a retry */
  
  /* check block */
  if (block != tftpc__gets(packet, 2)) {
    /* this DOES count as a retry */
    usleep(1000);
    if (--retries <= 0)
      return -ETIMEDOUT;
    goto sendagain;
  }

  return 0;
}

static int
tftpc__xrq(struct tftpc* c, const char* file, uint16_t type)
{
  ssize_t nr;
  uint16_t block = 1, op;
  uint8_t packet[512];
  uint8_t* p = packet, *end = packet + sizeof(packet);

  /*   2 bytes     string    1 byte     string   1 byte
   *  ------------------------------------------------
   * | Opcode |  Filename  |   0  |    Mode    |   0  |
   *  ------------------------------------------------
   */
  tftpc__puts(&p, end, type);
  if (tftpc__putstr(&p, end, file) < 0)
    return -1;
  if (tftpc__putstr(&p, end, "octet") < 0)
    return -1;

  /* fill out fromaddr at start of transaction */
  TFTPC__SOCKADDR(c, c->fromaddr);

  nr = sendto(c->fd, packet, p - packet, 0, (struct sockaddr*)&c->fromaddr, sizeof(c->fromaddr));
  if (nr < 0)
    return -errno;
  return 0;
}

static int
tftpc__translate_err(int tftp_err)
{
  switch (tftp_err) {
  case 0:
  default:
    return EIO;
  case 1:
    return ENOENT;
  case 2:
    return EACCES;
  case 3:
    return ENOSPC;
  case 4:
    return ENOTSUP;
  case 5:
    return EPROTO;
  case 6:
    return EEXIST;
  case 7:
    return EINVAL;
  }
}

static int
tftpc__geterr(const void* packet)
{
  return tftpc__translate_err(tftpc__gets(packet, 2));
}

/**
 * @brief Perform a TFTP PUT to a remote file
 * Part of the @b PUBLIC API.
 * @param c Client context
 * @param file Name of the file on the remote server to PUT to
 * @param data Buffer containing the data you wish to send
 * @param size Size of the data in buffer
 * @returns 0 on success, negative error code on failure
 */
static int
tftpc_put(struct tftpc* c, const char* file, const void* data, size_t size)
{
  ssize_t nr;
  uint16_t block = 1, op;
  uint8_t packet[512];
  uint8_t* p = packet, *end = packet + sizeof(packet);

  /* send initial write request */
  nr = tftpc__xrq(c, file, TFTP_WRQ);
  if (nr < 0)
    return nr;

recvagain:
  struct sockaddr_in sa = c->fromaddr;
  socklen_t sl = sizeof(c->fromaddr);

  nr = recvfrom(c->fd, packet, sizeof(packet), 0, (struct sockaddr*)&sa, &sl);
  if (nr < 0)
    return -errno;
    
  /* reject bogus hosts */
  if (!tftpc__match_addr(sa, c->fromaddr))
    goto recvagain;
  c->fromaddr.sin_port = sa.sin_port;

  /* Expect either error or ACK */
  op = tftpc__gets(packet, 0);
  if (op == TFTP_ERROR)
    return -tftpc__geterr(packet);
  else if(op != TFTP_ACK)
    return -EPROTO;

  const uint8_t* ptr = (const uint8_t*)data;
  while (size > 0) {
    nr = tftpc__do_data(c, block, ptr, size > 512 ? 512 : size);
    if (nr < 0)
      return nr;
    size = 512 > size ? 0 : size - 512;
    ptr += 512;
  }
  return 0;
}

/**
 * @brief Recv data, write data into buffer
 * @returns Returns number of bytes written to buf, or negative error code.
 */
static int
tftpc__recv_data(struct tftpc* c, uint16_t block, void* buffer)
{
  ssize_t nr;
  uint8_t packet[512 + 4];
  uint16_t op;

recvagain:
  struct sockaddr_in sa = c->fromaddr;
  socklen_t sl = sizeof(c->fromaddr);

  nr = recvfrom(c->fd, packet, sizeof(packet), 0, (struct sockaddr*)&sa, &sl);
  if (nr < 0)
    return -errno;

  /* reject bogus hosts */
  if (!tftpc__match_addr(sa, c->fromaddr))
    goto recvagain;

  c->fromaddr.sin_port = sa.sin_port;

  /*  2 bytes    2 bytes       n bytes
   *  ---------------------------------
   * | 03    |   Block #  |    Data    |
   *  ---------------------------------
   */

  /* expect either error or data */
  op = tftpc__gets(packet, 0);
  if (op == TFTP_ERROR)
    return -tftpc__geterr(packet);
  else if (op != TFTP_DATA)
    return -EPROTO;
  
  /* if we get data for a block we did not expect,
   * tell caller to send the previous ACK again */
  if (tftpc__gets(packet, 2) != block)
    return -EAGAIN;

  /* copy into out buffer */
  memcpy(buffer, packet + 4, nr - 4);

  return nr - 4;
}

/**
 * @brief ACK a block of data
 */
static int
tftpc__send_ack(struct tftpc* c, uint16_t block)
{
  uint8_t packet[4];
  uint8_t *p = packet, *end = packet + sizeof(packet);

  /*  2 bytes    2 bytes
   *  -------------------
   * | 04    |   Block #  |
   *  --------------------
   */
  tftpc__puts(&p, end, TFTP_ACK);
  tftpc__puts(&p, end, block);
  
  struct sockaddr_in sa = c->fromaddr;

  ssize_t nr = sendto(c->fd, packet, sizeof(packet), 0, (struct sockaddr*)&sa, sizeof(sa));
  if (nr < 0)
    return -errno;
  return 0;
}

/**
 * @brief Performs a TFTP get operation into memory.
 * Part of the @b PUBLIC API.
 * @param c Context
 * @param file File
 * @param data Pointer to a void* to hold data. Must not be NULL.
 * @param size Pointer to a variable to hold the resulting data size. Must not be NULL.
 */
static int
tftpc_get(struct tftpc* c, const char* file, void** data, size_t* size)
{
  ssize_t nr, off = 0, err = 0;
  uint16_t block = 1, op;
  uint8_t packet[512];
  uint8_t* p = packet, *end = packet + sizeof(packet);

  assert(data);
  assert(size);

  /* send read-request */
  if ((nr = tftpc__xrq(c, file, TFTP_RRQ)) < 0)
    return nr;

  /* preallocate space for 64 blocks (32K) */
  *data = malloc(512 * 64);
  *size = 512 * 64;

  uint8_t* dp = (uint8_t*)*data;
  do {
recv_data:
    nr = tftpc__recv_data(c, block, dp);

    /* if -EAGAIN, send ack for previous block, again */
    if (nr == -EAGAIN) {
      printf("ACKing block %d again\n", block-1);
      if ((err = tftpc__send_ack(c, block-1)) < 0) {
        goto fail;
      }
      goto recv_data;
    } else if (nr < 0) {
      err = nr;
      goto fail;
    }

    /* adjust offset */
    dp += nr;

    /* resize buffer if needed */
    if (dp - (uint8_t*)*data >= *size - 512) {
      ssize_t oldpos = dp - (uint8_t*)(*data);
      *size += 64 * 512;
      *data = realloc(*data, *size);
      dp = (uint8_t*)*data + oldpos;
    }

    /* ack the block */
    if ((err = tftpc__send_ack(c, block)) < 0) {
      goto fail;
    }
    block++;
  } while(nr == 512);

  /* adjust size so it matches what we actually read */
  *size = dp - (uint8_t*)*data;

  return 0;
fail:
  free(*data);
  *size = 0;
  return err;
}

#ifdef __cplusplus
}
#endif