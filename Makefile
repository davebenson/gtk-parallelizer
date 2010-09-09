gtk-parallelizer: gtk-parallelizer.c
	gcc -o $@ $^ `pkg-config --cflags --libs gtk+-2.0`

clean:
	rm -f gtk-parallelizer
