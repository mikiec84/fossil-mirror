/*
** Copyright (c) 2007 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)

** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This file contains code that implements the client-side HTTP protocol
*/
#include "config.h"
#include "http.h"
#include <assert.h>

#ifdef _WIN32
#include <io.h>
#ifndef isatty
#define isatty(d) _isatty(d)
#endif
#ifndef fileno
#define fileno(s) _fileno(s)
#endif
#endif


#if INTERFACE
/*
** Bits of the mHttpFlags parameter to http_exchange()
*/
#define HTTP_USE_LOGIN   0x00001     /* Add a login card to the sync message */
#define HTTP_GENERIC     0x00002     /* Generic HTTP request */
#define HTTP_VERBOSE     0x00004     /* HTTP status messages */
#define HTTP_QUIET       0x00008     /* No surplus output */
#define HTTP_NOCOMPRESS  0x00010     /* Omit payload compression */
#endif

/* Maximum number of HTTP Authorization attempts */
#define MAX_HTTP_AUTH 2

/* Keep track of HTTP Basic Authorization failures */
static int fSeenHttpAuth = 0;

/*
** Construct the "login" card with the client credentials.
**
**       login LOGIN NONCE SIGNATURE
**
** The LOGIN is the user id of the client.  NONCE is the sha1 checksum
** of all payload that follows the login card.  SIGNATURE is the sha1
** checksum of the nonce followed by the user password.
**
** Write the constructed login card into pLogin.  pLogin is initialized
** by this routine.
*/
static void http_build_login_card(Blob *pPayload, Blob *pLogin){
  Blob nonce;          /* The nonce */
  const char *zLogin;  /* The user login name */
  const char *zPw;     /* The user password */
  Blob pw;             /* The nonce with user password appended */
  Blob sig;            /* The signature field */

  blob_zero(pLogin);
  if( g.url.user==0 || fossil_strcmp(g.url.user, "anonymous")==0 ){
     return;  /* If no login card for users "nobody" and "anonymous" */
  }
  if( g.url.isSsh ){
     return;  /* If no login card for SSH: */
  }
  blob_zero(&nonce);
  blob_zero(&pw);
  sha1sum_blob(pPayload, &nonce);
  blob_copy(&pw, &nonce);
  zLogin = g.url.user;
  if( g.url.passwd ){
    zPw = g.url.passwd;
  }else if( g.cgiOutput ){
    /* Password failure while doing a sync from the web interface */
    cgi_printf("*** incorrect or missing password for user %h\n", zLogin);
    zPw = 0;
  }else{
    /* Password failure while doing a sync from the command-line interface */
    url_prompt_for_password();
    zPw = g.url.passwd;
  }

  /* The login card wants the SHA1 hash of the password, so convert the
  ** password to its SHA1 hash if it isn't already a SHA1 hash.
  */
  /* fossil_print("\nzPw=[%s]\n", zPw); // TESTING ONLY */
  if( zPw && zPw[0] ) zPw = sha1_shared_secret(zPw, zLogin, 0);

  blob_append(&pw, zPw, -1);
  sha1sum_blob(&pw, &sig);
  blob_appendf(pLogin, "login %F %b %b\n", zLogin, &nonce, &sig);
  blob_reset(&pw);
  blob_reset(&sig);
  blob_reset(&nonce);
}

/*
** Construct an appropriate HTTP request header.  Write the header
** into pHdr.  This routine initializes the pHdr blob.  pPayload is
** the complete payload (including the login card) already compressed.
*/
static void http_build_header(
  Blob *pPayload,              /* the payload that will be sent */
  Blob *pHdr,                  /* construct the header here */
  const char *zAltMimetype     /* Alternative mimetype */
){
  int nPayload = pPayload ? blob_size(pPayload) : 0;

  blob_zero(pHdr);
  blob_appendf(pHdr, "%s %s%s HTTP/1.0\r\n",
               nPayload>0 ? "POST" : "GET", g.url.path,
               g.url.path[0]==0 ? "/" : "");
  if( g.url.proxyAuth ){
    blob_appendf(pHdr, "Proxy-Authorization: %s\r\n", g.url.proxyAuth);
  }
  if( g.zHttpAuth && g.zHttpAuth[0] ){
    const char *zCredentials = g.zHttpAuth;
    char *zEncoded = encode64(zCredentials, -1);
    blob_appendf(pHdr, "Authorization: Basic %s\r\n", zEncoded);
    fossil_free(zEncoded);
  }
  blob_appendf(pHdr, "Host: %s\r\n", g.url.hostname);
  blob_appendf(pHdr, "User-Agent: %s\r\n", get_user_agent());
  if( g.url.isSsh ) blob_appendf(pHdr, "X-Fossil-Transport: SSH\r\n");
  if( nPayload ){
    if( zAltMimetype ){
      blob_appendf(pHdr, "Content-Type: %s\r\n", zAltMimetype);
    }else if( g.fHttpTrace ){
      blob_appendf(pHdr, "Content-Type: application/x-fossil-debug\r\n");
    }else{
      blob_appendf(pHdr, "Content-Type: application/x-fossil\r\n");
    }
    blob_appendf(pHdr, "Content-Length: %d\r\n", blob_size(pPayload));
  }
  blob_append(pHdr, "\r\n", 2);
}

/*
** Use Fossil credentials for HTTP Basic Authorization prompt
*/
static int use_fossil_creds_for_httpauth_prompt(void){
  Blob x;
  char c;
  prompt_user("Use Fossil username and password (y/N)? ", &x);
  c = blob_str(&x)[0];
  blob_reset(&x);
  return ( c=='y' || c=='Y' );
}

/*
** Prompt to save HTTP Basic Authorization information
*/
static int save_httpauth_prompt(void){
  Blob x;
  char c;
  if( (g.url.flags & URL_REMEMBER)==0 ) return 0;
  prompt_user("Remember Basic Authorization credentials (Y/n)? ", &x);
  c = blob_str(&x)[0];
  blob_reset(&x);
  return ( c!='n' && c!='N' );
}

/*
** Get the HTTP Basic Authorization credentials from the user
** when 401 is received.
*/
char *prompt_for_httpauth_creds(void){
  Blob x;
  char *zUser;
  char *zPw;
  char *zPrompt;
  char *zHttpAuth = 0;
  if( !isatty(fileno(stdin)) ) return 0;
  zPrompt = mprintf("\n%s authorization required by\n%s\n",
    g.url.isHttps==1 ? "Encrypted HTTPS" : "Unencrypted HTTP", g.url.canonical);
  fossil_print("%s", zPrompt);
  free(zPrompt);
  if ( g.url.user && g.url.passwd && use_fossil_creds_for_httpauth_prompt() ){
    zHttpAuth = mprintf("%s:%s", g.url.user, g.url.passwd);
  }else{
    prompt_user("Basic Authorization user: ", &x);
    zUser = mprintf("%b", &x);
    zPrompt = mprintf("HTTP password for %b: ", &x);
    blob_reset(&x);
    prompt_for_password(zPrompt, &x, 0);
    zPw = mprintf("%b", &x);
    zHttpAuth = mprintf("%s:%s", zUser, zPw);
    free(zUser);
    free(zPw);
    free(zPrompt);
    blob_reset(&x);
  }
  if( save_httpauth_prompt() ){
    set_httpauth(zHttpAuth);
  }
  return zHttpAuth;
}

/*
** Sign the content in pSend, compress it, and send it to the server
** via HTTP or HTTPS.  Get a reply, uncompress the reply, and store the reply
** in pRecv.  pRecv is assumed to be uninitialized when
** this routine is called - this routine will initialize it.
**
** The server address is contain in the "g" global structure.  The
** url_parse() routine should have been called prior to this routine
** in order to fill this structure appropriately.
*/
int http_exchange(
  Blob *pSend,                /* Message to be sent */
  Blob *pReply,               /* Write the reply here */
  int mHttpFlags,             /* Flags.  See above */
  int maxRedirect,            /* Max number of redirects */
  const char *zAltMimetype    /* Alternative mimetype if not NULL */
){
  Blob login;           /* The login card */
  Blob payload;         /* The complete payload including login card */
  Blob hdr;             /* The HTTP request header */
  int closeConnection;  /* True to close the connection when done */
  int iLength;          /* Expected length of the reply payload */
  int iRecvLen;         /* Received length of the reply payload */
  int rc = 0;           /* Result code */
  int iHttpVersion;     /* Which version of HTTP protocol server uses */
  char *zLine;          /* A single line of the reply header */
  int i;                /* Loop counter */
  int isError = 0;      /* True if the reply is an error message */
  int isCompressed = 1; /* True if the reply is compressed */

  if( transport_open(&g.url) ){
    fossil_warning("%s", transport_errmsg(&g.url));
    return 1;
  }

  /* Construct the login card and prepare the complete payload */
  if( blob_size(pSend)==0 ){
    blob_zero(&payload);
  }else{
    blob_zero(&login);
    if( mHttpFlags & HTTP_USE_LOGIN ) http_build_login_card(pSend, &login);
    if( g.fHttpTrace || (mHttpFlags & HTTP_NOCOMPRESS)!=0 ){
      payload = login;
      blob_append(&payload, blob_buffer(pSend), blob_size(pSend));
    }else{
      blob_compress2(&login, pSend, &payload);
      blob_reset(&login);
    }
  }

  /* Construct the HTTP request header */
  http_build_header(&payload, &hdr, zAltMimetype);

  /* When tracing, write the transmitted HTTP message both to standard
  ** output and into a file.  The file can then be used to drive the
  ** server-side like this:
  **
  **      ./fossil test-http <http-request-1.txt
  */
  if( g.fHttpTrace ){
    static int traceCnt = 0;
    char *zOutFile;
    FILE *out;
    traceCnt++;
    zOutFile = mprintf("http-request-%d.txt", traceCnt);
    out = fopen(zOutFile, "wb");
    if( out ){
      fwrite(blob_buffer(&hdr), 1, blob_size(&hdr), out);
      fwrite(blob_buffer(&payload), 1, blob_size(&payload), out);
      fclose(out);
    }
    free(zOutFile);
    zOutFile = mprintf("http-reply-%d.txt", traceCnt);
    out = fopen(zOutFile, "wb");
    transport_log(out);
    free(zOutFile);
  }

  /*
  ** Send the request to the server.
  */
  if( mHttpFlags & HTTP_VERBOSE ){
    fossil_print("URL: %s\n", g.url.canonical);
    fossil_print("Sending %d byte header and %d byte payload\n",
                  blob_size(&hdr), blob_size(&payload));
  }
  transport_send(&g.url, &hdr);
  transport_send(&g.url, &payload);
  blob_reset(&hdr);
  blob_reset(&payload);
  transport_flip(&g.url);

  /*
  ** Read and interpret the server reply
  */
  closeConnection = 1;
  iLength = -1;
  while( (zLine = transport_receive_line(&g.url))!=0 && zLine[0]!=0 ){
    if( mHttpFlags & HTTP_VERBOSE ){
      fossil_print("Read: [%s]\n", zLine);
    }
    if( fossil_strnicmp(zLine, "http/1.", 7)==0 ){
      if( sscanf(zLine, "HTTP/1.%d %d", &iHttpVersion, &rc)!=2 ) goto write_err;
      if( rc==401 ){
        if( fSeenHttpAuth++ < MAX_HTTP_AUTH ){
          if( g.zHttpAuth ){
            if( g.zHttpAuth ) free(g.zHttpAuth);
          }
          g.zHttpAuth = prompt_for_httpauth_creds();
          transport_close(&g.url);
          return http_exchange(pSend, pReply, mHttpFlags,
                               maxRedirect, zAltMimetype);
        }
      }
      if( rc!=200 && rc!=301 && rc!=302 && rc!=307 && rc!=308 ){
        int ii;
        for(ii=7; zLine[ii] && zLine[ii]!=' '; ii++){}
        while( zLine[ii]==' ' ) ii++;
        fossil_warning("server says: %s", &zLine[ii]);
        goto write_err;
      }
      if( iHttpVersion==0 ){
        closeConnection = 1;
      }else{
        closeConnection = 0;
      }
    }else if( g.url.isSsh && fossil_strnicmp(zLine, "status:", 7)==0 ){
      if( sscanf(zLine, "Status: %d", &rc)!=1 ) goto write_err;
      if( rc!=200 && rc!=301 && rc!=302 && rc!=307 && rc!=308 ){
        int ii;
        for(ii=7; zLine[ii] && zLine[ii]!=' '; ii++){}
        while( zLine[ii]==' ' ) ii++;
        fossil_warning("server says: %s", &zLine[ii]);
        goto write_err;
      }
      closeConnection = 0;
    }else if( fossil_strnicmp(zLine, "content-length:", 15)==0 ){
      for(i=15; fossil_isspace(zLine[i]); i++){}
      iLength = atoi(&zLine[i]);
    }else if( fossil_strnicmp(zLine, "connection:", 11)==0 ){
      char c;
      for(i=11; fossil_isspace(zLine[i]); i++){}
      c = zLine[i];
      if( c=='c' || c=='C' ){
        closeConnection = 1;
      }else if( c=='k' || c=='K' ){
        closeConnection = 0;
      }
    }else if( ( rc==301 || rc==302 || rc==307 || rc==308 ) &&
                fossil_strnicmp(zLine, "location:", 9)==0 ){
      int i, j;
      int wasHttps;

      if ( --maxRedirect == 0){
        fossil_warning("redirect limit exceeded");
        goto write_err;
      }
      for(i=9; zLine[i] && zLine[i]==' '; i++){}
      if( zLine[i]==0 ){
        fossil_warning("malformed redirect: %s", zLine);
        goto write_err;
      }
      j = strlen(zLine) - 1;
      while( j>4 && fossil_strcmp(&zLine[j-4],"/xfer")==0 ){
         j -= 4;
         zLine[j] = 0;
      }
      if( (mHttpFlags & HTTP_QUIET)==0 ){
        fossil_print("redirect with status %d to %s\n", rc, &zLine[i]);
      }
      wasHttps = g.url.isHttps;
      url_parse(&zLine[i], 0);
      if( wasHttps && !g.url.isHttps ){
        fossil_warning("cannot redirect from HTTPS to HTTP");
        goto write_err;
       }
      transport_close(&g.url);
      transport_global_shutdown(&g.url);
      fSeenHttpAuth = 0;
      if( g.zHttpAuth ) free(g.zHttpAuth);
      g.zHttpAuth = get_httpauth();
      if( rc==301 || rc==308 ) url_remember();
      return http_exchange(pSend, pReply, mHttpFlags,
                           maxRedirect, zAltMimetype);
    }else if( fossil_strnicmp(zLine, "content-type: ", 14)==0 ){
      if( fossil_strnicmp(&zLine[14], "application/x-fossil-debug", -1)==0 ){
        isCompressed = 0;
      }else if( fossil_strnicmp(&zLine[14],
                          "application/x-fossil-uncompressed", -1)==0 ){
        isCompressed = 0;
      }else{
        if( (mHttpFlags & HTTP_GENERIC)==0
         && fossil_strnicmp(&zLine[14], "application/x-fossil", -1)!=0
        ){
          isError = 1;
        }
      }
    }
  }
  if( iLength<0 ){
    fossil_warning("server did not reply");
    goto write_err;
  }
  if( rc!=200 ){
    fossil_warning("\"location:\" missing from %d redirect reply", rc);
    goto write_err;
  }

  /*
  ** Extract the reply payload that follows the header
  */
  blob_zero(pReply);
  blob_resize(pReply, iLength);
  iRecvLen = transport_receive(&g.url, blob_buffer(pReply), iLength);
  if( iRecvLen != iLength ){
    fossil_warning("response truncated: got %d bytes of %d", iRecvLen, iLength);
    goto write_err;
  }
  blob_resize(pReply, iLength);
  if( isError ){
    char *z;
    int i, j;
    z = blob_str(pReply);
    for(i=j=0; z[i]; i++, j++){
      if( z[i]=='<' ){
        while( z[i] && z[i]!='>' ) i++;
        if( z[i]==0 ) break;
      }
      z[j] = z[i];
    }
    z[j] = 0;
    fossil_warning("server sends error: %s", z);
    goto write_err;
  }
  if( isCompressed ) blob_uncompress(pReply, pReply);

  /*
  ** Close the connection to the server if appropriate.
  **
  ** FIXME:  There is some bug in the lower layers that prevents the
  ** connection from remaining open.  The easiest fix for now is to
  ** simply close and restart the connection for each round-trip.
  **
  ** For SSH we will leave the connection open.
  */
  if( ! g.url.isSsh ) closeConnection = 1; /* FIX ME */
  if( closeConnection ){
    transport_close(&g.url);
  }else{
    transport_rewind(&g.url);
  }
  return 0;

  /*
  ** Jump to here if an error is seen.
  */
write_err:
  transport_close(&g.url);
  return 1;
}

/*
** COMMAND: test-httpmsg
**
** Usage: %fossil test-httpmsg URL ?PAYLOAD? ?OPTIONS?
**
** Send an HTTP message to URL and get the reply. PAYLOAD is a file containing
** the payload, or "-" to read payload from standard input.  a POST message
** is sent if PAYLOAD is specified and is non-empty.  If PAYLOAD is omitted
** or is an empty file, then a GET message is sent.
**
** Options:
**
**     --compress                 Use ZLIB compression on the payload
**     --mimetype TYPE            Mimetype of the payload
**     --out FILE                 Store the reply in FILE
**     -v                         Verbose output
*/
void test_wget_command(void){
  const char *zMimetype;
  const char *zInFile;
  const char *zOutFile;
  Blob in, out;
  unsigned int mHttpFlags = HTTP_GENERIC|HTTP_NOCOMPRESS;

  zMimetype = find_option("mimetype",0,1);
  zOutFile = find_option("out","o",1);
  if( find_option("verbose","v",0)!=0 ) mHttpFlags |= HTTP_VERBOSE;
  if( find_option("compress",0,0)!=0 ) mHttpFlags &= ~HTTP_NOCOMPRESS;
  db_find_and_open_repository(OPEN_OK_NOT_FOUND|OPEN_ANY_SCHEMA, 0);
  if( g.argc!=3 && g.argc!=4 ){
    usage("URL ?PAYLOAD?");
  }
  zInFile = g.argc==4 ? g.argv[3] : 0;
  url_parse(g.argv[2], 0);
  if( g.url.protocol[0]!='h' ){
    fossil_fatal("the %s command supports only http: and https:", g.argv[1]);
  }
  if( zInFile ){
    blob_read_from_file(&in, zInFile, ExtFILE);
    if( zMimetype==0 ){
      if( fossil_strcmp(zInFile,"-")==0 ){
        zMimetype = "application/x-unknown";
      }else{
        zMimetype = mimetype_from_name(zInFile);
      }
    }
  }else{
    blob_init(&in, 0, 0);
  }
  blob_init(&out, 0, 0);
  if( (mHttpFlags & HTTP_VERBOSE)==0 && zOutFile==0 ){
    zOutFile = "-";
    mHttpFlags |= HTTP_QUIET;
  }
  http_exchange(&in, &out, mHttpFlags, 4, zMimetype);
  if( zOutFile ) blob_write_to_file(&out, zOutFile);
  blob_zero(&in);
  blob_zero(&out);
}
