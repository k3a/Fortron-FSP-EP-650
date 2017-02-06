# Fortron-FSP-EP-650

A small daemon for Fortron FSP EP 650 uninterruptible power supply.
It monitors a UPS connected via USB and presents realtime info including battery life via a HTTP server at http://127.0.0.1:2857.
It can also shutdown the host after some time.

Dependencies:
- libusb
- libmicrohttpd

Tested on Linux. 
No technical support of any sort.
I am using it on my home mini server, maybe it will be useful for you too.
Distributed 'as-is'.
