#
#	USB boot tool for ChinaChip CC1800 system-on-chip.
#
#	Copyright (C) 2011 Ignacio Garcia Perez <iggarpe@gmail.com>
#
#	This program is free software; you can redistribute it and/or modify
#	it under the terms of the GNU General Public License version 2 as
#	published by the Free Software Foundation.
#

all : usbtool

clean :
	rm -rf usbtool *.o

usbtool : main.o
	gcc -lusb -o $@ $<

%.o : %.c
	gcc -Wall -c -o $@ $<

