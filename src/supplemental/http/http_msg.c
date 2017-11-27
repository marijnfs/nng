//
// Copyright 2017 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"
#include "http.h"

// Note that as we parse headers, the rule is that if a header is already
// present, then we can append it to the existing header, separated by
// a comma.  From experience, for example, Firefox uses a Connection:
// header with two values, "keepalive", and "upgrade".
typedef struct http_header {
	char *        name;
	char *        value;
	nni_list_node node;
} http_header;

struct nni_http_entity {
	char * data;
	size_t size; // allocated/expected size
	size_t len;  // current length
	bool   own;  // if true, data is "ours", and should be freed
};

struct nni_http_req {
	nni_list        hdrs;
	nni_http_entity data;
	char *          meth;
	char *          uri;
	char *          vers;
	char *          buf;
	size_t          bufsz;
};

struct nni_http_res {
	nni_list        hdrs;
	nni_http_entity data;
	int             code;
	char *          rsn;
	char *          vers;
	char *          buf;
	size_t          bufsz;
};

static int
http_set_string(char **strp, const char *val)
{
	char *news;
	if ((news = nni_strdup(val)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_strfree(*strp);
	*strp = news;
	return (0);
}

static void
http_headers_reset(nni_list *hdrs)
{
	http_header *h;
	while ((h = nni_list_first(hdrs)) != NULL) {
		nni_list_remove(hdrs, h);
		if (h->name != NULL) {
			nni_strfree(h->name);
		}
		if (h->value != NULL) {
			nni_free(h->value, strlen(h->value) + 1);
		}
		NNI_FREE_STRUCT(h);
	}
}

static void
http_entity_reset(nni_http_entity *entity)
{
	if (entity->own && entity->size) {
		nni_free(entity->data, entity->size);
	}
	entity->data = NULL;
	entity->size = 0;
	entity->own  = false;
}

void
nni_http_req_reset(nni_http_req *req)
{
	http_headers_reset(&req->hdrs);
	http_entity_reset(&req->data);
	nni_strfree(req->vers);
	nni_strfree(req->meth);
	nni_strfree(req->uri);
	req->vers = req->meth = req->uri = NULL;
	if (req->bufsz) {
		req->buf[0] = '\0';
	}
}

void
nni_http_res_reset(nni_http_res *res)
{
	http_headers_reset(&res->hdrs);
	http_entity_reset(&res->data);
	nni_strfree(res->rsn);
	nni_strfree(res->vers);
	res->code = 0;
	if (res->bufsz) {
		res->buf[0] = '\0';
	}
}

void
nni_http_req_fini(nni_http_req *req)
{
	nni_http_req_reset(req);
	if (req->bufsz) {
		nni_free(req->buf, req->bufsz);
	}
	NNI_FREE_STRUCT(req);
}

void
nni_http_res_fini(nni_http_res *res)
{
	nni_http_res_reset(res);
	if (res->bufsz) {
		nni_free(res->buf, res->bufsz);
	}
	NNI_FREE_STRUCT(res);
}

static int
http_del_header(nni_list *hdrs, const char *key)
{
	http_header *h;
	NNI_LIST_FOREACH (hdrs, h) {
		if (strcasecmp(key, h->name) == 0) {
			nni_list_remove(hdrs, h);
			nni_strfree(h->name);
			nni_free(h->value, strlen(h->value) + 1);
			NNI_FREE_STRUCT(h);
			return (0);
		}
	}
	return (NNG_ENOENT);
}

int
nni_req_del_header(nni_http_req *req, const char *key)
{
	return (http_del_header(&req->hdrs, key));
}

int
nni_res_del_header(nni_http_res *res, const char *key)
{
	return (http_del_header(&res->hdrs, key));
}

static int
http_set_header(nni_list *hdrs, const char *key, const char *val)
{
	http_header *h;
	NNI_LIST_FOREACH (hdrs, h) {
		if (strcasecmp(key, h->name) == 0) {
			char * news;
			size_t len = strlen(val) + 1;
			if ((news = nni_alloc(len)) == NULL) {
				return (NNG_ENOMEM);
			}
			snprintf(news, len, "%s", val);
			nni_free(h->value, strlen(h->value) + 1);
			h->value = news;
			return (0);
		}
	}

	if ((h = NNI_ALLOC_STRUCT(h)) == NULL) {
		return (NNG_ENOMEM);
	}
	if ((h->name = nni_strdup(key)) == NULL) {
		NNI_FREE_STRUCT(h);
		return (NNG_ENOMEM);
	}
	if ((h->value = nni_alloc(strlen(val) + 1)) == NULL) {
		nni_strfree(h->name);
		NNI_FREE_STRUCT(h);
		return (NNG_ENOMEM);
	}
	strncpy(h->value, val, strlen(val) + 1);
	nni_list_append(hdrs, h);
	return (0);
}

int
nni_http_req_set_header(nni_http_req *req, const char *key, const char *val)
{
	return (http_set_header(&req->hdrs, key, val));
}

int
nni_http_res_set_header(nni_http_res *res, const char *key, const char *val)
{
	return (http_set_header(&res->hdrs, key, val));
}

static int
http_add_header(nni_list *hdrs, const char *key, const char *val)
{
	http_header *h;
	NNI_LIST_FOREACH (hdrs, h) {
		if (strcasecmp(key, h->name) == 0) {
			char * news;
			size_t len = strlen(h->value) + strlen(val) + 3;
			if ((news = nni_alloc(len)) == NULL) {
				return (NNG_ENOMEM);
			}
			snprintf(news, len, "%s, %s", h->value, val);
			nni_free(h->value, strlen(h->value) + 1);
			h->value = news;
			return (0);
		}
	}

	if ((h = NNI_ALLOC_STRUCT(h)) == NULL) {
		return (NNG_ENOMEM);
	}
	if ((h->name = nni_strdup(key)) == NULL) {
		NNI_FREE_STRUCT(h);
		return (NNG_ENOMEM);
	}
	if ((h->value = nni_alloc(strlen(val) + 1)) == NULL) {
		nni_strfree(h->name);
		NNI_FREE_STRUCT(h);
		return (NNG_ENOMEM);
	}
	strncpy(h->value, val, strlen(val) + 1);
	nni_list_append(hdrs, h);
	return (0);
}

int
nni_http_req_add_header(nni_http_req *req, const char *key, const char *val)
{
	return (http_add_header(&req->hdrs, key, val));
}

int
nni_http_res_add_header(nni_http_res *res, const char *key, const char *val)
{
	return (http_add_header(&res->hdrs, key, val));
}

static const char *
http_get_header(nni_list *hdrs, const char *key)
{
	http_header *h;
	NNI_LIST_FOREACH (hdrs, h) {
		if (strcasecmp(h->name, key) == 0) {
			return (h->value);
		}
	}
	return (NULL);
}

const char *
nni_http_req_get_header(nni_http_req *req, const char *key)
{
	return (http_get_header(&req->hdrs, key));
}

const char *
nni_http_res_get_header(nni_http_res *res, const char *key)
{
	return (http_get_header(&res->hdrs, key));
}

// http_entity_set_data sets the entity, but does not update the
// content-length header.
static void
http_entity_set_data(nni_http_entity *entity, const void *data, size_t size)
{
	if (entity->own) {
		nni_free(entity->data, entity->size);
	}
	entity->data = (void *) data;
	entity->size = size;
	entity->own  = false;
}

static int
http_entity_alloc_data(nni_http_entity *entity, size_t size)
{
	void *newdata;
	if ((newdata = nni_alloc(size)) == NULL) {
		return (NNG_ENOMEM);
	}
	http_entity_set_data(entity, newdata, size);
	entity->own = true;
	return (0);
}

static int
http_entity_copy_data(nni_http_entity *entity, const void *data, size_t size)
{
	int rv;
	if ((rv = http_entity_alloc_data(entity, size)) == 0) {
		memcpy(entity->data, data, size);
	}
	return (rv);
}

static int
http_set_content_length(nni_http_entity *entity, nni_list *hdrs)
{
	char buf[16];
	(void) snprintf(buf, sizeof(buf), "%u", (unsigned) entity->size);
	return (http_set_header(hdrs, "Content-Length", buf));
}

static void
http_entity_get_data(nni_http_entity *entity, void **datap, size_t *sizep)
{
	*datap = entity->data;
	*sizep = entity->size;
}

void
nni_http_req_get_data(nni_http_req *req, void **datap, size_t *sizep)
{
	http_entity_get_data(&req->data, datap, sizep);
}

void
nni_http_res_get_data(nni_http_res *res, void **datap, size_t *sizep)
{
	http_entity_get_data(&res->data, datap, sizep);
}

int
nni_http_req_set_data(nni_http_req *req, const void *data, size_t size)
{
	int rv;

	http_entity_set_data(&req->data, data, size);
	if ((rv = http_set_content_length(&req->data, &req->hdrs)) != 0) {
		http_entity_set_data(&req->data, NULL, 0);
	}
	return (rv);
}

int
nni_http_res_set_data(nni_http_res *res, const void *data, size_t size)
{
	int rv;

	http_entity_set_data(&res->data, data, size);
	if ((rv = http_set_content_length(&res->data, &res->hdrs)) != 0) {
		http_entity_set_data(&res->data, NULL, 0);
	}
	return (rv);
}

int
nni_http_req_copy_data(nni_http_req *req, const void *data, size_t size)
{
	int rv;

	if (((rv = http_entity_copy_data(&req->data, data, size)) != 0) ||
	    ((rv = http_set_content_length(&req->data, &req->hdrs)) != 0)) {
		http_entity_set_data(&req->data, NULL, 0);
		return (rv);
	}
	return (0);
}

int
nni_http_res_copy_data(nni_http_res *res, const void *data, size_t size)
{
	int rv;

	if (((rv = http_entity_copy_data(&res->data, data, size)) != 0) ||
	    ((rv = http_set_content_length(&res->data, &res->hdrs)) != 0)) {
		http_entity_set_data(&res->data, NULL, 0);
		return (rv);
	}
	return (0);
}

int
nni_http_res_alloc_data(nni_http_res *res, size_t size)
{
	return (http_entity_alloc_data(&res->data, size));
}

static int
http_parse_header(nni_list *hdrs, void *line)
{
	http_header *h;
	char *       key = line;
	char *       val;
	char *       end;

	// Find separation between key and value
	if ((val = strchr(key, ':')) == NULL) {
		return (NNG_EPROTO);
	}

	// Trim leading and trailing whitespace from header
	*val = '\0';
	val++;
	while (*val == ' ' || *val == '\t') {
		val++;
	}
	end = val + strlen(val);
	end--;
	while ((end > val) && (*end == ' ' || *end == '\t')) {
		*end = '\0';
		end--;
	}

	return (http_add_header(hdrs, key, val));
}

// http_sprintf_headers makes headers for an HTTP request or an HTTP response
// object.  Each header is dumped from the list. If the buf is NULL,
// or the sz is 0, then a dryrun is done, in order to allow the caller to
// determine how much space is needed. Returns the size of the space needed,
// not including the terminating NULL byte.  Truncation occurs if the size
// returned is >= the requested size.
static size_t
http_sprintf_headers(char *buf, size_t sz, nni_list *list)
{
	size_t       rv = 0;
	http_header *h;

	if (buf == NULL) {
		sz = 0;
	}

	NNI_LIST_FOREACH (list, h) {
		size_t l;
		l = snprintf(buf, sz, "%s: %s\r\n", h->name, h->value);
		if (buf != NULL) {
			buf += l;
		}
		sz = (sz > l) ? sz - l : 0;
		rv += l;
	}
	return (rv);
}

static int
http_asprintf(char **bufp, size_t *szp, nni_list *hdrs, const char *fmt, ...)
{
	va_list ap;
	size_t  len;
	size_t  n;
	char *  buf;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);

	len += http_sprintf_headers(NULL, 0, hdrs);
	len += 5; // \r\n\r\n\0

	if (len <= *szp) {
		buf = *bufp;
	} else {
		if ((buf = nni_alloc(len)) == NULL) {
			return (NNG_ENOMEM);
		}
		nni_free(*bufp, *szp);
		*bufp = buf;
		*szp  = len;
	}
	va_start(ap, fmt);
	n = vsnprintf(buf, len, fmt, ap);
	va_end(ap);
	buf += n;
	len -= n;
	n = http_sprintf_headers(buf, len, hdrs);
	buf += n;
	len -= n;
	snprintf(buf, len, "\r\n");
	return (0);
}

static int
http_req_prepare(nni_http_req *req)
{
	int rv;
	if ((req->uri == NULL) || (req->meth == NULL)) {
		return (NNG_EINVAL);
	}
	rv = http_asprintf(&req->buf, &req->bufsz, &req->hdrs, "%s %s %s\r\n",
	    req->meth, req->uri, req->vers != NULL ? req->vers : "HTTP/1.1");
	return (rv);
}

static int
http_res_prepare(nni_http_res *res)
{
	int rv;
	rv = http_asprintf(&res->buf, &res->bufsz, &res->hdrs, "%s %d %s\r\n",
	    res->vers != NULL ? res->vers : "HTTP/1.1", res->code,
	    res->rsn != NULL ? res->rsn : "Unknown Error");
	return (rv);
}

int
nni_http_req_get_buf(nni_http_req *req, void **data, size_t *szp)
{
	int rv;

	if ((req->buf == NULL) && (rv = http_req_prepare(req)) != 0) {
		return (rv);
	}
	*data = req->buf;
	*szp  = strlen(req->buf);
	return (0);
}

int
nni_http_res_get_buf(nni_http_res *res, void **data, size_t *szp)
{
	int rv;

	if ((res->buf == NULL) && (rv = http_res_prepare(res)) != 0) {
		return (rv);
	}
	*data = res->buf;
	*szp  = strlen(res->buf);
	return (0);
}

int
nni_http_req_init(nni_http_req **reqp)
{
	nni_http_req *req;
	if ((req = NNI_ALLOC_STRUCT(req)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&req->hdrs, http_header, node);
	req->buf       = NULL;
	req->bufsz     = 0;
	req->data.data = NULL;
	req->data.size = 0;
	req->data.own  = false;
	req->vers      = NULL;
	req->meth      = NULL;
	req->uri       = NULL;
	*reqp          = req;
	return (0);
}

int
nni_http_res_init(nni_http_res **resp)
{
	nni_http_res *res;
	if ((res = NNI_ALLOC_STRUCT(res)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&res->hdrs, http_header, node);
	res->buf       = NULL;
	res->bufsz     = 0;
	res->data.data = NULL;
	res->data.size = 0;
	res->data.own  = false;
	res->vers      = NULL;
	res->rsn       = NULL;
	res->code      = 0;
	*resp          = res;
	return (0);
}

const char *
nni_http_req_get_method(nni_http_req *req)
{
	return (req->meth);
}

const char *
nni_http_req_get_uri(nni_http_req *req)
{
	return (req->uri);
}

const char *
nni_http_req_get_version(nni_http_req *req)
{
	return (req->vers);
}

const char *
nni_http_res_get_version(nni_http_res *res)
{
	return (res->vers);
}

int
nni_http_req_set_version(nni_http_req *req, const char *vers)
{
	return (http_set_string(&req->vers, vers));
}

int
nni_http_res_set_version(nni_http_res *res, const char *vers)
{
	return (http_set_string(&res->vers, vers));
}

int
nni_http_req_set_uri(nni_http_req *req, const char *uri)
{
	return (http_set_string(&req->uri, uri));
}

int
nni_http_req_set_method(nni_http_req *req, const char *meth)
{
	return (http_set_string(&req->meth, meth));
}

int
nni_http_res_set_status(nni_http_res *res, int status, const char *reason)
{
	int rv;
	if ((rv = http_set_string(&res->rsn, reason)) == 0) {
		res->code = status;
	}
	return (rv);
}

int
nni_http_res_get_status(nni_http_res *res)
{
	return (res->code);
}

const char *
nni_http_res_get_reason(nni_http_res *res)
{
	return (res->rsn);
}

static int
http_scan_line(void *vbuf, size_t n, size_t *lenp)
{
	size_t len;
	char   lc;
	char * buf = vbuf;

	lc = 0;
	for (len = 0; len < n; len++) {
		char c = buf[len];
		if (c == '\n') {
			// Technically we should be receiving CRLF, but
			// debugging is easier with just LF, so we behave
			// following Postel's Law.
			if (lc != '\r') {
				buf[len] = '\0';
			} else {
				buf[len - 1] = '\0';
			}
			*lenp = len + 1;
			return (0);
		}
		// If we have a control character (other than CR), or a CR
		// followed by anything other than LF, then its an error.
		if (((c < ' ') && (c != '\r')) || (lc == '\r')) {
			return (NNG_EPROTO);
		}
		lc = c;
	}
	// Scanned the entire content, but did not find a line.
	return (NNG_EAGAIN);
}

static int
http_req_parse_line(nni_http_req *req, void *line)
{
	int   rv;
	char *method;
	char *uri;
	char *version;

	method = line;
	if ((uri = strchr(method, ' ')) == NULL) {
		return (NNG_EPROTO);
	}
	*uri = '\0';
	uri++;

	if ((version = strchr(uri, ' ')) == NULL) {
		return (NNG_EPROTO);
	}
	*version = '\0';
	version++;

	if (((rv = nni_http_req_set_method(req, method)) != 0) ||
	    ((rv = nni_http_req_set_uri(req, uri)) != 0) ||
	    ((rv = nni_http_req_set_version(req, version)) != 0)) {
		return (rv);
	}
	return (0);
}

static int
http_res_parse_line(nni_http_res *res, uint8_t *line)
{
	int   rv;
	char *reason;
	char *codestr;
	char *version;
	int   status;

	version = (char *) line;
	if ((codestr = strchr(version, ' ')) == NULL) {
		return (NNG_EPROTO);
	}
	*codestr = '\0';
	codestr++;

	if ((reason = strchr(codestr, ' ')) == NULL) {
		return (NNG_EPROTO);
	}
	*reason = '\0';
	reason++;

	status = atoi(codestr);
	if ((status < 100) || (status > 999)) {
		return (NNG_EPROTO);
	}

	if (((rv = nni_http_res_set_status(res, status, reason)) != 0) ||
	    ((rv = nni_http_res_set_version(res, version)) != 0)) {
		return (rv);
	}
	return (0);
}

// nni_http_req_parse parses a request (but not any attached entity data).
// The amount of data consumed is returned in lenp.  Returns zero on
// success, NNG_EPROTO on parse failure, NNG_EAGAIN if more data is
// required, or NNG_ENOMEM on memory exhaustion.  Note that lenp may
// be updated even in the face of errors (esp. NNG_EAGAIN, which is
// not an error so much as a request for more data.)
int
nni_http_req_parse(nni_http_req *req, void *buf, size_t n, size_t *lenp)
{

	size_t len = 0;
	size_t cnt;
	int    rv = 0;

	for (;;) {
		uint8_t *line;
		if ((rv = http_scan_line(buf, n, &cnt)) != 0) {
			break;
		}

		len += cnt;
		line = buf;
		buf  = line + cnt;
		n -= cnt;

		if (*line == '\0') {
			break;
		}

		if (req->vers != NULL) {
			rv = http_parse_header(&req->hdrs, line);
		} else {
			rv = http_req_parse_line(req, line);
		}

		if (rv != 0) {
			break;
		}
	}

	*lenp = len;
	return (rv);
}

int
nni_http_res_parse(nni_http_res *res, void *buf, size_t n, size_t *lenp)
{

	size_t len = 0;
	size_t cnt;
	int    rv = 0;
	for (;;) {
		uint8_t *line;
		if ((rv = http_scan_line(buf, n, &cnt)) != 0) {
			break;
		}

		len += cnt;
		line = buf;
		buf  = line + cnt;
		n -= cnt;

		if (*line == '\0') {
			break;
		}

		if (res->vers != NULL) {
			rv = http_parse_header(&res->hdrs, line);
		} else {
			rv = http_res_parse_line(res, line);
		}

		if (rv != 0) {
			break;
		}
	}

	*lenp = len;
	return (rv);
}

int
nni_http_res_init_error(nni_http_res **resp, uint16_t err)
{
	char *        rsn;
	char          rsnbuf[80];
	char          html[1024];
	nni_http_res *res;

	if ((nni_http_res_init(&res)) != 0) {
		return (NNG_ENOMEM);
	}

	// Note that it is expected that redirect URIs will update the
	// payload to reflect the target location.
	switch (err) {
	case NNI_HTTP_STATUS_STATUS_MOVED_PERMANENTLY:
		rsn = "Moved Permanently";
		break;
	case NNI_HTTP_STATUS_MULTIPLE_CHOICES:
		rsn = "Multiple Choices";
		break;
	case NNI_HTTP_STATUS_FOUND:
		rsn = "Found";
		break;
	case NNI_HTTP_STATUS_SEE_OTHER:
		rsn = "See Other";
		break;
	case NNI_HTTP_STATUS_TEMPORARY_REDIRECT:
		rsn = "Temporary Redirect";
		break;
	case NNI_HTTP_STATUS_BAD_REQUEST:
		rsn = "Bad Request";
		break;
	case NNI_HTTP_STATUS_UNAUTHORIZED:
		rsn = "Unauthorized";
		break;
	case NNI_HTTP_STATUS_PAYMENT_REQUIRED:
		rsn = "Payment Required";
		break;
	case NNI_HTTP_STATUS_NOT_FOUND:
		rsn = "Not Found";
		break;
	case NNI_HTTP_STATUS_METHOD_NOT_ALLOWED:
		// Caller must also supply an Allow: header
		rsn = "Method Not Allowed";
		break;
	case NNI_HTTP_STATUS_NOT_ACCEPTABLE:
		rsn = "Not Acceptable";
		break;
	case NNI_HTTP_STATUS_REQUEST_TIMEOUT:
		rsn = "Request Timeout";
		break;
	case NNI_HTTP_STATUS_CONFLICT:
		rsn = "Conflict";
		break;
	case NNI_HTTP_STATUS_GONE:
		rsn = "Gone";
		break;
	case NNI_HTTP_STATUS_LENGTH_REQUIRED:
		rsn = "Length Required";
		break;
	case NNI_HTTP_STATUS_PAYLOAD_TOO_LARGE:
		rsn = "Payload Too Large";
		break;
	case NNI_HTTP_STATUS_FORBIDDEN:
		rsn = "Forbidden";
		break;
	case NNI_HTTP_STATUS_URI_TOO_LONG:
		rsn = "URI Too Long";
		break;
	case NNI_HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE:
		rsn = "Unsupported Media Type";
		break;
	case NNI_HTTP_STATUS_EXPECTATION_FAILED:
		rsn = "Expectation Failed";
		break;
	case NNI_HTTP_STATUS_UPGRADE_REQUIRED:
		// Caller must add "Upgrade:" header.
		rsn = "Upgrade Required";
		break;
	case NNI_HTTP_STATUS_INTERNAL_SERVER_ERROR:
		rsn = "Internal Server Error";
		break;
	case NNI_HTTP_STATUS_HTTP_VERSION_NOT_SUPP:
		rsn = "HTTP version not supported";
		break;
	case NNI_HTTP_STATUS_NOT_IMPLEMENTED:
		rsn = "Not Implemented";
		break;
	case NNI_HTTP_STATUS_SERVICE_UNAVAILABLE:
		rsn = "Service Unavailable";
		break;
	default:
		snprintf(rsnbuf, sizeof(rsnbuf), "HTTP error code %d", err);
		rsn = rsnbuf;
		break;
	}

	// very simple builtin error page
	snprintf(html, sizeof(html),
	    "<head><title>%d %s</title></head>"
	    "<body><p/><h1 align=\"center\">"
	    "<span style=\"font-size: 36px; border-radius: 5px; "
	    "background-color: black; color: white; padding: 7px; "
	    "font-family: Arial, sans serif;\">%d</span></h1>"
	    "<p align=\"center\">"
	    "<span style=\"font-size: 24px; font-family: Arial, sans serif;\">"
	    "%s</span></p></body>",
	    err, rsn, err, rsn);

	nni_http_res_set_status(res, err, rsn);
	nni_http_res_copy_data(res, html, strlen(html));
	nni_http_res_set_version(res, "HTTP/1.1");
	nni_http_res_set_header(
	    res, "Content-Type", "text/html; charset=UTF-8");
	// We could set the date, but we don't necessarily have a portable
	// way to get the time of day.

	*resp = res;
	return (0);
}