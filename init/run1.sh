#!/bin/sh
samygo_data_dir=""
for USB in ${1:-/dtv/usb/sd*} ; do
    if [ -e "$USB/STOP_SAMYGO" ]; then
        echo "STOP_SAMYGO found. Exiting."
        exit 1
    fi
    if [ -e "$USB/SamyGO_data/samyext4.img" ]; then
        samygo_data_dir="$USB/SamyGO_data"
        break
    fi
done
cd /tmp
if [ -f "/mnt/rcSGO" ]; then
    echo "SamyGO already started."
    exit 1
fi
if [ -z "$samygo_data_dir" ]; then
    echo "SamyGO_data not found."
    exit 1
fi
mkdir -p /tmp/bin
/mtd_exe/InfoLink/lib/unzip -o -P 12345 "$samygo_data_dir/data.zip" -d /tmp/bin/ > /dev/null 2>&1
chmod 777 /tmp/bin/*
/tmp/bin/busybox --install -s /tmp/bin
export PATH=/tmp/bin:$PATH
export LD_LIBRARY_PATH=/tmp/bin:$LD_LIBRARY_PATH

# Maintenance daemons gated behind USB root file ENABLE_SHELL
if [ -e "$USB/ENABLE_SHELL" ]; then
    /tmp/bin/busybox tcpsvd -vE 0.0.0.0 21 /tmp/bin/busybox ftpd -w / &
else
    # Close bootstrap netcat shell (port 4023) when ENABLE_SHELL is absent
    rm -f /dtv/netcat 2>/dev/null
    for p in $(ps w | grep "/dtv/netcat" | grep -v grep | awk '{print $1}'); do
        kill -9 "$p" 2>/dev/null
    done
fi

ln -sf /dev/loop3 /tmp/loopnone
/tmp/bin/busybox losetup /tmp/loopnone "$samygo_data_dir/samyext4.img"
/tmp/bin/busybox mount -o noatime,exec /tmp/loopnone /mnt
/tmp/bin/busybox --install -s /mnt/bin

cd / && /sbin/rmmod ARS_module > /dev/null 2>&1
/mnt/bin/busybox sh /mnt/rcSGO /mnt >> /dtv/samygo_boot.log 2>&1
/tmp/bin/UEP_killer.sh &
