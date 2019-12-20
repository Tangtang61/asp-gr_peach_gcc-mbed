#include <kernel.h>
#include <t_syslog.h>
#include <t_stdlib.h>
#include "syssvc/serial.h"
#include "syssvc/syslog.h"
#include "kernel_cfg.h"
#include "sil.h"
#include "scif.h"

#include "mbed.h"
#include "EthernetInterface.h"
#include "syssvc/logtask.h"

#include "RomRamFileSystem.h"

#include    <wolfssl/ssl.h>          /* wolfSSL security library */
#include    <wolfssl/wolfcrypt/error-crypt.h>
#include    <wolfssl/wolfcrypt/logging.h>
#include    <user_settings.h>
#include    <sslClient.h>

/*
 *  サービスコールのエラーのログ出力
 */
Inline void
svc_perror(const char *file, int_t line, const char *expr, ER ercd)
{
	if (ercd < 0) {
		t_perror(LOG_ERROR, file, line, expr, ercd);
	}
}

#define	SVC_PERROR(expr)	svc_perror(__FILE__, __LINE__, #expr, (expr))

/**** User Selection *********/
/** Network setting **/
#define USE_DHCP               (1)                 /* Select  0(static configuration) or 1(use DHCP) */
#if (USE_DHCP == 0)
  #define IP_ADDRESS           ("192.168.0.2")     /* IP address      */
  #define SUBNET_MASK          ("255.255.255.0")   /* Subnet mask     */
  #define DEFAULT_GATEWAY      ("192.168.0.3")     /* Default gateway */
#endif

#define NETWORK_TYPE			(0)					/* Select  0(EthernetInterface) or 1(GR_PEACH_WlanBP3595) */
#if (NETWORK_TYPE == 1)
	#define WLAN_SSID			("ssid")	/* SSID */
	#define WLAN_PSK			("pass")		/* PSK(Pre-Shared Key) */

	#define WLAN_SECURITY		NSAPI_SECURITY_WPA2	/* NSAPI_SECURITY_NONE, NSAPI_SECURITY_WEP, NSAPI_SECURITY_WPA or NSAPI_SECURITY_WPA2 */
#endif

#if (NETWORK_TYPE == 0)
  #include "EthernetInterface.h"
  EthernetInterface network;
#elif (NETWORK_TYPE == 1)
  #include "GR_PEACH_WlanBP3595.h"
  GR_PEACH_WlanBP3595 network;
  DigitalOut usb1en(P3_8);
#else
  #error NETWORK_TYPE error
#endif /* NETWORK_TYPE */

static int SocketReceive(WOLFSSL* ssl, char *buf, int sz, void *sock)
{
    return ((TCPSocketConnection *)sock)->receive(buf, sz) ;
}

static int SocketSend(WOLFSSL* ssl, char *buf, int sz, void *sock)
{
    return ((TCPSocketConnection *)sock)->send(buf, sz);
}

//#define SERVER "www.wolfssl.com"
//#define HTTP_REQ "GET /wolfSSL/Home.html HTTP/1.0\r\nhost: www.wolfssl.com\r\n\r\n"
//#define SERVER "192.168.0.3"
//#define HTTP_REQ "GET /iisstart.htm HTTP/1.0\r\nhost: 192.168.0.3\r\n\r\n"
#define SERVER "os.mbed.com"
#define HTTP_REQ "GET /media/uploads/mbed_official/hello.txt HTTP/1.0\r\nhost: os.mbed.com\r\n\r\n"

#define HTTPS_PORT 443

/*
 *  clients initial contact with server. Socket to connect to: sock
 */
int ClientGreet(WOLFSSL *ssl)
{
   	#define MAXDATASIZE (1024*4)
    char       rcvBuff[MAXDATASIZE] = {0};
    int        ret ;

    if (wolfSSL_write(ssl, HTTP_REQ, strlen(HTTP_REQ)) < 0) {
        /* the message is not able to send, or error trying */
        ret = wolfSSL_get_error(ssl, 0);
        syslog(LOG_EMERG, "Write error[%d]:%s", ret, wc_GetErrorString(ret));
        return EXIT_FAILURE;
    }

    syslog(LOG_NOTICE, "Received:");
    while ((ret = wolfSSL_read(ssl, rcvBuff, sizeof(rcvBuff)-1)) > 0) {
        rcvBuff[ret] = '\0';
        syslog(LOG_NOTICE, "%s", rcvBuff);
    }
    return ret;
}

/*
 * applies TLS 1.2 security layer to data being sent.
 */
#define STATIC_BUFFER
#ifdef  STATIC_BUFFER
static byte memory[1024*256];
static byte memoryIO[1024*72];
#endif

int Security(TCPSocketConnection *socket)
{
    WOLFSSL_CTX* ctx = 0;
    WOLFSSL*     ssl;    /* create WOLFSSL object */
    int          ret = 0;

#ifdef  STATIC_BUFFER
    syslog(LOG_DEBUG, "wolfSSL_CTX_load_static_memory");
    /* set up static memory */
    if (wolfSSL_CTX_load_static_memory(&ctx, wolfTLSv1_2_client_method_ex, memory, sizeof(memory),0,1)
            != SSL_SUCCESS){
        syslog(LOG_EMERG, "unable to load static memory and create ctx");
        return  EXIT_FAILURE;
    }

    /* load in a buffer for IO */
    syslog(LOG_DEBUG, "wolfSSL_CTX_load_static_memory");
    if (wolfSSL_CTX_load_static_memory(&ctx, NULL, memoryIO, sizeof(memoryIO),
                                 WOLFMEM_IO_POOL_FIXED | WOLFMEM_TRACK_STATS, 1)
            != SSL_SUCCESS){
        syslog(LOG_EMERG, "unable to load static memory and create ctx");
	    return  EXIT_FAILURE;
    }
#else
    if ((ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method())) == NULL) {
        syslog(LOG_EMERG, "wolfSSL_new error.");
        return EXIT_FAILURE;
    }
#endif

    wolfSSL_SetIORecv(ctx, SocketReceive) ;
    wolfSSL_SetIOSend(ctx, SocketSend) ;
    wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, 0);

    if ((ssl = wolfSSL_new(ctx)) == NULL) {
        syslog(LOG_EMERG, "wolfSSL_new error.");
        return EXIT_FAILURE;
    }

    wolfSSL_SetIOReadCtx(ssl, (void *)socket) ;
    wolfSSL_SetIOWriteCtx(ssl, (void *)socket) ;

    ret = wolfSSL_connect(ssl);
    if (ret == SSL_SUCCESS) {
        syslog(LOG_NOTICE, "TLS Connected") ;
        ret = ClientGreet(ssl);
    } else {
        ret = wolfSSL_get_error(ssl, 0);
        syslog(LOG_EMERG, "TLS Connect error[%d], %s", ret, wc_GetErrorString(ret));
        return EXIT_FAILURE;
    }
    /* frees all data before client termination */
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();

    return ret;
}

void
sslClient_main(intptr_t exinf) {
    EthernetInterface network;
    TCPSocketConnection socket;

	/* syslogの設定 */
    SVC_PERROR(syslog_msk_log(LOG_UPTO(LOG_INFO), LOG_UPTO(LOG_EMERG)));

    syslog(LOG_NOTICE, "sslClient:");
    syslog(LOG_NOTICE, "Sample program starts (exinf = %d).", (int_t) exinf);
    syslog(LOG_NOTICE, "LOG_NOTICE: Network Setting up...");

#if (USE_DHCP == 1)
    if (network.init() != 0) {                             //for DHCP Server
#else
    if (network.init(IP_ADDRESS, SUBNET_MASK, DEFAULT_GATEWAY) != 0) { //for Static IP Address (IPAddress, NetMasks, Gateway)
#endif
		syslog(LOG_NOTICE, "Network Initialize Error");
        return;
    }
    syslog(LOG_NOTICE, "Network was initialized successfully");
    while (network.connect(5000) != 0) {
        syslog(LOG_NOTICE, "LOG_NOTICE: Network Connect Error");
    }

	dly_tsk(3000);
	
    syslog(LOG_NOTICE, "MAC Address is %s", network.getMACAddress());
    syslog(LOG_NOTICE, "IP Address is %s", network.getIPAddress());
    syslog(LOG_NOTICE, "NetMask is %s", network.getNetworkMask());
    syslog(LOG_NOTICE, "Gateway Address is %s", network.getGateway());
    syslog(LOG_NOTICE, "Network Setup OK...");

    while (socket.connect(SERVER, HTTPS_PORT) < 0) {
        syslog(LOG_EMERG, "Unable to connect to (%s) on port (%d)", SERVER, HTTPS_PORT);
		dly_tsk(1000);
    }
    Security(&socket);
    socket.close();
    syslog(LOG_NOTICE, "program end");	
}

// set mac address
void mbed_mac_address(char *mac) {
	// PEACH1
    mac[0] = 0x00;
    mac[1] = 0x02;
    mac[2] = 0xF7;
    mac[3] = 0xF0;
    mac[4] = 0x00;
    mac[5] = 0x00;
}

 /*
 *  周期ハンドラ
 *
 *  HIGH_PRIORITY，MID_PRIORITY，LOW_PRIORITY の各優先度のレディキュー
 *  を回転させる．
 */
bool_t led_state = true;
void cyclic_handler(intptr_t exinf)
{
	if (led_state == true) {
		led_state = false;
	} else {
		led_state = true;
	}
	set_led(BLUE_LED, led_state);
}
