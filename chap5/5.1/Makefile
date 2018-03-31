all:
	ls
	# nasm -f elf hello.asm -o hello.o
	# Use 64 bits instead of 32 for your loader and compile it with the following command
	nasm -f elf64 hello.asm -o hello.o
	ls
	ld -s hello.o -o hello.elf64
	ls

