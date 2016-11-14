/*
 * delaydns.c - A DNS proxy that adds delay to replies
 *
 * Copyright (c) 2016, NLnet Labs. All rights reserved.
 * 
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <getdns/getdns_extra.h>
#include <stdio.h>
#include <string.h>


typedef struct transaction_t {
	getdns_transaction_t    request_id;
	getdns_dict            *request;
	getdns_dict            *response;

	/* For scheduling the delay */
	long                    delay;
	getdns_context         *context;
	getdns_eventloop       *loop;
	getdns_eventloop_event  ev;
} transaction_t;


void transaction_destroy(transaction_t *trans)
{
	getdns_dict_destroy(trans->response);
	getdns_dict_destroy(trans->request);
	free(trans);
}


void fatal_cleanup(transaction_t *trans)
{
	(void) getdns_reply(trans->context, NULL, trans->request_id);
	(void) getdns_context_set_listen_addresses(trans->context, NULL, NULL, NULL);
	transaction_destroy(trans);
}


void delay_cb(void *userarg)
{
	transaction_t *trans = userarg;

	trans->loop->vmt->clear(trans->loop, &trans->ev);
	if (getdns_reply(trans->context, trans->response, trans->request_id))
		(void) getdns_reply(trans->context, NULL, trans->request_id);

	transaction_destroy(trans);
}


void response_cb(getdns_context *context, getdns_callback_type_t callback_type,
    getdns_dict *response, void *userarg, getdns_transaction_t trans_id)
{
	transaction_t *trans = userarg;
	uint32_t qid;

	trans->response = response;
	if (callback_type != GETDNS_CALLBACK_COMPLETE) {
		(void) getdns_dict_set_int(
		    trans->request, "/header/rcode", GETDNS_RCODE_SERVFAIL);
		if (getdns_reply(context, trans->request, trans->request_id))
			(void) getdns_reply(context, NULL, trans->request_id);

		transaction_destroy(trans);
		return;
	}
	if (!getdns_dict_get_int(trans->request, "/header/id", &qid)
	&&  !getdns_dict_set_int(response, "/replies_tree/0/header/id", qid)
	&& (( trans->delay > 0 
	   && !getdns_context_get_eventloop(context, &trans->loop)
	   && !trans->loop->vmt->schedule(trans->loop, -1, trans->delay, &trans->ev)
	    ) || getdns_reply(context, response, trans->request_id)
	      || getdns_reply(context, NULL, trans->request_id)))
		return;
	
	fatal_cleanup(trans);
}


void handler(getdns_context *context, getdns_callback_type_t callback_type,
    getdns_dict *request, void *userarg, getdns_transaction_t request_id)
{
	getdns_bindata  *qname;
	char            *qname_str = NULL;
	uint32_t         qtype;
	transaction_t   *trans = NULL;
	getdns_dict     *qext = NULL;
	getdns_dict     *header;
	getdns_dict     *opt_params;
	uint32_t         opt_type;

	(void) userarg;

	if ((trans = malloc(sizeof(transaction_t)))) {
		(void) memset(trans, 0, sizeof(transaction_t));
		trans->request_id = request_id;
		trans->request = request;
		trans->context = context;
		trans->ev.userarg = trans;
		trans->ev.timeout_cb = delay_cb;
	}
	if (!trans
	||  !(qext = getdns_dict_create())
	||  getdns_dict_get_dict(request, "header", &header)
	||  getdns_dict_set_dict(qext, "header", header)

	||  (  getdns_dict_get_int(request, "/additional/0/type", &opt_type) == 0
	    && opt_type == GETDNS_RRTYPE_OPT
	    && (  getdns_dict_get_dict(request, "/additional/0", &opt_params)
	       || getdns_dict_set_dict(qext, "add_opt_parameters", opt_params)))

	||  getdns_dict_get_bindata(request, "/question/qname", &qname)
	||  getdns_convert_dns_name_to_fqdn(qname, &qname_str)
	||  ((trans->delay = strtol(qname_str, NULL, 10)), 0)
	||  getdns_dict_get_int(request, "/question/qtype", &qtype)
	||  getdns_general(context, qname_str, qtype, qext, trans, NULL, response_cb))
		fatal_cleanup(trans);

	getdns_dict_destroy(qext);
	free(qname_str);
}


int main(int argc, const char **argv)
{
	getdns_context   *context   = NULL;
	getdns_list      *upstreams = NULL;
	getdns_list      *listeners = NULL;
	getdns_list      *addr_list;
	const char       *addr_str;
	getdns_dict      *address   = NULL;
	size_t            n_upstreams = 0, n_listeners = 0, i;
	getdns_return_t   r = GETDNS_RETURN_GOOD;

	/* Make upstreams and listeners list */
	if (!(upstreams = getdns_list_create())
	||  !(listeners = getdns_list_create()))
		r = GETDNS_RETURN_MEMORY_ERROR;

	else while (--argc) {
		if (argv[1][0] == '@') {
			addr_str = argv[1] + 1;
			addr_list = upstreams;
			i = n_upstreams++;
		} else {
			addr_str = argv[1];
			addr_list = listeners;
			i = n_listeners++;
		}
		if ((r = getdns_str2dict(addr_str, &address))
		||  (r = getdns_list_set_dict(addr_list, i, address)))
			break;

		getdns_dict_destroy(address);
		address = NULL;
		argv++;
	}
	if (r || n_listeners < 1 || n_upstreams < 1)
		fprintf(stderr,"usage: %s [ @<authoritative IP> ... ] \\"
			       "          [ <listen IP> ... ]\n", *argv);

	/* Configure getdns context */
	else if (! r
	     &&  !(r = getdns_context_create(&context, 0))
	     &&  !(r = getdns_context_set_resolution_type(context, GETDNS_RESOLUTION_STUB))
	     &&  !(r = getdns_context_set_upstream_recursive_servers(context, upstreams))
	     &&  !(r = getdns_context_set_listen_addresses(context, listeners, NULL, handler)))

		getdns_context_run(context);

	getdns_dict_destroy(address);
	getdns_list_destroy(upstreams);
	getdns_list_destroy(listeners);
	getdns_context_destroy(context);
	if (r) {
		fprintf(stderr, "%s\n", getdns_get_errorstr_by_id(r));
		exit(EXIT_FAILURE);
	}
	return 0;
}
