/*
** Copyright (c) 2014 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
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
** This is a stand-alone utility program that is part of the Fossil build
** process.  This program reads files named on the command line and converts
** them into ANSI-C static char array variables.  Output is written onto
** standard output.
**
** The makefiles use this utility to package various resources (large scripts,
** GIF images, etc) that are separate files in the source code as byte
** arrays in the resulting executable.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
** Read the entire content of the file named zFilename into memory obtained
** from malloc() and return a pointer to that memory.  Write the size of the
** file into *pnByte.
*/
static unsigned char *read_file(const char *zFilename, int *pnByte){
  FILE *in;
  unsigned char *z;
  int nByte;
  int got;
  in = fopen(zFilename, "rb");
  if( in==0 ){
    return 0;
  }
  fseek(in, 0, SEEK_END);
  *pnByte = nByte = ftell(in);
  fseek(in, 0, SEEK_SET);
  z = malloc( nByte+1 );
  if( z==0 ){
    fprintf(stderr, "failed to allocate %d bytes\n", nByte+1);
    exit(1);
  }
  got = fread(z, 1, nByte, in);
  fclose(in);
  z[got] = 0;
  return z;
}

/*
** Try to compress a javascript file by removing unnecessary whitespace.
**
** Warning:  This compression routine does not necessarily work for any
** arbitrary Javascript source file.  But it should work ok for the
** well-behaved source files in this project.
*/
static void compressJavascript(unsigned char *z, int *pn){
  int n = *pn;
  int i, j, k;
  for(i=j=0; i<n; i++){
    unsigned char c = z[i];
    if( c=='/' ){
      if( z[i+1]=='*' ){
        while( j>0 && (z[j-1]==' ' || z[j-1]=='\t') ){ j--; }
        for(k=i+3; k<n && (z[k]!='/' || z[k-1]!='*'); k++){}
        i = k;
        continue;
      }else if( z[i+1]=='/' ){
        while( j>0 && (z[j-1]==' ' || z[j-1]=='\t') ){ j--; }
        for(k=i+2; k<n && z[k]!='\n'; k++){}
        i = k-1;
        continue;
      }
    }
    if( c=='\n' ){
      while( j>0 && isspace(z[j-1]) ) j--;
      z[j++] = '\n';
      while( i+1<n && isspace(z[i+1]) ) i++;
      continue;
    }
    z[j++] = c;
  }
  z[j] = 0;
  *pn = j;
}

/*
** There is an instance of the following for each file translated.
*/
typedef struct Resource Resource;
struct Resource {
  char *zName;
  int nByte;
  int idx;
};

/*
** Compare two Resource objects for sorting purposes.  They sort
** in zName order so that Fossil can search for resources using
** a binary search.
*/
static int compareResource(const void *a, const void *b){
  Resource *pA = (Resource*)a;
  Resource *pB = (Resource*)b;
  return strcmp(pA->zName, pB->zName);
}

int main(int argc, char **argv){
  int i, sz;
  int j, n;
  Resource *aRes;
  int nRes;
  unsigned char *pData;
  int nErr = 0;
  int nSkip;
  int nPrefix = 0;
  int nName;

  if( argc>3 && strcmp(argv[1],"--prefix")==0 ){
    nPrefix = (int)strlen(argv[2]);
    argc -= 2;
    argv += 2;
  }
  nRes = argc - 1;
  aRes = malloc( nRes*sizeof(aRes[0]) );
  if( aRes==0 ){
    fprintf(stderr, "malloc failed\n");
    return 1;
  }
  for(i=0; i<argc-1; i++){
    aRes[i].zName = argv[i+1];
  }
  qsort(aRes, nRes, sizeof(aRes[0]), compareResource);
  printf("/* Automatically generated code:  Do not edit.\n**\n"
         "** Rerun the \"mkbuiltin.c\" program or rerun the Fossil\n"
         "** makefile to update this source file.\n"
         "*/\n");
  for(i=0; i<nRes; i++){
    pData = read_file(aRes[i].zName, &sz);
    if( pData==0 ){
      fprintf(stderr, "Cannot open file [%s]\n", aRes[i].zName);
      nErr++;
      continue;
    }

    /* Skip initial lines beginning with # */
    nSkip = 0;
    while( pData[nSkip]=='#' ){
      while( pData[nSkip]!=0 && pData[nSkip]!='\n' ){ nSkip++; }
      if( pData[nSkip]=='\n' ) nSkip++;
    }

    /* Compress javascript source files */
    nName = (int)strlen(aRes[i].zName);
    if( (nName>3 && strcmp(&aRes[i].zName[nName-3],".js")==0)
     || (nName>7  && strcmp(&aRes[i].zName[nName-7], "/js.txt")==0)
    ){
      int x = sz-nSkip;
      compressJavascript(pData+nSkip, &x);
      sz = x + nSkip;
    }

    aRes[i].nByte = sz - nSkip;
    aRes[i].idx = i;
    printf("/* Content of file %s */\n", aRes[i].zName);
    printf("static const unsigned char bidata%d[%d] = {\n  ",
           i, sz+1-nSkip);
    for(j=nSkip, n=0; j<=sz; j++){
      printf("%3d", pData[j]);
      if( j==sz ){
        printf(" };\n");
      }else if( n==14 ){
        printf(",\n  ");
        n = 0;
      }else{
        printf(", ");
        n++;
      }
    }
    free(pData);
  }
  printf("typedef struct BuiltinFileTable BuiltinFileTable;\n");
  printf("struct BuiltinFileTable {\n");
  printf("  const char *zName;\n");
  printf("  const unsigned char *pData;\n");
  printf("  int nByte;\n");
  printf("};\n");
  printf("static const BuiltinFileTable aBuiltinFiles[] = {\n");
  for(i=0; i<nRes; i++){
    char *z = aRes[i].zName;
    if( strlen(z)>=nPrefix ) z += nPrefix;
    while( z[0]=='.' || z[0]=='/' || z[0]=='\\' ){ z++; }
    aRes[i].zName = z;
    while( z[0] ){
      if( z[0]=='\\' ) z[0] = '/';
      z++;
    }
  }
  qsort(aRes, nRes, sizeof(aRes[0]), compareResource);
  for(i=0; i<nRes; i++){
    printf("  { \"%s\", bidata%d, %d },\n",
           aRes[i].zName, aRes[i].idx, aRes[i].nByte);
  }
  printf("};\n");
  return nErr;
}
