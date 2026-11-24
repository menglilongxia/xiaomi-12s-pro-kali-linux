#!/bin/sh
if grep -q okay /proc/device-tree/soc@0/display-subsystem@ae00000/status 2>/dev/null; then
    PF="true"
else
    PF="false"
fi
cat > /etc/X11/xorg.conf.d/20-simpledrm.conf <<XEOF
Section "Monitor"
    Identifier "PhonePanel"
    Option "Rotate" "right"
EndSection

Section "Device"
    Identifier "UnicornDRM"
    Driver "modesetting"
    Option "Monitor-None-1" "PhonePanel"
    Option "Monitor-DSI-1" "PhonePanel"
    Option "AccelMethod" "glamor"
    Option "PageFlip" "$PF"
EndSection
XEOF
