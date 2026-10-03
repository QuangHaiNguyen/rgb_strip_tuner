# Opening the LED tuner on your home network

Once the device has joined your Wi-Fi network, the WS2812 tuner page is available from any browser on the same network.

## Address

Type the full address, including `http://`:

```
http://rgb-tuner.local/
```

Without the `http://` prefix, some browsers treat `rgb-tuner.local` as a search term, or try HTTPS first. The device serves plain HTTP only.

## If `rgb-tuner.local` does not open: use the IP address

Some devices cannot resolve `.local` names. Use the device's IP address instead: `http://<a.b.c.d>/`. There are two ways to find it:

- **Serial log:** after every connection the device prints a line like this:
  ```
  station IP address 192.168.1.42, fallback URL http://192.168.1.42/
  ```
- **Router:** look for the client named `rgb-tuner` in your router's list of DHCP clients (leases).

The page and the `Send` button work the same way at the IP address.

## Client notes

- **Windows 10/11, macOS, iOS:** `.local` names work out of the box.
- **Android:** many Android versions and vendor builds do not resolve `.local` names in the browser. Use the IP address.
- **Linux:** `.local` names need `avahi-daemon` running and `libnss-mdns` installed, with `mdns4_minimal` listed on the `hosts:` line of `/etc/nsswitch.conf`. Ubuntu desktop has this by default; minimal and server installs usually do not.
- The computer or phone must be on the same network and subnet as the device. Guest networks, and routers with "AP isolation" or "client isolation" turned on, block the name lookup. The IP address may still work.
- After setting up Wi-Fi through the device's own access point, reconnect your phone or computer to your home network before opening `http://rgb-tuner.local/`.

## One device per network

Only one device on a network can be `rgb-tuner.local`. If a second device claims the name, it is renamed automatically, for example to `rgb-tuner-2.local`. The serial log then shows a warning with the name in use:

```
mDNS hostname conflict: using rgb-tuner-2.local instead of rgb-tuner.local
```

Open the renamed address, or use the IP address.

## Access and security

- The tuner needs **no password**. Anyone on your home network can open it and change the LED strip timing.
- Only pages served by the device itself (at `rgb-tuner.local`, the renamed name, or its IP address) can submit tuner values. A web page from any other site that tries to send values to the device is rejected with `403 Forbidden origin`.
- Wi-Fi setup is not available on the home network. To change Wi-Fi credentials, hold the setup button (GPIO9) for one second to switch the device into provisioning mode.
