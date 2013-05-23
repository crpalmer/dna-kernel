#!/system/bin/sh

F=/sdcard/crpalmer-cpufreq-
SYS=/sys/devices/system/cpu/cpu

did_anything=

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
        did_anything="yes"
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

# On stock-based ROMs, HTC overrides the minimum clock
# speed in a boot complete hook which is obnoxiou and
# so we will just lock down the cpufreq entirely.

if [ "$did_anything" = "yes" ]
then
    echo "Locking down cpufreq"
    echo 1 > /sys/devices/system/cpu/cpufreq/locked/locked
else
    echo "Didn't change anything, no need to lock it down"
fi
