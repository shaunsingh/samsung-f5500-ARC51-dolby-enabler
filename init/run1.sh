#!/bin/sh
#
# Second stage of the SamyGO start; run.sh calls it with the mount point of
# the stick. Mounts SamyGO_data/samyext4.img on /mnt and starts SamyGO there.
#
# The root shell on port 4023 that got us here and the FTP server need no
# password, so they stay up only while the root of the stick holds a file
# ENABLE_SHELL. A file STOP_SAMYGO there keeps SamyGO from starting at all.
#
data=
# shellcheck disable=SC2231
for usb in ${1:-/dtv/usb/sd*}; do
	if [ -e "$usb/STOP_SAMYGO" ]; then
		echo "STOP_SAMYGO found. Exiting."
		exit 1
	fi
	if [ -e "$usb/SamyGO_data/samyext4.img" ]; then
		data=$usb/SamyGO_data
		break
	fi
done
if [ -z "$data" ]; then
	echo "SamyGO_data not found."
	exit 1
fi
if [ -f /mnt/rcSGO ]; then
	echo "SamyGO already started."
	exit 1
fi

mkdir -p /tmp/bin
/mtd_exe/InfoLink/lib/unzip -o -P 12345 "$data/data.zip" -d /tmp/bin/ > /dev/null 2>&1
chmod 755 /tmp/bin/*
/tmp/bin/busybox --install -s /tmp/bin
export PATH="/tmp/bin:$PATH"
export LD_LIBRARY_PATH="/tmp/bin:$LD_LIBRARY_PATH"

if [ -e "$usb/ENABLE_SHELL" ]; then
	tcpsvd -vE 0.0.0.0 21 ftpd -w / &
else
	rm -f /dtv/netcat
	pkill -9 -f /dtv/netcat
fi

ln -sf /dev/loop3 /tmp/loopnone
losetup /tmp/loopnone "$data/samyext4.img"
mount -o noatime,exec /tmp/loopnone /mnt
/tmp/bin/busybox --install -s /mnt/bin

cd / || exit 1
/sbin/rmmod ARS_module > /dev/null 2>&1
/mnt/bin/busybox sh /mnt/rcSGO /mnt >> /dtv/samygo_boot.log 2>&1
/tmp/bin/UEP_killer.sh &
