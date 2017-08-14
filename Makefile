CC=gcc
CFLAGS=-Wall -O3 -std=gnu99 
#-DDEBUG=1
SRCDIR=src
EXPLDIR=examples
INCDIR=-I$(SRCDIR) -I$(EXPLDIR)
ODIR=obj
LDIR=lib
LIBS=-lrt

.PHONY: default
default: all

odir:
	mkdir -p $(ODIR)

ldir:
	mkdir -p $(LDIR)
	
pagesize: odir
	$(CC) $(SRCDIR)/pagesize.c -o $(ODIR)/pagesize $(CFLAGS) $(INCDIR) $(LIBS)

defpagesize: pagesize
	$(eval PSIZE := -DPAGE_SIZE=$(shell $(ODIR)/pagesize))
	
nvmalloc.o: defpagesize odir
	$(CC) -fPIC -c $(SRCDIR)/nvmalloc.c -o $(ODIR)/nvmalloc.o $(CFLAGS) $(PSIZE) $(LIBS)
	$(CC) -c $(SRCDIR)/nvmalloc.c -o $(ODIR)/nvmalloc_sta.o $(CFLAGS) $(PSIZE) $(LIBS)
	
libnvmalloc.so: nvmalloc.o ldir
	$(CC) -shared -fPIC -o $(LDIR)/libnvmalloc.so $(ODIR)/nvmalloc.o $(LIBS)
	
libnvmalloc.a: nvmalloc.o ldir	
	ar rcs $(LDIR)/libnvmalloc.a $(ODIR)/nvmalloc_sta.o

examples: odir defpagesize libnvmalloc.so
	$(CC) -c $(EXPLDIR)/linked_list.c -o $(ODIR)/linked_list.o $(CFLAGS) $(PSIZE) $(INCDIR) $(LIBS)
	$(CC) -L$(LDIR) $(ODIR)/linked_list.o -o $(ODIR)/linkedlist $(CFLAGS) $(PSIZE) $(INCDIR) $(LIBS) -lnvmalloc
	
clean:
	rm -fr $(ODIR)
	
all: libnvmalloc.a libnvmalloc.so examples
