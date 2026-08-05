/*-------------------------------------------------------------------------
 *
 * mys_set.h
 *	 MySQL ENUM/SET label comparison support.
 *
 * MySQL compares ENUM and SET labels according to the declared character
 * set/collation rather than PostgreSQL's database collation.  The parser
 * maps the compatibility collations it supports to one of these profiles
 * and uses the same comparator as the runtime checks and normalizers.
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_SET_H
#define MYS_SET_H

/* SQL-visible profile names also used while constructing generated DDL. */
#define MYS_LABEL_PROFILE_AI		"ai"
#define MYS_LABEL_PROFILE_AS_CS	"as_cs"
#define MYS_LABEL_PROFILE_BINARY	"binary"

typedef enum MysLabelProfile
{
	/* Unicode ICU primary strength; trailing ASCII spaces are insignificant. */
	MYS_LABEL_PROFILE_KIND_AI,
	/* Byte-exact after trimming trailing ASCII spaces. */
	MYS_LABEL_PROFILE_KIND_AS_CS,
	/* Byte-exact; every byte, including trailing spaces, is significant. */
	MYS_LABEL_PROFILE_KIND_BINARY
} MysLabelProfile;

extern MysLabelProfile mys_label_profile_from_name(const char *name);
extern int mys_compare_enum_set_labels(const char *left, int left_len,
								  const char *right, int right_len,
								  MysLabelProfile profile);

#endif							/* MYS_SET_H */
