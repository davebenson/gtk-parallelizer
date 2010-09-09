all: gtk-parallelizer pline

gtk-parallelizer: gtk-parallelizer.c
	gcc -o $@ $^ `pkg-config --cflags --libs gtk+-2.0`

pline: pline-main.c parallelizer.c parallelizer.h
	gcc -o $@ pline-main.c parallelizer.c `pkg-config --cflags --libs glib-2.0`


clean:
	rm -f gtk-parallelizer
