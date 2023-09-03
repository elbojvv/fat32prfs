# fat32prfs
Ransomware safe file system

This is a ransomware safe file system for Linux. It works on a FAT32 storage device. When a file is modified a backup with a time stamp is made. This backup can only be read and not be modified. After a ransomware attack all backup files can be restored to the point before the attack. Therefore this is a Protected Reversible File System (PRFS).

This PRFS was developed for a Raspberry Pi Zero W which can be used as file server with Samba and a FTP daemon. 

The PRFS has three modes. The description above is the normal PRFS mode. A second mode is read-only. The third mode is reversed PRFS (rPRFS) in wich only the backup files can be deleted and the normal files are read-only.

After an attack on the client, the drive can be put in read-only. On a clean client machine all files can be analyzed and restored to the point before the attack.

If one wants to clean the drive of all backup files, one should put the drive first in read-only mode. It should be checked that all normal files are not infected. Thereafter the drive can be put in tPRFS mode and the backup files can be deleted to clean up space. So even when this operation would be done while having ransomware on the client, the ransomware cannot infect any files (without having a backup). 

This kernel module was developed on Linux kernel version 6.1.21+ on a Raspberry Pi Zero W with Raspberry Pi OS.

# Compiling
First the kernel headers should be downloaded with:
```
sudo apt-get install raspberrypi-kernel-headers
```

The modules can be made with:
```
make
```

You should make a mount point for the prfs directory. It is free to choose, but I used:
```
mkdir /mnt/prfs
```

# Installing 
The steps below can also be found in the load file.

The kernel modules should be loaded in the following order: 
```
sudo insmod prfsmode.ko
sudo insmod fatprfs.ko
sudo insmod vfatprfs.ko
```

You should have a FAT32 formatted storage device. One can use a USB storage device or a partition on the SD card. If the OS has automounted this to FAT32 one first has to unmount this. With as example automounted on /media/pi/0FA7-9420:
```
sudo umount /media/pi/0FA7-9420
```

Now you can mount the storage device, with as example the storage device /dev/mmcblk0p3 and mount point /mnt/prfs:
```
sudo mount /dev/mmcblk0p3 -t vfatprfs -o rw,nosuid,nodev,relatime,uid=1000,gid=1000,fmask=0022,dmask=0022 /mnt/prfs
```

Assuming one also has the switch see: [PRFS switch](https://github.com/elbojvv/prfsswitch) one has to start the switch program:
```
sudo ../prfsswitch/prfs_switch &
```

Otherwise you can write the number 0, 1 or 2 (for PRFS, read-only and rPRFS modes) to /proc/prfs_mode, either from the shell or from your own program. This should be done as super user. Note, the fat32prfs kernel module starts in read only mode until another mode is written. Example:
```
echo 0 | sudo tee /proc/prfs_mode 
```

# Known issues:
- Unloading the kernel modules does not work. This seems to be a Linux problem.
- I have not made yet a good backup for removing and renaming files. Therefore these operations are blocked in PRFS en read-only modes and are only allowed on backup files in rPRFS mode. 
