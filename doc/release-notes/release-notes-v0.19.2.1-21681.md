v0.19.2.1-21681 Release Notes
=============================

Bitcoin Core version v0.19.2.1-21681 is now available from:

  https://github.com/tradecraftio/mergemine/releases/tag/v0.19.2.1-21681

This is the first release of the v0.19 stable branch of Bitcoin Core with the stratum mining server and Tradecraft/Freicoin merge-mining patches applied.

Please report bugs related to the stratum mining server implementation or Tradecraft/Freicoin merge-mining at the Tradecraft issue tracker on GitHub:

  https://github.com/tradecraftio/mergemine/issues

Please report other bugs using Bitcoin the issue tracker at GitHub:

  https://github.com/bitcoin/bitcoin/issues

To receive security and update notifications for the stratum mining server and
merge-mining patches, please subscribe to:

  https://tradecraft.groups.io/g/announce/

To receive security notifications for Bitcoin Core, please subscribe to:

  https://bitcoincore.org/en/list/announcements/join/

How to Upgrade
--------------

If you are running an older version, shut it down. Wait until it has completely shut down (which might take a few minutes for older versions), then run the installer (on Windows) or just copy over `/Applications/Bitcoin-Qt` (on Mac) or `bitcoind`/`bitcoin-qt` (on Linux).

Upgrading directly from a version of Bitcoin Core that has reached its EOL is possible, but it might take some time if the datadir needs to be migrated.  Old wallet versions of Bitcoin Core are generally supported.

Compatibility
-------------

Bitcoin Core is supported and extensively tested on operating systems using the Linux kernel, macOS 10.10+, and Windows 7 and newer.  It is not recommended to use Bitcoin Core on unsupported systems.

Bitcoin Core should also work on most other Unix-like systems but is not as frequently tested on them.

From Bitcoin Core v0.17 onwards, macOS versions earlier than 10.10 are no longer supported, as Bitcoin Core is now built using Qt 5.9.x which requires macOS 10.10+.  Additionally, Bitcoin Core does not yet change appearance when macOS "dark mode" is activated.

In addition to previously supported CPU platforms, this release's pre-compiled distribution provides binaries for the RISC-V platform.

Notable changes
---------------

There have been no notable changes in the stratum mining server or the Tradecraft/Freicoin merge-mining patches between v0.18.1.1-19960 and this release.  Please see the Bitcoin Core release notes for notable changes between the upstream versions upon which these releases are based.

Credits
-------

Thanks to everyone who directly contributed to this release:

- Mark Friedenbach

As well as to everyone that helped with translations on [Transifex](https://www.transifex.com/tradecraft/freicoin-1/).
