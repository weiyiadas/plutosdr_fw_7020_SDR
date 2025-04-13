This code is modified from pluto-fw-v0.38

In the root directory there is a file SCH.pdf which is the schematic diagram

* Compilation environment
  
 In order to avoid accidents.
 
 it is best to compile and install Vivado2022.2 in Ubuntu 18.04 desktop version：
 
![image](https://github.com/user-attachments/assets/7725210c-4c26-4afb-8eb7-61274f4dc616)


* Build Instructions
```bash
 sudo apt-get install git build-essential fakeroot libncurses5-dev libssl-dev ccache
 sudo apt-get install dfu-util u-boot-tools device-tree-compiler libssl1.0-dev mtools
 sudo apt-get install bc python cpio zip unzip rsync file wget
 git clone --recursive https://github.com/weiyiadas/plutosdr_fw_7010_SDR.git
 cd plutosdr-fw
 export VIVADO_SETTINGS=/opt/Xilinx/Vivado/2022.2/settings64.sh
 make

```


* Make SD card boot image

  After consolidation build is completed, build SD card boot image.
 ```bash
 make sdimg
 ```
  Generate the SD image in the build_sdimg folder, and generate the following files

![1744533580659](https://github.com/user-attachments/assets/fa599cc4-be92-48e0-b6b3-710a280cb6fa)

 Format the SD card to FAT32

 Copy all files to the SD card, and then insert the SD card into the development board
