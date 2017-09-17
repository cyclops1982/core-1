/* Copyright (c) 2009-2017 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "strescape.h"
#include "istream.h"
#include "istream-concat.h"
#include "ostream.h"
#include "istream-dot.h"
#include "safe-mkstemp.h"
#include "restrict-access.h"
#include "anvil-client.h"
#include "master-service.h"
#include "master-service-ssl.h"
#include "iostream-ssl.h"
#include "rfc822-parser.h"
#include "message-date.h"
#include "auth-master.h"
#include "mail-storage-service.h"
#include "index/raw/raw-storage.h"
#include "lda-settings.h"
#include "lmtp-settings.h"
#include "mail-autoexpunge.h"
#include "lmtp-local.h"
#include "mail-deliver.h"
#include "message-address.h"
#include "main.h"
#include "client.h"
#include "commands.h"
#include "lmtp-proxy.h"

#define ERRSTR_TEMP_MAILBOX_FAIL "451 4.3.0 <%s> Temporary internal error"
#define ERRSTR_TEMP_USERDB_FAIL_PREFIX "451 4.3.0 <%s> "
#define ERRSTR_TEMP_USERDB_FAIL \
	ERRSTR_TEMP_USERDB_FAIL_PREFIX "Temporary user lookup failure"

#define LMTP_PROXY_DEFAULT_TIMEOUT_MSECS (1000*125)

int cmd_lhlo(struct client *client, const char *args)
{
	struct rfc822_parser_context parser;
	string_t *domain = t_str_new(128);
	const char *p;
	int ret = 0;

	if (*args == '\0') {
		client_send_line(client, "501 Missing hostname");
		return 0;
	}

	/* domain / address-literal */
	rfc822_parser_init(&parser, (const unsigned char *)args, strlen(args),
			   NULL);
	if (*args != '[')
		ret = rfc822_parse_dot_atom(&parser, domain);
	else {
		for (p = args+1; *p != ']'; p++) {
			if (*p == '\\' || *p == '[')
				break;
		}
		if (strcmp(p, "]") != 0)
			ret = -1;
	}
	if (ret < 0) {
		str_truncate(domain, 0);
		str_append(domain, "invalid");
	}

	client_state_reset(client, "LHLO");
	client_send_line(client, "250-%s", client->my_domain);
	if (master_service_ssl_is_enabled(master_service) &&
	    client->ssl_iostream == NULL)
		client_send_line(client, "250-STARTTLS");
	if (client_is_trusted(client))
		client_send_line(client, "250-XCLIENT ADDR PORT TTL TIMEOUT");
	client_send_line(client, "250-8BITMIME");
	client_send_line(client, "250-ENHANCEDSTATUSCODES");
	client_send_line(client, "250 PIPELINING");

	i_free(client->lhlo);
	client->lhlo = i_strdup(str_c(domain));
	client_state_set(client, "LHLO", "");
	return 0;
}

int cmd_starttls(struct client *client)
{
	struct ostream *plain_output = client->output;
	const char *error;

	if (client->ssl_iostream != NULL) {
		o_stream_nsend_str(client->output,
				   "443 5.5.1 TLS is already active.\r\n");
		return 0;
	}

	if (master_service_ssl_init(master_service,
				    &client->input, &client->output,
				    &client->ssl_iostream, &error) < 0) {
		i_error("TLS initialization failed: %s", error);
		o_stream_nsend_str(client->output,
			"454 4.7.0 Internal error, TLS not available.\r\n");
		return 0;
	}
	o_stream_nsend_str(plain_output,
			   "220 2.0.0 Begin TLS negotiation now.\r\n");
	if (ssl_iostream_handshake(client->ssl_iostream) < 0) {
		client_destroy(client, NULL, NULL);
		return -1;
	}
	return 0;
}

int cmd_mail(struct client *client, const char *args)
{
	struct smtp_address *address;
	enum smtp_param_parse_error pperror;
	const char *error;

	if (client->state.mail_from != NULL) {
		client_send_line(client, "503 5.5.1 MAIL already given");
		return 0;
	}

	if (strncasecmp(args, "FROM:", 5) != 0) {
		client_send_line(client, "501 5.5.4 Invalid parameters");
		return 0;
	}
	if (smtp_address_parse_path_full(pool_datastack_create(), args + 5,
					 SMTP_ADDRESS_PARSE_FLAG_ALLOW_EMPTY,
					 &address, &error, &args) < 0) {
		client_send_line(client, "501 5.5.4 Invalid FROM: %s", error);
		return 0;
	}
	if (*args == ' ')
		args++;
	else if (*args != '\0') {
		client_send_line(client, "501 5.5.4 Invalid FROM: "
			"Invalid character in path");
		return 0;
	}

	/* [SP Mail-parameters] */
	if (smtp_params_mail_parse(client->state_pool, args,
		SMTP_CAPABILITY_8BITMIME, FALSE,
		&client->state.mail_params, &pperror, &error) < 0) {
		switch (pperror) {
		case SMTP_PARAM_PARSE_ERROR_BAD_SYNTAX:
			client_send_line(client, "501 5.5.4 %s", error);
			break;
		case SMTP_PARAM_PARSE_ERROR_NOT_SUPPORTED:
			client_send_line(client, "555 5.5.4 %s", error);
			break;
		default:
			i_unreached();
		}
		return 0;
	}

	client->state.mail_from =
		smtp_address_clone(client->state_pool, address);
	p_array_init(&client->state.rcpt_to, client->state_pool, 64);
	client_send_line(client, "250 2.1.0 OK");
	client_state_set(client, "MAIL FROM",
		smtp_address_encode(address));

	if (client->lmtp_set->lmtp_user_concurrency_limit > 0) {
		/* connect to anvil before dropping privileges */
		lmtp_anvil_init();
	}

	client->state.mail_from_timeval = ioloop_timeval;
	return 0;
}

static bool
client_proxy_rcpt_parse_fields(struct lmtp_proxy_rcpt_settings *set,
			       const char *const *args, const char **address)
{
	const char *p, *key, *value;
	bool proxying = FALSE, port_set = FALSE;

	for (; *args != NULL; args++) {
		p = strchr(*args, '=');
		if (p == NULL) {
			key = *args;
			value = "";
		} else {
			key = t_strdup_until(*args, p);
			value = p + 1;
		}

		if (strcmp(key, "proxy") == 0)
			proxying = TRUE;
		else if (strcmp(key, "host") == 0)
			set->host = value;
		else if (strcmp(key, "hostip") == 0) {
			if (net_addr2ip(value, &set->hostip) < 0) {
				i_error("proxy: Invalid hostip %s", value);
				return FALSE;
			}
		} else if (strcmp(key, "port") == 0) {
			if (net_str2port(value, &set->port) < 0) {
				i_error("proxy: Invalid port number %s", value);
				return FALSE;
			}
			port_set = TRUE;
		} else if (strcmp(key, "proxy_timeout") == 0) {
			if (str_to_uint(value, &set->timeout_msecs) < 0) {
				i_error("proxy: Invalid proxy_timeout value %s", value);
				return FALSE;
			}
			set->timeout_msecs *= 1000;
		} else if (strcmp(key, "protocol") == 0) {
			if (strcmp(value, "lmtp") == 0) {
				set->protocol = SMTP_PROTOCOL_LMTP;
				if (!port_set)
					set->port = 24;
			} else if (strcmp(value, "smtp") == 0) {
				set->protocol = SMTP_PROTOCOL_SMTP;
				if (!port_set)
					set->port = 25;
			} else {
				i_error("proxy: Unknown protocol %s", value);
				return FALSE;
			}
		} else if (strcmp(key, "user") == 0 ||
			   strcmp(key, "destuser") == 0) {
			/* changing the username */
			*address = value;
		} else {
			/* just ignore it */
		}
	}
	if (proxying && set->host == NULL) {
		i_error("proxy: host not given");
		return FALSE;
	}
	return proxying;
}

static bool
client_proxy_is_ourself(const struct client *client,
			const struct lmtp_proxy_rcpt_settings *set)
{
	struct ip_addr ip;

	if (set->port != client->local_port)
		return FALSE;

	if (set->hostip.family != 0)
		ip = set->hostip;
	else {
		if (net_addr2ip(set->host, &ip) < 0)
			return FALSE;
	}
	if (!net_ip_compare(&ip, &client->local_ip))
		return FALSE;
	return TRUE;
}

static bool client_proxy_rcpt(struct client *client,
			      struct smtp_address *address,
			      const char *username, const char *detail, char delim,
			      struct smtp_params_rcpt *params)
{
	struct auth_master_connection *auth_conn;
	struct lmtp_proxy_rcpt_settings set;
	struct auth_user_info info;
	struct mail_storage_service_input input;
	const char *const *fields, *errstr, *orig_username = username;
	struct smtp_address *user;
	pool_t pool;
	int ret;

	i_zero(&input);
	input.module = input.service = "lmtp";
	mail_storage_service_init_settings(storage_service, &input);

	i_zero(&info);
	info.service = master_service_get_name(master_service);
	info.local_ip = client->local_ip;
	info.remote_ip = client->remote_ip;
	info.local_port = client->local_port;
	info.remote_port = client->remote_port;

	pool = pool_alloconly_create("auth lookup", 1024);
	auth_conn = mail_storage_service_get_auth_conn(storage_service);
	ret = auth_master_pass_lookup(auth_conn, username, &info,
				      pool, &fields);
	if (ret <= 0) {
		errstr = ret < 0 && fields[0] != NULL ? t_strdup(fields[0]) :
			t_strdup_printf(ERRSTR_TEMP_USERDB_FAIL,
				smtp_address_encode(address));
		pool_unref(&pool);
		if (ret < 0) {
			client_send_line(client, "%s", errstr);
			return TRUE;
		} else {
			/* user not found from passdb. try userdb also. */
			return FALSE;
		}
	}

	i_zero(&set);
	set.port = client->local_port;
	set.protocol = SMTP_PROTOCOL_LMTP;
	set.timeout_msecs = LMTP_PROXY_DEFAULT_TIMEOUT_MSECS;
	set.params = *params;

	if (!client_proxy_rcpt_parse_fields(&set, fields, &username)) {
		/* not proxying this user */
		pool_unref(&pool);
		return FALSE;
	}
	if (strcmp(username, orig_username) != 0) {
		if (smtp_address_parse_username(pool_datastack_create(),
						username, &user, &errstr) < 0) {
			i_error("%s: Username `%s' returned by passdb lookup is not a valid SMTP address",
				orig_username, username);
			client_send_line(client, "550 5.3.5 <%s> "
				"Internal user lookup failure",
				smtp_address_encode(address));
			pool_unref(&pool);
			return FALSE;
		}
		/* username changed. change the address as well */
		if (*detail == '\0') {
			address = user;
		} else {
			address = smtp_address_add_detail_temp(user, detail, delim);
		}
	} else if (client_proxy_is_ourself(client, &set)) {
		i_error("Proxying to <%s> loops to itself", username);
		client_send_line(client, "554 5.4.6 <%s> "
				 "Proxying loops to itself",
				 smtp_address_encode(address));
		pool_unref(&pool);
		return TRUE;
	}

	if (client->proxy_ttl <= 1) {
		i_error("Proxying to <%s> appears to be looping (TTL=0)",
			username);
		client_send_line(client, "554 5.4.6 <%s> "
				 "Proxying appears to be looping (TTL=0)",
				 username);
		pool_unref(&pool);
		return TRUE;
	}
	if (array_count(&client->state.rcpt_to) != 0) {
		client_send_line(client, "451 4.3.0 <%s> "
			"Can't handle mixed proxy/non-proxy destinations",
			smtp_address_encode(address));
		pool_unref(&pool);
		return TRUE;
	}
	if (client->proxy == NULL) {
		struct lmtp_proxy_settings proxy_set;

		i_zero(&proxy_set);
		proxy_set.my_hostname = client->my_domain;
		proxy_set.dns_client_socket_path = dns_client_socket_path;
		proxy_set.session_id = client->state.session_id;
		proxy_set.source_ip = client->remote_ip;
		proxy_set.source_port = client->remote_port;
		proxy_set.proxy_ttl = client->proxy_ttl-1;

		client->proxy = lmtp_proxy_init(&proxy_set, client->output);
		lmtp_proxy_mail_from(client->proxy, client->state.mail_from,
			&client->state.mail_params);
	}
	if (lmtp_proxy_add_rcpt(client->proxy, address, &set) < 0)
		client_send_line(client, "451 4.4.0 Remote server not answering");
	else
		client_send_line(client, "250 2.1.5 OK");
	pool_unref(&pool);
	return TRUE;
}

int cmd_rcpt(struct client *client, const char *args)
{
	struct mail_recipient *rcpt;
	struct mail_storage_service_input input;
	struct smtp_address *address;
	const char *username, *detail;
	enum smtp_param_parse_error pperror;
	const char *error = NULL;
	char delim = '\0';
	int ret = 0;

	if (client->state.mail_from == NULL) {
		client_send_line(client, "503 5.5.1 MAIL needed first");
		return 0;
	}

	if (strncasecmp(args, "TO:", 3) != 0) {
		client_send_line(client, "501 5.5.4 Invalid parameters");
		return 0;
	}
	if (smtp_address_parse_path_full(pool_datastack_create(), args + 3,
					 SMTP_ADDRESS_PARSE_FLAG_ALLOW_LOCALPART,
					 &address, &error, &args) < 0) {
		client_send_line(client, "501 5.5.4 Invalid TO: %s", error);
		return 0;
	}
	if (*args == ' ')
		args++;
	else if (*args != '\0') {
		client_send_line(client, "501 5.5.4 Invalid TO: "
			"Invalid character in path");
		return 0;
	}

	rcpt = p_new(client->state_pool, struct mail_recipient, 1);
	rcpt->client = client;

	/* [SP Rcpt-parameters] */
	if (smtp_params_rcpt_parse(client->state_pool, args,
		SMTP_CAPABILITY_DSN, FALSE,
		&rcpt->params, &pperror, &error) < 0) {
		switch (pperror) {
		case SMTP_PARAM_PARSE_ERROR_BAD_SYNTAX:
			client_send_line(client, "501 5.5.4 %s", error);
			break;
		case SMTP_PARAM_PARSE_ERROR_NOT_SUPPORTED:
			client_send_line(client, "555 5.5.4 %s", error);
			break;
		default:
			i_unreached();
		}
		return 0;
	}

	smtp_address_detail_parse_temp(
		client->unexpanded_lda_set->recipient_delimiter,
		address, &username, &delim, &detail);

	client_state_set(client, "RCPT TO",
		smtp_address_encode(address));

	if (client->lmtp_set->lmtp_proxy) {
		if (client_proxy_rcpt(client, address, username, detail, delim,
				      &rcpt->params))
			return 0;
	}

	/* Use a unique session_id for each mail delivery. This is especially
	   important for stats process to not see duplicate sessions. */
	if (array_count(&client->state.rcpt_to) == 0)
		rcpt->session_id = client->state.session_id;
	else {
		rcpt->session_id =
			p_strdup_printf(client->state_pool, "%s:%u",
					client->state.session_id,
					array_count(&client->state.rcpt_to)+1);
	}

	i_zero(&input);
	input.module = input.service = "lmtp";
	input.username = username;
	input.local_ip = client->local_ip;
	input.remote_ip = client->remote_ip;
	input.local_port = client->local_port;
	input.remote_port = client->remote_port;
	input.session_id = rcpt->session_id;

	ret = mail_storage_service_lookup(storage_service, &input,
					  &rcpt->service_user, &error);

	if (ret < 0) {
		i_error("Failed to lookup user %s: %s", username, error);
		client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL,
			smtp_address_encode(address));
		return 0;
	}
	if (ret == 0) {
		client_send_line(client,
				 "550 5.1.1 <%s> User doesn't exist: %s",
				 smtp_address_encode(address), username);
		return 0;
	}
	if (client->proxy != NULL) {
		/* NOTE: if this restriction is ever removed, we'll also need
		   to send different message bodies to local and proxy
		   (with and without Return-Path: header) */
		client_send_line(client, "451 4.3.0 <%s> "
			"Can't handle mixed proxy/non-proxy destinations",
			smtp_address_encode(address));
		mail_storage_service_user_unref(&rcpt->service_user);
		return 0;
	}

	rcpt->address = smtp_address_clone(client->state_pool, address);
	rcpt->detail = p_strdup(client->state_pool, detail);

	if (client->lmtp_set->lmtp_user_concurrency_limit == 0) {
		(void)cmd_rcpt_finish(client, rcpt);
		return 0;
	} else {
		/* NOTE: username may change as the result of the userdb
		   lookup. Look up the new one via service_user. */
		const struct mail_storage_service_input *input =
			mail_storage_service_user_get_input(rcpt->service_user);
		const char *query = t_strconcat("LOOKUP\t",
			master_service_get_name(master_service),
			"/", str_tabescape(input->username), NULL);
		io_remove(&client->io);
		rcpt->anvil_query = anvil_client_query(anvil, query,
					rcpt_anvil_lookup_callback, rcpt);
		/* stop processing further commands while anvil query is
		   pending */
		return rcpt->anvil_query == NULL ? 0 : -1;
	}
}

int cmd_quit(struct client *client, const char *args ATTR_UNUSED)
{
	client_send_line(client, "221 2.0.0 OK");
	/* don't log the (state name) for successful QUITs */
	i_info("Disconnect from %s: Successful quit", client_remote_id(client));
	client->disconnected = TRUE;
	client_destroy(client, NULL, NULL);
	return -1;
}

int cmd_vrfy(struct client *client, const char *args ATTR_UNUSED)
{
	client_send_line(client, "252 2.3.3 Try RCPT instead");
	return 0;
}

int cmd_rset(struct client *client, const char *args ATTR_UNUSED)
{
	client_state_reset(client, "RSET");
	client_send_line(client, "250 2.0.0 OK");
	return 0;
}

int cmd_noop(struct client *client, const char *args ATTR_UNUSED)
{
	client_send_line(client, "250 2.0.0 OK");
	return 0;
}

static void client_rcpt_fail_all(struct client *client)
{
	struct mail_recipient *const *rcptp;

	array_foreach(&client->state.rcpt_to, rcptp) {
		client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL,
				 smtp_address_encode((*rcptp)->address));
	}
}

static struct istream *client_get_input(struct client *client)
{
	struct client_state *state = &client->state;
	struct istream *cinput, *inputs[3];

	inputs[0] = i_stream_create_from_data(state->added_headers,
					      strlen(state->added_headers));

	if (state->mail_data_output != NULL) {
		o_stream_unref(&state->mail_data_output);
		inputs[1] = i_stream_create_fd(state->mail_data_fd,
					       MAIL_READ_FULL_BLOCK_SIZE);
		i_stream_set_init_buffer_size(inputs[1],
					      MAIL_READ_FULL_BLOCK_SIZE);
	} else {
		inputs[1] = i_stream_create_from_data(state->mail_data->data,
						      state->mail_data->used);
	}
	inputs[2] = NULL;

	cinput = i_stream_create_concat(inputs);
	i_stream_set_name(cinput, "<lmtp DATA>");
	i_stream_unref(&inputs[0]);
	i_stream_unref(&inputs[1]);
	return cinput;
}

static int client_open_raw_mail(struct client *client, struct istream *input)
{
	static const char *wanted_headers[] = {
		"From", "To", "Message-ID", "Subject", "Return-Path",
		NULL
	};
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
	struct mailbox_header_lookup_ctx *headers_ctx;
	enum mail_error error;

	if (raw_mailbox_alloc_stream(client->raw_mail_user, input,
				     (time_t)-1, smtp_address_encode(client->state.mail_from),
				     &box) < 0) {
		i_error("Can't open delivery mail as raw: %s",
			mailbox_get_last_internal_error(box, &error));
		mailbox_free(&box);
		client_rcpt_fail_all(client);
		return -1;
	}

	trans = mailbox_transaction_begin(box, 0, __func__);

	headers_ctx = mailbox_header_lookup_init(box, wanted_headers);
	client->state.raw_mail = mail_alloc(trans, 0, headers_ctx);
	mailbox_header_lookup_unref(&headers_ctx);
	mail_set_seq(client->state.raw_mail, 1);
	return 0;
}

static void
client_input_data_write_local(struct client *client, struct istream *input)
{
	struct mail_deliver_session *session;
	uid_t old_uid, first_uid;

	if (client_open_raw_mail(client, input) < 0)
		return;

	session = mail_deliver_session_init();
	old_uid = geteuid();
	first_uid = client_deliver_to_rcpts(client, session);
	mail_deliver_session_deinit(&session);

	if (client->state.first_saved_mail != NULL) {
		struct mail *mail = client->state.first_saved_mail;
		struct mailbox_transaction_context *trans = mail->transaction;
		struct mailbox *box = trans->box;
		struct mail_user *user = box->storage->user;

		/* just in case these functions are going to write anything,
		   change uid back to user's own one */
		if (first_uid != old_uid) {
			if (seteuid(0) < 0)
				i_fatal("seteuid(0) failed: %m");
			if (seteuid(first_uid) < 0)
				i_fatal("seteuid() failed: %m");
		}

		mail_free(&mail);
		mailbox_transaction_rollback(&trans);
		mailbox_free(&box);
		mail_user_autoexpunge(user);
		mail_user_unref(&user);
	}

	if (old_uid == 0) {
		/* switch back to running as root, since that's what we're
		   practically doing anyway. it's also important in case we
		   lose e.g. config connection and need to reconnect to it. */
		if (seteuid(0) < 0)
			i_fatal("seteuid(0) failed: %m");
		/* enable core dumping again. we need to chdir also to
		   root-owned directory to get core dumps. */
		restrict_access_allow_coredumps(TRUE);
		if (chdir(base_dir) < 0)
			i_error("chdir(%s) failed: %m", base_dir);
	}
}

static void client_input_data_finish(struct client *client)
{
	client_io_reset(client);
	client_state_reset(client, "DATA finished");
	if (i_stream_have_bytes_left(client->input))
		client_input_handle(client);
}

static void client_proxy_finish(void *context)
{
	struct client *client = context;

	lmtp_proxy_deinit(&client->proxy);
	client_input_data_finish(client);
}

static const char *client_get_added_headers(struct client *client)
{
	string_t *str = t_str_new(200);
	void **sets;
	const struct lmtp_settings *lmtp_set;
	const struct smtp_address *rcpt_to = NULL;
	const char *host;

	if (array_count(&client->state.rcpt_to) == 1) {
		struct mail_recipient *const *rcptp =
			array_idx(&client->state.rcpt_to, 0);

		sets = mail_storage_service_user_get_set((*rcptp)->service_user);
		lmtp_set = sets[3];

		switch (lmtp_set->parsed_lmtp_hdr_delivery_address) {
		case LMTP_HDR_DELIVERY_ADDRESS_NONE:
			break;
		case LMTP_HDR_DELIVERY_ADDRESS_FINAL:
			rcpt_to = (*rcptp)->address;
			break;
		case LMTP_HDR_DELIVERY_ADDRESS_ORIGINAL:
			rcpt_to = (*rcptp)->params.orcpt.addr;
			if (rcpt_to == NULL)
				rcpt_to = (*rcptp)->address;
			break;
		}
	}

	/* don't set Return-Path when proxying so it won't get added twice */
	if (array_count(&client->state.rcpt_to) > 0) {
		str_printfa(str, "Return-Path: <%s>\r\n",
			    smtp_address_encode(client->state.mail_from));
		if (rcpt_to != NULL) {
			str_printfa(str, "Delivered-To: %s\r\n",
				smtp_address_encode(rcpt_to));
		}
	}

	str_printfa(str, "Received: from %s", client->lhlo);
	host = net_ip2addr(&client->remote_ip);
	if (host[0] != '\0')
		str_printfa(str, " ([%s])", host);
	str_append(str, "\r\n");
	if (client->ssl_iostream != NULL) {
		str_printfa(str, "\t(using %s)\r\n",
			    ssl_iostream_get_security_string(client->ssl_iostream));
	}
	str_printfa(str, "\tby %s with LMTP id %s",
		    client->my_domain, client->state.session_id);

	str_append(str, "\r\n\t");
	if (rcpt_to != NULL)
		str_printfa(str, "for <%s>", smtp_address_encode(rcpt_to));
	str_printfa(str, "; %s\r\n", message_date_create(ioloop_time));
	return str_c(str);
}

static void client_input_data_write(struct client *client)
{
	struct istream *input;

	/* stop handling client input until saving/proxying is finished */
	timeout_remove(&client->to_idle);
	io_remove(&client->io);
	i_stream_destroy(&client->dot_input);

	client->state.data_end_timeval = ioloop_timeval;

	input = client_get_input(client);
	if (array_count(&client->state.rcpt_to) != 0)
		client_input_data_write_local(client, input);
	if (client->proxy != NULL) {
		client_state_set(client, "DATA", "proxying");
		lmtp_proxy_start(client->proxy, input,
				 client_proxy_finish, client);
	} else {
		client_input_data_finish(client);
	}
	i_stream_unref(&input);
}

static int client_input_add_file(struct client *client,
				 const unsigned char *data, size_t size)
{
	struct client_state *state = &client->state;
	string_t *path;
	int fd;

	if (state->mail_data_output != NULL) {
		/* continue writing to file */
		if (o_stream_send(state->mail_data_output,
				  data, size) != (ssize_t)size)
			return -1;
		return 0;
	}

	/* move everything to a temporary file. */
	path = t_str_new(256);
	mail_user_set_get_temp_prefix(path, client->raw_mail_user->set);
	fd = safe_mkstemp_hostpid(path, 0600, (uid_t)-1, (gid_t)-1);
	if (fd == -1) {
		i_error("Temp file creation to %s failed: %m", str_c(path));
		return -1;
	}

	/* we just want the fd, unlink it */
	if (i_unlink(str_c(path)) < 0) {
		/* shouldn't happen.. */
		i_close_fd(&fd);
		return -1;
	}

	state->mail_data_fd = fd;
	state->mail_data_output = o_stream_create_fd_file(fd, 0, FALSE);
	o_stream_set_name(state->mail_data_output, str_c(path));
	o_stream_cork(state->mail_data_output);

	o_stream_nsend(state->mail_data_output,
		       state->mail_data->data, state->mail_data->used);
	o_stream_nsend(client->state.mail_data_output, data, size);
	if (o_stream_flush(client->state.mail_data_output) < 0) {
		i_error("write(%s) failed: %s", str_c(path),
			o_stream_get_error(client->state.mail_data_output));
		return -1;
	}
	return 0;
}

static int
client_input_add(struct client *client, const unsigned char *data, size_t size)
{
	if (client->state.mail_data->used + size <=
	    CLIENT_MAIL_DATA_MAX_INMEMORY_SIZE &&
	    client->state.mail_data_output == NULL) {
		buffer_append(client->state.mail_data, data, size);
		return 0;
	} else {
		return client_input_add_file(client, data, size);
	}
}

static void client_input_data_handle(struct client *client)
{
	const unsigned char *data;
	size_t size;
	ssize_t ret;

	while ((ret = i_stream_read(client->dot_input)) > 0 || ret == -2) {
		data = i_stream_get_data(client->dot_input, &size);
		if (client_input_add(client, data, size) < 0) {
			client_destroy(client, "451 4.3.0",
				       "Temporary internal failure");
			return;
		}
		i_stream_skip(client->dot_input, size);
	}
	if (ret == 0)
		return;

	if (client->dot_input->stream_errno != 0) {
		/* client probably disconnected */
		client_destroy(client, NULL, NULL);
		return;
	}

	/* the ending "." line was seen. begin saving the mail. */
	client_input_data_write(client);
}

static void client_input_data(struct client *client)
{
	if (client_input_read(client) < 0)
		return;

	client_input_data_handle(client);
}

int cmd_data(struct client *client, const char *args ATTR_UNUSED)
{
	if (client->state.mail_from == NULL) {
		client_send_line(client, "503 5.5.1 MAIL needed first");
		return 0;
	}
	if (array_count(&client->state.rcpt_to) == 0 && client->proxy == NULL) {
		client_send_line(client, "554 5.5.1 No valid recipients");
		return 0;
	}

	client->state.added_headers =
		p_strdup(client->state_pool, client_get_added_headers(client));

	i_assert(client->state.mail_data == NULL);
	client->state.mail_data = buffer_create_dynamic(default_pool, 1024*64);

	i_assert(client->dot_input == NULL);
	client->dot_input = i_stream_create_dot(client->input, TRUE);
	client_send_line(client, "354 OK");
	/* send the DATA reply immediately before we start handling any data */
	o_stream_uncork(client->output);

	io_remove(&client->io);
	client_state_set(client, "DATA", "");
	client->io = io_add(client->fd_in, IO_READ, client_input_data, client);
	client_input_data_handle(client);
	return -1;
}

int cmd_xclient(struct client *client, const char *args)
{
	const char *const *tmp;
	struct ip_addr remote_ip;
	in_port_t remote_port = 0;
	unsigned int ttl = UINT_MAX, timeout_secs = 0;
	bool args_ok = TRUE;

	if (!client_is_trusted(client)) {
		client_send_line(client, "550 You are not from trusted IP");
		return 0;
	}
	remote_ip.family = 0;
	for (tmp = t_strsplit(args, " "); *tmp != NULL; tmp++) {
		if (strncasecmp(*tmp, "ADDR=", 5) == 0) {
			const char *addr = *tmp + 5;
			bool ipv6 = FALSE;

			if (strncasecmp(addr, "IPV6:", 5) == 0) {
				addr += 5;
				ipv6 = TRUE;
			}
			if (net_addr2ip(addr, &remote_ip) < 0 ||
			    (ipv6 && remote_ip.family != AF_INET6))
				args_ok = FALSE;
		} else if (strncasecmp(*tmp, "PORT=", 5) == 0) {
			if (net_str2port(*tmp + 5, &remote_port) < 0)
				args_ok = FALSE;
		} else if (strncasecmp(*tmp, "TTL=", 4) == 0) {
			if (str_to_uint(*tmp + 4, &ttl) < 0)
				args_ok = FALSE;
		} else if (strncasecmp(*tmp, "TIMEOUT=", 8) == 0) {
			if (str_to_uint(*tmp + 8, &timeout_secs) < 0)
				args_ok = FALSE;
		}
	}
	if (!args_ok) {
		client_send_line(client, "501 Invalid parameters");
		return 0;
	}

	/* args ok, set them and reset the state */
	client_state_reset(client, "XCLIENT");
	if (remote_ip.family != 0)
		client->remote_ip = remote_ip;
	if (remote_port != 0)
		client->remote_port = remote_port;
	if (ttl != UINT_MAX)
		client->proxy_ttl = ttl;
	client->proxy_timeout_secs = timeout_secs;
	client_send_line(client, "220 %s %s", client->my_domain,
			 client->lmtp_set->login_greeting);
	return 0;
}
