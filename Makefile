# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux fat filesystem support.
#

#obj-m+=fatprfs.o

#obj-$(CONFIG_FAT_FS) += fat.o
#obj-$(CONFIG_VFAT_FS) += vfat.o
#obj-$(CONFIG_MSDOS_FS) += msdos.o

obj-m += prfsmode.o
obj-m += fatprfs.o
obj-m += vfatprfs.o
obj-m += msdosprfs.o

prfsmode-m := proc_handler.o
fatprfs-m := cache.o dir.o fatent.o file.o inode.o misc.o nfs.o
vfatprfs-m := namei_vfat.o
msdosprfs-m := namei_msdos.o


obj-$(CONFIG_FAT_KUNIT_TEST) += fat_test.o

all:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) clean
