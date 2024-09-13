#!/bin/bash

BINDIR=bin
ORCA_LIB=../Orca/build/bin
ORCA_SRC=../Orca/src
RESDIR=../resources


INCLUDES="-I$ORCA_SRC"
LIBS="-L$ORCA_LIB -lorca"
FLAGS="-mmacos-version-min=10.15.4 -DOC_DEBUG -DLOG_COMPILE_DEBUG"

mkdir -p $BINDIR
clang -g $FLAGS $LIBS $INCLUDES -o $BINDIR/test src/main.c

cp $ORCA_LIB/liborca.dylib $BINDIR/
cp $ORCA_LIB/libwebgpu.dylib $BINDIR/

install_name_tool -add_rpath "@executable_path" $BINDIR/test
