

usbups: main.cpp
	g++ -I/usr/include/libusb-1.0/ -lusb-1.0 -lmicrohttpd -o $@ $<
