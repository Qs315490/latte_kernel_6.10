[声音修复](./audio.md)

# WiFI
复制 `brcmfmac4356-pcie.Xiaomi Inc-Mipad2.txt` 到 `/lib/firmware/brcm` 文件夹下。
如果无效，重命名文件为 `brcmfmac4356-pcie.txt`。

# 蓝牙
复制 `BCM4356A2.hcd` 到 `/lib/firmware/brcm` 文件夹下。

# Intel Atom ISP
重命名文件 `shisp_2401a0_v21.bin.bak` 为 `shisp_2401a0_v21.bin`。
复制 `shisp_2401a0_v21.bin` 到 `/lib/firmware/` 文件夹下。

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
 KEYBOARD_KEY_700e0=unknown	# LeftCtrl -> ignore
 KEYBOARD_KEY_700e3=unknown	# LeftMeta -> ignore
 KEYBOARD_KEY_70016=menu	# S -> menu
 KEYBOARD_KEY_70029=leftmeta	# Esc -> LeftMeta (Windows key / Win8 tablets home)
 KEYBOARD_KEY_7002a=back	# Backspace -> back

```