#include "config.h"
#include <ccan/array_size/array_size.h>
#include <ccan/cast/cast.h>
#include <ccan/htable/htable_type.h>
#include <ccan/tal/str/str.h>
#include <common/bolt11.h>
#include <common/bolt12_merkle.h>
#include <common/gossmap.h>
#include <common/gossmods_listpeerchannels.h>
#include <common/json_param.h>
#include <common/json_stream.h>
#include <common/memleak.h>
#include <common/pseudorand.h>
#include <common/utils.h>
#include <errno.h>
#include <plugins/renepay/json.h>
#include <plugins/renepay/mods.h>
#include <plugins/renepay/payplugin.h>
#include <plugins/renepay/routetracker.h>
#include <plugins/renepay/sendpay.h>
#include <stdio.h>

// TODO(eduardo): notice that pending attempts performed with another
// pay plugin are not considered by the uncertainty network in renepay,
// it would be nice if listsendpay would give us the route of pending
// sendpays.

struct pay_plugin *pay_plugin;

static void memleak_mark(struct plugin *p, struct htable *memtable)
{
	memleak_scan_obj(memtable, pay_plugin);
	memleak_scan_htable(memtable,
			    &pay_plugin->uncertainty->chan_extra_map->raw);
	memleak_scan_htable(memtable, &pay_plugin->payment_map->raw);
	memleak_scan_htable(memtable, &pay_plugin->pending_routes->raw);
}

static const char *init(struct command *init_cmd,
			const char *buf UNUSED, const jsmntok_t *config UNUSED)
{
	struct plugin *p = init_cmd->plugin;
	size_t num_channel_updates_rejected = 0;

	tal_steal(p, pay_plugin);
	pay_plugin->plugin = p;
	pay_plugin->last_time = 0;

	rpc_scan(init_cmd, "getinfo", take(json_out_obj(NULL, NULL, NULL)),
		 "{id:%}", JSON_SCAN(json_to_node_id, &pay_plugin->my_id));

	/* BOLT #4:
	 * ## `max_htlc_cltv` Selection
	 *
	 * This ... value is defined as 2016 blocks, based on historical value
	 * deployed by Lightning implementations.
	 */
	/* FIXME: Typo in spec for CLTV in descripton!  But it breaks our spelling check, so we omit it above */
	pay_plugin->maxdelay_default = 2016;
	/* max-locktime-blocks deprecated in v24.05, but still grab it! */
	rpc_scan(init_cmd, "listconfigs",
		 take(json_out_obj(NULL, NULL, NULL)),
		 "{configs:"
		 "{max-locktime-blocks?:{value_int:%}}}",
		 JSON_SCAN(json_to_number, &pay_plugin->maxdelay_default)
		 );

	pay_plugin->payment_map = tal(pay_plugin, struct payment_map);
	payment_map_init(pay_plugin->payment_map);

	pay_plugin->pending_routes = tal(pay_plugin, struct route_map);
	route_map_init(pay_plugin->pending_routes);

	pay_plugin->gossmap = gossmap_load(pay_plugin,
					   GOSSIP_STORE_FILENAME,
					   plugin_gossmap_logcb,
					   p);

	if (!pay_plugin->gossmap)
		plugin_err(p, "Could not load gossmap %s: %s",
			   GOSSIP_STORE_FILENAME, strerror(errno));
	if (num_channel_updates_rejected)
		plugin_log(p, LOG_DBG,
			   "gossmap ignored %zu channel updates",
			   num_channel_updates_rejected);
	pay_plugin->uncertainty = uncertainty_new(pay_plugin);
	int skipped_count =
	    uncertainty_update(pay_plugin->uncertainty, pay_plugin->gossmap);
	if (skipped_count)
		plugin_log(pay_plugin->plugin, LOG_UNUSUAL,
			   "%s: uncertainty was updated but %d channels have "
			   "been ignored.",
			   __func__, skipped_count);

	plugin_set_memleak_handler(p, memleak_mark);
	return NULL;
}

static struct command_result *json_renepaystatus(struct command *cmd,
						 const char *buf,
						 const jsmntok_t *params)
{
	const char *invstring;
	struct json_stream *ret;

	if (!param(cmd, buf, params,
		   p_opt("invstring", param_invstring, &invstring),
		   NULL))
		return command_param_failed();

	ret = jsonrpc_stream_success(cmd);
	json_array_start(ret, "paystatus");
	if(invstring)
	{
		/* select the payment that matches this invoice */

		if (bolt12_has_prefix(invstring))
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "BOLT12 invoices are not yet supported.");

		char *fail;
		struct bolt11 *b11 =
		    bolt11_decode(tmpctx, invstring, plugin_feature_set(cmd->plugin),
				  NULL, chainparams, &fail);
		if (b11 == NULL)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "Invalid bolt11: %s", fail);

		struct payment *payment =
		    payment_map_get(pay_plugin->payment_map, b11->payment_hash);

		if(payment)
		{
			json_object_start(ret, NULL);
			json_add_payment(ret, payment);
			json_object_end(ret);
		}
	}else
	{
		/* show all payments */
		struct payment_map_iter it;
		for (struct payment *p =
			 payment_map_first(pay_plugin->payment_map, &it);
		     p; p = payment_map_next(pay_plugin->payment_map, &it)) {
			json_object_start(ret, NULL);
			json_add_payment(ret, p);
			json_object_end(ret);
		}
	}
	json_array_end(ret);

	return command_finished(cmd, ret);
}

static struct command_result * payment_start(struct payment *p)
{
	assert(p);
	p->status = PAYMENT_PENDING;
	plugin_log(pay_plugin->plugin, LOG_DBG, "Starting renepay");
	p->exec_state = 0;
	return payment_continue(p);
}

static struct command_result *json_renepay(struct command *cmd, const char *buf,
					   const jsmntok_t *params)
{
	/* === Parse command line arguments === */
	// TODO check if we leak some of these temporary variables

	const char *invstr;
	struct amount_msat *msat;
	struct amount_msat *maxfee;
	struct amount_msat *inv_msat = NULL;
	u32 *maxdelay;
	u32 *retryfor;
	const char *description;
	const char *label;
	struct route_exclusion **exclusions;

	// dev options
	bool *use_shadow;

	// MCF options
	u64 *base_fee_penalty_millionths; // base fee to proportional fee
	u64 *prob_cost_factor_millionths; // prob. cost to proportional fee
	u64 *riskfactor_millionths; // delay to proportional proportional fee
	u64 *min_prob_success_millionths; // target probability

	/* base probability of success, probability for a randomly picked
	 * channel to be able to forward a payment request of amount greater
	 * than zero. */
	u64 *base_prob_success_millionths;

	u64 invexpiry;
	struct sha256 *payment_hash = NULL;

	if (!param(cmd, buf, params,
		   p_req("invstring", param_invstring, &invstr),
		   p_opt("amount_msat", param_msat, &msat),
		   p_opt("maxfee", param_msat, &maxfee),

		   p_opt_def("maxdelay", param_number, &maxdelay,
			     /* maxdelay has a configuration default value named
			      * "max-locktime-blocks", this is retrieved at
			      * init. */
			     pay_plugin->maxdelay_default),

		   p_opt_def("retry_for", param_number, &retryfor,
			     60), // 60 seconds
		   p_opt("description", param_string, &description),
		   p_opt("label", param_string, &label),
		   p_opt("exclude", param_route_exclusion_array, &exclusions),

		   p_opt_dev("dev_use_shadow", param_bool, &use_shadow, true),

		   // MCF options
		   p_opt_dev("dev_base_fee_penalty", param_millionths,
			     &base_fee_penalty_millionths,
			     10000000), // default is 10.0
		   p_opt_dev("dev_prob_cost_factor", param_millionths,
			     &prob_cost_factor_millionths,
			     10000000), // default is 10.0
		   p_opt_dev("dev_riskfactor", param_millionths,
			     &riskfactor_millionths, 1), // default is 1e-6
		   p_opt_dev("dev_min_prob_success", param_millionths,
			     &min_prob_success_millionths,
			     800000), // default is 0.8
		   p_opt_dev("dev_base_prob_success", param_millionths,
			     &base_prob_success_millionths,
			     980000), // default is 0.98
		   NULL))
		return command_param_failed();

	if (*base_prob_success_millionths == 0 ||
	    *base_prob_success_millionths > 1000000)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Base probability should be a number "
				    "greater than zero and no greater than 1.");

	/* === Parse invoice === */

	char *fail;
	struct bolt11 *b11 = NULL;
	struct tlv_invoice *b12 = NULL;

	if (bolt12_has_prefix(invstr)) {
		b12 = invoice_decode(tmpctx, invstr, strlen(invstr),
				     plugin_feature_set(cmd->plugin),
				     chainparams, &fail);
		if (b12 == NULL)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "Invalid bolt12 invoice: %s", fail);

		invexpiry = invoice_expiry(b12);
		if (b12->invoice_amount) {
			inv_msat = tal(tmpctx, struct amount_msat);
			*inv_msat = amount_msat(*b12->invoice_amount);
		}
		payment_hash = b12->invoice_payment_hash;
	} else {
		b11 = bolt11_decode(tmpctx, invstr,
				    plugin_feature_set(cmd->plugin),
				    description, chainparams, &fail);
		if (b11 == NULL)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "Invalid bolt11 invoice: %s", fail);

		/* Sanity check */
		if (feature_offered(b11->features, OPT_VAR_ONION) &&
		    !b11->payment_secret)
			return command_fail(
			    cmd, JSONRPC2_INVALID_PARAMS,
			    "Invalid bolt11 invoice:"
			    " sets feature var_onion with no secret");
		inv_msat = b11->msat;
		invexpiry = b11->timestamp + b11->expiry;
		payment_hash = &b11->payment_hash;
	}

	/* === Set default values for non-trivial constraints  === */

	// Obtain amount from invoice or from arguments
	if (msat && inv_msat)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "amount_msat parameter cannot be specified "
				    "on an invoice with an amount");
	if (!msat) {
		if (!inv_msat)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "amount_msat parameter required");
		msat = tal_dup(tmpctx, struct amount_msat, inv_msat);
	}

	// Default max fee is 5 sats, or 0.5%, whichever is *higher*
	if (!maxfee) {
		struct amount_msat fee = amount_msat_div(*msat, 200);
		if (amount_msat_less(fee, AMOUNT_MSAT(5000)))
			fee = AMOUNT_MSAT(5000);
		maxfee = tal_dup(tmpctx, struct amount_msat, &fee);
	}
	assert(msat);
	assert(maxfee);
	assert(maxdelay);
	assert(retryfor);
	assert(use_shadow);
	assert(base_fee_penalty_millionths);
	assert(prob_cost_factor_millionths);
	assert(riskfactor_millionths);
	assert(min_prob_success_millionths);
	assert(base_prob_success_millionths);

	/* === Is it expired? === */

	const u64 now_sec = time_now().ts.tv_sec;
	if (now_sec > invexpiry)
		return command_fail(cmd, PAY_INVOICE_EXPIRED,
				    "Invoice expired");

	/* === Get payment === */

	// one payment_hash one payment is not assumed, it is enforced
	assert(payment_hash);
	struct payment *payment =
	    payment_map_get(pay_plugin->payment_map, *payment_hash);

	if(!payment)
	{
		payment = payment_new(tmpctx, payment_hash, invstr);
		if (!payment)
			return command_fail(cmd, PLUGIN_ERROR,
					    "failed to create a new payment");

		struct payment_info *pinfo = &payment->payment_info;
		pinfo->label = tal_strdup_or_null(payment, label);
		pinfo->description = tal_strdup_or_null(payment, description);

		if (b11) {
			pinfo->payment_secret =
			    tal_steal(payment, b11->payment_secret);
			pinfo->payment_metadata =
			    tal_steal(payment, b11->metadata);
			pinfo->routehints = tal_steal(payment, b11->routes);
			pinfo->destination = b11->receiver_id;
			pinfo->final_cltv = b11->min_final_cltv_expiry;

			pinfo->blinded_paths = NULL;
			pinfo->blinded_payinfos = NULL;
		} else {
			pinfo->payment_secret = NULL;
			pinfo->routehints = NULL;
			pinfo->payment_metadata = NULL;

			pinfo->blinded_paths =
			    tal_steal(payment, b12->invoice_paths);
			pinfo->blinded_payinfos =
			    tal_steal(payment, b12->invoice_blindedpay);

			node_id_from_pubkey(&pinfo->destination,
					    b12->invoice_node_id);

			/* FIXME: there is a different cltv_final for each
			 * blinded path, can we send this information to
			 * askrene? */
			u32 max_final_cltv = 0;
			for (size_t i = 0; i < tal_count(pinfo->blinded_payinfos);
			     i++) {
				u32 final_cltv =
				    pinfo->blinded_payinfos[i]->cltv_expiry_delta;
				if (max_final_cltv < final_cltv)
					max_final_cltv = final_cltv;
			}
			pinfo->final_cltv = max_final_cltv;
		}
		/* When dealing with BOLT12 blinded paths we compute the
		 * routing targeting a fake node to enable
		 * multi-destination minimum-cost-flow. Every blinded
		 * path entry node will be linked to this fake node
		 * using fake channels as well. This is also useful for
		 * solving self-payments. */
		payment->routing_destination = tal(payment, struct node_id);
		if (!node_id_from_hexstr("0200000000000000000000000000000000000"
					 "00000000000000000000000000001",
					 66, payment->routing_destination))
			abort();

		if (!payment_set_constraints(
			payment, *msat, *maxfee, *maxdelay, *retryfor,
			*base_fee_penalty_millionths,
			*prob_cost_factor_millionths, *riskfactor_millionths,
			*min_prob_success_millionths,
			*base_prob_success_millionths, use_shadow,
			cast_const2(const struct route_exclusion **,
				    exclusions)) ||
		    !payment_refresh(payment))
			return command_fail(
			    cmd, PLUGIN_ERROR,
			    "failed to update the payment parameters");

		if (!payment_register_command(payment, cmd))
			return command_fail(cmd, PLUGIN_ERROR,
					    "failed to register command");

		// good to go
		payment = tal_steal(pay_plugin, payment);
		payment_map_add(pay_plugin->payment_map, payment);
		return payment_start(payment);
	}

	/* === Start or continue payment === */
	if (payment->status == PAYMENT_SUCCESS) {
		assert(payment_commands_empty(payment));
		// this payment is already a success, we show the result
		struct json_stream *result = jsonrpc_stream_success(cmd);
		json_add_payment(result, payment);
		return command_finished(cmd, result);
	}

	if (payment->status == PAYMENT_FAIL) {
		// FIXME: fail if invstring does not match
		// FIXME: fail if payment_hash does not match
		if (!payment_set_constraints(
			payment, *msat, *maxfee, *maxdelay, *retryfor,
			*base_fee_penalty_millionths,
			*prob_cost_factor_millionths, *riskfactor_millionths,
			*min_prob_success_millionths,
			*base_prob_success_millionths, use_shadow,
			cast_const2(const struct route_exclusion **,
				    exclusions)) ||
		    !payment_refresh(payment))
			return command_fail(
			    cmd, PLUGIN_ERROR,
			    "failed to update the payment parameters");

		// this payment already failed, we try again
		assert(payment_commands_empty(payment));
		if (!payment_register_command(payment, cmd))
			return command_fail(cmd, PLUGIN_ERROR,
					    "failed to register command");

		return payment_start(payment);
	}

	// else: this payment is pending we continue its execution, we merge all
	// calling cmds into a single payment request
	assert(payment->status == PAYMENT_PENDING);
	if (!payment_register_command(payment, cmd))
		return command_fail(cmd, PLUGIN_ERROR,
				    "failed to register command");
	return command_still_pending(cmd);
}

static const struct plugin_command commands[] = {
	{
		"renepaystatus",
		json_renepaystatus
	},
	{
		"renepay",
		json_renepay
	},
	{
		"renesendpay",
		json_renesendpay
	},
};

static const struct plugin_notification notifications[] = {
	{
		"sendpay_success",
		notification_sendpay_success,
	},
	{
		"sendpay_failure",
		notification_sendpay_failure,
	}
};

int main(int argc, char *argv[])
{
	setup_locale();

	/* Most gets initialized in init(), but set debug options here. */
	pay_plugin = tal(NULL, struct pay_plugin);
	pay_plugin->debug_mcf = pay_plugin->debug_payflow = false;

	plugin_main(
		argv,
		init, NULL,
		PLUGIN_RESTARTABLE,
		/* init_rpc */ true,
		/* features */ NULL,
		commands, ARRAY_SIZE(commands),
		notifications, ARRAY_SIZE(notifications),
		/* hooks */ NULL, 0,
		/* notification topics */ NULL, 0,
		plugin_option("renepay-debug-mcf", "flag",
			"Enable renepay MCF debug info.",
			flag_option, NULL, &pay_plugin->debug_mcf),
		plugin_option("renepay-debug-payflow", "flag",
			"Enable renepay payment flows debug info.",
			flag_option, NULL, &pay_plugin->debug_payflow),
		NULL);

	return 0;
}
