#!/bin/sh
rm -f config.log
rm -f config.status
rm -f configure
rm -f makefile
autoreconf -i
rm -rf autom4te.cache aclocal.m4
