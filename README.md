bmstpd: Bridge Multiple Spanning Tree Protocol Daemon
=============================================

BMSTPD is an open source user-space daemon licensed under GPLv2, it is
a fork of MSTPD extended for:
- Linux Bridge per VLAN STP states
- Linux Bridge MSTIs
- Support for VLAN STP states on switchdev devices <sup>(currently only on Mellanox Spectrum based switches through included kernel patches)</sup>
- Support for MSTIs on DSA devices <sup>(depends on kernel version 5.18+ and support in DSA kernel driver)</sup>
- Permanent file tree based configuration

BMSTPD is reported to be compliant with IXIA ANVL RSTP test suite, with
the notable exception of looped-back BPDUs (see discussion on the matter
on the Implementation Features wiki page:
https://github.com/dtitech/bmstpd/wiki/ImplementationFeatures).

**Important note!** MSTP part of the code is currently in experimental phase,
so it should be mostly stable, but it can behave unexpectedly in corner cases.
Be warry if using.

Official repository: https://github.com/dtitech/bmstpd

Implementation Features
-----------------------

See the wiki page: https://github.com/dtitech/bmstpd/wiki/ImplementationFeatures

Also BMSTPD includes a number of useful features which are not defined in
802.1Q-2005 standard, but are found on many commercial switches. Namely:

  - BPDU Guard. Added by Satish Ashok.

  - Bridge Assurance. Added by Satish Ashok.
  (WARNING: it might be dropped in the future and replaced by the
  AutoIsolate feature of 802.1Q-2011)

  - Enhanced statistics: TX/RX BPDU/TCN counters, Forwarding/Blocking
  transitions counters, last 2 ports which caused topology change. Added
  by Satish Ashok.

Goals of the project
--------------------

 - Allow for RSTP and MSTP support on Linux based machines and switches

 - Create reliable and well-tested MSTP code.

 - Get some user base - the more users of this code, the more test
 coverage is.

Current state
-------------

The daemon depends on the bridge Linux kernel code to gather basic info
about bridge, such as bridge state (up/down, STP on/off), slave
interfaces for the bridge and their state (up/down). Also daemon
translates CIST states to the kernel bridge slaves, so bmstpd in (r)stp
mode can be used as replacement for the current rstpd (and even for the
in-kernel stp!).

MSTP standard (802.1Q-2005) requires from the bridge to be heavily
interconnected with the VLANs infrastructure, namely:

  - Support several (2 .. 4094) independent FIDs (Forwarding Information
  Databases). Each VLAN belongs to the one of FIDs, and learning of the
  MAC addresses is done independently in the each FID (independent
  learning as opposed to shared learning in the current Linux bridges);

  - Support several (2 .. 64) Multiple Spanning Tree Instances in
  addition to the CIST. Each FID belongs to the one of MSTIs or CIST;

  - Support per-MSTI port states (Discarding / Learning / Forwarding) so
  that each bridge port can have different states for different MSTIs.

Linux bridging supports either IVL or SVL, but does not support dynamic
allocation of VIDs to FIDs.

This is controlled via the VLAN filtering attribute:

  - When VLAN filtering is enabled, the bridge supports 4094 VLANs with
  IVL, and requires configuration of all VLANs permitted;
  - When VLAN filtering is disabled, the bridge is not VLAN aware and only
  learns via the MAC address (into a separate "untagged" FID), so it does
  SVL.

Additionally Linux bridging gained support for MSTIs in version 5.18:

  - VLANs configured on the bridge can be assigned to MSTIs;
  - Once a VLANs is assigned to a MSTI, the port states of ports that are
  members of that VLAN can be configured

BMSTPD currently supports Linux bridge MSTIs when bridge is configured
with MST enabled, this is supported mainly on software bridges, but there
are some DSA drivers which have MST support.

With bridge MST disabled, daemon will use per VLAN stp states if they are
supported in running kernel, this mode is usable on software bridge and
on Mellanox Spectrum based platforms with included kernel patches.

> [!WARNING]
> Linux bridges with MST enabled are mostly untested.

> [!NOTE]
> Running with per VLAN stp states in software, or on Mellanox Spectrum
> based platforms with included kernel patches is tested, and used in
> production environment.

ACKNOWLEDGEMENTS
----------------

Initial code was written by looking at (and shamelessly stealing some
parts from):

  - [rstpd](https://github.com/shemminger/RSTP) by Srinivas Aji `<Aji_Srinivas
    ? emc DOT com>` and Stephen Hemminger `<stephen ? networkplumber DOT org> ,
    used as the base of `mstpd`

  - [rstplib](https://sourceforge.net/projects/rstplib/) by Alex Rozin `<alexr
    ? nbase DOT co DOT il>` and Michael Rozhavsky `<mike ? nbase DOT co DOT
    il>`, as inspiration for `mstp.{c,h}`
