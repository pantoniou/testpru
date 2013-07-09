#
# Makefile for an example remote proc PRU program
# It is assumed that the PRU compiler environment has been set
#

.PHONY: all clean

all: testpru0.stripped testpru0.lst testpru1.stripped testpru1.lst

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

OBJS0:=testpru0.obj syscall0.obj debug.obj pru_vring.obj asmutil.obj
OBJS1:=testpru1.obj syscall1.obj debug.obj asmutil.obj

%.obj: %.c
	$(CC) $(CFLAGS) -c $<

%.obj: %.asm
	$(CC) $(CFLAGS) -c $<

testpru0: $(OBJS0)
	$(CC) $(CFLAGS) $^ -z $(LDFLAGS) -o $@

testpru1: $(OBJS1)
	$(CC) $(CFLAGS) $^ -z $(LDFLAGS) -o $@

testpru0.stripped: testpru0
	$(STRIP) $(STRIPFLAGS) $< -o $@

testpru1.stripped: testpru1
	$(STRIP) $(STRIPFLAGS) $< -o $@

testpru0.lst: testpru0
	$(OBJDUMP) -1 $< > $@

testpru1.lst: testpru1
	$(OBJDUMP) -1 $< > $@

clean:
	rm -f \
		testpru0 testpru0.asm \
		testpru1 testpru1.asm \
		*.obj *.lst *.out *.stripped \
		tags

distclean: clean
	rm -f *.pp

# include any generated deps
-include $(patsubst %.obj,%.pp,$(OBJS))
