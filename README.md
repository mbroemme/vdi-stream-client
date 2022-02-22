# VDI Stream Client

[![Build Status](https://travis-ci.com/mbroemme/vdi-stream-client.svg?branch=main)](https://travis-ci.com/mbroemme/vdi-stream-client)
[![GitHub release](https://img.shields.io/github/release/mbroemme/vdi-stream-client.svg)](https://github.com/mbroemme/vdi-stream-client/releases)
[![GitHub issues](https://img.shields.io/github/issues/mbroemme/vdi-stream-client.svg)](https://github.com/mbroemme/vdi-stream-client/issues)
[![GitHub forks](https://img.shields.io/github/forks/mbroemme/vdi-stream-client.svg)](https://github.com/mbroemme/vdi-stream-client/network/members)
[![GitHub stars](https://img.shields.io/github/stars/mbroemme/vdi-stream-client.svg)](https://github.com/mbroemme/vdi-stream-client/stargazers)
[![GitHub license](https://img.shields.io/github/license/mbroemme/vdi-stream-client.svg)](https://github.com/mbroemme/vdi-stream-client/blob/main/LICENSE)
[![GitHub downloads](https://img.shields.io/github/downloads/mbroemme/vdi-stream-client/total.svg)](https://github.com/mbroemme/vdi-stream-client/releases)

A very tiny and low latency desktop streaming client for remote Windows guests
with GPU passthrough which supports [Nvidia NVENC](https://en.wikipedia.org/wiki/Nvidia_NVENC),
[AMD VCE](https://en.wikipedia.org/wiki/Video_Coding_Engine), [VCN](https://en.wikipedia.org/wiki/Video_Core_Next)
and [Intel Quick Sync Video](https://en.wikipedia.org/wiki/Intel_Quick_Sync_Video).

# Overview

VDI Stream Client is a tiny and low latency Linux client which connects to [Parsec Host](https://parsec.app/)
running on Windows. It allows to run fully GPU accelerated Windows desktop
environment on a remote machine while streaming the content to a local Linux
system which process keyboard and mouse input. The remote server can be a
virtual machine running on-top of on-premise deployments of [Virtuozzo Hybrid
Server](https://www.virtuozzo.com/support/all-products/virtuozzo-hybrid-server.html),
[KVM](https://www.linux-kvm.org/page/Main_Page) or [Xen](https://xenproject.org/).
It can also connect to public clouds like [Amazon (EC2 G3 Accelerated)](https://aws.amazon.com/ec2/instance-types/g3/)
or [Microsoft Azure (NV6)](https://azure.microsoft.com/en-us/pricing/details/virtual-machines/windows/).
It uses [Simple DirectMedia Layer (SDL2)](https://libsdl.org/) for low-level
access to audio, video, keyboard, mouse and clipboard.

# Motivation

I'm using Linux on Desktop since more than 20 years already for several
reasons - customization, small and tiny floating window managers like [Fluxbox](http://fluxbox.org/),
ideal platform for engineers and developers and many more. However over the
past couple of years my daily job required more attention which rely on
Microsoft Windows applications like Microsoft Office and full fledged groupware
solution like Microsoft Outlook. There exist open source alternative like [LibreOffice](https://www.libreoffice.org/)
and [Thunderbird](https://www.thunderbird.net/) plus various Exchange
connectors or even [SOGo](https://sogo.nu/). They are all good in their niche
but they didn't reach compatibility to satisfy me. I tried a lot of different
ways of solving the problem (see below in the comparison table) but at the end
I have only three main goals:

1. GPU passthrough with remote support. Using hosted remote Windows desktop
   with full-fledged GPU acceleration for 3D, video decoding and encoding to
   run any kind of application with full GPU support as they would have been
   used locally.
2. Low latency network connection. Using laptop running Linux to connect from
   anywhere to the Windows desktop and do my daily work. This must work via
   fast network like ethernet, roaming wireless and low bandwidth with unstable
   mobile broadband.
3. Being very energy efficient on client. The laptop battery drain should not
   significantly increase while working on the remote desktop. Also the server
   should not become more energy hungry than needed for simple streaming.

# Comparison

In Linux ecosystem there exist already wide range of options to run Windows
guest environment as virtual machine on-top of a Linux host and access either
its local display or via remote display protocol, all of them have their pros
and cons. Look at the table below for brief comparison.

Method                | Local | Remote | 3D
----------------------|-------|--------|------------------
[QXL with Spice](https://www.spice-space.org/)        | Yes   | Yes    | No
[GPU Passthrough](https://www.kernel.org/doc/Documentation/vfio.txt)       | Yes   | No     | Yes
[KVM FrameRelay](https://looking-glass.io/)        | Yes   | No     | Yes
[iGVT-g](https://www.kernel.org/doc/Documentation/vfio-mediated-device.txt)                | Yes   | No     | Yes (Intel only)
[Virgil 3D](https://virgil3d.github.io/)             | Yes   | Yes    | Yes (Linux only)
[SPICE Streaming Agent](https://gitlab.freedesktop.org/spice/spice-streaming-agent) | Yes   | Yes    | Yes (Linux only)
[Moonlight](https://moonlight-stream.org/)             | Yes   | Yes    | Yes (Nvidia only)
[Parsec](https://parsec.app/)                | Yes   | Yes    | Yes

So why another streaming client is needed if one for Windows, Linux and macOS
exist already? Well the Parsec client is focusing on gaming capabilities while
VDI Stream Client is focusing on having a reliable daily working environment.
The table below gives a brief overview about differences.

Features                | VDI Stream Client | Parsec
------------------------|-------------------|---------------
Keyboard Input          | Full              | Partial
Mouse Input             | Full              | Partial
Gamepad Input           | No                | Yes
Clipboard Sharing       | Text only         | Text only
Remote                  | Yes               | Yes
DirectX                 | Yes               | Yes
OpenGL                  | Yes               | Yes
Resolution Sync         | Host-to-Client    | Client-to-Host
Alt+Tab Integration     | Yes               | No
Minimal GUI             | Yes               | No
System SDL2             | Yes               | No
Auto Reconnect          | Yes               | No
Screensaver Integration | Yes               | No
USB Redirection         | Yes               | No
[Color Mode 4:4:4](https://en.wikipedia.org/wiki/Chroma_subsampling)        | [SDK Bug](https://github.com/parsec-cloud/parsec-sdk/issues/36)           | Yes

# Requirements

Any recent GPU with [OpenGL](https://en.wikipedia.org/wiki/OpenGL) and [VA-API](https://en.wikipedia.org/wiki/Video_Acceleration_API)
support will work. When it comes to 4:4:4 color mode, part of the decoding work
is made with [FFmpeg](https://ffmpeg.org/) (see notes below regarding 4:4:4
status). Nothing exist without drawbacks and that one for Parsec is that it is
mandatory to have an [account](https://parsec.app/signup) which is
completely free. However communication between client and host is always made
directly. Also the SDK is only available as pre-compiled library, so for those
who fully rely on an open-source system, stop reading here. Some features are
only available with a commercial subscription under [Parsec Warp](https://parsec.app/warp).
It applies to 4:4:4 color mode which is required to encode images and streams
without chroma subsampling for sharp and crystal clear text.

# Features

* Fully CLI based customization. User can specify all required parameters via
  command line switches, allow perfect integration into different window
  managers.
* Desktop in window support with [XGrabKeyboard](https://tronche.com/gui/x/xlib/input/XGrabKeyboard.html)
  and [XUngrabKeyboard](https://tronche.com/gui/x/xlib/input/XUngrabKeyboard.html)
  while leaving mouse pointer ungrabbed. Supporting Alt+Tab and Windows special
  keys grab, even if assigned to the window manager.
* SDL window is created without `SDL_WINDOW_RESIZABLE` flag set, so window
  cannot be resized by window manager and always uses native host resolution
  (no more blurry desktop).
* Host to client resolution sync with automatic window resize. User can change
  resolution in Windows control panel and client polls for the changes and
  adjust client window size.
* Specify client window size via command line and host will change native
  resolution to it once the connection has been established.
* Configurable mouse wheel sensitivity. User can specify mouse scroll speed via
  command line switch serving different needs and requirements.
* Modular architecture and can be extended with additional streaming host
  services like Nvidia GameStream via Moonlight libraries.
* Screen saver and screen locker support. By default screen saver support is
  enabled but user can specify command line switch to avoid setting [SDL_EnableScreenSaver](https://wiki.libsdl.org/SDL_EnableScreenSaver)
  and disallow X server to lock the local screen.
* Built-in USB redirection with automatic reconnect support using
  [usbredir protocol](https://gitlab.freedesktop.org/spice/usbredir). User can
  specify via command line which local USB devices will be redirected to the
  host (e.g. to forward a webcam with microphone). However usbredir protocol is
  currently only supported if using KVM, Xen or Virtuozzo Hybrid Server with
  QEMU. Other virtualization solutions may use standard Linux USB/IP server and
  native [Windows client driver](https://github.com/cezanne/usbip-win).

# Parsec Warp

* Support for disabling chroma subsampling to support color mode 4:4:4 with
  sharp and crisp text and better colors. Available for Nvidia Hosts running
  GTX 900 or better.

# Known Issues

* Color mode 4:4:4 is not yet supported by the SDK. There is an open bug at [Parsec SDK GitHub](https://github.com/parsec-cloud/parsec-sdk/issues/36)
  and Parsec team want to fix it with next SDK release. In official [Parsec](https://parsec.app/downloads)
  client it works already with some internal callbacks in the SDK.
* Resolution changes from client connected to the host are not persistent and
  only valid within the session. Once the last client disconnects the host
  restores original resolution.
* No macOS and Windows support yet. However porting should be fairly easy but I
  haven't tested it and pull requests are welcome.

# Building

VDI Stream Client uses GNU Build System to configure, build and install the
application. It requires `sdl2`, `sdl2_ttf`, `libx11`, `libglvnd`, `libusb`,
`usbredir` and the [Parsec SDK](https://github.com/parsec-cloud/parsec-sdk).
The build system will search the SDK first in build directory and use DSO
linking, the resulting binary will be redistributable but you need to ship
Parsec library somehow. If not found, it will search in system-wide include and
library directories and link accordingly, binary may not be redistributable.
For build and install use the commands below and if `--prefix=/usr` is used,
the `make install` command must be run as root user.

```
git clone https://github.com/parsec-cloud/parsec-sdk
./configure --prefix=/usr &&
make &&
make install
```

Arch Linux users can download ready-to-use `PKGBUILD` file available from
[Arch User Repository (AUR)](https://aur.archlinux.org/packages/vdi-stream-client/), following these [build](https://wiki.archlinux.org/index.php/Arch_User_Repository#Build_the_package) and [install](https://wiki.archlinux.org/index.php/Arch_User_Repository#Install_the_package) instructions.

# Usage

VDI Stream Client requires that the Parsec Windows host ([x86_64](https://builds.parsecgaming.com/package/parsec-windows.exe)
or [x86](https://builds.parsecgaming.com/package/parsec-windows32.exe)) is
running and you have created a [free account](https://parsec.app/signup).
There are several advanced configuration options for visual and network
latency improvements described in the Parsec [documentation](https://support.parsec.app/hc/en-us/articles/360001562772-All-Advanced-Configuration-Options)
in the "Hosting Settings" chapter.

The client requires a `sessionID` and a `peerID` obtained through the Parsec
API to identify users to make secure connections. For convenience you can use
[tools/parsec-login](tools/parsec-login) script to retrieve list of available
hosts. This is one-time operation whenever you add Parsec host application to
another Windows machine.

## USB Redirection

If you are using [libvirt](https://libvirt.org/) for virtualization, you can
redirect local USB devices to your Windows host. It is highly recommended to
add XHCI controller because it is the only one which supports USB 1.1, USB 2.0
and USB 3.0, the QEMU UHCI and EHCI controllers support only their respective
USB standard. Windows 10 is shipped by default with an XHCI USB 3.0 driver. Add
the following to your `<devices>` section in domain XML for an USB 3.0
controller:

```
<controller type='usb' model='qemu-xhci'/>
```

With the controller you can start using USB redirection via TCP/IP protocol.
Each USB device is redirected through an independent port and the firewall of
your server must allow incoming TCP/IP connection for that. VDI Stream Client
supports monitoring of hotplug events (e.g. plug and unplug) of the redirected
USB devices to allow automatic reassignment to the Windows host. Add the
following to your `<devices>` section in domain XML to enable redirection of
one local USB device and replace `<ip>` with the IP address of your
virtualization host:

```
<redirdev bus='usb' type='tcp'>
  <source mode='bind' host='<ip>' service='4000'/>
</redirdev>
```

You need to add additional `<redirdev>` sections to redirect multiple devices
and increase the port number accordingly. After reloading libvirt and starting
the Virtual Machine with the configuration above, you can redirect a local USB
device. You can get a list of available devices with their `<vendorID>` and
`<productID>` with `lsusb` command.

```
vdi-stream-client --session <sessionID> --peer <peerID> --redirect <vendorID>:<productID>@<ip>#4000
```

Once connection is established it will redirect the local USB device to the
host and setup drivers. Multiple USB devices can be redirected using `,`
separator to the `--redirect` switch. They will be reassigned if the client
reconnects to the host. Moreover, devices are reconnected to the host
automatically when you unplug and plug them again to the USB port. If the
devices are not connected during startup of the client, they are monitored and
redirected as soon as they are attached to the client.

# Notes

USB redirection integration doesn't support any kind of encryption. The main
goal of this client is to support in-house desktop streaming between client and
host. Please use a VPN like [WireGuard](https://www.wireguard.com/) if you are
using it across [WAN](https://en.wikipedia.org/wiki/Wide_area_network)
connections.

# Future

Building a truly and fully open-source GPU accelerated desktop streaming
solution requires a host service running inside the Windows environment.
Hardware encoding is supported by FFmpeg for the major GPU vendors. VDI Stream
Host application seems reasonable.

# Thanks

A special thanks goes to Parsec Cloud, Inc. as they provided a Parsec Warp
subscription to support this project.

# Support

If you want to help out with development without providing code yourself, you
can always donate to the project directly using the following platforms.

* [GitHub](https://github.com/sponsors/mbroemme)
* [PayPal](https://www.paypal.com/donate?hosted_button_id=TJLZRF9BPVX22)
* Bitcoin - bc1qh2pssvhac7629k3ndmmpw3azsm4yjz9jvaaycr
* Litecoin - ltc1qfz93z928vr36d9ksp5mstt385t84mdx4shsv35

# Videos

| [![Unigine Valley](http://img.youtube.com/vi/QU3rVi3JyvY/mqdefault.jpg)](http://www.youtube.com/watch?v=QU3rVi3JyvY "Unigine Valley")    | [![3DMark Night Raid Demo](http://img.youtube.com/vi/NLhCHEwChzM/mqdefault.jpg)](https://www.youtube.com/watch?v=NLhCHEwChzM "3DMark Night Raid Demo") | [![3DMark Wild Life Benchmark](http://img.youtube.com/vi/dfT5Glya4Yw/mqdefault.jpg)](https://www.youtube.com/watch?v=dfT5Glya4Yw "3DMark Wild Life Benchmark") |
|------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------|
| [![USB Redirection](http://img.youtube.com/vi/_KP94X3J9n4/mqdefault.jpg)](https://www.youtube.com/watch?v=_KP94X3J9n4 "USB Redirection") | [![Video Playback](http://img.youtube.com/vi/5NDn_mNogNk/mqdefault.jpg)](https://www.youtube.com/watch?v=5NDn_mNogNk "Video Playback")                 | [![Unreal Tournament 2004](http://img.youtube.com/vi/q4QOEhNK-us/mqdefault.jpg)](https://www.youtube.com/watch?v=q4QOEhNK-us "Unreal Tournament 2004")         |
