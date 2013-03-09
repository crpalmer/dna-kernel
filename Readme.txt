--Please follow below command to download the official android toolchain: (arm-eabi-4.6)
        
                git clone https://android.googlesource.com/platform/prebuilt

                NOTE: the tool ¡¥git¡¦ will need to be installed first; for example, on Ubuntu, the installation command would be: apt-get install git

--Modify the .bashrc to add the toolchain path, like bellowing example:

								PATH=/usr/local/share/toolchain-eabi-4.6/bin:$PATH 

--Start 
$make ARCH=arm CROSS_COMPILE=$TOP/prebuilts/gcc/linux-x86/arm/arm-eabi-4.6/bin/arm-eabi- apq8064_defconfig
$make ARCH=arm CROSS_COMPILE=$TOP/prebuilts/gcc/linux-x86/arm/arm-eabi-4.6/bin/arm-eabi- -j8

$TOP is an absolute path to android JB code base


--Clean
								$make clean

--Files path
After build process is finished, there should a zImage under arch/arm/boot/
l. fastboot flash zimage arch/arm/boot/zImage
2. Update all kernel modules as follow
	 adb remount
   adb push ./drivers/input/evbug.ko system/lib/modules/
   adb push ./drivers/crypto/msm/qcedev.ko system/lib/modules/
   adb push ./drivers/crypto/msm/qcrypto.ko system/lib/modules/
   adb push ./drivers/crypto/msm/qce40.ko system/lib/modules/
   adb push ./drivers/misc/eeprom/eeprom_93cx6.ko system/lib/modules/
   adb push ./drivers/spi/spidev.ko system/lib/modules/
   adb push ./drivers/scsi/scsi_wait_scan.ko system/lib/modules/
   adb push ./drivers/video/backlight/lcd.ko system/lib/modules/
   adb push ./drivers/bluetooth/bluetooth-power.ko system/lib/modules/
   adb push ./drivers/net/ethernet/micrel/ks8851.ko system/lib/modules/
   adb push ./drivers/net/wireless/bcmdhd_4334/bcmdhd.ko system/lib/modules/
   adb push ./drivers/media/video/gspca/gspca_main.ko system/lib/modules/
   adb push ./crypto/ansi_cprng.ko system/lib/modules/
   adb push ./arch/arm/mach-msm/msm-buspm-dev.ko system/lib/modules/
   adb push ./arch/arm/mach-msm/reset_modem.ko system/lib/modules/
   adb push ./block/test-iosched.ko system/lib/modules/
   adb shell chmod 0644 system/lib/modules/*
   adb reboot