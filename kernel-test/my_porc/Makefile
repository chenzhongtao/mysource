
obj-m += myproc.o
obj-m += procfs_example.o
obj-m += seq_file.o

KDIR ?= /usr/src/linux-2.6.32-431.el6

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean