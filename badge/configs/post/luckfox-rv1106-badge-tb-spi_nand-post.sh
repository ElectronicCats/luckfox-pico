#!/bin/bash
#
# File: configs/post/luckfox-rv1106-badge-tb-spi_nand-post.sh
# Purpose: SDK post-build script: rootfs layout, MTP startup, and app supervision.
#

# remove unused files
function remove_data()
{
	if [ "$RK_BUILD_APP_TO_OEM_PARTITION" != "y" ];then
		# delete except app1, app2, app3, do like this: delete_except=".*/\|.*/app1\|.*/app2\|.*app3"
		delete_except=".*/\|.*/badge_photo\|.*/overlay_web\|.*/rndis_dhcp"
		find "$RK_PROJECT_PACKAGE_ROOTFS_DIR/oem/usr/bin" ! -regex $delete_except -delete

		# badge_photo dlopen()s librkaiq.so
		delete_except=".*/\|.*/librkaiq.so"
		find "$RK_PROJECT_PACKAGE_ROOTFS_DIR/oem/usr/lib" ! -regex $delete_except -delete

		local unused_in_oem=(\
			# wifi ko
			# "usr/ko/cfg80211.ko" \
			# "usr/ko/gf128mul.ko" \
			# "usr/ko/ctr.ko"  \
			# "usr/ko/libaes.ko" \
			# "usr/ko/sha256_generic.ko" \
			# "usr/ko/aes_generic.ko" \
			# "usr/ko/ccm.ko" \
			# "usr/ko/libarc4.ko" \
			# "usr/ko/cmac.ko" \
			# "usr/ko/mac80211.ko" \
			# "usr/ko/ghash-generic.ko" \
			# "usr/ko/libsha256.ko" \
			# "usr/ko/gcm.ko" \
			# vendor_storage ko
			"usr/ko/rk_vendor_storage.ko" \
			"usr/ko/mtd_vendor_storage.ko" \
			# rve.ko
			"usr/ko/rve.ko" \
			# emmc flash
			"usr/ko/jbd2.ko" \
			"usr/ko/mbcache.ko" \
			"usr/ko/crc16.ko" \
			"usr/ko/ext4.ko" \
			# unuesd scripts
			"usr/ko/*.sh" \
			# unused data files
			"usr/lib/*.data" \
			# oem 
			"/oem/usr/lib/*.so*" \
			"/oem/usr/lib/*.md" \
			"/oem/usr/lib/librkdemuxer.so" \
			"/oem/usr/bin/j2s4b_dev" \
			)

		for i in ${unused_in_oem[@]}
		do
			if [ -f $RK_PROJECT_PACKAGE_ROOTFS_DIR/$i ] ;then 
				rm -rf $RK_PROJECT_PACKAGE_ROOTFS_DIR/oem/$i
			fi
		done
	fi

	local unused_in_rootfs=(\
		"lib/libatomic.so*" \
		"etc/iqfiles/*" \
		"etc/services" \
		"etc/protocols"  \
		"lib/libstdc++.so.6.0.25-gdb.py" \
		"lib/libitm.so*" \
		"usr/share/udhcpc" \
		"usr/bin/flash*" \
		"usr/bin/dnsmasq" \
		"usr/bin/hostapd" \
		"usr/bin/iperf" \
		"usr/bin/wifi_start.sh" \
		"mnt/sdcard" \
		)

	for i in ${unused_in_rootfs[@]}
	do
		if [ -f $RK_PROJECT_PACKAGE_USERDATA_DIR/$i ] ;then 	
			rm -rf $RK_PROJECT_PACKAGE_ROOTFS_DIR/$i
		fi
	done

	# disable S20linkmount
	mv $RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/init.d/S20linkmount $RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/init.d/_S20linkmount

}

function copy_data() {
	# For writable data, package it into the userdata partition
	mkdir -p $RK_PROJECT_PACKAGE_USERDATA_DIR
	#cp -r $RK_PROJECT_PATH_APP/userdata/* $RK_PROJECT_PACKAGE_USERDATA_DIR

	# AIQ needs the JSON IQ; make_meta only accepts the BIN for the MCU.
	local iq_dst="$RK_PROJECT_PACKAGE_ROOTFS_DIR/oem/usr/share/iqfiles"
	mkdir -p "$iq_dst"
	if [ -f "$RK_PROJECT_PATH_MEDIA/isp_iqfiles/sc3336_CMK-OT2119-PC1_30IRC-F16.json" ]; then
		cp -f "$RK_PROJECT_PATH_MEDIA/isp_iqfiles/sc3336_CMK-OT2119-PC1_30IRC-F16.json" "$iq_dst/"
	fi
}

function change_mode() {
	# uvc related files
	chmod a+x $RK_PROJECT_PACKAGE_ROOTFS_DIR/usr/bin/usb_config.sh
}

function rewrite_script() {
	# ADB shell expects /system/bin/sh (Android path).
	mkdir -p $RK_PROJECT_PACKAGE_ROOTFS_DIR/system/bin
	ln -sf /bin/sh $RK_PROJECT_PACKAGE_ROOTFS_DIR/system/bin/sh

	# export default rk_log_level for app
	echo "export rt_log_level=4" >>$RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/profile.d/RkEnv.sh

	# remove unused scripts
	rm $RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/init.d/S21appinit*
	rm $RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/init.d/S20urandom*

	# rewrite rcS
	cat >$RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/init.d/rcS <<'EOF'
#!/bin/sh
echo 256 > /proc/sys/kernel/threads-max

if [ -f "/etc/profile.d/RkEnv.sh" ]; then
	. /etc/profile.d/RkEnv.sh
fi

mkdir -p /dev/pts
mountpoint -q /dev/pts || mount -t devpts devpts /dev/pts 2>/dev/null
[ -e /dev/ptmx ] || ln -sf pts/ptmx /dev/ptmx

echo "badge: mount userdata" > /dev/kmsg
if [ -x /etc/init.d/_S20linkmount ]; then
	/etc/init.d/_S20linkmount start >/tmp/linkmount.log 2>&1
fi
mkdir -p /userdata/photos 2>/dev/null

echo "badge: start app" > /dev/kmsg
(
	while true; do
		if [ -e /tmp/badge_photo_pause ]; then
			sleep 1
			continue
		fi
		badge_photo >>/tmp/badge_photo.log 2>&1
		echo "badge_photo exited; restarting" > /dev/kmsg
		sleep 1
	done
) &

# Do not expose MTP, ADB or the web UI until the camera pipeline has produced
# a frame. The 35 s timeout keeps a recovery path if the camera fails.
echo "badge: wait system" > /dev/kmsg
i=0
while [ ! -s /tmp/badge_system.ready ] && [ $i -lt 70 ]; do
	sleep 0.5
	i=$((i + 1))
done
if [ -s /tmp/badge_system.ready ]; then
	echo "badge: system ready" > /dev/kmsg
else
	echo "badge: system timeout; exposing USB for recovery" > /dev/kmsg
fi

echo "badge: start mtp" > /dev/kmsg
if [ -x /etc/init.d/S49mtp ]; then
	/etc/init.d/S49mtp start
fi
EOF
	chmod +x $RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/init.d/rcS

	# rewrite rcK
	cat >$RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/init.d/rcK <<'EOF'
#!/bin/sh
echo "badge: stopping"
touch /tmp/badge_photo_pause
killall badge_photo 2>/dev/null
if [ -x /etc/init.d/S49mtp ]; then
	/etc/init.d/S49mtp stop
fi
sync
umount /userdata 2>/dev/null
EOF
	chmod +x $RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/init.d/rcK

	# rewrite profile
	cat >$RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/profile <<EOF
export PATH="/bin:/sbin:/usr/bin:/usr/sbin"

if [ "\$PS1" ]; then
	if [ "\`id -u\`" -eq 0 ]; then
    	export PS1='[\u@\h \W]# '
	else
		export PS1='[\u@\h \W]$ '
	fi
fi

export PAGER='/bin/more '
export EDITOR='/bin/vi'

# Source configuration files from /etc/profile.d
for i in /etc/profile.d/*.sh ; do
	if [ -r "\$i" ]; then
		. \$i
	fi
	unset i
done
EOF
	chmod +x $RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/profile

	# rewrite hostname
	cat >$RK_PROJECT_PACKAGE_ROOTFS_DIR/etc/hostname <<EOF
luckfox
EOF

}

#=========================
# post
#=========================
remove_data
copy_data
change_mode
rewrite_script