/* SPDX-License-Identifier: ISC
 *
 * Copyright (C) 2015-2021 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 * Copyright (C) 2019-2021 Matt Dunwoodie <ncon@noconroy.net>
 */

#ifndef __COOKIE_H__
#define __COOKIE_H__

#include <sys/types.h>
#include <sys/time.h>
#include <sys/rwlock.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <crypto/siphash/siphash.h>
#include "crypto.h"

#define COOKIE_MAC_SIZE		16
#define COOKIE_KEY_SIZE		32
#define COOKIE_NONCE_SIZE	XCHACHA20POLY1305_NONCE_SIZE
#define COOKIE_COOKIE_SIZE	16
#define COOKIE_SECRET_SIZE	32
#define COOKIE_INPUT_SIZE	32
#define COOKIE_ENCRYPTED_SIZE	(COOKIE_COOKIE_SIZE + COOKIE_MAC_SIZE)

struct cookie_macs {
	uint8_t	mac1[COOKIE_MAC_SIZE];
	uint8_t	mac2[COOKIE_MAC_SIZE];
};

struct cookie_maker {
	uint8_t		cp_mac1_key[COOKIE_KEY_SIZE];
	uint8_t		cp_cookie_key[COOKIE_KEY_SIZE];

	struct rwlock	cp_lock;
	uint8_t		cp_cookie[COOKIE_COOKIE_SIZE];
	sbintime_t	cp_birthdate;	/* sbinuptime */
	bool		cp_mac1_valid;
	uint8_t		cp_mac1_last[COOKIE_MAC_SIZE];
};

struct cookie_checker {
	struct rwlock	cc_key_lock;
	uint8_t		cc_mac1_key[COOKIE_KEY_SIZE];
	uint8_t		cc_cookie_key[COOKIE_KEY_SIZE];

	struct rwlock	cc_secret_lock;
	sbintime_t	cc_secret_birthdate;	/* sbinuptime */
	uint8_t		cc_secret[COOKIE_SECRET_SIZE];
};

int	cookie_init(void);
void	cookie_deinit(void);
void	cookie_checker_init(struct cookie_checker *);
void	cookie_checker_update(struct cookie_checker *,
	    const uint8_t[COOKIE_INPUT_SIZE]);
void	cookie_checker_create_payload(struct cookie_checker *,
	    struct cookie_macs *cm, uint8_t[COOKIE_NONCE_SIZE],
	    uint8_t [COOKIE_ENCRYPTED_SIZE], struct sockaddr *);
void	cookie_maker_init(struct cookie_maker *, const uint8_t[COOKIE_INPUT_SIZE]);
int	cookie_maker_consume_payload(struct cookie_maker *,
	    uint8_t[COOKIE_NONCE_SIZE], uint8_t[COOKIE_ENCRYPTED_SIZE]);
void	cookie_maker_mac(struct cookie_maker *, struct cookie_macs *,
	    void *, size_t);
int	cookie_checker_validate_macs(struct cookie_checker *,
	    struct cookie_macs *, void *, size_t, int, struct sockaddr *);

#ifdef SELFTESTS
void	cookie_selftest(void);
#endif /* SELFTESTS */

#endif /* __COOKIE_H__ */
