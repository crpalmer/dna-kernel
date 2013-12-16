#!/system/bin/sh

F=/sdcard/crpalmer-cpufreq-
SYS=/sys/devices/system/cpu/cpu
must_lock=0

Fgov=$F"governor"
if [ -r "$Fgov" ]
then
    cpugov=`head -1 $Fgov`
    for cpu in 0 1 2 3
    do
	echo "Setting governor to $cpugov"
	echo "$cpugov" > $SYS$cpu/cpufreq/scaling_governor
	cat $SYS$cpu/cpufreq/scaling_governor
    done

    # /system/etc/init.post_boot.sh will reset the governor.  If we changed it
    # then lock down the driver to allow our setting to win
    must_lock=1
else
    echo "Not changing the governor"
fi

for cpu in 0 1 2 3
do
    echo "Ensuring cpu$cpu is online"
    echo 1 > "$SYS$cpu"/online
done

for which in min max
do
   Fwhich=$F"$which"
   if [ -r $Fwhich ]
   then
	freq=`cat $Fwhich`
	for cpu in 0 1 2 3
	do
	    echo "Setting $which of $freq for cpu$cpu"
	    echo $freq > "$SYS$cpu"/cpufreq/scaling_"$which"_freq
	done
   else
	echo "Not changing the $which frequency"
   fi
done

if [ "$must_lock" = 1 ]; then
    echo "Locking down cpufreq"
    echo 1 > /sys/devices/system/cpu/cpufreq/locked/locked
fi
