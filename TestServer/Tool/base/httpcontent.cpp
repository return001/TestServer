﻿/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2011, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at http://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/
#include "httpcontent.h"
#include <stdlib.h>
#include "HttpDownload.h"
#include "../zlib/zlib.h"
#include <stdio.h>
#include <string.h>

using namespace Tool;

/* Comment this out if zlib is always going to be at least ver. 1.2.0.4
   (doing so will reduce code size slightly). */
#define OLD_ZLIB_SUPPORT 0

#define DSIZ 16384 /* buffer size for decompressed data */

#define GZIP_MAGIC_0 0x1f
#define GZIP_MAGIC_1 0x8b

/* gzip flag byte */
#define ASCII_FLAG   0x01 /* bit 0 set: file probably ascii text */
#define HEAD_CRC     0x02 /* bit 1 set: header CRC present */
#define EXTRA_FIELD  0x04 /* bit 2 set: extra field present */
#define ORIG_NAME    0x08 /* bit 3 set: original file name present */
#define COMMENT      0x10 /* bit 4 set: file comment present */
#define RESERVED     0xE0 /* bits 5..7: reserved */

static void*
zalloc_cb(void* opaque, unsigned int items, unsigned int size)
{
  (void) opaque;
  /* not a typo, keep it calloc() */
  return (void*) calloc(items, size);
}

static void
zfree_cb(void* opaque, void* ptr)
{
  (void) opaque;
  free(ptr);
}

static int
process_zlib_error(z_stream *z)
{
  if(z->msg)
    printf("Error while processing content unencoding: %s",z->msg);
  else
    printf ("Error while processing content unencoding: Unknown failure within decompression software.");

  return -1;
}

static int
exit_zlib(z_stream *z, int *zlib_init, int result)
{
  inflateEnd(z);
  *zlib_init = CHttpDownload::ZLIB_UNINIT;
  return result;
}

static int inflate_stream(CHttpDownload* k)
{
  int allow_restart = 1;
  z_stream *z = &k->z;          /* zlib state structure */
  uInt nread = z->avail_in;
  Bytef *orig_in = z->next_in;
  int status;                   /* zlib status */
  int result = 0;   /* Curl_client_write status */
  char *decomp;                 /* Put the decompressed data here. */

  /* Dynamically allocate a buffer for decompression because it's uncommonly
     large to hold on the stack */
  decomp = (char*)malloc(DSIZ);
  if(decomp == NULL) {
    return exit_zlib(z, &k->zlib_init, -1);
  }

  /* because the buffer size is fixed, iteratively decompress and transfer to
     the client via client_write. */
  for(;;) {
    /* (re)set buffer for decompressed output for every iteration */
    z->next_out = (Bytef *)decomp;
    z->avail_out = DSIZ;

    status = inflate(z, Z_SYNC_FLUSH);
    if(status == Z_OK || status == Z_STREAM_END) {
      allow_restart = 0;
      if((DSIZ - z->avail_out)) {

		int ret = k->writeZlibDeBuffer((unsigned char*)decomp,DSIZ - z->avail_out);
        if(ret == 0) {
          free(decomp);
          return exit_zlib(z, &k->zlib_init, result);
        }
      }

      /* Done? clean up, return */
      if(status == Z_STREAM_END) {
        free(decomp);
        if(inflateEnd(z) == Z_OK)
          return exit_zlib(z, &k->zlib_init, result);
        else
          return exit_zlib(z, &k->zlib_init, process_zlib_error(z));
      }

      /* Done with these bytes, exit */

      /* status is always Z_OK at this point! */
      if(z->avail_in == 0) {
        free(decomp);
        return result;
      }
    }
    else if(allow_restart && status == Z_DATA_ERROR) {
      /* some servers seem to not generate zlib headers, so this is an attempt
         to fix and continue anyway */

      (void) inflateEnd(z);     /* don't care about the return code */
      if(inflateInit2(z, -MAX_WBITS) != Z_OK) {
        free(decomp);
        return exit_zlib(z, &k->zlib_init, process_zlib_error(z));
      }
      z->next_in = orig_in;
      z->avail_in = nread;
      allow_restart = 0;
      continue;
    }
    else {                      /* Error; exit loop, handle below */
      free(decomp);
      return exit_zlib(z, &k->zlib_init, process_zlib_error(z));
    }
  }
  /* Will never get here */
}

int Http_unencode_deflate_write(Tool::CHttpDownload* k,unsigned char* httbBuf,unsigned int httpBufLen)
{
  z_stream *z = &k->z;          /* zlib state structure */

  /* Initialize zlib? */
  if(k->zlib_init == CHttpDownload::ZLIB_UNINIT) {
    memset(z, 0, sizeof(z_stream));
    z->zalloc = (alloc_func)zalloc_cb;
    z->zfree = (free_func)zfree_cb;

    if(inflateInit(z) != Z_OK)
      return process_zlib_error(z);
    k->zlib_init = CHttpDownload::ZLIB_INIT;
  }

  /* Set the compressed input when this function is called */
  z->next_in = httbBuf;
  z->avail_in = httpBufLen;

  /* Now uncompress the data */
  return inflate_stream(k);
}

#ifdef OLD_ZLIB_SUPPORT
/* Skip over the gzip header */
enum eGZIP_RET{
  GZIP_OK,
  GZIP_BAD,
  GZIP_UNDERFLOW
};
static eGZIP_RET check_gzip_header(unsigned char const *data, unsigned int len, unsigned int *headerlen)
{
  int method, flags;
  const unsigned int totallen = len;

  /* The shortest header is 10 bytes */
  if(len < 10)
    return GZIP_UNDERFLOW;

  if((data[0] != GZIP_MAGIC_0) || (data[1] != GZIP_MAGIC_1))
    return GZIP_BAD;

  method = data[2];
  flags = data[3];

  if(method != Z_DEFLATED || (flags & RESERVED) != 0) {
    /* Can't handle this compression method or unknown flag */
    return GZIP_BAD;
  }

  /* Skip over time, xflags, OS code and all previous bytes */
  len -= 10;
  data += 10;

  if(flags & EXTRA_FIELD) {
    unsigned int extra_len;

    if(len < 2)
      return GZIP_UNDERFLOW;

    extra_len = (data[1] << 8) | data[0];

    if(len < (extra_len+2))
      return GZIP_UNDERFLOW;

    len -= (extra_len + 2);
    data += (extra_len + 2);
  }

  if(flags & ORIG_NAME) {
    /* Skip over NUL-terminated file name */
    while(len && *data) {
      --len;
      ++data;
    }
    if(!len || *data)
      return GZIP_UNDERFLOW;

    /* Skip over the NUL */
    --len;
    ++data;
  }

  if(flags & COMMENT) {
    /* Skip over NUL-terminated comment */
    while(len && *data) {
      --len;
      ++data;
    }
    if(!len || *data)
      return GZIP_UNDERFLOW;

    /* Skip over the NUL */
    --len;
  }

  if(flags & HEAD_CRC) {
    if(len < 2)
      return GZIP_UNDERFLOW;

    len -= 2;
  }

  *headerlen = totallen - len;
  return GZIP_OK;
}
#endif

int Http_unencode_gzip_write(Tool::CHttpDownload* k,unsigned char* httbBuf,unsigned int httpBufLen)
{
  z_stream *z = &k->z;          /* zlib state structure */

  /* Initialize zlib? */
  if(k->zlib_init == CHttpDownload::ZLIB_UNINIT) {
    memset(z, 0, sizeof(z_stream));
    z->zalloc = (alloc_func)zalloc_cb;
    z->zfree = (free_func)zfree_cb;

    if(strcmp(zlibVersion(), "1.2.0.4") >= 0) {
      /* zlib ver. >= 1.2.0.4 supports transparent gzip decompressing */
      if(inflateInit2(z, MAX_WBITS+32) != Z_OK) {
        return process_zlib_error(z);
      }
      k->zlib_init = CHttpDownload::ZLIB_INIT_GZIP; /* Transparent gzip decompress state */
    }
    else {
      /* we must parse the gzip header ourselves */
      if(inflateInit2(z, -MAX_WBITS) != Z_OK) {
        return process_zlib_error( z);
      }
      k->zlib_init = CHttpDownload::ZLIB_INIT;   /* Initial call state */
    }
  }

  if(k->zlib_init == CHttpDownload::ZLIB_INIT_GZIP) {
    /* Let zlib handle the gzip decompression entirely */
    z->next_in = httbBuf;
    z->avail_in = httpBufLen;
    /* Now uncompress the data */
    return inflate_stream(k);
  }

#ifndef OLD_ZLIB_SUPPORT
  /* Support for old zlib versions is compiled away and we are running with
     an old version, so return an error. */
  return exit_zlib(z, &k->zlib_init, CURLE_FUNCTION_NOT_FOUND);

#else
  /* This next mess is to get around the potential case where there isn't
   * enough data passed in to skip over the gzip header.  If that happens, we
   * malloc a block and copy what we have then wait for the next call.  If
   * there still isn't enough (this is definitely a worst-case scenario), we
   * make the block bigger, copy the next part in and keep waiting.
   *
   * This is only required with zlib versions < 1.2.0.4 as newer versions
   * can handle the gzip header themselves.
   */

  switch (k->zlib_init) {
  /* Skip over gzip header? */
  case CHttpDownload::ZLIB_INIT:
  {
    /* Initial call state */
    unsigned int hlen;

    switch (check_gzip_header(httbBuf, httpBufLen, &hlen)) {
    case GZIP_OK:
      z->next_in = httbBuf + hlen;
      z->avail_in = (uInt)(httpBufLen) - hlen;
      k->zlib_init = CHttpDownload::ZLIB_GZIP_INFLATING; /* Inflating stream state */
      break;

    case GZIP_UNDERFLOW:
      /* We need more data so we can find the end of the gzip header.  It's
       * possible that the memory block we malloc here will never be freed if
       * the transfer abruptly aborts after this point.  Since it's unlikely
       * that circumstances will be right for this code path to be followed in
       * the first place, and it's even more unlikely for a transfer to fail
       * immediately afterwards, it should seldom be a problem.
       */
      z->avail_in = (uInt)httpBufLen;
      z->next_in = (unsigned char*)malloc(z->avail_in);
      if(z->next_in == NULL) {
        return exit_zlib(z, &k->zlib_init, -1);
      }
      memcpy(z->next_in, httbBuf, z->avail_in);
      k->zlib_init = CHttpDownload::ZLIB_GZIP_HEADER;   /* Need more gzip header data state */
      /* We don't have any data to inflate yet */
      return 0;

    case GZIP_BAD:
    default:
      return exit_zlib(z, &k->zlib_init, process_zlib_error(z));
    }

  }
  break;

  case CHttpDownload::ZLIB_GZIP_HEADER:
  {
    /* Need more gzip header data state */
    unsigned int hlen;
    unsigned char *oldblock = z->next_in;

    z->avail_in += (uInt)httpBufLen;
    z->next_in = (unsigned char*)realloc(z->next_in, z->avail_in);
    if(z->next_in == NULL) {
      free(oldblock);
      return exit_zlib(z, &k->zlib_init, -1);
    }
    /* Append the new block of data to the previous one */
    memcpy(z->next_in + z->avail_in - httpBufLen, httbBuf
		, httpBufLen);

    switch (check_gzip_header(z->next_in, z->avail_in, &hlen)) {
    case GZIP_OK:
      /* This is the zlib stream data */
      free(z->next_in);
      /* Don't point into the malloced block since we just freed it */
      z->next_in = (Bytef *)httbBuf + hlen + httpBufLen - z->avail_in;
      z->avail_in = (uInt)(z->avail_in - hlen);
      k->zlib_init = CHttpDownload::ZLIB_GZIP_INFLATING;   /* Inflating stream state */
      break;

    case GZIP_UNDERFLOW:
      /* We still don't have any data to inflate! */
      return 0;

    case GZIP_BAD:
    default:
      free(z->next_in);
      return exit_zlib(z, &k->zlib_init, process_zlib_error(z));
    }

  }
  break;

  case CHttpDownload::ZLIB_GZIP_INFLATING:
  default:
    /* Inflating stream state */
    z->next_in = (Bytef *)httbBuf;
    z->avail_in = (uInt)httpBufLen;
    break;
  }

  if(z->avail_in == 0) {
    /* We don't have any data to inflate; wait until next time */
    return 0;
  }

  /* We've parsed the header, now uncompress the data */
  return inflate_stream(k);
#endif
}

void Http_unencode_cleanup(Tool::CHttpDownload* k)
{
  z_stream *z = &k->z;
  if(k->zlib_init != CHttpDownload::ZLIB_UNINIT)
    exit_zlib(z, &k->zlib_init, 0);
}
