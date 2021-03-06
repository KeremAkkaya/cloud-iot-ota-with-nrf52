/*
 * Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/*

Copyright 2019 Ravikiran Bukkasagara <contact@ravikiranb.com>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
    
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

/*
	Modifications to original code:
	* Changed the way certificates are loaded.
	* Entropy source is on-chip nRF52 RNG IP.
	* Debug prints. 
	* Loading certifcates are optional.
*/

#ifdef __cplusplus
extern "C" {
#endif

#include "network_interface.h"
#include "timer_platform.h"
#include <stdbool.h>
#include <string.h>

#include "aws_iot_error.h"
#include "aws_iot_log.h"
#include "network_interface.h"
#include "network_platform.h"

#include "nrf_drv_rng.h"
#include "rofs.h"

#ifdef MBEDTLS_DEBUG_C
#include "mbedtls/debug.h"
#include "uart_print.h"

#define MBEDTLS_DEBUG_LEVEL_NONE 0
#define MBEDTLS_DEBUG_LEVEL_ERROR 1
#define MBEDTLS_DEBUG_LEVEL_STATE_CHANGE 2
#define MBEDTLS_DEBUG_LEVEL_INFO 3
#define MBEDTLS_DEBUG_LEVEL_VERBOSE 4

#define MBEDTLS_DEBUG_LEVEL MBEDTLS_DEBUG_LEVEL_STATE_CHANGE
#endif

/* This is the value used for ssl read timeout */
#define IOT_SSL_READ_TIMEOUT 10

/* This defines the value of the debug buffer that gets allocated.
 * The value can be altered based on memory constraints
 */
#ifdef ENABLE_IOT_DEBUG
#define MBEDTLS_DEBUG_BUFFER_SIZE 2048
unsigned char debug_buf[MBEDTLS_DEBUG_BUFFER_SIZE];
#endif

#ifdef MBEDTLS_MEMORY_BUFFER_ALLOC_C
static unsigned char mbed_tls_mem_buffer[40 * 1024U];
#endif

static int entropy_rng_src(void* data, unsigned char* output, size_t len, size_t* olen)
{
    (void)data;

    nrf_drv_rng_block_rand(output, len);

    *olen = len;

    return 0;
}

#ifdef MBEDTLS_DEBUG_C
static void send_debug_msg(void* ctx, int level, const char* file, int line, const char* str)
{
    (void)ctx;
    dbg_printf(DEBUG_LEVEL_DEBUG, "mbed: %d: %s\r\n", level, str);
}
#endif

/*
 * This is a function to do further verification if needed on the cert received
 */
static int _iot_tls_verify_cert(void* data, mbedtls_x509_crt* crt, int depth, uint32_t* flags)
{
    char buf[1024];
    ((void)data);

    IOT_DEBUG("\nVerify requested for (Depth %d):\n", depth);
    mbedtls_x509_crt_info(buf, sizeof(buf) - 1, "", crt);
    IOT_DEBUG("%s", buf);

    if ((*flags) == 0) {
        IOT_DEBUG("  This certificate has no flags\n");
    } else {
        IOT_DEBUG(buf, sizeof(buf), "  ! ", *flags);
        IOT_DEBUG("%s\n", buf);
    }

    return 0;
}

void _iot_tls_set_connect_params(Network* pNetwork, const char* pRootCALocation, const char* pDeviceCertLocation,
    const char* pDevicePrivateKeyLocation, const char* pDestinationURL,
    uint16_t destinationPort, uint32_t timeout_ms, bool ServerVerificationFlag)
{
    pNetwork->tlsConnectParams.DestinationPort = destinationPort;
    pNetwork->tlsConnectParams.pDestinationURL = pDestinationURL;
    pNetwork->tlsConnectParams.pDeviceCertLocation = pDeviceCertLocation;
    pNetwork->tlsConnectParams.pDevicePrivateKeyLocation = pDevicePrivateKeyLocation;
    pNetwork->tlsConnectParams.pRootCALocation = pRootCALocation;
    pNetwork->tlsConnectParams.timeout_ms = timeout_ms;
    pNetwork->tlsConnectParams.ServerVerificationFlag = ServerVerificationFlag;
}

IoT_Error_t iot_tls_init(Network* pNetwork, const char* pRootCALocation, const char* pDeviceCertLocation,
    const char* pDevicePrivateKeyLocation, const char* pDestinationURL,
    uint16_t destinationPort, uint32_t timeout_ms, bool ServerVerificationFlag)
{

    _iot_tls_set_connect_params(pNetwork, pRootCALocation, pDeviceCertLocation, pDevicePrivateKeyLocation,
        pDestinationURL, destinationPort, timeout_ms, ServerVerificationFlag);

    pNetwork->connect = iot_tls_connect;
    pNetwork->read = iot_tls_read;
    pNetwork->write = iot_tls_write;
    pNetwork->disconnect = iot_tls_disconnect;
    pNetwork->isConnected = iot_tls_is_connected;
    pNetwork->destroy = iot_tls_destroy;

    pNetwork->tlsDataParams.flags = 0;

#ifdef MBEDTLS_MEMORY_BUFFER_ALLOC_C
    mbedtls_platform_set_fprintf(dbg_fprintf);
    //mbedtls_platform_set_exit(dummy_exit_buffer_alloc);
    mbedtls_memory_buffer_alloc_init(mbed_tls_mem_buffer, sizeof(mbed_tls_mem_buffer));
#endif

    return SUCCESS;
}

IoT_Error_t iot_tls_is_connected(Network* pNetwork)
{
    /* Use this to add implementation which can check for physical layer disconnect */
    return NETWORK_PHYSICAL_LAYER_CONNECTED;
}

IoT_Error_t iot_tls_connect(Network* pNetwork, TLSConnectParams* params)
{
    int ret = 0;
    const char* pers = "aws_iot_tls_wrapper";
    TLSDataParams* tlsDataParams = NULL;
    char portBuffer[6];
    char vrfy_buf[512];
    //const char *alpnProtocols[] = { "x-amzn-mqtt-ca", NULL };
    const unsigned char* cert_filemem;
    const rofs_file_info_t* certinfo;

    if (NULL == pNetwork) {
        return NULL_VALUE_ERROR;
    }

    if (NULL != params) {
        _iot_tls_set_connect_params(pNetwork, params->pRootCALocation, params->pDeviceCertLocation,
            params->pDevicePrivateKeyLocation, params->pDestinationURL,
            params->DestinationPort, params->timeout_ms, params->ServerVerificationFlag);
    }

    tlsDataParams = &(pNetwork->tlsDataParams);

    mbedtls_net_init(&(tlsDataParams->server_fd));
    mbedtls_ssl_init(&(tlsDataParams->ssl));
    mbedtls_ssl_config_init(&(tlsDataParams->conf));
    mbedtls_ctr_drbg_init(&(tlsDataParams->ctr_drbg));
    mbedtls_x509_crt_init(&(tlsDataParams->cacert));
    mbedtls_x509_crt_init(&(tlsDataParams->clicert));
    mbedtls_pk_init(&(tlsDataParams->pkey));

    IOT_DEBUG("\n  . Seeding the random number generator...");
    mbedtls_entropy_init(&(tlsDataParams->entropy));

    //Add custom entropy source.
    ret = mbedtls_entropy_add_source(&(tlsDataParams->entropy),
        entropy_rng_src,
        NULL,
        0,
        MBEDTLS_ENTROPY_SOURCE_STRONG);

    if (ret)
        return NETWORK_MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;

    if ((ret = mbedtls_ctr_drbg_seed(&(tlsDataParams->ctr_drbg), mbedtls_entropy_func, &(tlsDataParams->entropy),
             (const unsigned char*)pers, strlen(pers)))
        != 0) {
        IOT_ERROR(" failed\n  ! mbedtls_ctr_drbg_seed returned -0x%x\n", -ret);
        return NETWORK_MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    }

#ifdef MBEDTLS_DEBUG_C
    mbedtls_ssl_conf_dbg(&(tlsDataParams->conf), send_debug_msg, NULL);
    mbedtls_debug_set_threshold(MBEDTLS_DEBUG_LEVEL);
#endif

    if (pNetwork->tlsConnectParams.pRootCALocation) {
        IOT_DEBUG("  . Loading the CA root certificate ...");
        ret = rofs_readfile(pNetwork->tlsConnectParams.pRootCALocation, &cert_filemem, &certinfo);
        if (ret < 0)
            return NETWORK_X509_ROOT_CRT_PARSE_ERROR;

        ret = mbedtls_x509_crt_parse(&(tlsDataParams->cacert), cert_filemem, certinfo->length + certinfo->null_added);
        if (ret < 0) {
            IOT_ERROR(" failed\n  !  mbedtls_x509_crt_parse returned -0x%x while parsing root cert\n\n", -ret);
            return NETWORK_X509_ROOT_CRT_PARSE_ERROR;
        }

        IOT_DEBUG(" ok (%d skipped)\n", ret);
    } else {
        pNetwork->tlsConnectParams.ServerVerificationFlag = false;
    }

    if (pNetwork->tlsConnectParams.pDeviceCertLocation) {
        IOT_DEBUG("  . Loading the client cert. and key...");

        ret = rofs_readfile(pNetwork->tlsConnectParams.pDeviceCertLocation, &cert_filemem, &certinfo);
        if (ret < 0)
            return NETWORK_X509_DEVICE_CRT_PARSE_ERROR;

        ret = mbedtls_x509_crt_parse(&(tlsDataParams->clicert), cert_filemem, certinfo->length + certinfo->null_added);
        if (ret != 0) {
            IOT_ERROR(" failed\n  !  mbedtls_x509_crt_parse returned -0x%x while parsing device cert\n\n", -ret);
            return NETWORK_X509_DEVICE_CRT_PARSE_ERROR;
        }

        ret = rofs_readfile(pNetwork->tlsConnectParams.pDevicePrivateKeyLocation, &cert_filemem, &certinfo);
        if (ret < 0)
            return NETWORK_PK_PRIVATE_KEY_PARSE_ERROR;

        ret = mbedtls_pk_parse_key(&(tlsDataParams->pkey), cert_filemem, certinfo->length + certinfo->null_added, (const unsigned char*)"", 0);
        if (ret != 0) {
            IOT_ERROR(" failed\n  !  mbedtls_pk_parse_key returned -0x%x while parsing private key\n\n", -ret);
            IOT_DEBUG(" path : %s ", pNetwork->tlsConnectParams.pDevicePrivateKeyLocation);
            return NETWORK_PK_PRIVATE_KEY_PARSE_ERROR;
        }
    }

    IOT_DEBUG(" ok\n");
    snprintf(portBuffer, 6, "%d", pNetwork->tlsConnectParams.DestinationPort);
    IOT_DEBUG("  . Connecting to %s/%s...", pNetwork->tlsConnectParams.pDestinationURL, portBuffer);
    if ((ret = mbedtls_net_connect(&(tlsDataParams->server_fd), pNetwork->tlsConnectParams.pDestinationURL,
             portBuffer, MBEDTLS_NET_PROTO_TCP))
        != 0) {
        IOT_ERROR(" failed\n  ! mbedtls_net_connect returned -0x%x\n\n", -ret);
        switch (ret) {
        case MBEDTLS_ERR_NET_SOCKET_FAILED:
            return NETWORK_ERR_NET_SOCKET_FAILED;
        case MBEDTLS_ERR_NET_UNKNOWN_HOST:
            return NETWORK_ERR_NET_UNKNOWN_HOST;
        case MBEDTLS_ERR_NET_CONNECT_FAILED:
        default:
            return NETWORK_ERR_NET_CONNECT_FAILED;
        };
    }

    ret = mbedtls_net_set_block(&(tlsDataParams->server_fd));
    if (ret != 0) {
        IOT_ERROR(" failed\n  ! net_set_(non)block() returned -0x%x\n\n", -ret);
        return SSL_CONNECTION_ERROR;
    }
    IOT_DEBUG(" ok\n");

    IOT_DEBUG("  . Setting up the SSL/TLS structure...");
    if ((ret = mbedtls_ssl_config_defaults(&(tlsDataParams->conf), MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
             MBEDTLS_SSL_PRESET_DEFAULT))
        != 0) {
        IOT_ERROR(" failed\n  ! mbedtls_ssl_config_defaults returned -0x%x\n\n", -ret);
        return SSL_CONNECTION_ERROR;
    }

    mbedtls_ssl_conf_verify(&(tlsDataParams->conf), _iot_tls_verify_cert, NULL);
    if (pNetwork->tlsConnectParams.ServerVerificationFlag == true) {
        mbedtls_ssl_conf_authmode(&(tlsDataParams->conf), MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&(tlsDataParams->conf), MBEDTLS_SSL_VERIFY_OPTIONAL);
    }
    mbedtls_ssl_conf_rng(&(tlsDataParams->conf), mbedtls_ctr_drbg_random, &(tlsDataParams->ctr_drbg));

    if (pNetwork->tlsConnectParams.pRootCALocation) {
        mbedtls_ssl_conf_ca_chain(&(tlsDataParams->conf), &(tlsDataParams->cacert), NULL);
    }

    if (pNetwork->tlsConnectParams.pDeviceCertLocation) {
        if ((ret = mbedtls_ssl_conf_own_cert(&(tlsDataParams->conf), &(tlsDataParams->clicert), &(tlsDataParams->pkey))) != 0) {
            IOT_ERROR(" failed\n  ! mbedtls_ssl_conf_own_cert returned %d\n\n", ret);
            return SSL_CONNECTION_ERROR;
        }
    }

    mbedtls_ssl_conf_read_timeout(&(tlsDataParams->conf), pNetwork->tlsConnectParams.timeout_ms);

/* Use the AWS IoT ALPN extension for MQTT if port 443 is requested. */
#if 0
	//Enable ALPN Protocols in mbedTLS config.
	if(443 == pNetwork->tlsConnectParams.DestinationPort) {
		if((ret = mbedtls_ssl_conf_alpn_protocols(&(tlsDataParams->conf), alpnProtocols)) != 0) {
			IOT_ERROR(" failed\n  ! mbedtls_ssl_conf_alpn_protocols returned -0x%x\n\n", -ret);
			return SSL_CONNECTION_ERROR;
		}
	}
#endif
    /* Assign the resulting configuration to the SSL context. */
    if ((ret = mbedtls_ssl_setup(&(tlsDataParams->ssl), &(tlsDataParams->conf))) != 0) {
        IOT_ERROR(" failed\n  ! mbedtls_ssl_setup returned -0x%x\n\n", -ret);
        return SSL_CONNECTION_ERROR;
    }
    if ((ret = mbedtls_ssl_set_hostname(&(tlsDataParams->ssl), pNetwork->tlsConnectParams.pDestinationURL)) != 0) {
        IOT_ERROR(" failed\n  ! mbedtls_ssl_set_hostname returned %d\n\n", ret);
        return SSL_CONNECTION_ERROR;
    }
    IOT_DEBUG("\n\nSSL state connect : %d ", tlsDataParams->ssl.state);
    mbedtls_ssl_set_bio(&(tlsDataParams->ssl), &(tlsDataParams->server_fd), mbedtls_net_send, NULL,
        mbedtls_net_recv_timeout);
    IOT_DEBUG(" ok\n");

    IOT_DEBUG("\n\nSSL state connect : %d ", tlsDataParams->ssl.state);
    IOT_DEBUG("  . Performing the SSL/TLS handshake...");
    while ((ret = mbedtls_ssl_handshake(&(tlsDataParams->ssl))) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            IOT_ERROR(" failed\n  ! mbedtls_ssl_handshake returned -0x%x\n", -ret);
            if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                IOT_ERROR("    Unable to verify the server's certificate. "
                          "Either it is invalid,\n"
                          "    or you didn't set ca_file or ca_path "
                          "to an appropriate value.\n"
                          "    Alternatively, you may want to use "
                          "auth_mode=optional for testing purposes.\n");
            }
            return SSL_CONNECTION_ERROR;
        }
    }

    IOT_DEBUG(" ok\n    [ Protocol is %s ]\n    [ Ciphersuite is %s ]\n", mbedtls_ssl_get_version(&(tlsDataParams->ssl)),
        mbedtls_ssl_get_ciphersuite(&(tlsDataParams->ssl)));
    if ((ret = mbedtls_ssl_get_record_expansion(&(tlsDataParams->ssl))) >= 0) {
        IOT_DEBUG("    [ Record expansion is %d ]\n", ret);
    } else {
        IOT_DEBUG("    [ Record expansion is unknown (compression) ]\n");
    }

    IOT_DEBUG("  . Verifying peer X.509 certificate...");

    if (pNetwork->tlsConnectParams.ServerVerificationFlag == true) {
        if ((tlsDataParams->flags = mbedtls_ssl_get_verify_result(&(tlsDataParams->ssl))) != 0) {
            IOT_ERROR(" failed\n");
            mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", tlsDataParams->flags);
            IOT_ERROR("%s\n", vrfy_buf);
            ret = SSL_CONNECTION_ERROR;
        } else {
            IOT_DEBUG(" ok\n");
            ret = SUCCESS;
        }
    } else {
        IOT_DEBUG(" Server Verification skipped\n");
        ret = SUCCESS;
    }

#ifdef ENABLE_IOT_DEBUG
    if (mbedtls_ssl_get_peer_cert(&(tlsDataParams->ssl)) != NULL) {
        IOT_DEBUG("  . Peer certificate information    ...\n");
        mbedtls_x509_crt_info((char*)debug_buf, sizeof(debug_buf) - 1, "      ", mbedtls_ssl_get_peer_cert(&(tlsDataParams->ssl)));
        IOT_DEBUG("%s\n", debug_buf);
    }
#endif

    mbedtls_ssl_conf_read_timeout(&(tlsDataParams->conf), IOT_SSL_READ_TIMEOUT);

    return (IoT_Error_t)ret;
}

IoT_Error_t iot_tls_write(Network* pNetwork, unsigned char* pMsg, size_t len, Timer* timer, size_t* written_len)
{
    size_t written_so_far;
    bool isErrorFlag = false;
    int frags;
    int ret = 0;
    TLSDataParams* tlsDataParams = &(pNetwork->tlsDataParams);

    for (written_so_far = 0, frags = 0;
         written_so_far < len && !has_timer_expired(timer); written_so_far += ret, frags++) {
        while (!has_timer_expired(timer) && (ret = mbedtls_ssl_write(&(tlsDataParams->ssl), pMsg + written_so_far, len - written_so_far)) <= 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                IOT_ERROR(" failed\n  ! mbedtls_ssl_write returned -0x%x\n\n", -ret);
                /* All other negative return values indicate connection needs to be reset.
		 		* Will be caught in ping request so ignored here */
                isErrorFlag = true;
                break;
            }
        }
        if (isErrorFlag) {
            break;
        }
    }

    *written_len = written_so_far;

    if (isErrorFlag) {
        return NETWORK_SSL_WRITE_ERROR;
    } else if (has_timer_expired(timer) && written_so_far != len) {
        return NETWORK_SSL_WRITE_TIMEOUT_ERROR;
    }

    return SUCCESS;
}

IoT_Error_t iot_tls_read(Network* pNetwork, unsigned char* pMsg, size_t len, Timer* timer, size_t* read_len)
{
    mbedtls_ssl_context* ssl = &(pNetwork->tlsDataParams.ssl);
    size_t rxLen = 0;
    int ret;

    while (len > 0) {
        // This read will timeout after IOT_SSL_READ_TIMEOUT if there's no data to be read
        ret = mbedtls_ssl_read(ssl, pMsg, len);
        if (ret > 0) {
            rxLen += ret;
            pMsg += ret;
            len -= ret;
        } else if (ret == 0 || (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_TIMEOUT)) {
            return NETWORK_SSL_READ_ERROR;
        }

        // Evaluate timeout after the read to make sure read is done at least once
        if (has_timer_expired(timer)) {
            break;
        }
    }

    if (len == 0) {
        *read_len = rxLen;
        return SUCCESS;
    }

    if (rxLen == 0) {
        return NETWORK_SSL_NOTHING_TO_READ;
    } else {
        return NETWORK_SSL_READ_TIMEOUT_ERROR;
    }
}

IoT_Error_t iot_tls_disconnect(Network* pNetwork)
{
    mbedtls_ssl_context* ssl = &(pNetwork->tlsDataParams.ssl);
    int ret = 0;
    do {
        ret = mbedtls_ssl_close_notify(ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    /* All other negative return values indicate connection needs to be reset.
	 * No further action required since this is disconnect call */

    return SUCCESS;
}

IoT_Error_t iot_tls_destroy(Network* pNetwork)
{
    TLSDataParams* tlsDataParams = &(pNetwork->tlsDataParams);

    mbedtls_net_free(&(tlsDataParams->server_fd));

    mbedtls_x509_crt_free(&(tlsDataParams->clicert));
    mbedtls_x509_crt_free(&(tlsDataParams->cacert));
    mbedtls_pk_free(&(tlsDataParams->pkey));
    mbedtls_ssl_free(&(tlsDataParams->ssl));
    mbedtls_ssl_config_free(&(tlsDataParams->conf));
    mbedtls_ctr_drbg_free(&(tlsDataParams->ctr_drbg));
    mbedtls_entropy_free(&(tlsDataParams->entropy));

    return SUCCESS;
}

#ifdef __cplusplus
}
#endif
