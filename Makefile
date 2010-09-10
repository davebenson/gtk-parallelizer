all: gtk-parallelizer pline

gtk-parallelizer: gtk-parallelizer.c
	gcc -g -o $@ $^ `pkg-config --cflags --libs gtk+-2.0`

pline: pline-main.c parallelizer.c parallelizer.h g-source-fd.c
	gcc -g -o $@ pline-main.c parallelizer.c g-source-fd.c `pkg-config --cflags --libs glib-2.0`


clean:
	rm -f gtk-parallelizer pline
