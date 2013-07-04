#
# Makefile for an example remote proc PRU program
# It is assumed that the PRU compiler environment has been set
#

.PHONY: all clean

all: testpru.stripped testpru.lst

CC=clpru
LD=lnkpru
AR=arpru
OBJDUMP=dispru
STRIP=strippru

# -v3				PRU version 3
# --c99 			C99 support
# --gcc 			Enable GCC extensions
# -O3				Optimization level maximum
# --printf_support=minimal 	Minimal printf
# -ppd				Generate dependencies *.pp
# -ppa				Continue after generating deps
# -DDEBUG			Enable debug
# CFLAGS= -v3 --c99 --gcc -O3 --printf_support=minimal -ppd -ppa -DDEBUG 
CFLAGS= -v3 --c99 --gcc -O3 --printf_support=minimal -ppd -ppa

# -cr 				Link using RAM auto init model (loader assisted)
# -x				Reread libs until no unresolved symbols found
LDFLAGS=-cr --diag_warning=225 -llnk-am33xx.cmd -x

STRIPFLAGS=

OBJS:=testpru.obj syscall.obj debug.obj pru_vring.obj

%.obj: %.c
	$(CC) $(CFLAGS) -c $<

%.obj: %.asm
	$(CC) $(CFLAGS) -c $<

testpru: $(OBJS)
	$(CC) $(CFLAGS) $^ -z $(LDFLAGS) -o $@

testpru.stripped: testpru
	$(STRIP) $(STRIPFLAGS) $< -o $@

testpru.lst: testpru
	$(OBJDUMP) -1 $< > $@

clean:
	rm -f testpru testpru.asm *.obj *.lst *.out *.stripped \
		tags

distclean: clean
	rm -f *.pp

# include any generated deps
-include $(patsubst %.obj,%.pp,$(OBJS))
