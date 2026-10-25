# eth1-mode (GL-MT3000, OpenWrt 25)

Umschaltung von `eth1` über den seitlichen Schiebeschalter (`BTN_0`). `eth0` wird nicht referenziert.

| Modus | eth1                           | Wireless-Client (`sta_section`) |
|-------|--------------------------------|---------------------------------|
| `wan` | einziger Port von `wan` (DHCP) | `disabled=1`                    |
| `lan` | Port von `br-lan`              | `disabled=0`, Uplink            |

Der Wireless-Client (Standard `wifinet3`) muss als `wifi-iface` mit `mode=sta` bereits
eingerichtet sein (SSID, Schlüssel, `network`, z. B. `wwan` in der Firewall-Zone `wan`).
`eth1-mode` schaltet nur dessen `disabled`-Option um.

`wan6` wird, falls vorhanden, auf `device='@wan'` gesetzt.

## Dateien

| Datei | Funktion |
|-------|----------|
| `files/usr/sbin/eth1-mode` | Umschaltlogik (`wan`, `lan`, `status`, `sync`, `switch 0\|1`) |
| `files/etc/config/eth1-mode` | UCI-Konfiguration (Schalterzuordnung, STA-Parameter) |
| `files/etc/rc.button/BTN_0` | Hotplug-Handler des Schiebeschalters |
| `files/etc/init.d/eth1-mode` | Abgleich mit Schalterstellung beim Boot (`START=19`, vor `network`) |

## Installation

```sh
scp -O -r files/* root@192.168.1.1:/
ssh root@192.168.1.1
chmod 755 /usr/sbin/eth1-mode /etc/rc.button/BTN_0 /etc/init.d/eth1-mode
uci set eth1-mode.main.sta_section='<wifi-iface>'   # nur falls nicht wifinet3
uci commit eth1-mode
/etc/init.d/eth1-mode enable
eth1-mode status
```

## Inbetriebnahme

1. `eth1-mode status` ausgeben, Schalter umlegen, erneut ausgeben.
2. Stimmt die Zuordnung nicht, `pressed_mode` umkehren:
   `uci set eth1-mode.main.pressed_mode='wan'; uci commit eth1-mode`
3. Wird `switch: unbekannt` ausgegeben, Label prüfen: `cat /sys/kernel/debug/gpio` und `switch_label` anpassen.

Log: `logread -e eth1-mode`
