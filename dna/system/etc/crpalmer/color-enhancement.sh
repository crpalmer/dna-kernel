#!/system/bin/sh

F=/sdcard/crpalmer-color-enhancement

enabled=n
use_m7=n

if [ -r $F ]
then
    value="`head -1 $F`"
    if [ "$value" = "m7" ]
    then
	use_m7=y
    fi
    enabled=y
fi

echo $enabled > /sys/module/board_monarudo_all/parameters/mdp_gamma
echo $use_m7 > /sys/module/board_monarudo_all/parameters/mdp_gamma_m7

echo "color-enhancement (gamma): enabled=$enabled use_m7=$use_m7"
echo "color-enhancement (gamma): You must turn the display off and on for this change to take effect"
