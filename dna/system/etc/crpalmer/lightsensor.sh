#!/system/bin/sh

F=/sdcard/crpalmer-stock-lightsensor

if [ -r $F ]
then
    echo "Reverting to stock lightsensor table"
    echo 1 3 21 3a1 5a0 15ee 2169 307f 3f96 ffff > /sys/class/optical_sensors/lightsensor/ls_adc_table
else
    echo "Leaving lightsensor table unchanged"
fi
