# etphidiap
Elan Touchpad Firmware Update (HID Interface)
---
    Get ELAN TouchPad firmware version & Update Firmware.

Compilation
--- 
    make: to build the exectue project "etphidiap".
    $ make
   
   
Run
---
Get Firmware Version :
  ./etphid_updater -g
  
Get Module ID :
  ./etphid_updater -m
  
Get Hardware ID :
  ./etphid_updater -w
  
Update Firmware : 
  ./etphid_updater -b {bin_file}
