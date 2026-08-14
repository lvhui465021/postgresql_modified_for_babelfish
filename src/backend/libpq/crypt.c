/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Functions for dealing with encrypted passwords stored in
 *	  pg_authid.rolpassword.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/crypt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "catalog/pg_authid.h"
#include "common/cryptohash.h"
#include "common/md5.h"
#include "common/scram-common.h"
#include "libpq/crypt.h"
#include "libpq/scram.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/* Enables deprecation warnings for MD5 passwords. */
bool		md5_password_warnings = true;

static bool mysql_native_password_decode(const char *secret,
										uint8 *stage2);
static bool mysql_native_password_sha1(const uint8 *first, size_t first_len,
									const uint8 *second, size_t second_len,
									uint8 *result);

/*
 * Fetch stored password for a user, for authentication.
 *
 * On error, returns NULL, and stores a palloc'd string describing the reason,
 * for the postmaster log, in *logdetail.  The error reason should *not* be
 * sent to the client, to avoid giving away user information!
 */
char *
get_role_password(const char *role, const char **logdetail)
{
	TimestampTz vuntil = 0;
	HeapTuple	roleTup;
	Datum		datum;
	bool		isnull;
	char	   *shadow_pass;

	/* Get role info from pg_authid */
	roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
	if (!HeapTupleIsValid(roleTup))
	{
		*logdetail = psprintf(_("Role \"%s\" does not exist."),
							  role);
		return NULL;			/* no such user */
	}

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolpassword, &isnull);
	if (isnull)
	{
		ReleaseSysCache(roleTup);
		*logdetail = psprintf(_("User \"%s\" has no password assigned."),
							  role);
		return NULL;			/* user has no password */
	}
	shadow_pass = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolvaliduntil, &isnull);
	if (!isnull)
		vuntil = DatumGetTimestampTz(datum);

	ReleaseSysCache(roleTup);

	/*
	 * Password OK, but check to be sure we are not past rolvaliduntil
	 */
	if (!isnull && vuntil < GetCurrentTimestamp())
	{
		*logdetail = psprintf(_("User \"%s\" has an expired password."),
							  role);
		return NULL;
	}

	return shadow_pass;
}

/*
 * Scan a rolpasswordext-style newline-separated verifier list for an entry
 * whose type matches 'wanted'.  Returns a palloc'd copy of the matching
 * verifier, or NULL if none is found.
 *
 * The whole list is always scanned to completion, even after a match is
 * found: any MD5/SCRAM entry (those belong only in rolpassword, never
 * rolpasswordext -- see IV-2 in P3-2_AUTH_SPEC.md) or any entry
 * get_password_type() can't identify (IV-3) makes the entire list
 * untrustworthy, so the whole thing is treated as corrupt and rejected
 * rather than partially trusted.
 */
static char *
find_verifier_of_type(const char *verifier_list, PasswordType wanted)
{
	char	   *copy = pstrdup(verifier_list);
	char	   *line = copy;
	char	   *result = NULL;

	while (line != NULL && *line != '\0')
	{
		char	   *nl = strchr(line, '\n');
		PasswordType type;

		if (nl != NULL)
			*nl = '\0';

		type = get_password_type(line);

		if (type == PASSWORD_TYPE_PLAINTEXT ||
			type == PASSWORD_TYPE_MD5 ||
			type == PASSWORD_TYPE_SCRAM_SHA_256)
		{
			ereport(LOG,
					(errmsg("rolpasswordext contains an invalid verifier entry")));
			if (result != NULL)
				pfree(result);
			pfree(copy);
			return NULL;
		}

		if (result == NULL && type == wanted)
			result = pstrdup(line);

		line = (nl != NULL) ? nl + 1 : NULL;
	}

	pfree(copy);
	return result;
}

/*
 * Protocol-aware counterpart of get_role_password().
 *
 * kind == COMPAT_PROTOCOL_POSTGRES or COMPAT_PROTOCOL_TDS: identical to
 * get_role_password() -- only rolpassword is consulted.  TDS's own
 * authentication (babelfishpg_tds's CheckAuthPassword()) already calls the
 * native get_role_password() + plain_crypt_verify() directly and fully
 * reuses PG's SCRAM/MD5 verifier format, so it needs no rolpasswordext
 * entries of its own; this function exists mainly so callers can pass a
 * CompatibilityProtocolKind uniformly without a special case.
 *
 * kind == COMPAT_PROTOCOL_MYSQL: look for a verifier of type 'wanted'
 * first in rolpasswordext, falling back to rolpassword *only if* its
 * actual type (via get_password_type(), not the password_encryption GUC)
 * matches 'wanted'.  This dual gate (exact type match here, plus each
 * *_verify() function's own format check) is what keeps a MySQL
 * connection from ever being handed a SCRAM/MD5 secret -- see SEC-1..SEC-3
 * in P3-2_AUTH_SPEC.md.
 */
char *
get_role_password_ext(const char *role, CompatibilityProtocolKind kind,
					   PasswordType wanted, const char **logdetail)
{
	TimestampTz vuntil = 0;
	HeapTuple	roleTup;
	Datum		datum;
	bool		isnull;
	char	   *result = NULL;

	if (kind == COMPAT_PROTOCOL_POSTGRES || kind == COMPAT_PROTOCOL_TDS)
		return get_role_password(role, logdetail);

	roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
	if (!HeapTupleIsValid(roleTup))
	{
		*logdetail = psprintf(_("Role \"%s\" does not exist."), role);
		return NULL;			/* no such user */
	}

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							 Anum_pg_authid_rolpasswordext, &isnull);
	if (!isnull)
	{
		char	   *ext_list = TextDatumGetCString(datum);

		result = find_verifier_of_type(ext_list, wanted);
		pfree(ext_list);
	}

	if (result == NULL)
	{
		/* Fall back to rolpassword, but only if its real type matches. */
		datum = SysCacheGetAttr(AUTHNAME, roleTup,
								 Anum_pg_authid_rolpassword, &isnull);
		if (!isnull)
		{
			char	   *shadow_pass = TextDatumGetCString(datum);

			if (get_password_type(shadow_pass) == wanted)
				result = shadow_pass;
			else
				pfree(shadow_pass);
		}
	}

	if (result == NULL)
	{
		ReleaseSysCache(roleTup);
		*logdetail = psprintf(_("User \"%s\" has no password assigned for this protocol."),
							  role);
		return NULL;
	}

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							 Anum_pg_authid_rolvaliduntil, &isnull);
	if (!isnull)
		vuntil = DatumGetTimestampTz(datum);

	ReleaseSysCache(roleTup);

	if (!isnull && vuntil < GetCurrentTimestamp())
	{
		*logdetail = psprintf(_("User \"%s\" has an expired password."), role);
		pfree(result);
		return NULL;
	}

	return result;
}

/*
 * Return a new newline-separated verifier list where any existing entry of
 * the same type as 'new_verifier' has been dropped and 'new_verifier'
 * appended, so the result holds at most one verifier per PasswordType (IV-1
 * in P3-2_AUTH_SPEC.md).  'old_list' may be NULL for a role with no
 * existing rolpasswordext entries.
 *
 * Any pre-existing entry this function can't make sense of (MD5/SCRAM,
 * which belong only in rolpassword -- IV-2; or unrecognized/plaintext --
 * IV-3) is dropped with a WARNING rather than propagated.  Unlike the read
 * path (find_verifier_of_type(), which fails the whole list closed on any
 * such entry so a corrupt value can never be partially trusted for
 * authentication), the write path's job is to let an administrator set a
 * clean new password even if what's on disk is already damaged -- erroring
 * out here would leave them unable to fix it via ALTER ROLE.
 */
char *
merge_verifier_into_list(const char *old_list, const char *new_verifier)
{
	PasswordType new_type = get_password_type(new_verifier);
	StringInfoData buf;

	initStringInfo(&buf);

	if (old_list != NULL)
	{
		char	   *copy = pstrdup(old_list);
		char	   *line = copy;

		while (line != NULL && *line != '\0')
		{
			char	   *nl = strchr(line, '\n');
			PasswordType type;

			if (nl != NULL)
				*nl = '\0';

			type = get_password_type(line);

			if (type == new_type)
			{
				/* superseded by new_verifier below */
			}
			else if (type == PASSWORD_TYPE_PLAINTEXT ||
					 type == PASSWORD_TYPE_MD5 ||
					 type == PASSWORD_TYPE_SCRAM_SHA_256)
			{
				ereport(WARNING,
						(errmsg("dropping invalid pre-existing rolpasswordext entry")));
			}
			else
			{
				if (buf.len > 0)
					appendStringInfoChar(&buf, '\n');
				appendStringInfoString(&buf, line);
			}

			line = (nl != NULL) ? nl + 1 : NULL;
		}
		pfree(copy);
	}

	if (buf.len > 0)
		appendStringInfoChar(&buf, '\n');
	appendStringInfoString(&buf, new_verifier);

	return buf.data;
}

/*
 * What kind of a password type is 'shadow_pass'?
 */
PasswordType
get_password_type(const char *shadow_pass)
{
	char	   *encoded_salt;
	int			iterations;
	int			key_length = 0;
	pg_cryptohash_type hash_type;
	uint8		stored_key[SCRAM_MAX_KEY_LEN];
	uint8		server_key[SCRAM_MAX_KEY_LEN];

	if (strncmp(shadow_pass, "md5", 3) == 0 &&
		strlen(shadow_pass) == MD5_PASSWD_LEN &&
		strspn(shadow_pass + 3, MD5_PASSWD_CHARSET) == MD5_PASSWD_LEN - 3)
		return PASSWORD_TYPE_MD5;
	if (parse_scram_secret(shadow_pass, &iterations, &hash_type, &key_length,
						   &encoded_salt, stored_key, server_key))
		return PASSWORD_TYPE_SCRAM_SHA_256;
	if (strlen(shadow_pass) == MYSQL_NATIVE_PASSWORD_SECRET_LEN &&
		strncmp(shadow_pass, MYSQL_NATIVE_PASSWORD_PREFIX,
				sizeof(MYSQL_NATIVE_PASSWORD_PREFIX) - 1) == 0 &&
		strspn(shadow_pass + sizeof(MYSQL_NATIVE_PASSWORD_PREFIX) - 1,
				"0123456789abcdef") == MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH * 2)
		return PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD;
	return PASSWORD_TYPE_PLAINTEXT;
}

/*
 * Given a user-supplied password, convert it into a secret of
 * 'target_type' kind.
 *
 * If the password is already in encrypted form, we cannot reverse the
 * hash, so it is stored as it is regardless of the requested type.
 */
char *
encrypt_password(PasswordType target_type, const char *role,
				 const char *password)
{
	PasswordType guessed_type = get_password_type(password);
	char	   *encrypted_password = NULL;
	const char *errstr = NULL;

	if (guessed_type != PASSWORD_TYPE_PLAINTEXT)
	{
		/*
		 * Cannot convert an already-encrypted password from one format to
		 * another, so return it as it is.
		 */
		encrypted_password = pstrdup(password);
	}
	else
	{
		switch (target_type)
		{
			case PASSWORD_TYPE_MD5:
				encrypted_password = palloc(MD5_PASSWD_LEN + 1);

				if (!pg_md5_encrypt(password, (uint8 *) role, strlen(role),
									encrypted_password, &errstr))
					elog(ERROR, "password encryption failed: %s", errstr);
				break;

			case PASSWORD_TYPE_SCRAM_SHA_256:
				encrypted_password = pg_be_scram_build_secret(password);
				break;

			case PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD:
				encrypted_password = mysql_native_password_encrypt(password);
				break;

			case PASSWORD_TYPE_PLAINTEXT:
				elog(ERROR, "cannot encrypt password with 'plaintext'");
				break;
		}
	}

	Assert(encrypted_password);

	/*
	 * Valid password hashes may be very long, but we don't want to store
	 * anything that might need out-of-line storage, since de-TOASTing won't
	 * work during authentication because we haven't selected a database yet
	 * and cannot read pg_class. 512 bytes should be more than enough for all
	 * practical use, so fail for anything longer.
	 */
	if (encrypted_password &&	/* keep compiler quiet */
		strlen(encrypted_password) > MAX_ENCRYPTED_PASSWORD_LEN)
	{
		/*
		 * We don't expect any of our own hashing routines to produce hashes
		 * that are too long.
		 */
		Assert(guessed_type != PASSWORD_TYPE_PLAINTEXT);

		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("encrypted password is too long"),
				 errdetail("Encrypted passwords must be no longer than %d bytes.",
						   MAX_ENCRYPTED_PASSWORD_LEN)));
	}

	if (md5_password_warnings &&
		get_password_type(encrypted_password) == PASSWORD_TYPE_MD5)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING_DEPRECATED_FEATURE),
				 errmsg("setting an MD5-encrypted password"),
				 errdetail("MD5 password support is deprecated and will be removed in a future release of PostgreSQL."),
				 errhint("Refer to the PostgreSQL documentation for details about migrating to another password type.")));

	return encrypted_password;
}

/*
 * Check MD5 authentication response, and return STATUS_OK or STATUS_ERROR.
 *
 * 'shadow_pass' is the user's correct password or password hash, as stored
 * in pg_authid.rolpassword.
 * 'client_pass' is the response given by the remote user to the MD5 challenge.
 * 'md5_salt' is the salt used in the MD5 authentication challenge.
 *
 * In the error case, save a string at *logdetail that will be sent to the
 * postmaster log (but not the client).
 */
int
md5_crypt_verify(const char *role, const char *shadow_pass,
				 const char *client_pass,
				 const uint8 *md5_salt, int md5_salt_len,
				 const char **logdetail)
{
	int			retval;
	char		crypt_pwd[MD5_PASSWD_LEN + 1];
	const char *errstr = NULL;

	Assert(md5_salt_len > 0);

	if (get_password_type(shadow_pass) != PASSWORD_TYPE_MD5)
	{
		/* incompatible password hash format. */
		*logdetail = psprintf(_("User \"%s\" has a password that cannot be used with MD5 authentication."),
							  role);
		return STATUS_ERROR;
	}

	/*
	 * Compute the correct answer for the MD5 challenge.
	 */
	/* stored password already encrypted, only do salt */
	if (!pg_md5_encrypt(shadow_pass + strlen("md5"),
						md5_salt, md5_salt_len,
						crypt_pwd, &errstr))
	{
		*logdetail = errstr;
		return STATUS_ERROR;
	}

	if (strcmp(client_pass, crypt_pwd) == 0)
		retval = STATUS_OK;
	else
	{
		*logdetail = psprintf(_("Password does not match for user \"%s\"."),
							  role);
		retval = STATUS_ERROR;
	}

	return retval;
}

/*
 * Check given password for given user, and return STATUS_OK or STATUS_ERROR.
 *
 * 'shadow_pass' is the user's correct password hash, as stored in
 * pg_authid.rolpassword.
 * 'client_pass' is the password given by the remote user.
 *
 * In the error case, store a string at *logdetail that will be sent to the
 * postmaster log (but not the client).
 */
int
plain_crypt_verify(const char *role, const char *shadow_pass,
				   const char *client_pass,
				   const char **logdetail)
{
	char		crypt_client_pass[MD5_PASSWD_LEN + 1];
	const char *errstr = NULL;

	/*
	 * Client sent password in plaintext.  If we have an MD5 hash stored, hash
	 * the password the client sent, and compare the hashes.  Otherwise
	 * compare the plaintext passwords directly.
	 */
	switch (get_password_type(shadow_pass))
	{
		case PASSWORD_TYPE_SCRAM_SHA_256:
			if (scram_verify_plain_password(role,
											client_pass,
											shadow_pass))
			{
				return STATUS_OK;
			}
			else
			{
				*logdetail = psprintf(_("Password does not match for user \"%s\"."),
									  role);
				return STATUS_ERROR;
			}
			break;

		case PASSWORD_TYPE_MD5:
			if (!pg_md5_encrypt(client_pass,
								(uint8 *) role,
								strlen(role),
								crypt_client_pass,
								&errstr))
			{
				*logdetail = errstr;
				return STATUS_ERROR;
			}
			if (strcmp(crypt_client_pass, shadow_pass) == 0)
				return STATUS_OK;
			else
			{
				*logdetail = psprintf(_("Password does not match for user \"%s\"."),
									  role);
				return STATUS_ERROR;
			}
			break;

		case PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD:
			*logdetail = psprintf(_("User \"%s\" has a password that cannot be used with PostgreSQL password authentication."),
							  role);
			return STATUS_ERROR;

		case PASSWORD_TYPE_PLAINTEXT:

			/*
			 * We never store passwords in plaintext, so this shouldn't
			 * happen.
			 */
			break;
	}

	/*
	 * This shouldn't happen.  Plain "password" authentication is possible
	 * with any kind of stored password hash.
	 */
	*logdetail = psprintf(_("Password of user \"%s\" is in unrecognized format."),
						  role);
	return STATUS_ERROR;
}

/*
 * mysql_native_password_sha1
 *
 * Calculate SHA1(first || second).  Passing NULL with length zero is valid.
 */
static bool
mysql_native_password_sha1(const uint8 *first, size_t first_len,
						   const uint8 *second, size_t second_len,
						   uint8 *result)
{
	pg_cryptohash_ctx *ctx;
	bool		ok = false;

	ctx = pg_cryptohash_create(PG_SHA1);
	if (ctx == NULL)
		return false;

	if (pg_cryptohash_init(ctx) == 0 &&
		(first_len == 0 || pg_cryptohash_update(ctx, first, first_len) == 0) &&
		(second_len == 0 || pg_cryptohash_update(ctx, second, second_len) == 0) &&
		pg_cryptohash_final(ctx, result,
							MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH) == 0)
		ok = true;

	pg_cryptohash_free(ctx);
	return ok;
}

/* Decode the lowercase hexadecimal SHA1(SHA1(password)) component. */
static bool
mysql_native_password_decode(const char *secret, uint8 *stage2)
{
	const char *hex = secret + sizeof(MYSQL_NATIVE_PASSWORD_PREFIX) - 1;
	int			i;

	for (i = 0; i < MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH; i++)
	{
		char		hi = hex[i * 2];
		char		lo = hex[i * 2 + 1];
		int			high;
		int			low;

		if (hi >= '0' && hi <= '9')
			high = hi - '0';
		else if (hi >= 'a' && hi <= 'f')
			high = hi - 'a' + 10;
		else
			return false;

		if (lo >= '0' && lo <= '9')
			low = lo - '0';
		else if (lo >= 'a' && lo <= 'f')
			low = lo - 'a' + 10;
		else
			return false;

		stage2[i] = (uint8) ((high << 4) | low);
	}

	return true;
}

/*
 * mysql_native_password_encrypt
 *
 * Return the OpenHalo-format secret for a plaintext MySQL native password.
 * This is intentionally separate from password_encryption, whose allowed
 * PostgreSQL values remain MD5 and SCRAM-SHA-256.
 */
char *
mysql_native_password_encrypt(const char *password)
{
	static const char hex[] = "0123456789abcdef";
	uint8		stage1[MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH];
	uint8		stage2[MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH];
	char	   *secret;
	int			i;

	if (!mysql_native_password_sha1((const uint8 *) password, strlen(password),
								NULL, 0, stage1) ||
		!mysql_native_password_sha1(stage1, sizeof(stage1), NULL, 0, stage2))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not calculate MySQL native password hash")));

	secret = palloc(MYSQL_NATIVE_PASSWORD_SECRET_LEN + 1);
	memcpy(secret, MYSQL_NATIVE_PASSWORD_PREFIX,
		   sizeof(MYSQL_NATIVE_PASSWORD_PREFIX) - 1);
	for (i = 0; i < MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH; i++)
	{
		secret[sizeof(MYSQL_NATIVE_PASSWORD_PREFIX) - 1 + i * 2] =
			hex[stage2[i] >> 4];
		secret[sizeof(MYSQL_NATIVE_PASSWORD_PREFIX) - 1 + i * 2 + 1] =
			hex[stage2[i] & 0x0F];
	}
	secret[MYSQL_NATIVE_PASSWORD_SECRET_LEN] = '\0';

	explicit_bzero(stage1, sizeof(stage1));
	explicit_bzero(stage2, sizeof(stage2));
	return secret;
}

/*
 * mysql_native_password_verify
 *
 * Verify a mysql_native_password scramble against an OpenHalo-format secret:
 *
 *   client response = SHA1(password) XOR SHA1(salt || SHA1(SHA1(password)))
 */
int
mysql_native_password_verify(const char *role, const char *shadow_pass,
							 const uint8 *client_response,
							 size_t client_response_len,
							 const uint8 *salt, size_t salt_len,
							 const char **logdetail)
{
	uint8		stage1[MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH];
	uint8		stage2[MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH];
	uint8		scramble[MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH];
	uint8		candidate_stage2[MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH];
	int			i;
	int			result = STATUS_ERROR;

	if (get_password_type(shadow_pass) != PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD)
	{
		*logdetail = psprintf(_("User \"%s\" has a password that cannot be used with mysql_native_password authentication."),
						  role);
		return STATUS_ERROR;
	}

	if (client_response_len != MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH ||
		!mysql_native_password_decode(shadow_pass, stage2) ||
		!mysql_native_password_sha1(salt, salt_len, stage2, sizeof(stage2),
									 scramble))
	{
		*logdetail = psprintf(_("Invalid mysql_native_password response for user \"%s\"."),
						  role);
		goto done;
	}

	for (i = 0; i < MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH; i++)
		stage1[i] = client_response[i] ^ scramble[i];

	if (!mysql_native_password_sha1(stage1, sizeof(stage1), NULL, 0,
								  candidate_stage2))
	{
		*logdetail = psprintf(_("Could not verify mysql_native_password response for user \"%s\"."),
						  role);
		goto done;
	}

	if (timingsafe_bcmp(stage2, candidate_stage2, sizeof(stage2)) == 0)
		result = STATUS_OK;
	else
		*logdetail = psprintf(_("Password does not match for user \"%s\"."), role);

done:
	explicit_bzero(stage1, sizeof(stage1));
	explicit_bzero(stage2, sizeof(stage2));
	explicit_bzero(scramble, sizeof(scramble));
	explicit_bzero(candidate_stage2, sizeof(candidate_stage2));
	return result;
}


/*
 * mysql_caching_sha2_password_verify
 *
 * MySQL 8.0+ default auth: stored = SHA256(SHA256(password)).
 * Client sends XOR(SHA256(password), SHA256(scramble + stored)).
 * Verify: SHA256(XOR(resp, SHA256(scramble + stored))) == stored.
 */
int
mysql_caching_sha2_password_verify(const char *role, const char *hex_hash,
                                    const uint8 *client_response,
                                    size_t client_response_len,
                                    const uint8 *salt, size_t salt_len,
                                    const char **logdetail)
{
    uint8       stored_hash[32];
    uint8       scramble_hash[32];
    uint8       recovered[32];
    uint8       candidate[32];
    int         i;
    int         result = STATUS_ERROR;

    if (hex_hash == NULL || strlen(hex_hash) != 64)
    {
        *logdetail = "Invalid caching_sha2_password hash";
        return STATUS_ERROR;
    }
    for (i = 0; i < 32; i++)
    {
        char hx[3] = {hex_hash[i*2], hex_hash[i*2+1], 0};
        stored_hash[i] = (uint8) strtol(hx, NULL, 16);
    }

    if (client_response_len != 32) { *logdetail = "Bad SHA256 response length"; goto done; }

    /* scramble_hash = SHA256(salt || stored_hash) */
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, salt_len);
    SHA256_Update(&ctx, stored_hash, sizeof(stored_hash));
    SHA256_Final(scramble_hash, &ctx);

    /* recovered = client_resp XOR scramble_hash */
    for (i = 0; i < 32; i++) recovered[i] = client_response[i] ^ scramble_hash[i];

    /* candidate = SHA256(recovered); compare with stored */
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, recovered, sizeof(recovered));
    SHA256_Final(candidate, &ctx);

    result = (timingsafe_bcmp(stored_hash, candidate, 32) == 0) ? STATUS_OK : STATUS_ERROR;
    if (result != STATUS_OK)
        *logdetail = "Password does not match";

done:
    explicit_bzero(stored_hash, 32);
    explicit_bzero(scramble_hash, 32);
    explicit_bzero(recovered, 32);
    explicit_bzero(candidate, 32);
    return result;
}
