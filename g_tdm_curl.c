/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//Curl interface functions. OpenTDM can use libcurl to fetch and POST to the
//website, used for downloading configs and uploading stats (todo).

#include "g_local.h"
#include "g_tdm.h"

#ifdef HAVE_CURL

#define CURL_DISABLE_DEPRECATION
#include <curl/curl.h>

typedef struct dlhandle_s
{
	CURL			*curl;
	size_t			fileSize;
	size_t			position;
	char			filePath[1024];
	char			URL[2048];
	char			*tempBuffer;
	qboolean		inuse;
	tdm_download_t	*tdm_handle;
} dlhandle_t;

//we need this high in case a sudden server switch causes a bunch of people
//to connect, we want to be able to download their configs
#define MAX_DOWNLOADS	16

//size limits for configs, must be power of two
#define MAX_DLSIZE	(1 << 20)	// 1 MiB
#define MIN_DLSIZE	(1 << 15)	// 32 KiB

static dlhandle_t	downloads[MAX_DOWNLOADS];

static CURLM		*multi;
static int			handleCount;

/*
===============
HTTP_EscapePath

Properly escapes a path with HTTP %encoding. libcurl's function
seems to treat '/' and such as illegal chars and encodes almost
the entire URL...
===============
*/
static void HTTP_EscapePath (const char *filePath, char *escaped)
{
	int		i;
	size_t	len;
	char	*p;

	p = escaped;

	len = strlen (filePath);
	for (i = 0; i < len; i++)
	{
		if (!isalnum (filePath[i]) && !strchr ("/-_.~", filePath[i]))
		{
			sprintf (p, "%%%02x", filePath[i]);
			p += 3;
		}
		else
		{
			*p = filePath[i];
			p++;
		}
	}
	p[0] = 0;

	//using ./ in a url is legal, but all browsers condense the path and some IDS / request
	//filtering systems act a bit funky if http requests come in with uncondensed paths.
	len = strlen(escaped);
	p = escaped;
	while ((p = strstr (p, "./")))
	{
		memmove (p, p+2, len - (p - escaped) - 1);
		len -= 2;
	}
}

/*
===============
HTTP_Recv

libcurl callback.
===============
*/
static size_t EXPORT HTTP_Recv (void *ptr, size_t size, size_t nmemb, void *stream)
{
	dlhandle_t	*dl;
	size_t		new_size, bytes;

	dl = (dlhandle_t *)stream;

	if (!size || !nmemb)
		return 0;

	if (size > SIZE_MAX / nmemb)
		goto oversize;

	if (dl->position > MAX_DLSIZE)
		goto oversize;

	bytes = size * nmemb;
	if (bytes >= MAX_DLSIZE - dl->position)
		goto oversize;

	//grow buffer in MIN_DLSIZE chunks. +1 for NUL.
	new_size = (dl->position + bytes + MIN_DLSIZE) & ~(MIN_DLSIZE - 1);
	if (new_size > dl->fileSize) {
		char		*tmp;

		tmp = dl->tempBuffer;
		dl->tempBuffer = gi.TagMalloc ((int)new_size, TAG_GAME);
		if (tmp)
		{
			memcpy (dl->tempBuffer, tmp, dl->fileSize);
			gi.TagFree (tmp);
		}
		dl->fileSize = new_size;
	}

	memcpy (dl->tempBuffer + dl->position, ptr, bytes);
	dl->position += bytes;
	dl->tempBuffer[dl->position] = 0;

	return bytes;

oversize:
	gi.dprintf ("Suspiciously large file while trying to download %s!\n", dl->URL);
	return 0;
}

static int EXPORT CURL_Debug (CURL *c, curl_infotype type, char *data, size_t size, void *ptr)
{
	if (type == CURLINFO_TEXT)
		gi.dprintf ("  OpenTDM HTTP DEBUG: %s", data);

	return 0;
}

/*
===============
HTTP_StartDownload

Actually starts a download by adding it to the curl multi
handle.
===============
*/
static qboolean HTTP_StartDownload (dlhandle_t *dl)
{
	cvar_t				*hostname;
	char				escapedFilePath[1024*3];

	hostname = gi.cvar ("hostname", NULL, 0);
	if (!hostname)
		TDM_Error ("HTTP_StartDownload: Couldn't get hostname cvar");

	dl->tempBuffer = NULL;
	dl->fileSize = 0;
	dl->position = 0;

	if (!dl->curl)
		dl->curl = curl_easy_init ();
	if (!dl->curl)
		return false;

	HTTP_EscapePath (dl->filePath, escapedFilePath);

	Com_sprintf (dl->URL, sizeof(dl->URL), "http://%s%s%s", g_http_domain->string, g_http_path->string, escapedFilePath);

	curl_easy_setopt (dl->curl, CURLOPT_ACCEPT_ENCODING, "");

	if (g_http_debug->value)
	{
		curl_easy_setopt (dl->curl, CURLOPT_DEBUGFUNCTION, CURL_Debug);
		curl_easy_setopt (dl->curl, CURLOPT_VERBOSE, 1L);
	}
	else
	{
		curl_easy_setopt (dl->curl, CURLOPT_DEBUGFUNCTION, NULL);
		curl_easy_setopt (dl->curl, CURLOPT_VERBOSE, 0L);
	}

	curl_easy_setopt (dl->curl, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt (dl->curl, CURLOPT_WRITEDATA, dl);
	if (g_http_bind->string[0])
		curl_easy_setopt (dl->curl, CURLOPT_INTERFACE, g_http_bind->string);
	else
		curl_easy_setopt (dl->curl, CURLOPT_INTERFACE, NULL);

	curl_easy_setopt (dl->curl, CURLOPT_WRITEFUNCTION, HTTP_Recv);

	if (g_http_proxy->string[0])
		curl_easy_setopt (dl->curl, CURLOPT_PROXY, g_http_proxy->string);
	else
		curl_easy_setopt (dl->curl, CURLOPT_PROXY, NULL);
	curl_easy_setopt (dl->curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt (dl->curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt (dl->curl, CURLOPT_USERAGENT, "OpenTDM (" OPENTDM_VERSION ")");
	curl_easy_setopt (dl->curl, CURLOPT_REFERER, hostname->string);
	curl_easy_setopt (dl->curl, CURLOPT_URL, dl->URL);
	curl_easy_setopt (dl->curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS | 0L);
	curl_easy_setopt (dl->curl, CURLOPT_PRIVATE, dl);

	if (curl_multi_add_handle (multi, dl->curl) != CURLM_OK)
	{
		gi.dprintf ("HTTP_StartDownload: curl_multi_add_handle: error\n");
		return false;
	}

	dl->inuse = true;
	handleCount++;
	return true;
}

/*
===============
HTTP_Init

Init libcurl.
===============
*/
void HTTP_Init (void)
{
	if (curl_global_init (CURL_GLOBAL_NOTHING))
		gi.error ("curl_global_init failed");

	multi = curl_multi_init ();
	if (!multi)
		gi.error ("curl_multi_init failed");

	gi.dprintf ("%s initialized.\n", curl_version());
}

void HTTP_Shutdown (void)
{
	int		i;

	if (multi)
	{
		curl_multi_cleanup (multi);
		multi = NULL;
	}

	for (i = 0; i < MAX_DOWNLOADS; i++)
	{
		if (downloads[i].curl)
			curl_easy_cleanup(downloads[i].curl);
	}

	memset(downloads, 0, sizeof(downloads));
	handleCount = 0;

	curl_global_cleanup ();
}

/*
===============
CL_FinishHTTPDownload

A download finished, find out what it was, whether there were any errors and
if so, how severe. If none, rename file and other such stuff.
===============
*/
static void HTTP_FinishDownload (void)
{
	int			msgs_in_queue;
	CURLMsg		*msg;
	CURLcode	result;
	dlhandle_t	*dl;
	CURL		*curl;
	long		responseCode;
	double		timeTaken;
	double		fileSize;

	do
	{
		msg = curl_multi_info_read (multi, &msgs_in_queue);

		if (!msg)
		{
			gi.dprintf ("HTTP_FinishDownload: Odd, no message for us...\n");
			return;
		}

		if (msg->msg != CURLMSG_DONE)
		{
			gi.dprintf ("HTTP_FinishDownload: Got some weird message...\n");
			continue;
		}

		curl = msg->easy_handle;
		curl_easy_getinfo(curl, CURLINFO_PRIVATE, &dl);

		result = msg->data.result;

		switch (result)
		{
			//for some reason curl returns CURLE_OK for a 404...
			case CURLE_HTTP_RETURNED_ERROR:
			case CURLE_OK:
				curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &responseCode);
				if (responseCode == 404)
				{
					TDM_HandleDownload (dl->tdm_handle, NULL, 0, responseCode);
					gi.dprintf ("HTTP: %s: 404 File Not Found\n", dl->URL);
				}
				else if (responseCode == 200)
				{
					TDM_HandleDownload (dl->tdm_handle, dl->tempBuffer, dl->position, responseCode);

					//show some stats
					curl_easy_getinfo (curl, CURLINFO_TOTAL_TIME, &timeTaken);
					curl_easy_getinfo (curl, CURLINFO_SIZE_DOWNLOAD, &fileSize);

					gi.dprintf ("HTTP: Finished %s: %.f bytes, %.2fkB/sec\n", dl->URL, fileSize, (fileSize / 1024.0) / timeTaken);
				}
				else
				{
					TDM_HandleDownload (dl->tdm_handle, NULL, 0, responseCode);
					gi.dprintf ("HTTP Error: %s: response code %ld\n", dl->URL, responseCode);
				}
				break;

			//fatal error
			default:
				TDM_HandleDownload (dl->tdm_handle, NULL, 0, 0);
				gi.dprintf ("HTTP Error: %s: %s\n", dl->URL, curl_easy_strerror (result));
				break;
		}

		if (dl->tempBuffer) {
			gi.TagFree (dl->tempBuffer);
			dl->tempBuffer = NULL;
		}

		curl_multi_remove_handle (multi, dl->curl);
		dl->inuse = false;
	} while (msgs_in_queue > 0);
}

qboolean HTTP_QueueDownload (tdm_download_t *d)
{
	dlhandle_t	*dl;
	int			i;

	if (!g_http_enabled->value)
	{
		if (d->type == DL_CONFIG)
			gi.cprintf (d->initiator, PRINT_HIGH, "HTTP functions are disabled on this server.\n");
		return false;
	}

	for (i = 0; i < MAX_DOWNLOADS; i++)
	{
		dl = &downloads[i];
		if (!dl->inuse)
			break;
	}

	if (i == MAX_DOWNLOADS)
	{
		if (d->type == DL_CONFIG)
			gi.cprintf (d->initiator, PRINT_HIGH, "The server is too busy to download configs right now.\n");
		return false;
	}

	dl->tdm_handle = d;
	strcpy (dl->filePath, d->path);
	if (HTTP_StartDownload (dl))
		return true;

	if (d->type == DL_CONFIG)
		gi.cprintf (d->initiator, PRINT_HIGH, "Couldn't start HTTP download.\n");
	return false;
}

/*
===============
HTTP_RunDownloads

This calls curl_multi_perform to actually do stuff. Called every frame to process
downloads.
===============
*/
void HTTP_RunDownloads (void)
{
	int			newHandleCount;
	CURLMcode	ret;

	//nothing to do!
	if (!handleCount)
		return;

	ret = curl_multi_perform (multi, &newHandleCount);
	if (ret != CURLM_OK)
	{
		gi.dprintf ("HTTP_RunDownloads: curl_multi_perform error.\n");
		return;
	}

	if (newHandleCount < handleCount)
	{
		HTTP_FinishDownload ();
		handleCount = newHandleCount;
	}
}
#else
void HTTP_RunDownloads (void)
{
}

void HTTP_Init (void)
{
	gi.dprintf ("WARNING: OpenTDM was built without libcurl. Some features will be unavailable.\n");
}

void HTTP_Shutdown (void)
{
}

qboolean HTTP_QueueDownload (tdm_download_t *d)
{
	if (d->type == DL_CONFIG)
		gi.cprintf (d->initiator, PRINT_HIGH, "HTTP functions are not compiled on this server.\n");
	return false;
}
#endif
