[声音修复](./audio.md)

# WiFI
复制 `brcmfmac4356-pcie.Xiaomi Inc-Mipad2.txt` 到 `/lib/firmware/brcm` 文件夹下。
如果无效，重命名文件为 `brcmfmac4356-pcie.txt`。

# 蓝牙
复制 `BCM4356A2.hcd` 到 `/lib/firmware/brcm` 文件夹下。

# Intel Atom ISP
复制 `shisp_2401a0_v21.bin` 到 `/lib/firmware/` 文件夹下。

## 摄像头
OV5693 主线驱动，效果一般。  
T4KA3 主线驱动，7.2 起已收录，开启 `CONFIG_VIDEO_T4KA3`（模块名 `t4ka3`）即可，ACPI HID 为 `XMCC0003`。  
> 7.2 起后置 T4KA3 摄像头使用上游驱动（`drivers/media/i2c/t4ka3.c`，`CONFIG_VIDEO_T4KA3`，ACPI HID `XMCC0003`），本地 patch 已失效并移除。  
这个驱动是根据 [[PATCH v7] media: Add t4ka3 camera sensor driver](https://lore.kernel.org/all/20241108091844.151033-1-hpa@redhat.com/) 修改的，感谢 `Kate Hsuan <hpa@redhat.com>` 的贡献。

# 触摸屏底部按键
内容来自 [systemd/systemd-stable](https://github.com/systemd/systemd-stable/blob/v255-stable/hwdb.d/60-keyboard.hwdb)
添加 `/etc/udev/hwdb.d/60-keyboard.hwdb` 文件
```
###########################################################
# Xiaomi
###########################################################

# Fix mapping of menu / home / back capacitive buttons on bottom bezel
# Menu: LeftMeta + S   -> menu      (ignore LeftMeta, map S to menu)
# Home: LeftCtrl + Esc -> LeftMeta  (ignore LeftCtrl, map Esc to LeftMeta)
# Back: Backspace      -> back      (map backspace to back)
evdev:name:FTSC1000:00 2808:509C Keyboard:dmi:*:svnXiaomiInc:pnMipad2:*
 KEYBOARD_KEY_700e0=reserved	# LeftCtrl -> ignore
 KEYBOARD_KEY_700e3=reserved	# LeftMeta -> ignore
 KEYBOARD_KEY_70016=menu	# S -> menu
 KEYBOARD_KEY_70029=leftmeta	# Esc -> LeftMeta (Windows key / Win8 tablets home)
 KEYBOARD_KEY_7002a=back	# Backspace -> back

```

# 视频解码加速
arch上libva版本比较新降级即可，2.21.0-1版本可用。  
arch使用i965驱动解码单元，包名：libva-intel-driver  
https://github.com/intel/libva/issues/830

