IMG:=floppy.img
FLOPPY:=/mnt/floppy/

all:
	nasm boot.asm -o boot.bin
	nasm loader.asm -o loader.bin
	nasm -f elf -o kernel.o kernel.asm
	nasm -f elf -o string.o string.asm
	nasm -f elf -o kliba.o kliba.asm
	gcc -m32 -c -fno-builtin -o start.o start.c
	ld -m elf_i386 -s -Ttext 0x30400 -o kernel.bin \
		kernel.o string.o start.o kliba.o

	dd if=boot.bin of=floppy.img bs=512 count=1 conv=notrunc
	sudo mount -o loop floppy.img /mnt/floppy/
	sudo cp loader.bin $(FLOPPY) -v
	sudo cp kernel.bin $(FLOPPY) -v
	sudo umount /mnt/floppy/

clean:
	rm -f kernel.o string.o start.o kliba.o kernel.bin
