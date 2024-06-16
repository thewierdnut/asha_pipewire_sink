# ASHA (Audio Streaming for Hearing Aids) Pipewire Sink
A sample ASHA implementation designed to work with pipewire and bluez.

This project mostly follows the [Hearing Aid Audio Support Using Bluetooth LE](https://source.android.com/docs/core/connect/bluetooth/asha) document created by Google. Note that the specification varies in minor ways from the actual implementation used in the android source code.

This project will recognize ASHA-compatible hearing devices that have been connected via bluetooth, and it will create virtual pipewire sinks that the user can select to stream audio to the hearing devices.

## Caveats
### ASHA is dead. Long live ASHA
Android's ASHA has been superceded by Bluetooth LE audio. Bluez already supports LE Audio, and I rather suspect that future hearing devices will no longer support the older ASHA protocol.
However:
- There are a large number of existing devices that use ASHA. Additionally, some of the newest releases are still ASHA-only. Since these devices tend to be very expensive, they have an extremely long replacement cycle. I suspect that many of these devices will be around for at least another ten years.
- LE Audio-enabled hearing devices are dual mode, meaning they can use either ASHA or LE Audio. This dual functionality has proven useful since LE Audio implementation by manufacturers has been problematic so far.


### Sound quality
If you have the ability to use LE Audio, then you should definitely prefer it over this project because:
- LE Audio offers lower latency.
- Your devices can operate in hands-free mode, functioning as both receivers and input microphones (a feature not available with ASHA).
- LE Audio utilizes a vastly improved audio codec called LC3, replacing the outdated G.722 codec. While this upgrade is promising in theory, hearing devices are limited by the capabilities of their receivers, restricting their output to 8-12kHz. Consequently, there are improvements in sound quality, albeit marginal.

However, if your hearing devices only support ASHA, then read on.

### Difficult setup
The ASHA spec relies on the central manipulating the stream properties to match what the hearing device expects, but bluez is designed to allow the peripheral to set those properties itself (spoiler alert, they don't). In order to get a good listening experience, you will have to manually configure the bluetooth service and the bluetooth kernel module to set those properties yourself.

### Alternatives are coming
There is at least one effort (see @ford-prefect's [branch here](https://github.com/asymptotic-io/bluez/tree/asha-support), along with the [discussions here](https://github.com/bluez/bluez/pull/836)) to implement ASHA which just recently got merged into BlueZ. I am fully looking forward to throwing away my code and using integrated support instead, but I don't expect it to land into mainstream stable Linux distributions for some time.

## Setup
### A compatible bluetooth adapter
You will need at least a bluetooth 5.0 adapter. I recommend 5.2 if you can get it.

Bluetooth adapters vary a lot in quality and compatibilty. I have tested this on an ASUS BT-500 usb adaptor and on an Intel AX200 wifi/bluetooth adapter. The ASUS device claimed 2M PHY and DLE support, but was still only able to stream to one hearing aid reliably. btmon captures showed that it was only sending 27 bytes of data at a time, apparently not using the DLE support. The Intel device was able to work reliably for both devices, but only after manually enabling 2M PHY.

### Enable LE credit based flow control.
The ASHA spec requires LE credit based flow control, which is turned off by default in the Linux bluetooth kernel module. This can be turned on using the `enable_ecred` module paramter. On my system, I have created this file:

**/etc/modprobe.d/bluetooth_asha.conf**
```
options bluetooth enable_ecred=1
```
After adding this file, you will want to reload the bluetooth module. The easiest way is probably just to reboot your system.

### Change the default connection interval
The ASHA spec requires the central to set the connection interval to match the data transfer rate. Supposedly this is flexible, but I have only ever gotten a 20ms transfer rate to work on my device. Your results may vary. The default interval set by bluez is 30ms, but this can be adjusted by editing the bluetooth configuration. These configuration items will already already be present, but they are commented out, and have the wrong values. Note that these values are set in units of 1.25ms, so 20 / 1.25 = 16.

**/etc/bluetooth/main.conf**
```
[LE]
# LE default connection parameters.  These values are superceeded by any
# specific values provided via the Load Connection Parameters interface
MinConnectionInterval=16
MaxConnectionInterval=16
ConnectionLatency=10
ConnectionSupervisionTimeout=100
```
You will want to restart bluetooth after making these config changes
```
sudo systemctl restart bluetooth
```

### Enable 2M PHY (optional)
With 1M PHY (the default), I can only reliably stream audio to a single hearing aid. In order to utilize both hearing aids, I have to manually enable 2M PHY. I haven't been able to find a configuration option or a bluez interface that will do this, so I have had to resort to using btmgmt commands to do this. You can check your existing enabled PHYs with `btmgmt phy`. If the `Configurable phys` contains `LE2MTX` and `LE2MRX`, but the `Selected phys` does not, then you can copy your existing `Selected phys`, and add the values `LE2MTX LE2MRX` to it. On my system, that looks like this:

```sh
# Check the existing phys
$ sudo btmgmt phy
Supported phys: BR1M1SLOT BR1M3SLOT BR1M5SLOT EDR2M1SLOT EDR2M3SLOT EDR2M5SLOT EDR3M1SLOT EDR3M3SLOT EDR3M5SLOT LE1MTX LE1MRX LE2MTX LE2MRX LECODEDTX LECODEDRX
Configurable phys: BR1M3SLOT BR1M5SLOT EDR2M1SLOT EDR2M3SLOT EDR2M5SLOT EDR3M1SLOT EDR3M3SLOT EDR3M5SLOT LE2MTX LE2MRX LECODEDTX LECODEDRX
Selected phys: BR1M1SLOT BR1M3SLOT BR1M5SLOT EDR2M1SLOT EDR2M3SLOT EDR2M5SLOT EDR3M1SLOT EDR3M3SLOT EDR3M5SLOT LE1MTX LE1MRX
# copy the Selected phys, and add the new LE2MTX LE2MRX values to it
$ sudo btmgmt phy BR1M1SLOT BR1M3SLOT BR1M5SLOT EDR2M1SLOT EDR2M3SLOT EDR2M5SLOT EDR3M1SLOT EDR3M3SLOT EDR3M5SLOT LE1MTX LE1MRX LE2MTX LE2MRX
```

I haven't yet found a configuration setting that will make this setup persist through a power cycle of the bluetooth adapter. This step must be run before connecting your devices.

## Building
### Prerequisites

* glib development packages
* pipewire development packages
* CMake 3.9
* C++14 compiler

On my debian development system, I can install all of this with:
```
sudo apt-get install build-essential cmake libglib2.0-dev libpipewire-0.3-dev
```

### Compiling
Fairly standard cmake-based compile:

```
mkdir build
cd build
cmake ..
make
```

## Running
### Enable 2MPHY if your adapter and your devices support it.
This is not mandatory, but I've only been able to reliably stream to both hearing aids with this enabled. This must be done before connecting your hearing aids. Please read [Enable 2M Phy](#enable-2m-phy-optional) for more details, but on my box, it looks like this:
```
sudo btmgmt phy BR1M1SLOT BR1M3SLOT BR1M5SLOT EDR2M1SLOT EDR2M3SLOT EDR2M5SLOT EDR3M1SLOT EDR3M3SLOT EDR3M5SLOT LE1MTX LE1MRX LE2MTX LE2MRX
```
### Connect your hearing aids
Connect your hearing aids using your standard bluetooth device dialog.
### Check your setup
Run the `asha_connection_test` utility. This will check for common configuration mistakes and provide some debugging output that will be of use for troubleshooting.
### Run the pipewire sink
Run `asha_pipewire_sink`. If you want to see lots of debug output, you can set the environment variable `G_MESSAGES_DEBUG=all`

Once `asha_pipewire_sink` detects your devices, it should create a new virtual pipewire sink that you can select as an audio device. You may have to select `Show Virtual Devices` in the KDE volume control or `Show: All Output Devices` in pavucontrol to see it.

If the audio is choppy, delayed, or sounds like it is shifting from ear to ear, then your adapter may not be able to keep up with the bandwidth requirements. Try connecting a single device and see if the quality improves.

## Feedback
Please use the issue reporter to let me know of your experiences, positive or negative, trying to get this to work. I would love to generate a compatibility table, and perhaps to troubleshoot and add support for additional devices.

Please check [the wiki](https://github.com/thewierdnut/asha_pipewire_sink/wiki) for current testing status of various devices.

## Known issues
- stream is more stable (and disconnect less likely) when the hearing devices are connected prior to starting asha_pipewire stream or asha_stream_test
- additionl simultaneously connected BT peripherals besides hearing devices will led to instability
- switching app windows or opening sound panel might result in a glitchy stream
