/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  Interface to libpq/crypt.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/crypt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include "datatype/timestamp.h"

/*
 * Valid password hashes may be very long, but we don't want to store anything
 * that might need out-of-line storage, since de-TOASTing won't work during
 * authentication because we haven't selected a database yet and cannot read
 * pg_class.  512 bytes should be more than enough for all practical use, and
 * our own password encryption routines should never produce hashes longer than
 * this.
 */
#define MAX_ENCRYPTED_PASSWORD_LEN (512)

/*
 * OpenHalo's MySQL native-password secret format.  It stores
 * SHA1(SHA1(password)) as lowercase hexadecimal after this prefix.  The
 * format is deliberately not exposed through password_encryption: standard
 * PostgreSQL sessions must continue to use only PostgreSQL-supported secret
 * formats.
 */
#define MYSQL_NATIVE_PASSWORD_PREFIX "mysql_native_password:"
#define MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH    20
#define MYSQL_CACHING_SHA2_DIGEST_LENGTH       32
#define MYSQL_NATIVE_PASSWORD_SECRET_LEN \
	(sizeof(MYSQL_NATIVE_PASSWORD_PREFIX) - 1 + \
	 MYSQL_NATIVE_PASSWORD_DIGEST_LENGTH * 2)

/* Enables deprecation warnings for MD5 passwords. */
extern PGDLLIMPORT bool md5_password_warnings;

/*
 * Types of password hashes or secrets.
 *
 * Plaintext passwords can be passed in by the user, in a CREATE/ALTER USER
 * command. Standard PostgreSQL password handling encrypts them to MD5 or
 * SCRAM-SHA-256 before storing on disk, and those remain the only values
 * selectable through password_encryption.  A compatibility migration can
 * additionally store an explicitly prefixed compatibility secret in
 * pg_authid.rolpassword; it is accepted only by that protocol's dedicated
 * authentication path.
 */
typedef enum PasswordType
{
	PASSWORD_TYPE_PLAINTEXT = 0,
	PASSWORD_TYPE_MD5,
	PASSWORD_TYPE_SCRAM_SHA_256,
	PASSWORD_TYPE_MYSQL_NATIVE_PASSWORD,
	PASSWORD_TYPE_MYSQL_CACHING_SHA2_PASSWORD,
} PasswordType;

extern PasswordType get_password_type(const char *shadow_pass);
extern char *encrypt_password(PasswordType target_type, const char *role,
							  const char *password);

extern char *get_role_password(const char *role, const char **logdetail);

extern int	md5_crypt_verify(const char *role, const char *shadow_pass,
							 const char *client_pass, const uint8 *md5_salt,
							 int md5_salt_len, const char **logdetail);
extern int	plain_crypt_verify(const char *role, const char *shadow_pass,
							   const char *client_pass,
							   const char **logdetail);

extern char *mysql_native_password_encrypt(const char *password);
extern int	mysql_native_password_verify(const char *role,
									 const char *shadow_pass,
									 const uint8 *client_response,
									 size_t client_response_len,
									 const uint8 *salt, size_t salt_len,
									 const char **logdetail);

#endif
extern int mysql_caching_sha2_password_verify(const char *role, const char *hex_hash, const uint8 *client_response, size_t client_response_len, const uint8 *salt, size_t salt_len, const char **logdetail);
