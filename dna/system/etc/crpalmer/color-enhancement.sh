#!/system/bin/sh

F=/sdcard/crpalmer-color-enhancement

if [ -r $F ]
then
    echo "Eanbling HTC's 'color_enhancement' (over saturation of whites)"
    mode=y
else
    echo "Disabling HTC's 'color_enhancement' (over saturation of whites)"
    mode=n
fi

echo $mode > /sys/module/board_monarudo_all/parameters/mdp_gamma
echo "You must turn the display off and on for this change to take effect"
