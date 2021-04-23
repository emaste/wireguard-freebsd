/* SPDX-License-Identifier: ISC
 *
 * Copyright (C) 2015-2021 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 * Copyright (C) 2019-2021 Matt Dunwoodie <ncon@noconroy.net>
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/param.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/malloc.h> /* Because systm doesn't include M_NOWAIT, M_DEVBUF */
#include <sys/socket.h>

#include "support.h"
#include "wg_cookie.h"

#define COOKIE_MAC1_KEY_LABEL	"mac1----"
#define COOKIE_COOKIE_KEY_LABEL	"cookie--"
#define COOKIE_SECRET_MAX_AGE	120
#define COOKIE_SECRET_LATENCY	5

/* Constants for initiation rate limiting */
#define RATELIMIT_SIZE		(1 << 13)
#define RATELIMIT_SIZE_MAX	(RATELIMIT_SIZE * 8)
#define INITIATIONS_PER_SECOND	20
#define INITIATIONS_BURSTABLE	5
#define INITIATION_COST		(SBT_1S / INITIATIONS_PER_SECOND)
#define TOKEN_MAX		(INITIATION_COST * INITIATIONS_BURSTABLE)
#define ELEMENT_TIMEOUT		1
#define IPV4_MASK_SIZE		4 /* Use all 4 bytes of IPv4 address */
#define IPV6_MASK_SIZE		8 /* Use top 8 bytes (/64) of IPv6 address */

struct ratelimit_entry {
	LIST_ENTRY(ratelimit_entry)	 r_entry;
	sa_family_t			 r_af;
	union {
		struct in_addr		 r_in;
#ifdef INET6
		struct in6_addr		 r_in6;
#endif
	};
	sbintime_t			 r_last_time;	/* sbinuptime */
	uint64_t			 r_tokens;
};

struct ratelimit {
	uint8_t				 rl_secret[SIPHASH_KEY_LENGTH];

	struct rwlock			 rl_lock;
	struct callout			 rl_gc;
	LIST_HEAD(, ratelimit_entry)	*rl_table;
	u_long				 rl_table_mask;
	size_t				 rl_table_num;
};

static void	cookie_precompute_key(uint8_t *,
			const uint8_t[COOKIE_INPUT_SIZE], const char *);
static void	cookie_macs_mac1(struct cookie_macs *, const void *, size_t,
			const uint8_t[COOKIE_KEY_SIZE]);
static void	cookie_macs_mac2(struct cookie_macs *, const void *, size_t,
			const uint8_t[COOKIE_COOKIE_SIZE]);
static int	cookie_timer_expired(sbintime_t, uint32_t, uint32_t);
static void	cookie_checker_make_cookie(struct cookie_checker *,
			uint8_t[COOKIE_COOKIE_SIZE], struct sockaddr *);
static int	ratelimit_init(struct ratelimit *);
static void	ratelimit_deinit(struct ratelimit *);
static void	ratelimit_gc_callout(void *);
static void	ratelimit_gc_schedule(struct ratelimit *);
static void	ratelimit_gc(struct ratelimit *, bool);
static int	ratelimit_allow(struct ratelimit *, struct sockaddr *);
static uint64_t siphash13(const uint8_t [SIPHASH_KEY_LENGTH], const void *, size_t);

static struct ratelimit ratelimit_v4;
#ifdef INET6
static struct ratelimit ratelimit_v6;
#endif
static uma_zone_t ratelimit_zone;

/* Public Functions */
int
cookie_init(void)
{
	int res;

	ratelimit_zone = uma_zcreate("wg ratelimit", sizeof(struct ratelimit),
	     NULL, NULL, NULL, NULL, 0, 0);

	if ((res = ratelimit_init(&ratelimit_v4)) != 0)
		return res;
#ifdef INET6
	if ((res = ratelimit_init(&ratelimit_v6)) != 0) {
		ratelimit_deinit(&ratelimit_v4);
		return res;
	}
#endif
	return 0;
}

void
cookie_deinit(void)
{
	uma_zdestroy(ratelimit_zone);
	ratelimit_deinit(&ratelimit_v4);
#ifdef INET6
	ratelimit_deinit(&ratelimit_v6);
#endif
}

void
cookie_checker_init(struct cookie_checker *cc)
{
	bzero(cc, sizeof(*cc));

	rw_init(&cc->cc_key_lock, "cookie_checker_key");
	rw_init(&cc->cc_secret_lock, "cookie_checker_secret");
}

void
cookie_checker_update(struct cookie_checker *cc,
    const uint8_t key[COOKIE_INPUT_SIZE])
{
	rw_wlock(&cc->cc_key_lock);
	if (key) {
		cookie_precompute_key(cc->cc_mac1_key, key, COOKIE_MAC1_KEY_LABEL);
		cookie_precompute_key(cc->cc_cookie_key, key, COOKIE_COOKIE_KEY_LABEL);
	} else {
		bzero(cc->cc_mac1_key, sizeof(cc->cc_mac1_key));
		bzero(cc->cc_cookie_key, sizeof(cc->cc_cookie_key));
	}
	rw_wunlock(&cc->cc_key_lock);
}

void
cookie_checker_create_payload(struct cookie_checker *cc,
    struct cookie_macs *cm, uint8_t nonce[COOKIE_NONCE_SIZE],
    uint8_t ecookie[COOKIE_ENCRYPTED_SIZE], struct sockaddr *sa)
{
	uint8_t cookie[COOKIE_COOKIE_SIZE];

	cookie_checker_make_cookie(cc, cookie, sa);
	arc4random_buf(nonce, COOKIE_NONCE_SIZE);

	rw_rlock(&cc->cc_key_lock);
	xchacha20poly1305_encrypt(ecookie, cookie, COOKIE_COOKIE_SIZE,
	    cm->mac1, COOKIE_MAC_SIZE, nonce, cc->cc_cookie_key);
	rw_runlock(&cc->cc_key_lock);

	explicit_bzero(cookie, sizeof(cookie));
}

void
cookie_maker_init(struct cookie_maker *cp, const uint8_t key[COOKIE_INPUT_SIZE])
{
	bzero(cp, sizeof(*cp));
	cookie_precompute_key(cp->cp_mac1_key, key, COOKIE_MAC1_KEY_LABEL);
	cookie_precompute_key(cp->cp_cookie_key, key, COOKIE_COOKIE_KEY_LABEL);
	rw_init(&cp->cp_lock, "cookie_maker");
}

int
cookie_maker_consume_payload(struct cookie_maker *cp,
    uint8_t nonce[COOKIE_NONCE_SIZE], uint8_t ecookie[COOKIE_ENCRYPTED_SIZE])
{
	int ret = 0;
	uint8_t cookie[COOKIE_COOKIE_SIZE];

	rw_wlock(&cp->cp_lock);

	if (!cp->cp_mac1_valid) {
		ret = ETIMEDOUT;
		goto error;
	}

	if (xchacha20poly1305_decrypt(cookie, ecookie, COOKIE_ENCRYPTED_SIZE,
	    cp->cp_mac1_last, COOKIE_MAC_SIZE, nonce, cp->cp_cookie_key) == 0) {
		ret = EINVAL;
		goto error;
	}

	memcpy(cp->cp_cookie, cookie, COOKIE_COOKIE_SIZE);
	cp->cp_birthdate = getsbinuptime();
	cp->cp_mac1_valid = false;

error:
	rw_wunlock(&cp->cp_lock);
	return ret;
}

void
cookie_maker_mac(struct cookie_maker *cp, struct cookie_macs *cm, void *buf,
		size_t len)
{
	rw_rlock(&cp->cp_lock);

	cookie_macs_mac1(cm, buf, len, cp->cp_mac1_key);

	memcpy(cp->cp_mac1_last, cm->mac1, COOKIE_MAC_SIZE);
	cp->cp_mac1_valid = true;

	if (!cookie_timer_expired(cp->cp_birthdate,
	    COOKIE_SECRET_MAX_AGE - COOKIE_SECRET_LATENCY, 0))
		cookie_macs_mac2(cm, buf, len, cp->cp_cookie);
	else
		bzero(cm->mac2, COOKIE_MAC_SIZE);

	rw_runlock(&cp->cp_lock);
}

int
cookie_checker_validate_macs(struct cookie_checker *cc, struct cookie_macs *cm,
		void *buf, size_t len, int busy, struct sockaddr *sa)
{
	struct cookie_macs our_cm;
	uint8_t cookie[COOKIE_COOKIE_SIZE];

	/* Validate incoming MACs */
	rw_rlock(&cc->cc_key_lock);
	cookie_macs_mac1(&our_cm, buf, len, cc->cc_mac1_key);
	rw_runlock(&cc->cc_key_lock);

	/* If mac1 is invald, we want to drop the packet */
	if (timingsafe_bcmp(our_cm.mac1, cm->mac1, COOKIE_MAC_SIZE) != 0)
		return EINVAL;

	if (busy != 0) {
		cookie_checker_make_cookie(cc, cookie, sa);
		cookie_macs_mac2(&our_cm, buf, len, cookie);

		/* If the mac2 is invalid, we want to send a cookie response */
		if (timingsafe_bcmp(our_cm.mac2, cm->mac2, COOKIE_MAC_SIZE) != 0)
			return EAGAIN;

		/* If the mac2 is valid, we may want rate limit the peer.
		 * ratelimit_allow will return either 0 or ECONNREFUSED,
		 * implying there is no ratelimiting, or we should ratelimit
		 * (refuse) respectively. */
		if (sa->sa_family == AF_INET)
			return ratelimit_allow(&ratelimit_v4, sa);
#ifdef INET6
		else if (sa->sa_family == AF_INET6)
			return ratelimit_allow(&ratelimit_v6, sa);
#endif
		else
			return EAFNOSUPPORT;
	}
	return 0;
}

/* Private functions */
static void
cookie_precompute_key(uint8_t *key, const uint8_t input[COOKIE_INPUT_SIZE],
    const char *label)
{
	struct blake2s_state blake;

	blake2s_init(&blake, COOKIE_KEY_SIZE);
	blake2s_update(&blake, label, strlen(label));
	blake2s_update(&blake, input, COOKIE_INPUT_SIZE);
	/* TODO we shouldn't need to provide outlen to _final. we can align
	 * this with openbsd after fixing the blake library. */
	blake2s_final(&blake, key);
}

static void
cookie_macs_mac1(struct cookie_macs *cm, const void *buf, size_t len,
    const uint8_t key[COOKIE_KEY_SIZE])
{
	struct blake2s_state state;
	blake2s_init_key(&state, COOKIE_MAC_SIZE, key, COOKIE_KEY_SIZE);
	blake2s_update(&state, buf, len);
	blake2s_final(&state, cm->mac1);
}

static void
cookie_macs_mac2(struct cookie_macs *cm, const void *buf, size_t len,
		const uint8_t key[COOKIE_COOKIE_SIZE])
{
	struct blake2s_state state;
	blake2s_init_key(&state, COOKIE_MAC_SIZE, key, COOKIE_COOKIE_SIZE);
	blake2s_update(&state, buf, len);
	blake2s_update(&state, cm->mac1, COOKIE_MAC_SIZE);
	blake2s_final(&state, cm->mac2);
}

static __inline int
cookie_timer_expired(sbintime_t timer, uint32_t sec, uint32_t nsec)
{
	sbintime_t now = getsbinuptime();
	return (now > (timer + sec * SBT_1S + nstosbt(nsec))) ? ETIMEDOUT : 0;
}

static void
cookie_checker_make_cookie(struct cookie_checker *cc,
		uint8_t cookie[COOKIE_COOKIE_SIZE], struct sockaddr *sa)
{
	struct blake2s_state state;

	rw_wlock(&cc->cc_secret_lock);
	if (cookie_timer_expired(cc->cc_secret_birthdate,
	    COOKIE_SECRET_MAX_AGE, 0)) {
		arc4random_buf(cc->cc_secret, COOKIE_SECRET_SIZE);
		cc->cc_secret_birthdate = getsbinuptime();
	}
	blake2s_init_key(&state, COOKIE_COOKIE_SIZE, cc->cc_secret,
	    COOKIE_SECRET_SIZE);
	rw_wunlock(&cc->cc_secret_lock);

	if (sa->sa_family == AF_INET) {
		blake2s_update(&state, (uint8_t *)&satosin(sa)->sin_addr,
				sizeof(struct in_addr));
		blake2s_update(&state, (uint8_t *)&satosin(sa)->sin_port,
				sizeof(in_port_t));
		blake2s_final(&state, cookie);
#ifdef INET6
	} else if (sa->sa_family == AF_INET6) {
		blake2s_update(&state, (uint8_t *)&satosin6(sa)->sin6_addr,
				sizeof(struct in6_addr));
		blake2s_update(&state, (uint8_t *)&satosin6(sa)->sin6_port,
				sizeof(in_port_t));
		blake2s_final(&state, cookie);
#endif
	} else {
		arc4random_buf(cookie, COOKIE_COOKIE_SIZE);
	}
}

static int
ratelimit_init(struct ratelimit *rl)
{
	rw_init(&rl->rl_lock, "ratelimit_lock");
	callout_init_rw(&rl->rl_gc, &rl->rl_lock, 0);
	arc4random_buf(rl->rl_secret, sizeof(rl->rl_secret));
	rl->rl_table = hashinit_flags(RATELIMIT_SIZE, M_DEVBUF,
	    &rl->rl_table_mask, M_NOWAIT);
	rl->rl_table_num = 0;
	return rl->rl_table == NULL ? ENOBUFS : 0;
}

static void
ratelimit_deinit(struct ratelimit *rl)
{
	rw_wlock(&rl->rl_lock);
	callout_stop(&rl->rl_gc);
	ratelimit_gc(rl, true);
	hashdestroy(rl->rl_table, M_DEVBUF, rl->rl_table_mask);
	rw_wunlock(&rl->rl_lock);
}

static void
ratelimit_gc_callout(void *_rl)
{
	/* callout will wlock rl_lock for us */
	ratelimit_gc(_rl, false);
}

static void
ratelimit_gc_schedule(struct ratelimit *rl)
{
	/* Trigger another GC if needed. There is no point calling GC if there
	 * are no entries in the table. We also want to ensure that GC occurs
	 * on a regular interval, so don't override a currently pending GC.
	 *
	 * In the case of a forced ratelimit_gc, there will be no entries left
	 * so we will will not schedule another GC. */
	if (rl->rl_table_num > 0 && !callout_pending(&rl->rl_gc))
		callout_reset(&rl->rl_gc, ELEMENT_TIMEOUT * hz,
		    ratelimit_gc_callout, rl);
}

static void
ratelimit_gc(struct ratelimit *rl, bool force)
{
	size_t i;
	struct ratelimit_entry *r, *tr;
	sbintime_t expiry;

	rw_assert(&rl->rl_lock, RA_WLOCKED);

	if (rl->rl_table_num == 0)
		return;

	expiry = getsbinuptime() - ELEMENT_TIMEOUT * SBT_1S;

	for (i = 0; i < RATELIMIT_SIZE; i++) {
		LIST_FOREACH_SAFE(r, &rl->rl_table[i], r_entry, tr) {
			if (r->r_last_time < expiry || force) {
				rl->rl_table_num--;
				LIST_REMOVE(r, r_entry);
				uma_zfree(ratelimit_zone, r);
			}
		}
	}

	ratelimit_gc_schedule(rl);
}

static int
ratelimit_allow(struct ratelimit *rl, struct sockaddr *sa)
{
	uint64_t key, tokens;
	sbintime_t diff, now;
	struct ratelimit_entry *r;
	int ret = ECONNREFUSED;

	if (sa->sa_family == AF_INET)
		key = siphash13(rl->rl_secret, &satosin(sa)->sin_addr,
				IPV4_MASK_SIZE);
#ifdef INET6
	else if (sa->sa_family == AF_INET6)
		key = siphash13(rl->rl_secret, &satosin6(sa)->sin6_addr,
				IPV6_MASK_SIZE);
#endif
	else
		return ret;

	rw_wlock(&rl->rl_lock);

	LIST_FOREACH(r, &rl->rl_table[key & rl->rl_table_mask], r_entry) {
		if (r->r_af != sa->sa_family)
			continue;

		if (r->r_af == AF_INET && bcmp(&r->r_in,
		    &satosin(sa)->sin_addr, IPV4_MASK_SIZE) != 0)
			continue;

#ifdef INET6
		if (r->r_af == AF_INET6 && bcmp(&r->r_in6,
		    &satosin6(sa)->sin6_addr, IPV6_MASK_SIZE) != 0)
			continue;
#endif

		/* If we get to here, we've found an entry for the endpoint.
		 * We apply standard token bucket, by calculating the time
		 * lapsed since our last_time, adding that, ensuring that we
		 * cap the tokens at TOKEN_MAX. If the endpoint has no tokens
		 * left (that is tokens <= INITIATION_COST) then we block the
		 * request, otherwise we subtract the INITITIATION_COST and
		 * return OK. */
		now = getsbinuptime();
		diff = now - r->r_last_time;
		r->r_last_time = now;

		tokens = r->r_tokens + diff;

		if (tokens > TOKEN_MAX)
			tokens = TOKEN_MAX;

		if (tokens >= INITIATION_COST) {
			r->r_tokens = tokens - INITIATION_COST;
			goto ok;
		} else {
			r->r_tokens = tokens;
			goto error;
		}
	}

	/* If we get to here, we didn't have an entry for the endpoint, let's
	 * add one if we have space. */
	if (rl->rl_table_num >= RATELIMIT_SIZE_MAX)
		goto error;

	/* Goto error if out of memory */
	if ((r = uma_zalloc(ratelimit_zone, M_NOWAIT)) == NULL)
		goto error;

	rl->rl_table_num++;

	/* Insert entry into the hashtable and ensure it's initialised */
	LIST_INSERT_HEAD(&rl->rl_table[key & rl->rl_table_mask], r, r_entry);
	r->r_af = sa->sa_family;
	if (r->r_af == AF_INET)
		memcpy(&r->r_in, &satosin(sa)->sin_addr, IPV4_MASK_SIZE);
#ifdef INET6
	else if (r->r_af == AF_INET6)
		memcpy(&r->r_in6, &satosin6(sa)->sin6_addr, IPV6_MASK_SIZE);
#endif

	r->r_last_time = getsbinuptime();
	r->r_tokens = TOKEN_MAX - INITIATION_COST;

	/* If we've added a new entry, let's trigger GC. */
	ratelimit_gc_schedule(rl);
ok:
	ret = 0;
error:
	rw_wunlock(&rl->rl_lock);
	return ret;
}

static uint64_t siphash13(const uint8_t key[SIPHASH_KEY_LENGTH], const void *src, size_t len)
{
	SIPHASH_CTX ctx;
	return (SipHashX(&ctx, 1, 3, key, src, len));
}

#ifdef SELFTESTS
#include "selftest/cookie.c"
#endif /* SELFTESTS */
