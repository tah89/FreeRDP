/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * FreeRDP Proxy Server
 *
 * Copyright 2019 Mati Shabtay <matishabtay@gmail.com>
 * Copyright 2019 Kobi Mizrachi <kmizrachi18@gmail.com>
 * Copyright 2019 Idan Freiberg <speidy@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winpr/crt.h>
#include <winpr/ssl.h>
#include <winpr/synch.h>
#include <winpr/string.h>
#include <winpr/winsock.h>
#include <winpr/thread.h>
#include <errno.h>

#include <freerdp/freerdp.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/channels.h>
#include <freerdp/build-config.h>

#include <freerdp/server/proxy/proxy_server.h>
#include <freerdp/server/proxy/proxy_log.h>

#include "pf_server.h"
#include <freerdp/server/proxy/proxy_config.h>
#include "pf_client.h"
#include <freerdp/server/proxy/proxy_context.h>
#include "pf_update.h"
#include "pf_rdpgfx.h"
#include "pf_disp.h"
#include "pf_rail.h"
#include "pf_channels.h"
#include "proxy_modules.h"
#include "pf_utils.h"

#define TAG PROXY_TAG("server")

static BOOL pf_server_parse_target_from_routing_token(rdpContext* context, char** target,
                                                      DWORD* port)
{
#define TARGET_MAX (100)
#define ROUTING_TOKEN_PREFIX "Cookie: msts="
	char* colon;
	size_t len;
	DWORD routing_token_length;
	const size_t prefix_len = strnlen(ROUTING_TOKEN_PREFIX, sizeof(ROUTING_TOKEN_PREFIX));
	const char* routing_token = freerdp_nego_get_routing_token(context, &routing_token_length);
	pServerContext* ps = (pServerContext*)context;

	if (!routing_token)
		return FALSE;

	if ((routing_token_length <= prefix_len) || (routing_token_length >= TARGET_MAX))
	{
		PROXY_LOG_ERR(TAG, ps, "invalid routing token length: %" PRIu32 "", routing_token_length);
		return FALSE;
	}

	len = routing_token_length - prefix_len;
	*target = malloc(len + 1);

	if (!(*target))
		return FALSE;

	CopyMemory(*target, routing_token + prefix_len, len);
	*(*target + len) = '\0';
	colon = strchr(*target, ':');

	if (colon)
	{
		/* port is specified */
		unsigned long p = strtoul(colon + 1, NULL, 10);

		if (p > USHRT_MAX)
		{
			free(*target);
			return FALSE;
		}

		*port = (DWORD)p;
		*colon = '\0';
	}

	return TRUE;
}

static BOOL pf_server_get_target_info(rdpContext* context, rdpSettings* settings,
                                      const proxyConfig* config)
{
	pServerContext* ps = (pServerContext*)context;
	proxyFetchTargetEventInfo ev = { 0 };

	WINPR_ASSERT(settings);
	WINPR_ASSERT(ps);
	WINPR_ASSERT(ps->pdata);

	ev.fetch_method = config->FixedTarget ? PROXY_FETCH_TARGET_METHOD_CONFIG
	                                      : PROXY_FETCH_TARGET_METHOD_LOAD_BALANCE_INFO;

	if (!pf_modules_run_filter(ps->pdata->module, FILTER_TYPE_SERVER_FETCH_TARGET_ADDR, ps->pdata,
	                           &ev))
		return FALSE;

	switch (ev.fetch_method)
	{
		case PROXY_FETCH_TARGET_METHOD_DEFAULT:
		case PROXY_FETCH_TARGET_METHOD_LOAD_BALANCE_INFO:
			return pf_server_parse_target_from_routing_token(context, &settings->ServerHostname,
			                                                 &settings->ServerPort);

		case PROXY_FETCH_TARGET_METHOD_CONFIG:
		{
			WINPR_ASSERT(config);
			settings->ServerPort = config->TargetPort > 0 ? 3389 : settings->ServerPort;
			settings->ServerHostname = _strdup(config->TargetHost);

			if (!settings->ServerHostname)
			{
				PROXY_LOG_ERR(TAG, ps, "strdup failed!");
				return FALSE;
			}

			return TRUE;
		}
		case PROXY_FETCH_TARGET_USE_CUSTOM_ADDR:
		{
			if (!ev.target_address)
			{
				WLog_ERR(TAG, "router: using CUSTOM_ADDR fetch method, but target_address == NULL");
				return FALSE;
			}

			settings->ServerHostname = _strdup(ev.target_address);
			if (!settings->ServerHostname)
			{
				PROXY_LOG_ERR(TAG, ps, "strdup failed!");
				return FALSE;
			}

			free(ev.target_address);
			settings->ServerPort = ev.target_port;
			return TRUE;
		}
		default:
			WLog_WARN(TAG, "unknown target fetch method: %d", ev.fetch_method);
			return FALSE;
	}

	return TRUE;
}

/* Event callbacks */
/**
 * This callback is called when the entire connection sequence is done (as
 * described in MS-RDPBCGR section 1.3)
 *
 * The server may start sending graphics output and receiving keyboard/mouse
 * input after this callback returns.
 */
static BOOL pf_server_post_connect(freerdp_peer* peer)
{
	pServerContext* ps;
	pClientContext* pc;
	rdpSettings* client_settings;
	proxyData* pdata;
	char** accepted_channels = NULL;
	size_t accepted_channels_count;
	size_t i;

	WINPR_ASSERT(peer);

	ps = (pServerContext*)peer->context;
	WINPR_ASSERT(ps);

	pdata = ps->pdata;
	WINPR_ASSERT(pdata);

	PROXY_LOG_INFO(TAG, ps, "Accepted client: %s", peer->settings->ClientHostname);
	accepted_channels = WTSGetAcceptedChannelNames(peer, &accepted_channels_count);
	if (accepted_channels)
	{
		for (i = 0; i < accepted_channels_count; i++)
			PROXY_LOG_INFO(TAG, ps, "Accepted channel: %s", accepted_channels[i]);

		free(accepted_channels);
	}

	pc = pf_context_create_client_context(peer->settings);
	if (pc == NULL)
	{
		PROXY_LOG_ERR(TAG, ps, "failed to create client context!");
		return FALSE;
	}

	client_settings = pc->context.settings;

	/* keep both sides of the connection in pdata */
	proxy_data_set_client_context(pdata, pc);

	if (!pf_server_get_target_info(peer->context, client_settings, pdata->config))
	{
		PROXY_LOG_INFO(TAG, ps, "pf_server_get_target_info failed!");
		return FALSE;
	}

	PROXY_LOG_INFO(TAG, ps, "remote target is %s:%" PRIu16 "", client_settings->ServerHostname,
	               client_settings->ServerPort);

	if (!pf_server_channels_init(ps, peer))
	{
		PROXY_LOG_INFO(TAG, ps, "failed to initialize server's channels!");
		return FALSE;
	}

	if (!pf_modules_run_hook(pdata->module, HOOK_TYPE_SERVER_POST_CONNECT, pdata, peer))
		return FALSE;

	/* Start a proxy's client in it's own thread */
	if (!(pdata->client_thread = CreateThread(NULL, 0, pf_client_start, pc, 0, NULL)))
	{
		PROXY_LOG_ERR(TAG, ps, "failed to create client thread");
		return FALSE;
	}

	return TRUE;
}

static BOOL pf_server_activate(freerdp_peer* peer)
{
	pServerContext* ps;
	proxyData* pdata;

	WINPR_ASSERT(peer);

	ps = (pServerContext*)peer->context;
	WINPR_ASSERT(ps);

	pdata = ps->pdata;
	WINPR_ASSERT(pdata);

	WINPR_ASSERT(peer->settings);
	peer->settings->CompressionLevel = PACKET_COMPR_TYPE_RDP8;
	if (!pf_modules_run_hook(pdata->module, HOOK_TYPE_SERVER_ACTIVATE, pdata, peer))
		return FALSE;
	return TRUE;
}

static BOOL pf_server_logon(freerdp_peer* peer, const SEC_WINNT_AUTH_IDENTITY* identity,
                            BOOL automatic)
{
	pServerContext* ps;
	proxyData* pdata;
	proxyServerPeerLogon info = { 0 };

	WINPR_ASSERT(peer);

	ps = (pServerContext*)peer->context;
	WINPR_ASSERT(ps);

	pdata = ps->pdata;
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(peer->settings);
	WINPR_ASSERT(identity);

	info.identity = identity;
	info.automatic = automatic;
	if (!pf_modules_run_filter(pdata->module, FILTER_TYPE_SERVER_PEER_LOGON, pdata, &info))
		return FALSE;
	return TRUE;
}

static BOOL pf_server_adjust_monitor_layout(freerdp_peer* peer)
{
	WINPR_ASSERT(peer);
	/* proxy as is, there's no need to do anything here */
	return TRUE;
}

static BOOL pf_server_receive_channel_data_hook(freerdp_peer* peer, UINT16 channelId,
                                                const BYTE* data, size_t size, UINT32 flags,
                                                size_t totalSize)
{
	pServerContext* ps;
	pClientContext* pc;
	proxyData* pdata;
	const proxyConfig* config;
	int pass;
	const char* channel_name = WTSChannelGetName(peer, channelId);

	WINPR_ASSERT(peer);

	ps = (pServerContext*)peer->context;
	WINPR_ASSERT(ps);

	pdata = ps->pdata;
	WINPR_ASSERT(pdata);

	pc = pdata->pc;
	config = pdata->config;
	WINPR_ASSERT(config);
	/*
	 * client side is not initialized yet, call original callback.
	 * this is probably a drdynvc message between peer and proxy server,
	 * which doesn't need to be proxied.
	 */
	if (!pc)
		goto original_cb;

	pass = pf_utils_channel_is_passthrough(config, channel_name);
	switch (pass)
	{
		case 0:
			return TRUE;
		case 1:
		{
			proxyChannelDataEventInfo ev;

			ev.channel_id = channelId;
			ev.channel_name = channel_name;
			ev.data = data;
			ev.data_len = size;
			ev.flags = flags;
			ev.total_size = totalSize;

			if (!pf_modules_run_filter(pdata->module, FILTER_TYPE_SERVER_PASSTHROUGH_CHANNEL_DATA,
			                           pdata, &ev))
				return TRUE; /* Silently ignore */

			return IFCALLRESULT(TRUE, pc->sendChannelData, pc, &ev);
		}
		default:
			break;
	}

original_cb:
	WINPR_ASSERT(pdata->server_receive_channel_data_original);
	return pdata->server_receive_channel_data_original(peer, channelId, data, size, flags,
	                                                   totalSize);
}

static BOOL pf_server_initialize_peer_connection(freerdp_peer* peer)
{
	pServerContext* ps;
	rdpSettings* settings;
	proxyData* pdata;
	const proxyConfig* config;
	proxyServer* server;

	WINPR_ASSERT(peer);

	settings = peer->settings;
	WINPR_ASSERT(settings);

	ps = (pServerContext*)peer->context;
	if (!ps)
		return FALSE;

	pdata = proxy_data_new();
	if (!pdata)
		return FALSE;

	proxy_data_set_server_context(pdata, ps);
	server = (proxyServer*)peer->ContextExtra;

	pdata->module = server->module;
	config = pdata->config = server->config;

	/* currently not supporting GDI orders */
	ZeroMemory(settings->OrderSupport, 32);
	peer->update->autoCalculateBitmapData = FALSE;

	settings->SupportMonitorLayoutPdu = TRUE;
	settings->SupportGraphicsPipeline = config->GFX;
	if (!freerdp_settings_set_string(settings, FreeRDP_CertificateFile, config->CertificateFile) ||
	    !freerdp_settings_set_string(settings, FreeRDP_CertificateContent,
	                                 config->CertificateContent) ||
	    !freerdp_settings_set_string(settings, FreeRDP_PrivateKeyFile, config->PrivateKeyFile) ||
	    !freerdp_settings_set_string(settings, FreeRDP_PrivateKeyContent,
	                                 config->PrivateKeyContent) ||
	    !freerdp_settings_set_string(settings, FreeRDP_RdpKeyFile, config->RdpKeyFile) ||
	    !freerdp_settings_set_string(settings, FreeRDP_RdpKeyContent, config->RdpKeyContent))
	{
		WLog_ERR(TAG, "Memory allocation failed (strdup)");
		return FALSE;
	}

	if (config->RemoteApp)
	{
		settings->RemoteApplicationSupportLevel =
		    RAIL_LEVEL_SUPPORTED | RAIL_LEVEL_DOCKED_LANGBAR_SUPPORTED |
		    RAIL_LEVEL_SHELL_INTEGRATION_SUPPORTED | RAIL_LEVEL_LANGUAGE_IME_SYNC_SUPPORTED |
		    RAIL_LEVEL_SERVER_TO_CLIENT_IME_SYNC_SUPPORTED |
		    RAIL_LEVEL_HIDE_MINIMIZED_APPS_SUPPORTED | RAIL_LEVEL_WINDOW_CLOAKING_SUPPORTED |
		    RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED;
		settings->RemoteAppLanguageBarSupported = TRUE;
	}

	settings->RdpSecurity = config->ServerRdpSecurity;
	settings->TlsSecurity = config->ServerTlsSecurity;
	settings->NlaSecurity = config->ServerNlaSecurity;
	settings->EncryptionLevel = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;
	settings->ColorDepth = 32;
	settings->SuppressOutput = TRUE;
	settings->RefreshRect = TRUE;
	settings->DesktopResize = TRUE;

	peer->PostConnect = pf_server_post_connect;
	peer->Activate = pf_server_activate;
	peer->Logon = pf_server_logon;
	peer->AdjustMonitorsLayout = pf_server_adjust_monitor_layout;
	peer->settings->MultifragMaxRequestSize = 0xFFFFFF; /* FIXME */

	/* virtual channels receive data hook */
	pdata->server_receive_channel_data_original = peer->ReceiveChannelData;
	peer->ReceiveChannelData = pf_server_receive_channel_data_hook;

	if (!ArrayList_Append(server->clients, pdata))
		return FALSE;

	CountdownEvent_AddCount(server->waitGroup, 1);
	return TRUE;
}

/**
 * Handles an incoming client connection, to be run in it's own thread.
 *
 * arg is a pointer to a freerdp_peer representing the client.
 */
static DWORD WINAPI pf_server_handle_peer(LPVOID arg)
{
	HANDLE eventHandles[32] = { 0 };
	HANDLE ChannelEvent;
	DWORD eventCount;
	DWORD tmp;
	DWORD status;
	pServerContext* ps;
	rdpContext* pc;
	proxyData* pdata;
	freerdp_peer* client = (freerdp_peer*)arg;
	proxyServer* server;

	WINPR_ASSERT(client);

	server = (proxyServer*)client->ContextExtra;
	WINPR_ASSERT(server);

	if (!pf_context_init_server_context(client))
		goto out_free_peer;

	if (!pf_server_initialize_peer_connection(client))
		goto out_free_peer;

	ps = (pServerContext*)client->context;
	WINPR_ASSERT(ps);

	pdata = ps->pdata;
	WINPR_ASSERT(pdata);

	WINPR_ASSERT(client->Initialize);
	client->Initialize(client);

	PROXY_LOG_INFO(TAG, ps, "new connection: proxy address: %s, client address: %s",
	               pdata->config->Host, client->hostname);
	/* Main client event handling loop */
	ChannelEvent = WTSVirtualChannelManagerGetEventHandle(ps->vcm);

	while (1)
	{
		eventCount = 0;
		{
			WINPR_ASSERT(client->GetEventHandles);
			tmp = client->GetEventHandles(client, &eventHandles[eventCount], 32 - eventCount);

			if (tmp == 0)
			{
				WLog_ERR(TAG, "Failed to get FreeRDP transport event handles");
				break;
			}

			eventCount += tmp;
		}
		eventHandles[eventCount++] = ChannelEvent;
		eventHandles[eventCount++] = pdata->abort_event;
		eventHandles[eventCount++] = WTSVirtualChannelManagerGetEventHandle(ps->vcm);
		status = WaitForMultipleObjects(eventCount, eventHandles, FALSE, INFINITE);

		if (status == WAIT_FAILED)
		{
			WLog_ERR(TAG, "WaitForMultipleObjects failed (status: %d)", status);
			break;
		}

		WINPR_ASSERT(client->CheckFileDescriptor);
		if (client->CheckFileDescriptor(client) != TRUE)
			break;

		if (WaitForSingleObject(ChannelEvent, 0) == WAIT_OBJECT_0)
		{
			if (!WTSVirtualChannelManagerCheckFileDescriptor(ps->vcm))
			{
				WLog_ERR(TAG, "WTSVirtualChannelManagerCheckFileDescriptor failure");
				goto fail;
			}
		}

		/* only disconnect after checking client's and vcm's file descriptors  */
		if (proxy_data_shall_disconnect(pdata))
		{
			WLog_INFO(TAG, "abort event is set, closing connection with peer %s", client->hostname);
			break;
		}

		switch (WTSVirtualChannelManagerGetDrdynvcState(ps->vcm))
		{
			/* Dynamic channel status may have been changed after processing */
			case DRDYNVC_STATE_NONE:

				/* Initialize drdynvc channel */
				if (!WTSVirtualChannelManagerCheckFileDescriptor(ps->vcm))
				{
					WLog_ERR(TAG, "Failed to initialize drdynvc channel");
					goto fail;
				}

				break;

			case DRDYNVC_STATE_READY:
				if (WaitForSingleObject(ps->dynvcReady, 0) == WAIT_TIMEOUT)
				{
					SetEvent(ps->dynvcReady);
				}

				break;

			default:
				break;
		}
	}

fail:

	PROXY_LOG_INFO(TAG, ps, "starting shutdown of connection");
	PROXY_LOG_INFO(TAG, ps, "stopping proxy's client");

	pc = (rdpContext*)pdata->pc;
	freerdp_client_stop(pc);

	pf_modules_run_hook(pdata->module, HOOK_TYPE_SERVER_SESSION_END, pdata, client);
	PROXY_LOG_INFO(TAG, ps, "freeing server's channels");
	pf_server_channels_free(ps, client);
	PROXY_LOG_INFO(TAG, ps, "freeing proxy data");
	ArrayList_Remove(server->clients, pdata);
	proxy_data_free(pdata);
	freerdp_client_context_free(pc);

	WINPR_ASSERT(client->Close);
	client->Close(client);

	WINPR_ASSERT(client->Disconnect);
	client->Disconnect(client);

out_free_peer:
	freerdp_peer_context_free(client);
	freerdp_peer_free(client);
	CountdownEvent_Signal(server->waitGroup, 1);
	ExitThread(0);
	return 0;
}

static BOOL pf_server_start_peer(freerdp_peer* client)
{
	HANDLE hThread;

	WINPR_ASSERT(client);

	if (!(hThread = CreateThread(NULL, 0, pf_server_handle_peer, (void*)client, 0, NULL)))
		return FALSE;

	CloseHandle(hThread);
	return TRUE;
}

static BOOL pf_server_peer_accepted(freerdp_listener* listener, freerdp_peer* client)
{
	WINPR_ASSERT(listener);
	WINPR_ASSERT(client);

	client->ContextExtra = listener->info;

	return pf_server_start_peer(client);
}

BOOL pf_server_start(proxyServer* server)
{
	WSADATA wsaData;

	WINPR_ASSERT(server);

	WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
	winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		goto error;

	WINPR_ASSERT(server->config);
	WINPR_ASSERT(server->listener);
	WINPR_ASSERT(server->listener->Open);
	if (!server->listener->Open(server->listener, server->config->Host, server->config->Port))
	{
		switch (errno)
		{
			case EADDRINUSE:
				WLog_ERR(TAG, "failed to start listener: address already in use!");
				break;
			case EACCES:
				WLog_ERR(TAG, "failed to start listener: insufficent permissions!");
				break;
			default:
				WLog_ERR(TAG, "failed to start listener: errno=%d", errno);
				break;
		}

		goto error;
	}

	return TRUE;

error:
	WSACleanup();
	return FALSE;
}

BOOL pf_server_start_from_socket(proxyServer* server, int socket)
{
	WSADATA wsaData;

	WINPR_ASSERT(server);

	WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
	winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		goto error;

	WINPR_ASSERT(server->listener);
	WINPR_ASSERT(server->listener->OpenFromSocket);
	if (!server->listener->OpenFromSocket(server->listener, socket))
	{
		switch (errno)
		{
			case EADDRINUSE:
				WLog_ERR(TAG, "failed to start listener: address already in use!");
				break;
			case EACCES:
				WLog_ERR(TAG, "failed to start listener: insufficent permissions!");
				break;
			default:
				WLog_ERR(TAG, "failed to start listener: errno=%d", errno);
				break;
		}

		goto error;
	}

	return TRUE;

error:
	WSACleanup();
	return FALSE;
}

BOOL pf_server_start_with_peer_socket(proxyServer* server, int peer_fd)
{
	struct sockaddr_storage peer_addr;
	socklen_t len = sizeof(peer_addr);
	freerdp_peer* client = freerdp_peer_new(peer_fd);

	WINPR_ASSERT(server);

	if (!client)
		goto fail;

	if (getpeername(peer_fd, (struct sockaddr*)&peer_addr, &len) != 0)
		goto fail;

	if (!freerdp_peer_set_local_and_hostname(client, &peer_addr))
		goto fail;

	client->ContextExtra = server;

	if (!pf_server_start_peer(client))
		goto fail;

	return TRUE;

fail:
	WLog_ERR(TAG, "PeerAccepted callback failed");
	freerdp_peer_free(client);
	return FALSE;
}

static void pf_server_clients_list_client_free(void* obj)
{
	proxyData* pdata = (proxyData*)obj;
	proxy_data_abort_connect(pdata);
}

static BOOL are_all_required_modules_loaded(proxyModule* module, const proxyConfig* config)
{
	size_t i;

	for (i = 0; i < pf_config_required_plugins_count(config); i++)
	{
		const char* plugin_name = pf_config_required_plugin(config, i);

		if (!pf_modules_is_plugin_loaded(module, plugin_name))
		{
			WLog_ERR(TAG, "Required plugin '%s' is not loaded. stopping.", plugin_name);
			return FALSE;
		}
	}

	return TRUE;
}

proxyServer* pf_server_new(const proxyConfig* config)
{
	wObject* obj;
	proxyServer* server;

	WINPR_ASSERT(config);

	server = calloc(1, sizeof(proxyServer));
	if (!server)
		return NULL;

	if (!pf_config_clone(&server->config, config))
		goto out;

	server->module = pf_modules_new(FREERDP_PROXY_PLUGINDIR, pf_config_modules(config),
	                                pf_config_modules_count(config));
	if (!server->module)
	{
		WLog_ERR(TAG, "failed to initialize proxy modules!");
		goto out;
	}

	pf_modules_list_loaded_plugins(server->module);
	if (!are_all_required_modules_loaded(server->module, server->config))
		goto out;

	server->stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!server->stopEvent)
		goto out;

	server->clients = ArrayList_New(TRUE);
	if (!server->clients)
		goto out;

	obj = ArrayList_Object(server->clients);
	obj->fnObjectFree = pf_server_clients_list_client_free;

	server->waitGroup = CountdownEvent_New(0);
	if (!server->waitGroup)
		goto out;

	server->listener = freerdp_listener_new();
	if (!server->listener)
		goto out;

	server->listener->info = server;
	server->listener->PeerAccepted = pf_server_peer_accepted;
	return server;

out:
	pf_server_free(server);
	return NULL;
}

BOOL pf_server_run(proxyServer* server)
{
	BOOL rc = TRUE;
	HANDLE eventHandles[32] = { 0 };
	DWORD eventCount;
	DWORD status;
	freerdp_listener* listener;

	WINPR_ASSERT(server);

	listener = server->listener;
	WINPR_ASSERT(listener);

	while (1)
	{
		WINPR_ASSERT(listener->GetEventHandles);
		eventCount = listener->GetEventHandles(listener, eventHandles, 32);

		if (0 == eventCount)
		{
			WLog_ERR(TAG, "Failed to get FreeRDP event handles");
			break;
		}

		WINPR_ASSERT(server->stopEvent);
		eventHandles[eventCount++] = server->stopEvent;
		status = WaitForMultipleObjects(eventCount, eventHandles, FALSE, INFINITE);

		if (WaitForSingleObject(server->stopEvent, 0) == WAIT_OBJECT_0)
			break;

		if (WAIT_FAILED == status)
		{
			WLog_ERR(TAG, "select failed");
			rc = FALSE;
			break;
		}

		WINPR_ASSERT(listener->CheckFileDescriptor);
		if (listener->CheckFileDescriptor(listener) != TRUE)
		{
			WLog_ERR(TAG, "Failed to check FreeRDP file descriptor");
			rc = FALSE;
			break;
		}
	}

	WINPR_ASSERT(listener->Close);
	listener->Close(listener);
	return rc;
}

void pf_server_stop(proxyServer* server)
{
	HANDLE waitHandle = INVALID_HANDLE_VALUE;

	if (!server)
		return;

	/* clear clients list, also disconnects every client */
	ArrayList_Clear(server->clients);

	/* block until all clients are disconnected */
	waitHandle = CountdownEvent_WaitHandle(server->waitGroup);
	if (WaitForSingleObject(waitHandle, INFINITE) != WAIT_OBJECT_0)
		WLog_ERR(TAG, "[%s]: WaitForSingleObject failed!", __FUNCTION__);

	/* signal main thread to stop and wait for the thread to exit */
	SetEvent(server->stopEvent);
}

void pf_server_free(proxyServer* server)
{
	if (!server)
		return;

	freerdp_listener_free(server->listener);
	ArrayList_Free(server->clients);
	CountdownEvent_Free(server->waitGroup);

	if (server->stopEvent)
		CloseHandle(server->stopEvent);

	pf_server_config_free(server->config);
	pf_modules_free(server->module);
	free(server);
}

BOOL pf_server_add_module(proxyServer* server, proxyModuleEntryPoint ep, void* userdata)
{
	WINPR_ASSERT(server);
	WINPR_ASSERT(ep);

	return pf_modules_add(server->module, ep, userdata);
}
