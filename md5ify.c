/* Copyright 1993-95 by Carl Harris, Jr. Copyright 1996 by Eric S. Raymond
 * All rights reserved.
 * For license terms, see the file COPYING in this directory.
 */

/***********************************************************************
  module:       md5ify.c
  project:      popclient
  programmer:   Carl Harris, ceharris@mal.com
  description:  Simple interface to MD5 module.

 ***********************************************************************/

#include <stdio.h>

#if defined(STDC_HEADERS)
#include <string.h>
#endif

#include "md5.h"

char *
MD5Digest (s)
char *s;
{
  int i;
  MD5_CTX context;
  unsigned char digest[16];
  static char ascii_digest [33];

  MD5Init(&context);
  MD5Update(&context, s, strlen(s));
  MD5Final(digest, &context);
  
  for (i = 0;  i < 16;  i++) 
    sprintf(ascii_digest+2*i, "%02x", digest[i]);
 
  return(ascii_digest);
}
