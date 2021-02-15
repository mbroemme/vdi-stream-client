# VDI Stream Client

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

Method          | Local | Remote | 3D
----------------|-------|--------|------------------
[QXL with Spice](https://www.spice-space.org/)  | Yes   | Yes    | No
[GPU Passthrough](https://www.kernel.org/doc/Documentation/vfio.txt) | Yes   | No     | Yes
[KVM FrameRelay](https://looking-glass.io/)  | Yes   | No     | Yes
[iGVT-g](https://www.kernel.org/doc/Documentation/vfio-mediated-device.txt)          | Yes   | No     | Yes (Intel only)
[Virgil 3D](https://virgil3d.github.io/)       | Yes   | Yes    | Yes (Linux only)
[Moonlight](https://moonlight-stream.org/)       | Yes   | Yes    | Yes (Nvidia only)
[Parsec](https://parsec.app/)          | Yes   | Yes    | Yes

So why another streaming client is needed if one for Windows, Linux and macOS
exist already? Well the Parsec client is focusing on gaming capabilities while
VDI Stream Client is focusing on having a reliable daily working environment.
The table below gives a brief overview about differences.

Features                | VDI Stream Client | Parsec
------------------------|-------------------|---------------
Keyboard Input          | Full              | Partial
Mouse Input             | Full              | Partial
Gamepad Input           | No                | Yes
Clipboard Sharing       | Yes               | Text only
Remote                  | Yes               | Yes
DirectX                 | Yes               | Yes
OpenGL                  | Yes               | Yes
Resolution Sync         | Host-to-Client    | Client-to-Host
Alt+Tab Integration     | Yes               | No
Minimal GUI             | Yes               | No
System SDL2             | Yes               | No
Auto Reconnect          | Yes               | No
Screensaver Integration | Yes               | No
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
* Full clipboard sharing support between host and client. User can copy and
  paste text and binary data like images between both environments using
  built-in [Spice Protocol](https://www.spice-space.org/).
* Modular architecture and can be extended with additional streaming host
  services like Nvidia GameStream via Moonlight libraries.
* Screen saver and screen locker support. By default screen saver support is
  enabled but user can specify command line switch to avoid setting [SDL_EnableScreenSaver](https://wiki.libsdl.org/SDL_EnableScreenSaver)
  and disallow X server to lock the local screen.

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
application. It requires `sdl2`, `sdl2_ttf`, `libx11`, `libglvnd` and the
[Parsec SDK](https://github.com/parsec-cloud/parsec-sdk). The build system
will search the SDK first in build directory and use DSO linking, the resulting
binary will be redistributable but you need to ship Parsec library somehow. If
not found, it will search in system-wide include and library directories and
link accordingly, binary may not be redistributable. For build and install use
the commands below and if `--prefix=/usr` is used, the `make install` command
must be run as root user.

```
git clone https://github.com/parsec-cloud/parsec-sdk
./configure --prefix=/usr &&
make &&
make install
```

Arch Linux users can download ready-to-use `PKGBUILD` file available from
[Arch User Repository (AUR)](https://aur.archlinux.org/packages/vdi-stream-client/), following [these](https://wiki.archlinux.org/index.php/Arch_User_Repository#Build_and_install_the_package) build and install instructions.

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

# Future

Building a truly and fully open-source GPU accelerated desktop streaming
solution requires a host service running inside the Windows environment.
Hardware encoding is supported by FFmpeg for the major GPU vendors. VDI Stream
Host application seems reasonable.

VDI Stream Client can use Spice Protocol for clipboard sharing of text and raw
data between client and host. The Spice Protocol is also capable to do USB
redirection of devices attached to the client and it will be added in a future
version. Moreover Spice Protocol is currently only supported if using KVM or
Xen virtualization.
