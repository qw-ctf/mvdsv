# MVDSV: a QuakeWorld server
![MVDSV Logo](https://raw.githubusercontent.com/QW-Group/mvdsv/master/resources/logo/mvdsv.png)

**[MVDSV][mvdsv]** (MultiView Demo SerVer) has been the most popular **QuakeWorld** server in the world for more than a decade because of its ability to record every player's point of view in a server side demo and provide many different game modes to enjoy **QuakeWorld** with.

## Getting Started

The following instructions will help you get a **[MVDSV][mvdsv]** server up and running on your local machine from prebuilt binaries. Details on how to compile your own **[MVDSV][mvdsv]** binary will also be included to match specific architectures or for development purposes.

## Supported architectures

The following architectures are fully supported by **[MVDSV][mvdsv]** and are available as prebuilt binaries:
* Linux amd64 (Intel and AMD 64-bits processors)
* Linux i686 (Intel and AMD 32-bit processors)
* Linux aarch (ARM 64-bit processors)
* Linux armhf (ARM 32-bit processors)
* Windows x64 (Intel and AMD 64-bits processors)
* Windows x86 (Intel and AMD 32-bit processors)
* macOS arm64 (Apple Silicon processors)

## Prebuilt binaries
You can find the prebuilt binaries on [this download page][mvdsv-builds].

## Prerequisites

None at the moment.

## Installing

For more detailed information we suggest looking at the [nQuake server][nquake-linux], which uses **[MVDSV][mvdsv]** and **[KTX][ktx]** as **QuakeWorld** server.

## Building binaries

### Build from source with CMake

Assuming you have installed essential build tools and ``CMake``
```bash
mkdir build && cmake -B build . && cmake --build build
```
Build artifacts would be inside ``build/`` directory, for unix like systems it would be ``mvdsv``.

You can also use ``build_cmake.sh`` script, it mostly suitable for cross compilation
and probably useless for experienced CMake user.
Some examples:
```
./build_cmake.sh linux-amd64
```
should build MVDSV for ``linux-amd64`` platform, release version, check [cross-cmake](tools/cross-cmake) directory for all platforms

```
B=Debug ./build_cmake.sh linux-amd64
```
should build MVDSV for linux-amd64 platform with debug

```
V=1 B=Debug ./build_cmake.sh linux-amd64
```
should build MVDSV for linux-amd64 platform with debug, verbose (useful if you need validate compiler flags)

```
G="Unix Makefiles" ./build_cmake.sh linux-amd64
```

force CMake generator to be unix makefiles

```
./build_cmake.sh linux-amd64
```

build MVDSV for ``linux-amd64`` version, you can provide
any platform combinations.

The WebRTC transport (see below) is built by default, with [libdatachannel][libdatachannel]
and [Mbed TLS][mbedtls]. Configuring downloads them and builds them with the server, which
takes ``git``, network access and a C++17 compiler. libdatachannel is a commit newer than
its last release, with a patch of ours (``cmake/patches``) that lets its C API take the CA
certificates to check a ``wss://`` server with. ``-DWEBRTC=OFF`` builds the server without
WebRTC.

## WebRTC clients

Clients in browsers and the FTE and SoftWorld clients can play over WebRTC data channels.
The server registers at a broker speaking FTE's WebRTC broker protocol, FTE's master by
default, and clients connect to it through the broker:

```
set sv_webrtc 1
set sv_webrtc_room myserver
```

The console then says where the server is hosted, for example
``WebRTC: hosting at rtcs://master.frag-net.com/myserver``, and clients ``connect`` to that
address. With an empty ``sv_webrtc_room`` the broker gives the server a number instead.

| Cvar | Default | Meaning |
|------|---------|---------|
| ``sv_webrtc`` | ``0`` | ``1`` hosts at the broker. It can be changed at any time. |
| ``sv_webrtc_broker`` | ``wss://master.frag-net.com:27950`` | The broker: ``ws://`` or ``wss://`` (FTE's ``rtc://``, ``rtcs://``, ``tcp://`` and ``tls://`` also work), with a host name or an IPv4 address. |
| ``sv_webrtc_broker_ca`` | ``""`` | A PEM file of the CA certificates to check a ``wss://`` broker's certificate with, in the base directory; empty for the system's. |
| ``sv_webrtc_broker_insecure`` | ``0`` | ``1`` skips checking a ``wss://`` broker's certificate, for a test broker with a self-signed one. |
| ``sv_webrtc_room`` | empty | The room at the broker; empty lets the broker choose a number, which changes each time the server reconnects to it. |
| ``sv_webrtc_port`` | ``0`` | The UDP port all WebRTC clients use; ``0`` lets the system choose. Set and open it behind a firewall. |
| ``sv_webrtc_stun`` | empty | A STUN server (``host:port``, a host name or an IPv4 address) to learn the server's public address from, instead of the broker. |
| ``sv_webrtc_cert``, ``sv_webrtc_key`` | ``cert.pem``, ``privkey.pem`` | The DTLS certificate and key, in the base directory. |
| ``sv_webrtc_mtu`` | ``0`` | The data channels' MTU; ``0`` keeps libdatachannel's 1280. |
| ``sv_webrtc_debug`` | ``0`` | ``1`` adds each connection's steps, ``2`` also the offers and answers, ``3`` and ``4`` more of libdatachannel's log. |

``webrtc_status`` shows the broker, the certificate's fingerprint and each WebRTC client.

The first time WebRTC starts, the server makes a self-signed certificate and key
(``cert.pem`` and ``privkey.pem`` in the base directory), so its fingerprint stays the same
from one start to the next. It never overwrites them: if only one of the two exists, or they
don't match, WebRTC stays off and the console says why. Remove both to have a new pair made.

WebRTC clients have their real IP address, so bans, ``status`` and logs work as for others.
A client that can only be reached through a TURN relay has the relay's address instead, so
banning it bans every client coming through that relay; the console says when a client is
relayed.

WebRTC uses the address families the server does, IPv4 and IPv6 by default, bound as ``-ip``
and ``-ip6`` bind the server. libdatachannel binds one address, so with both ``-ip`` and
``-ip6`` WebRTC is IPv4 only, and the console says so.

A WebRTC connection lasts as long as the game client it carries. One that doesn't join the
game within 10 seconds, or stays 10 seconds after its player has left, is closed. At most
8 connect at a time, the oldest of them making way for a new one, so that a flood of them
through the broker can't keep players out; and at most 4 from one host, an IPv4 address or
an IPv6 /64, may be joining at once.

The broker passes on the fingerprints the clients and the server check each other's
certificates with, so whoever could stand between the server and its broker could swap
them and read and change the WebRTC clients' traffic. Over ``wss://`` the server checks the
broker's certificate to stop that, against the CA certificates the system trusts: on
Windows those in its certificate store, elsewhere the file of them that OpenSSL reads (such
as ``/etc/ssl/certs/ca-certificates.crt``, or the one ``SSL_CERT_FILE`` names). Where there
is none, as in a container without the ``ca-certificates`` package, the server does not
connect to the broker until ``sv_webrtc_broker_ca`` names a file of them.

Older FTE clients may offer plain ICE without DTLS, which the server refuses
(``sv_webrtc_debug 2`` shows an offer without ``a=fingerprint``); ``net_ice_usewebrtc 1`` on
such a client fixes it.

## Versioning

For the versions available, see the [tags on this repository][mvdsv-tags].

## Authors

(Listed by last name alphabetic order)

* **Ivan Bolsunov** - *qqshka*
* **Dominic Evans** - *oldman*
* **Anton Gavrilov** - *tonik*
* **Dmitry Musatov** - *disconnect*
* **Peter Nicol** - *meag*
* **Alexandre Nizoux** - *deurk*
* **Tero Parkkonen** - *Renzo*
* **Vladimir Vladimirovich** - *VVD*

Names of those contributors have been lost but they have also helped with this project: *bliP*, *danfe*, *hdworak*, *HighlandeR*, *jhodge*, *kreon*, *SD-Angel*.

## Code of Conduct

We try to stick to our code of conduct when it comes to interaction around this project. See the [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) file for details.

## License

This project is licensed under the GPL-2.0 License - see the [LICENSE.md](LICENSE.md) file for details.

## Acknowledgments

* Thanks to **Jon "bps" Cednert** for the **[MVDSV][mvdsv]** logo.
* Thanks to the fine folks on [Quakeworld Discord][discord-qw] for their support and ideas.

[mvdsv]: https://github.com/QW-Group/mvdsv
[mvdsv-tags]: https://github.com/QW-Group/mvdsv/tags
[mvdsv-builds]: https://builds.quakeworld.nu/mvdsv
[ktx]: https://github.com/QW-Group/ktx
[nquake-linux]: https://github.com/nQuake/server-linux
[discord-qw]: http://discord.quake.world/
[mbedtls]: https://github.com/Mbed-TLS/mbedtls
[libdatachannel]: https://github.com/paullouisageneau/libdatachannel
