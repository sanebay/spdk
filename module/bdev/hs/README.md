Steps to build and test.
1. Boot a linux 6.x kernel with ublk and pci generic modules enabled.
CONFIG_BLK_DEV_UBLK=m
CONFIG_UIO_PCI_GENERIC=m

2.  Compile spdk with ublk.

cd spdk
./configure --with-ublk
make -j8

3. Compile the grpc server
cd module/bdev/hs
make -f Makefile.server
./server <memory_size_in_mb> <num_workers>


3. Create hs bdev. Two modes -m 0 for in memory and -m 1 for grpc

sudo build/bin/spdk_tgt

sudo scripts/rpc.py bdev_hs_create -b hs0 -s 127.0.0.1:50051 -t 100 -B 512 -m 1
hs0

4. Create ublk disk.
sudo scripts/rpc.py ublk_create_target
sudo scripts/rpc.py ublk_start_disk hs0 1 -q 2 -d 128
1

5. You should see /dev/ublkb1 device.

sudo fio --name=fiotest --filename=/dev/ublkb1 --size=1G --rw=randread
fiotest: (g=0): rw=randread, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=psync, iodepth=1
fio-3.16
Starting 1 process
Jobs: 1 (f=1): [r(1)][9.8%][r=12.4MiB/s][r=3186 IOPS][eta 01m:14s]
fiotest: (groupid=0, jobs=1): err= 0: pid=1105308: Mon May 22 19:59:31 2023
  read: IOPS=3229, BW=12.6MiB/s (13.2MB/s)(100MiB/7927msec)
    clat (usec): min=187, max=8872, avg=306.68, stdev=252.04
     lat (usec): min=187, max=8872, avg=306.89, stdev=252.04
    clat percentiles (usec):
     |  1.00th=[  204],  5.00th=[  225], 10.00th=[  235], 20.00th=[  253],
     | 30.00th=[  265], 40.00th=[  269], 50.00th=[  281], 60.00th=[  289],
     | 70.00th=[  302], 80.00th=[  322], 90.00th=[  355], 95.00th=[  388],
     | 99.00th=[  709], 99.50th=[ 1287], 99.90th=[ 4948], 99.95th=[ 5080],
     | 99.99th=[ 7570]
   bw (  KiB/s): min=10592, max=16072, per=100.00%, avg=12956.73, stdev=1395.98, samples=15
   iops        : min= 2648, max= 4018, avg=3239.13, stdev=349.03, samples=15
  lat (usec)   : 250=16.84%, 500=81.40%, 750=0.82%, 1000=0.27%
  lat (msec)   : 2=0.36%, 4=0.14%, 10=0.18%
  cpu          : usr=2.17%, sys=6.21%, ctx=25604, majf=0, minf=8
  IO depths    : 1=100.0%, 2=0.0%, 4=0.0%, 8=0.0%, 16=0.0%, 32=0.0%, >=64=0.0%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     issued rwts: total=25600,0,0,0 short=0,0,0,0 dropped=0,0,0,0
     latency   : target=0, window=0, percentile=100.00%, depth=1

Run status group 0 (all jobs):
   READ: bw=12.6MiB/s (13.2MB/s), 12.6MiB/s-12.6MiB/s (13.2MB/s-13.2MB/s), io=100MiB (105MB), run=7927-7927msec

Disk stats (read/write):
  ublkb1: ios=25535/0, merge=0/0, ticks=7015/0, in_queue=7015, util=98.71%
