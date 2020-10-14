#ifndef LIGHTNING_LIGHTNINGD_CHANNEL_STATE_H
#define LIGHTNING_LIGHTNINGD_CHANNEL_STATE_H
#include "config.h"

/* These are in the database, so don't renumber them! */
enum channel_state {
	/* In channeld, still waiting for lockin. */
	CHANNELD_AWAITING_LOCKIN = 2,

	/* Normal operating state. */
	CHANNELD_NORMAL,

	/* We are closing, pending HTLC resolution. */
	CHANNELD_SHUTTING_DOWN,

	/* Exchanging signatures on closing tx. */
	CLOSINGD_SIGEXCHANGE,

	/* Waiting for onchain event. */
	CLOSINGD_COMPLETE,

	/* Waiting for unilateral close to hit blockchain. */
	AWAITING_UNILATERAL,

	/* We've seen the funding spent, we're waiting for onchaind. */
	FUNDING_SPEND_SEEN,

	/* On chain */
	ONCHAIN,

	/* Final state after we have fully settled on-chain */
	CLOSED
};
#define CHANNEL_STATE_MAX CLOSED

enum state_change {
	REASON_UNKNOWN,  /* anything other than the reasons below */
	REASON_LOCAL,    /* known internal reasons, i.e. a failed HTLC */
	REASON_USER,     /* the operator or a plugin opened or closed a channel by 'hand' */
	REASON_REMOTE,   /* the remote bilateral close or funded a channel with us */
	REASON_ONCHAIN   /* something happened onchain that forces us to change the state */
};

#endif /* LIGHTNING_LIGHTNINGD_CHANNEL_STATE_H */
