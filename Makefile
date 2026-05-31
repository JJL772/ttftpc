all:
	$(CC) -g2 -o tftp tftpcmd.c
	
clean:
	rm tftp
	
.PHONY: all clean