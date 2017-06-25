# OrangeOs
Orange's一个操作系统的实现 实现代码
目前更新到第六章  


#### 1 开发环境  
linux mint 18 sarah  
Linux asus 4.4.0-21-generic x86_64  


安装开发工具：  
sudo apt install bochs（用命令安装的bochs2.6,已经支持调试）  
sudo apt install bochs-x  
sudo apt install bximage  
sudo apt install nasm  

利用bochs的模拟窗口工具栏的snapshot button，每小节的输出结果一般保存在snapshot.txt或snapshot.bmp   

tips:
##### 1 中断bochs模拟终端  
Ctrl+c  
之后输入help可以显示控制命令  
^C05893732802i[     ] Ctrl-C detected in signal handler.  
Next at t=5893732803
(0) [0x00000000000fff53] f000:ff53 (unk. ctxt): iret                      ; cf  
05893732803i[XGUI ] Mouse capture off  
<bochs:2> help  
h|help - show list of debugger commands  
h|help command - show short command description  
-*- Debugger control -*-  
    help, q|quit|exit, set, instrument, show, trace, trace-reg,  
    trace-mem, u|disasm, ldsym, slist  
-*- Execution control -*-  
    c|cont|continue, s|step, p|n|next, modebp, vmexitbp  
-*- Breakpoint management -*-  
    vb|vbreak, lb|lbreak, pb|pbreak|b|break, sb, sba, blist,  
    bpe, bpd, d|del|delete, watch, unwatch  
-*- CPU and memory contents -*-  
    x, xp, setpmem, writemem, crc, info,  
    r|reg|regs|registers, fp|fpu, mmx, sse, sreg, dreg, creg,  
    page, set, ptime, print-stack, ?|calc  
-*- Working with bochs param tree -*-  
    show "param", restore  
05893732803i[XGUI ] Mouse capture off  
<bochs:3>   


##### 2 关于 bochs 和 bochs 调试 键盘不响应   
每次进入虚拟机后，比如进入了free-dos，然后切换回终端，再次切换回来后就无法相应键盘输入了。  
如果是用鼠标切换，那就可以相应了  
或者 从其他程序切换回来后，再按一下alt键就又可以输入了  

#####3 freedos中的帮助命令   
freedos中输入?可显示所有可用命令  

##### 5 bochsrc中记录log的两种方法：  
log: bochsout.txt  
debugger_log: debugger.out  
man bochsrc  
1 全部记录 log:   Give the path of the log file you'd like Bochs debug and misc. verbiage to be written to.   If you really don't want it, make it /dev/null. 

              Example:  
                log: bochs.out  
                log: /dev/tty               (unix only)  
                log: /dev/null              (unix only)  

2 只记录调试输出debugger_log:  
              Give the path of the log file you'd like Bochs to log debugger output.  If you really don't want it, make it '/dev/null', or '-'.  

              Example:  
                log: debugger.out  
                log: /dev/null              (unix only)  
                log: -  



##### 6 bochs的模拟窗口工具栏的使用  
http://bochs.sourceforge.net/cgi-bin/topper.pl?name=New+Bochs+Documentation&url=http://bochs.sourceforge.net/doc/docbook/user/index.html  
5.3.2. The Bochs headerbar  
The headerbar appears on top of the Bochs simulation window. Here you can control the behavoiur of Bochs at runtime if you click on one of these buttons:  

floppy buttons  
Here you can toggle the status of the floppy media (inserted/ejected). Bochs for win32 presents you a small dialog box for changing the floppy image. You can setup floppy drives using floppya/floppyb option.  

cdrom button  
Here you can toggle the media status of the first CD-ROM drive (inserted/ejected). CD-ROM drives can be set up using ata(0-3)-master/-slave option. On some platforms this button brings a up a small dialog box for changing the CD-ROM image.

mouse button  
Here you can enable the creation of mouse events by the host. Once mouse events are captured, you cannot reach the button anymore, in order to disable capturing again. By default you can enable and disable the mouse capture pressing the CTRL key and the third (middle) mouse button. See the mouse option parameter 'toggle' for other methods to toggle the mouse capture.
Note: Changing the mouse capture at runtime is not supported by all display libraries, but it is already present on RFB, SDL, SDL2, VNCSRV, Win32, wxWidgets and X11.  
Note: Support for 2 button mouse to toggle the capture mode not yet complete - using another toggle method is recommended in that case.  

user button  
Press this button if you want to send the keyboard shortcut defined with the user_shortcut parameter of the keyboard option to the guest. Depending on the used display_library option, it may even be possible to edit the shortcut before sending it.

copy button  
The text mode screen text can be exported to the clipboard after pressing this button. The button has no effect in graphics mode.

paste button  
Text in the clipboard can also be pasted, through Bochs, to the guest OS, as simulated keystrokes. Keyboard mapping must be enabled to make this feature work.  

snapshot button  
Press this button if you want to save a snapshot of the Bochs screen. All text and graphics modes are now supported. If gui dialogs are supported (e.g. on win32) Bochs presents you a "Save as..." dialog box to specify the filename. All other platforms are using the fixed filenames "snapshot.txt" or "snapshot.bmp".  

config button  
This button stops the Bochs simulation and starts the runtime configuration. (see below).  

reset button  
Press this button to trigger a hardware reset.  

suspend button  
Press this button to save current simulation state to a disk. The simulation could be restored back using bochs -r command. For more details read the Save and restore simulation section.  

power button  
This button stops the simulation and quits bochs.  

Some of this features may not be implemented or work different on your host platform.  




#### 2 how to build and run
1.nasm boot.asm -o boot.bin  
2.nasm loader.asm -o loader.bin  
3.bximage 生成一个a.img镜像  
4.dd if=boot.bin of=a.img bs=512 count=1 conv=notrunc  
5.sudo mount -o loop a.img  /mnt/floppy/  
6.sudo cp loader.bin /mnt/floppy/ -v  
7.sudo cp kernel.bin /mnt/floppy/ -v  
8.sudo umount /mnt/floppy/  
9.bochs   



