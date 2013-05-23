#!/system/bin/sh

VDD=/sys/devices/system/cpu/cpufreq/vdd_table/vdd_levels
F=/sdcard/crpalmer-uv

if [ -r $F ]
then
     vdd0=`head -1 $F`
     vdd="$vdd0"000
     echo "Voltage adjustment: $vdd0 (really $vdd)"
     echo "$vdd" > $VDD
else
     echo "No voltage adjustment, missing $F"
fi
