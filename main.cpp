/***************************************************************************
 * Daemon for Fortron FSP EP 650					                       *
 *					                                                       *
 * Written by K3A - www.k3a.me					                           *
 * "Do what you want with it but mention original author" software license *
 ***************************************************************************/

// Tested on FSP EP 650 but may work for other models too
// Other models can have additional commands or more items in the responses.
//
// VendorID/ProductID is hardcodded
//
// My battery was purchased 8.8.2013

#include <libusb.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <microhttpd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h> 

using namespace std;

static libusb_context *ctx = NULL;
static bool io_problem = false, prev_io_problem = false;
static unsigned io_problem_ts = 0xffffffff;

void xfprintf( FILE* fp, const char* format, ... ) {
 	time_t rawtime;
	struct tm * timeinfo;
	char buffer [80];
	time (&rawtime);
	timeinfo = localtime (&rawtime);
	strftime (buffer,80,"%Y-%m-%d %H:%M:%S",timeinfo);

	char* fmt = (char*)malloc(strlen(format)+50);
	sprintf(fmt, "%s: %s", buffer, format);

    va_list args;
    va_start( args, format );
    vfprintf( fp, fmt, args );
    va_end( args );

	free(fmt);
}


static libusb_device_handle * ups_init()
{
	int r;

	if (!ctx)
	{
			r = libusb_init(&ctx);
			if(r < 0)
			{
				xfprintf(stderr, "Error: USB Init Error %d\n", r);
				return NULL;
			}

			libusb_set_debug(ctx, 3);
	}

	libusb_device_handle *dev = libusb_open_device_with_vid_pid(ctx, 1637, 20833); // Cypress
	if (!dev)
	{
		xfprintf(stderr, "Error: Can't open Cypress USB device 0665:5161!\n");
		return NULL;
	}

	if(libusb_kernel_driver_active(dev, 0) == 1)
	{
		fprintf(stdout, "Notice: Kernel driver is active\n");
		if(libusb_detach_kernel_driver(dev, 0) == 0)
			fprintf(stdout, "Notice: Kernel driver detached!\n");
	}

	r = libusb_claim_interface(dev, 0); // claim interface 0
	if(r < 0)
	{
		xfprintf(stderr, "Error: Cannot claim interface 0\n");
		return NULL;
	}

	prev_io_problem = io_problem = false;
	io_problem_ts = 0xffffffff;

	return dev;
}

static void ups_release(libusb_device_handle *dev)
{
	int r = libusb_release_interface(dev, 0); //release the claimed interface
	if(r != 0)
		xfprintf(stderr, "Error: Cannot release interface\n");

	libusb_close(dev); // close the device
	libusb_exit(ctx); // release libusb

	ctx = NULL;
}

static bool ups_write(libusb_device_handle *dev, const char* buff)
{
	int wrote = libusb_control_transfer(dev, 0x21, 0x9, 0x200, 0, (unsigned char*)buff, strlen(buff), 0);
	if (wrote != strlen(buff))
	{
		xfprintf(stderr, "Error while writing to USB device! Wrote %d bytes!\n", wrote);
		io_problem = true;
		if (io_problem_ts == 0xffffffff) io_problem_ts = time(NULL);
		return false;
	}
	return true;
}

// may return NULL
static char* ups_read(libusb_device_handle *dev)
{
	static char buff[2048];
	int read = 0;

	memset(buff, 0, sizeof(buff));

	int r = libusb_interrupt_transfer(dev, 0x81, (unsigned char*)buff, sizeof(buff), &read, 1000);
	if (r != 0 && r != -7 /*timeout*/)
	{
		xfprintf(stderr, "Error while reading from USB device: %d\n", r);
		io_problem = true;
		if (io_problem_ts == 0xffffffff) io_problem_ts = time(NULL);
	}

	buff[read] = 0;
	return buff;
}

static int upsShuttingDown = 0;
static void ups_shutdown(libusb_device_handle *dev)
{
	if (upsShuttingDown) return;

	char cmd[32];
	sprintf(cmd, "S01R0001\r"); // S delayShutdownInMinutesXX (.2)  R  onTimeWaitInMinutes XXXX (0001)
	ups_write(dev, cmd);
	upsShuttingDown = 1;
}

static void ups_cancel_shutdown(libusb_device_handle *dev)
{
	if (!upsShuttingDown) return;

	ups_write(dev, "C\r");
	upsShuttingDown = 0;
}

static void EmailAlert(const char* status, float voltage)
{
	static const char* TO = "your-email@example.com";
	char cmd[1024];

	//sprintf(cmd, "echo -e \"To: <%s>\\r\\nSubject: UPS Alert\\r\\n\\r\\n%s\\r\\nCurrent input voltage: %.1f\\r\\n.\\r\\n\" | sendmail \"%s\"", TO, status, voltage, TO);
	//system(cmd);
}

#define CMD_PING	"QPI\r"	// response: QPI\r
#define CMD_MODEL	"M\r"	// response: V\n in my case
#define CMD_STATUS	"QS\r"	// response: (232.8 232.8 230.7 006 50.0 13.7 --.- 00001000\r : inputVoltage inputVoltage outputVoltage loadLevel outputFrequency batteryVoltage unknown flagsa(see status chacters)
#define CMD_INFO	"F\r"	// response: #220.0 002 12.00 50.0\r : ratedInputOutput upsOrBatteryWarrany ratedBattery ratedOutputFrequency
#define CMD_TEST	"T\r"	// 10s self-test; no response
#define CMD_ALARM	"Q\r"	// alarm toggle; no response

/*
STATUS CHARACTERS
INDEX 0		   1			   2           3               4             5          6            7
VALUE 0		   0			   0           0               1             0          0            0
DESCR workOnBat   lowBatt				   ups fault   1=line-interact   test mode	            buzzer
						                                0=on-line
*/

static int shouldQuit = 0;
static int prevInputStatus = 1; // input status: 1-live, 0-disconnected
static struct {
	float	inputVoltage;
	float	outputVoltage;
	float	load;
	float	freq;
	float	battVoltage;
	float	battPerc;
	char	status[128];
} s_status;

static void  INThandler(int sig)
{
	char  c;
	signal(sig, SIG_IGN);
	shouldQuit = 1;
}

static void ShutdownHost() {
    xfprintf(stderr, "Battery capacity %.1f %%. Shutting the host down...\n", s_status.battPerc);
    fflush(stderr);
    EmailAlert("Shutting the host down", s_status.inputVoltage);
    sleep(1);
    system("/sbin/shutdown -h now");
}

// http server handler
static int http_handler(void * cls, struct MHD_Connection * connection, const char * url, const char * method, 
			const char * version, const char * upload_data, size_t * upload_data_size, void ** ptr) 
{
	static int dummy;
	struct MHD_Response * response;
	int ret;

	if (0 != strcmp(method, "GET"))
		return MHD_NO; // unexpected method 

	if (&dummy != *ptr) 
	{
		// http_handler is called twice for each request
		// skip the first call (as it contains headers only)
		*ptr = &dummy;
		return MHD_YES;
	}

	if (0 != *upload_data_size)
		return MHD_NO; // upload data in a GET!?
	
	*ptr = NULL; // clear context pointer

	char buf[4096];
	sprintf(buf,"<html><head><title>UPS Control</title><meta http-equiv=\"refresh\" content=\"0\"></head>"
				"<style type=\"text/css\">"
				"table {"
				"	font-family: verdana,arial,sans-serif;"
				"	font-size:11px;"
				"	color:#333333;"
				"	border-width: 1px;"
				"	border-color: #666666;"
				"	border-collapse: collapse;"
				"}"
				"table th {"
				"	border-width: 1px;"
				"	padding: 8px;"
				"	border-style: solid;"
				"	border-color: #666666;"
				"	background-color: #dedede;"
				"	text-align: right;;"
				"}"
				"table td {"
				"	border-width: 1px;"
				"	padding: 8px;"
				"	border-style: solid;"
				"	border-color: #666666;"
				"	background-color: #ffffff;"
				"}"
				"</style>"
				"<body><img style=\"float:left; width:230px; height:230px\" src=\"http://your.server.com/img_ups.jpg\"><table>"
				"<tr><th>Status</th><td><b>%s</b></td></tr>"
				"<tr><th>Input</th><td>%.1f V</td></tr>"
				"<tr><th>Output</th><td>%.1f V</td></tr>"
				"<tr><th>Frequency</th><td>%.0f Hz</td></tr>"
				"<tr><th>Load</th><td>%.0f %%</td></tr>"
				"<tr><th>Batt. Voltage</th><td>%.1f V</td></tr>"
				"<tr><th>Batt. Capacity</th><td>%.1f %%</td></tr>"
				"</table></body></html>",
				s_status.status, s_status.inputVoltage, s_status.outputVoltage, s_status.freq, s_status.load,
				s_status.battVoltage, s_status.battPerc );

	response = MHD_create_response_from_data(strlen(buf),
				   (void*)buf, MHD_NO, MHD_NO);

	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);

	MHD_destroy_response(response);

	return ret;
}

// MAIN ------------------------------------------------
int main() 
{
	signal(SIGINT, INThandler);
	fprintf(stdout, "USB UPS Daemon starting up...");
	xfprintf(stderr, "Starting up...\n");
	fflush(stdout);

	// reset status
	memset(&s_status, 0, sizeof(s_status));

	// start http server
    sockaddr_in localhost;
    memset(&localhost, 0, sizeof(localhost));
    localhost.sin_family = AF_INET;
    localhost.sin_addr.s_addr = inet_addr("127.0.0.1");
    localhost.sin_port = htons(2857);

	struct MHD_Daemon *  d = MHD_start_daemon (MHD_USE_DEBUG, 2857, NULL, (void*)NULL, &http_handler, 
									NULL, MHD_OPTION_SOCK_ADDR, &localhost, MHD_OPTION_END);

	// init USB connection
	libusb_device_handle *dev = ups_init();
	if (!dev) return 1;
	
	// check UPS model
	ups_write(dev, CMD_MODEL);
	char* model = ups_read(dev);
	if (!model || *model != 'V')
		fprintf(stdout, "Warning: Unsupported UPS model!\n");

	while(!shouldQuit)
	{
		if (!prev_io_problem && io_problem) 
			EmailAlert("Problem with USB comm", 0);
		prev_io_problem = io_problem;

		if (time(NULL)-io_problem_ts > 15*60) 
			ShutdownHost();

		ups_write(dev, CMD_STATUS);
		char* devstatus = ups_read(dev);
		if (!devstatus || !*devstatus)
		{
			xfprintf(stderr, "Error: No response from the device! Trying to reconnect...\n");
			ups_release(dev);
again:
			libusb_device_handle *dev = ups_init();
			if (!dev) 
			{
				sleep(3);
				goto again;
			}
			sleep(5);
			continue;
		}
		if (*devstatus != '(')
		{
			xfprintf(stderr, "Error: Garbage received. This may be due to an other app connecting to the UPS! '%s'\n", devstatus);
			sleep(1);
			continue;
		}

		printf("\033[0G\033[J"); // clear line

		char* tok = strtok(devstatus, " (\t\r");

		tok = strtok(NULL, " (\t\r");
		s_status.inputVoltage = atof(tok);
		printf("Input: %-8.1f", s_status.inputVoltage);

		tok = strtok(NULL, " (\t\r");
		s_status.outputVoltage = atof(tok);
		printf("Output: %-8.1f", s_status.outputVoltage);

		tok = strtok(NULL, " (\t\r");
		s_status.load = atof(tok);
		printf("Load: %-8.0f", s_status.load);

		tok = strtok(NULL, " (\t\r");
		s_status.freq = atof(tok);
		printf("Output Freq: %-8.0f", s_status.freq);

		tok = strtok(NULL, " (\t\r");
		s_status.battVoltage = atof(tok);
		printf("Batt Voltage: %-8.1f", s_status.battVoltage);

		//s_status.battPerc = (int)(90.0 * (s_status.battVoltage - 11.6) / 1.7);
		s_status.battPerc = (int)(90.91 * (s_status.battVoltage - 11.4)); // MAX 12.5 V
		if (s_status.battPerc < 0)
			s_status.battPerc = 0;
		else if (s_status.battPerc > 100)
			s_status.battPerc = 100;
		printf("Batt Capacity: %-8.0f", s_status.battPerc);

		tok = strtok(NULL, " (\t\r"); // skip unknown field

		char* flags = strtok(NULL, " (\t\r");
	
		// Mode from status
		const char* strStatus = "unknown";
		int battMode = 0;
		if (s_status.outputVoltage < 20 && flags[3] == '0')
			strStatus = "standby";
		else if (flags[0] == '0' && flags[5] == '0')
			strStatus = "line mode";
		else if (flags[0] == '0' && flags[5] == '1')
			strStatus = "battery test";
		else if (flags[0] == '1')
		{
			strStatus = "battery mode";
			battMode = 1;
		}

		// Fault mode??
		if (flags[3] == '1') 
		{
			strStatus = "fault mode";
			
			static int faultReported = 0;
			if(!faultReported) 
			{
				EmailAlert("UPS reports fault mode!", s_status.inputVoltage);
				faultReported = 1;
			}
		}

		strcpy(s_status.status, strStatus);

		printf("Status: %-8s", strStatus);
		if (flags[7] == '1')
		{
			printf(" (buzzer on - disabling)");
			ups_write(dev, CMD_ALARM);
		}

		fflush(stdout);

		MHD_run(d);

		if (battMode && (s_status.battPerc < 20 || flags[1] == '1'/*lowBatt*/))
		{
			ShutdownHost();
			//ups_shutdown(dev); - UNTESTED!!!!
		}

		int inpStatus = (s_status.inputVoltage > 205 && !battMode) ? 1 : 0;
		if (inpStatus) ups_cancel_shutdown(dev);

		if (prevInputStatus == 1 && inpStatus == 0)
			EmailAlert("Input disconnected", s_status.inputVoltage);
		else if (prevInputStatus == 0 && inpStatus == 1)
			EmailAlert("Input connected again", s_status.inputVoltage);

		prevInputStatus = inpStatus;

		// sleep but respond to http
		int waitSteps = 4;
		for(int s = 0; s<waitSteps; s++)
		{
			MHD_run(d);
			usleep(1000000.0/waitSteps);
		}
	}

	xfprintf(stderr, "Exiting...\n\n");

	MHD_stop_daemon(d);
	
	ups_release(dev);
	return 0;
}

