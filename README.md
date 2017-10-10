# Unofficial Kernel for Cyanogenmod 12.1 (SM-J105H)
![Spreadtrum SC8830](https://www.notebookcheck.net/typo3temp/_processed_/7/8/csm_SC8800D_1_01485188df.jpg "Samsung Galaxy J1 mini Duos")
###################################################################################################
HOW TO BUILD Unofficial Kernel for Cyanogenmod 12.1 SM-J105H                                      

1. How to Build
	- get Toolchain
	download and install arm-eabi-4.8 toolchain for ARM EABI.               
	Extract kernel source and move into the top directory.

	$ make cyanogenmod_j1mini3g_defconfig                                                    
	$ make -j2


2. Output files
	- Kernel : Kernel/arch/arm/boot/zImage
	- module : Kernel/drivers/*/*.ko

3. How to Clean	
    $ make clean
                                                                                   
###################################################################################################
