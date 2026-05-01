
PROM  = media/rom/sun2-multi-rev-10F.bin
DISK  = media/disk/disk.img
TAPE  = media/tape/tape

all:
	make -C m68k all
	make -C sim all

sunos20:
	cp media/disk/my-sun2-s2.0-disk.img $(DISK)
	ln -sf tape2.0 media/tape/tape

sunos32:
	cp media/disk/my-sun2-s3.2-disk.img $(DISK)
	ln -sf tape3.2 media/tape/tape

sunos35:
	cp media/disk/my-sun2-s3.5-disk.img $(DISK)
	rm -f media/tape/tape

run: all
	@test -f $(DISK) || (echo "No disk.img found. Run 'make sunos20', 'make sunos32', or 'make sunos35' first." && exit 1)
	sim/sim --prom=$(PROM) --disk=$(DISK) --tape=$(TAPE) --auto-abort

clean:
	make -C m68k clean
	make -C sim clean

