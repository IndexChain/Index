Index
===============

[![latest-release](https://img.shields.io/github/release/IndexChain/Index)](https://github.com/IndexChain/Index/releases)
[![GitHub last-release](https://img.shields.io/github/release-date/IndexChain/Index)](https://github.com/IndexChain/Index/releases)
[![GitHub downloads](https://img.shields.io/github/downloads/IndexChain/Index/total)](https://github.com/IndexChain/Index/releases)
[![GitHub commits-since-last-version](https://img.shields.io/github/commits-since/IndexChain/Index/latest/master)](https://github.com/IndexChain/Index/graphs/commit-activity)
[![GitHub commits-per-month](https://img.shields.io/github/commit-activity/m/IndexChain/Index)](https://github.com/IndexChain/Index/graphs/code-frequency)
[![GitHub last-commit](https://img.shields.io/github/last-commit/IndexChain/Index)](https://github.com/IndexChain/Index/commits/master)
[![Total alerts](https://img.shields.io/lgtm/alerts/g/IndexChain/Index.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/IndexChain/Index/alerts/)
[![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/IndexChain/Index.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/IndexChain/Index/context:cpp)

What is Index?
--------------

[Index](https://indexchain.org) is a privacy focused cryptocurrency that utilizes zero-knowledge proofs which allows users to destroy coins and then redeem them later for brand new ones with no transaction history. Index uses the [Sigma protocol](https://indexchain.org/what-is-sigma-and-why-is-it-replacing-zerocoin-in-index/) for privacy ,which has no trusted setup and small proof sizes. Index also utilises [Dandelion++](https://arxiv.org/abs/1805.11060) to obscure the originating IP of transactions without relying on any external services such as Tor/i2P.


How Index’s Privacy Technology Compares to the Competition [Uses zcoin's comparison image and details]
--------------
![A comparison chart of Index’s solutions with other leading privacy technologies can be found below](https://zcoin.io/wp-content/uploads/2019/04/zcoin_table_coloured5-01.png) 
read more https://indexchain.org/indexs-privacy-technology-compares-competition/

Running with Docker [Not supported yet]
===================

If you are already familiar with Docker, then running Index with Docker might be the the easier method for you. To run Index using this method, first install [Docker](https://store.docker.com/search?type=edition&offering=community). After this you may
continue with the following instructions.

Please note that we currently don't support the GUI when running with Docker. Therefore, you can only use RPC (via HTTP or the `index-cli` utility) to interact with Index via this method.

Pull our latest official Docker image:

```sh
docker pull IndexChain/Indexd
```

Start Index daemon:

```sh
docker run --detach --name indexd IndexChain/Indexd
```

View current block count (this might take a while since the daemon needs to find other nodes and download blocks first):

```sh
docker exec indexd index-cli getblockcount
```

View connected nodes:

```sh
docker exec indexd index-cli getpeerinfo
```

Stop daemon:

```sh
docker stop indexd
```

Backup wallet:

```sh
docker cp indexd:/home/indexd/.index/wallet.dat .
```

Start daemon again:

```sh
docker start indexd
```

Linux Build Instructions and Notes
==================================

Dependencies
----------------------
1.  Update packages

        sudo apt-get update

2.  Install required packages

        sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils libboost-all-dev libzmq3-dev libminizip-dev

3.  Install Berkeley DB 4.8

        sudo apt-get install software-properties-common
        sudo add-apt-repository ppa:bitcoin/bitcoin
        sudo apt-get update
        sudo apt-get install libdb4.8-dev libdb4.8++-dev

4.  Install QT 5

        sudo apt-get install libminiupnpc-dev
        sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler libqrencode-dev

Build
----------------------
1.  Clone the source:

        git clone https://github.com/IndexChain/Index

2.  Build Index-core:

    Configure and build the headless Index binaries as well as the GUI (if Qt is found).

    You can disable the GUI build by passing `--without-gui` to configure.
        
        ./autogen.sh
        ./configure
        make

3.  It is recommended to build and run the unit tests:

        make check


macOS Build Instructions and Notes
=====================================
See (doc/build-macos.md) for instructions on building on macOS.



Windows (64/32 bit) Build Instructions and Notes
=====================================
See (doc/build-windows.md) for instructions on building on Windows 64/32 bit.
