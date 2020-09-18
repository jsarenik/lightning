#include <bitcoin/privkey.h>
#include <wally_bip39.h>
#include <ccan/crypto/hkdf_sha256/hkdf_sha256.h>
#include <ccan/err/err.h>
#include <ccan/noerr/noerr.h>
#include <ccan/read_write_all/read_write_all.h>
#include <ccan/tal/path/path.h>
#include <ccan/str/str.h>
#include <common/bech32.h>
#include <common/derive_basepoints.h>
#include <common/node_id.h>
#include <common/type_to_string.h>
#include <common/utils.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>
#include <termios.h>

#define ERROR_HSM_FILE errno
#define ERROR_USAGE 2
#define ERROR_LIBSODIUM 3
#define ERROR_LIBWALLY 4
#define ERROR_KEYDERIV 5

static void show_usage(const char *progname)
{
	printf("%s <method> [arguments]\n", progname);
	printf("methods:\n");
	printf("	- decrypt <path/to/hsm_secret> <password>\n");
	printf("	- encrypt <path/to/hsm_secret> <password>\n");
	printf("	- dumpcommitments <node id> <channel dbid> <depth> "
	       "<path/to/hsm_secret> [hsm_secret password]\n");
	printf("	- guesstoremote <P2WPKH address> <node id> <tries> "
	       "<path/to/hsm_secret> [hsm_secret password]\n");
	printf("	- generatehsm <path/to/hsm_secret>\n");
	exit(0);
}

static bool ensure_hsm_secret_exists(int fd, const char *path)
{
	const char *config_dir = path_dirname(NULL, path);
	if (fsync(fd) != 0) {
		close(fd);
		return false;
	}
	if (close(fd) != 0)
		return false;

	fd = open(config_dir, O_RDONLY);
	if (fd < 0)
		return false;
	if (fsync(fd) != 0) {
		close(fd);
		return false;
	}

	close(fd);
	tal_free(config_dir);
	return true;
}

static void get_hsm_secret(struct secret *hsm_secret,
                           const char *hsm_secret_path)
{
	int fd;

	fd = open(hsm_secret_path, O_RDONLY);
	if (fd < 0)
		err(ERROR_HSM_FILE, "Could not open hsm_secret");
	if (!read_all(fd, hsm_secret, sizeof(*hsm_secret)))
		err(ERROR_HSM_FILE, "Could not read hsm_secret");
	close(fd);
}

/* Derive the encryption key from the password provided, and try to decrypt
 * the cipher. */
static void get_encrypted_hsm_secret(struct secret *hsm_secret,
                                     const char *hsm_secret_path,
                                     const char *passwd)
{
	int fd;
	struct secret key;
	u8 salt[16] = "c-lightning\0\0\0\0\0";
	crypto_secretstream_xchacha20poly1305_state crypto_state;
	u8 header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
	/* The cipher size is static with xchacha20poly1305. */
	u8 cipher[sizeof(struct secret) + crypto_secretstream_xchacha20poly1305_ABYTES];

	fd = open(hsm_secret_path, O_RDONLY);
	if (fd < 0)
		err(ERROR_HSM_FILE, "Could not open hsm_secret");

	if (!read_all(fd, header, crypto_secretstream_xchacha20poly1305_HEADERBYTES))
		err(ERROR_HSM_FILE, "Could not read cipher header");
	if (!read_all(fd, cipher, sizeof(cipher)))
		err(ERROR_HSM_FILE, "Could not read cipher body");

	if (crypto_pwhash(key.data, sizeof(key.data), passwd, strlen(passwd), salt,
	                  crypto_pwhash_argon2id_OPSLIMIT_MODERATE,
	                  crypto_pwhash_argon2id_MEMLIMIT_MODERATE,
	                  crypto_pwhash_ALG_ARGON2ID13) != 0)
		err(ERROR_LIBSODIUM, "Could not derive a key from the password.");
	if (crypto_secretstream_xchacha20poly1305_init_pull(&crypto_state, header,
	                                                    key.data) != 0)
		err(ERROR_LIBSODIUM, "Could not initialize the crypto state");
	if (crypto_secretstream_xchacha20poly1305_pull(&crypto_state, hsm_secret->data,
	                                               NULL, 0, cipher, sizeof(cipher),
	                                               NULL, 0) != 0)
		err(ERROR_LIBSODIUM, "Could not retrieve the seed. Wrong password ?");

	close(fd);
}

/* Taken from hsmd. */
static void get_channel_seed(struct secret *channel_seed, struct node_id *peer_id,
                             u64 dbid, struct secret *hsm_secret)
{
	struct secret channel_base;
	u8 input[sizeof(peer_id->k) + sizeof(dbid)];
	/*~ Again, "per-peer" should be "per-channel", but Hysterical Raisins */
	const char *info = "per-peer seed";

	/*~ We use the DER encoding of the pubkey, because it's platform
	 * independent.  Since the dbid is unique, however, it's completely
	 * unnecessary, but again, existing users can't be broken. */
	/* FIXME: lnd has a nicer BIP32 method for deriving secrets which we
	 * should migrate to. */
	hkdf_sha256(&channel_base, sizeof(struct secret), NULL, 0,
	            hsm_secret, sizeof(*hsm_secret),
	            /*~ Initially, we didn't support multiple channels per
	             * peer at all: a channel had to be completely forgotten
	             * before another could exist.  That was slightly relaxed,
	             * but the phrase "peer seed" is wired into the seed
	             * generation here, so we need to keep it that way for
	             * existing clients, rather than using "channel seed". */
	             "peer seed", strlen("peer seed"));
	memcpy(input, peer_id->k, sizeof(peer_id->k));
	BUILD_ASSERT(sizeof(peer_id->k) == PUBKEY_CMPR_LEN);
	/*~ For all that talk about platform-independence, note that this
	 * field is endian-dependent!  But let's face it, little-endian won.
	 * In related news, we don't support EBCDIC or middle-endian. */
	memcpy(input + PUBKEY_CMPR_LEN, &dbid, sizeof(dbid));

	hkdf_sha256(channel_seed, sizeof(*channel_seed),
	            input, sizeof(input),
	            &channel_base, sizeof(channel_base),
	            info, strlen(info));
}

static int decrypt_hsm(const char *hsm_secret_path, const char *passwd)
{
	int fd;
	struct stat st;
	struct secret hsm_secret;
	const char *dir, *backup;

	if (sodium_init() == -1)
		err(ERROR_LIBSODIUM,
		    "Could not initialize libsodium. Not enough entropy ?");

	dir = path_dirname(NULL, hsm_secret_path);
	backup = path_join(dir, dir, "hsm_secret.backup");

	if (stat(hsm_secret_path, &st) != 0)
		err(ERROR_HSM_FILE, "Could not stat hsm_secret");
	if (st.st_size <= 32)
		err(ERROR_HSM_FILE, "hsm_secret is not encrypted");
	get_encrypted_hsm_secret(&hsm_secret, hsm_secret_path, passwd);

	/* Create a backup file, "just in case". */
	rename(hsm_secret_path, backup);
	fd = open(hsm_secret_path, O_CREAT|O_EXCL|O_WRONLY, 0400);
	if (fd < 0)
		err(ERROR_HSM_FILE, "Could not open new hsm_secret");

	if (!write_all(fd, &hsm_secret, sizeof(hsm_secret))) {
		unlink_noerr(hsm_secret_path);
		close(fd);
		rename("hsm_secret.backup", hsm_secret_path);
		err(ERROR_HSM_FILE,
		    "Failure writing plaintext seed to hsm_secret.");
	}

	/* Be as paranoïd as in hsmd with the file state on disk. */
	if (!ensure_hsm_secret_exists(fd, hsm_secret_path)) {
		unlink_noerr(hsm_secret_path);
		rename(backup, hsm_secret_path);
		err(ERROR_HSM_FILE,
		    "Could not ensure hsm_secret existence.");
	}
	unlink_noerr(backup);
	tal_free(dir);

	printf("Succesfully decrypted hsm_secret, be careful now :-).\n");
	return 0;
}

static int encrypt_hsm(const char *hsm_secret_path, const char *passwd)
{
	int fd;
	struct stat st;
	struct secret key, hsm_secret;
	u8 salt[16] = "c-lightning\0\0\0\0\0";
	crypto_secretstream_xchacha20poly1305_state crypto_state;
	u8 header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
	/* The cipher size is static with xchacha20poly1305. */
	u8 cipher[sizeof(struct secret) + crypto_secretstream_xchacha20poly1305_ABYTES];
	const char *dir, *backup;

	dir = path_dirname(NULL, hsm_secret_path);
	backup = path_join(dir, dir, "hsm_secret.backup");

	if (sodium_init() == -1)
		err(ERROR_LIBSODIUM,
		    "Could not initialize libsodium. Not enough entropy ?");

	if (stat(hsm_secret_path, &st) != 0)
		err(ERROR_HSM_FILE, "Could not stat hsm_secret");
	if (st.st_size > 32)
		err(ERROR_USAGE, "hsm_secret is already encrypted");
	get_hsm_secret(&hsm_secret, hsm_secret_path);

	/* Derive the encryption key from the password provided, and try to encrypt
	 * the seed. */
	if (crypto_pwhash(key.data, sizeof(key.data), passwd, strlen(passwd), salt,
	                  crypto_pwhash_argon2id_OPSLIMIT_MODERATE,
	                  crypto_pwhash_argon2id_MEMLIMIT_MODERATE,
	                  crypto_pwhash_ALG_ARGON2ID13) != 0)
		err(ERROR_LIBSODIUM, "Could not derive a key from the password.");
	if (crypto_secretstream_xchacha20poly1305_init_push(&crypto_state, header,
	                                                    key.data) != 0)
		err(ERROR_LIBSODIUM, "Could not initialize the crypto state");
	if (crypto_secretstream_xchacha20poly1305_push(&crypto_state, cipher,
	                                               NULL, hsm_secret.data,
	                                               sizeof(hsm_secret.data),
	                                               NULL, 0, 0) != 0)
		err(ERROR_LIBSODIUM, "Could not encrypt the seed.");

	/* Create a backup file, "just in case". */
	rename(hsm_secret_path, backup);
	fd = open(hsm_secret_path, O_CREAT|O_EXCL|O_WRONLY, 0400);
	if (fd < 0)
		err(ERROR_HSM_FILE, "Could not open new hsm_secret");

	/* Write the encrypted hsm_secret. */
	if (!write_all(fd, header, sizeof(header))
		|| !write_all(fd, cipher, sizeof(cipher))) {
		unlink_noerr(hsm_secret_path);
		close(fd);
		rename(backup, hsm_secret_path);
		err(ERROR_HSM_FILE, "Failure writing cipher to hsm_secret.");
	}

	/* Be as paranoïd as in hsmd with the file state on disk. */
	if (!ensure_hsm_secret_exists(fd, hsm_secret_path)) {
		unlink_noerr(hsm_secret_path);
		rename(backup, hsm_secret_path);
		err(ERROR_HSM_FILE, "Could not ensure hsm_secret existence.");
	}
	unlink_noerr(backup);
	tal_free(dir);

	printf("Succesfully encrypted hsm_secret. You'll now have to pass the "
	       "--encrypted-hsm startup option.\n");
	return 0;
}

static int dump_commitments_infos(struct node_id *node_id, u64 channel_id,
                                  u64 depth, char *hsm_secret_path, char *passwd)
{
	struct sha256 shaseed;
	struct secret hsm_secret, channel_seed, per_commitment_secret;
	struct pubkey per_commitment_point;

	secp256k1_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY
	                                         | SECP256K1_CONTEXT_SIGN);

	if (passwd)
		get_encrypted_hsm_secret(&hsm_secret, hsm_secret_path, passwd);
	else
		get_hsm_secret(&hsm_secret, hsm_secret_path);
	get_channel_seed(&channel_seed, node_id, channel_id, &hsm_secret);

	derive_shaseed(&channel_seed, &shaseed);
	printf("shaseed: %s\n", type_to_string(tmpctx, struct sha256, &shaseed));
	for (u64 i = 0; i < depth; i++) {
		if (!per_commit_secret(&shaseed, &per_commitment_secret, i))
			err(ERROR_KEYDERIV, "Could not derive secret #%"PRIu64, i);
		printf("commit secret #%"PRIu64": %s\n",
		       i, tal_hexstr(tmpctx, per_commitment_secret.data,
		                     sizeof(per_commitment_secret.data)));
		if (!per_commit_point(&shaseed, &per_commitment_point, i))
			err(ERROR_KEYDERIV, "Could not derive point #%"PRIu64, i);
		printf("commit point #%"PRIu64": %s\n",
		       i, type_to_string(tmpctx, struct pubkey, &per_commitment_point));
	}

	return 0;
}

/* In case of an unilateral close from the remote side while we suffered a
 * loss of data, this tries to recover the private key from the `to_remote`
 * output.
 * This basically iterates over every `dbid` to derive the channel_seed and
 * then derives the payment basepoint to compare to the pubkey hash specified
 * in the witness programm.
 * Note that since a node generates the key for the to_remote output from its
 * *local* per_commitment_point, there is nothing we can do if
 * `option_static_remotekey` was not negotiated.
 *
 * :param address: The bech32 address of the v0 P2WPKH witness programm
 * :param node_id: The id of the node with which the channel was established
 * :param tries: How many dbids to try.
 * :param hsm_secret_path: The path to the hsm_secret
 * :param passwd: The *optional* hsm_secret password
 */
static int guess_to_remote(const char *address, struct node_id *node_id,
                           u64 tries, char *hsm_secret_path, char *passwd)
{
	struct secret hsm_secret, channel_seed, basepoint_secret;
	struct pubkey basepoint;
	struct ripemd160 pubkeyhash;
	/* We only support P2WPKH, hence 20. */
	u8 goal_pubkeyhash[20];
	/* See common/bech32.h for buffer size. */
	char hrp[strlen(address) - 6];
	int witver;
	size_t witlen;

	/* Get the hrp to accept addresses from any network. */
	if (bech32_decode(hrp, goal_pubkeyhash, &witlen, address, 90) != 1)
		errx(ERROR_USAGE, "Could not get address' network");
	if (segwit_addr_decode(&witver, goal_pubkeyhash, &witlen, hrp, address) != 1)
		errx(ERROR_USAGE, "Wrong bech32 address");

	secp256k1_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY
	                                         | SECP256K1_CONTEXT_SIGN);

	if (passwd)
		get_encrypted_hsm_secret(&hsm_secret, hsm_secret_path, passwd);
	else
		get_hsm_secret(&hsm_secret, hsm_secret_path);

	for (u64 dbid = 1; dbid < tries ; dbid++) {
		get_channel_seed(&channel_seed, node_id, dbid, &hsm_secret);
		if (!derive_payment_basepoint(&channel_seed,
		                              &basepoint, &basepoint_secret))
			errx(ERROR_KEYDERIV, "Could not derive basepoints for dbid %"PRIu64
			                     " and channel seed %s.", dbid,
			                     type_to_string(tmpctx,
			                                    struct secret, &channel_seed));

		pubkey_to_hash160(&basepoint, &pubkeyhash);
		if (memcmp(pubkeyhash.u.u8, goal_pubkeyhash, 20) == 0) {
			printf("bech32      : %s\n", address);
			printf("pubkey hash : %s\n",
			       tal_hexstr(tmpctx, pubkeyhash.u.u8, 20));
			printf("pubkey      : %s \n",
			       type_to_string(tmpctx, struct pubkey, &basepoint));
			printf("privkey     : %s \n",
			       type_to_string(tmpctx, struct secret, &basepoint_secret));
			return 0;
		}
	}

	printf("Could not find any basepoint matching the provided witness programm.\n"
	       "Are you sure that the channel used `option_static_remotekey` ?\n");
	return 1;
}

static void read_mnemonic(char *mnemonic) {
	printf("Introduce the BIP39 24 word list:\n");
	char word[50];
	uint n_words;
	for (n_words = 0; n_words < 24; n_words++) {
		scanf("%s", word);
		/* We need to concat the strings using ' ' so libwally can
		 * validate it */
		if (n_words == 0) {
			strcpy(mnemonic, word);
		} else {
			strcat(mnemonic, " ");
			strcat(mnemonic, word);
		}
	}
	/* scanf() leaves the newline character in the input buffer */
	getchar();
}

static void read_passphrase(char *passphrase) {
	struct termios current_term, temp_term;
	printf("Warning: remember that different passphrase yield different "
	       "bitcoin wallets.\n");
	printf("Leave it empty for using the value by default (echo is "
	       "disabled now).\n");
	printf("Enter your passphrase: ");

	/* Change terminal options so we do not echo the passphrase */
	if (tcgetattr(fileno(stdin), &current_term) != 0)
		errx(ERROR_USAGE, "Could not get current terminal options.");
	temp_term = current_term;
	temp_term.c_lflag &= ~ECHO;
	if (tcsetattr(fileno(stdin), TCSAFLUSH, &temp_term) != 0)
		errx(ERROR_USAGE, "Could not disable passphrase echoing.");
	/* If we don't flush we might end up being buffered and we might seem
	 * to hang while we wait for the password. */
	fflush(stdout);

	size_t passphrase_size;
	if (getline(&passphrase, &passphrase_size, stdin) < 0)
		errx(ERROR_USAGE, "Could not read passphrase from stdin.");
	printf("passphrase size: %ld\n", passphrase_size);
	/* Replace the last character which will always be '\n' */
	passphrase[strlen(passphrase) - 1] = '\0';

	if (tcsetattr(fileno(stdin), TCSAFLUSH, &current_term) != 0)
		errx(ERROR_USAGE, "Could not restore terminal options.");
}

static int generate_hsm(const char *hsm_secret_path)
{
	char mnemonic[BIP39_WORDLIST_LEN];
	read_mnemonic(mnemonic);
	if (bip39_mnemonic_validate(NULL, mnemonic) != 0)
		errx(ERROR_USAGE, "Invalid mnemonic");
	
	char *passphrase = NULL;
	read_passphrase(passphrase);

	u8 bip32_seed[BIP39_SEED_LEN_512];
	size_t bip32_seed_len;

	if (bip39_mnemonic_to_seed(mnemonic, passphrase, bip32_seed, sizeof(bip32_seed), &bip32_seed_len) != WALLY_OK)
		errx(ERROR_LIBWALLY, "Unable to derive BIP32 seed from BIP39 mnemonic");
	

	int fd = open(hsm_secret_path, O_CREAT|O_EXCL|O_WRONLY, 0400);
	if (fd < 0) {
		errx(ERROR_USAGE, "Unable to create hsm_secret file");
	}
	if (!write_all(fd, bip32_seed, bip32_seed_len))
		errx(ERROR_USAGE, "Error writing secret to hsm_secret file");

	if (fsync(fd) != 0)
		errx(ERROR_USAGE, "Error fsycning hsm_secret file");

	/* This should never fail if fsync succeeded. But paranoia is good, and bugs exist */
	if (close(fd) != 0) 
		errx(ERROR_USAGE, "Error closing hsm_secret file");
	
	printf("New hsm_secret file created. Use hsmtool to encrypt the BIP32 seed if needed");

	return 0;
}

int main(int argc, char *argv[])
{
	const char *method;

	setup_locale();
	err_set_progname(argv[0]);

	method = argc > 1 ? argv[1] : NULL;
	if (!method)
		show_usage(argv[0]);

	if (streq(method, "decrypt")) {
		if (argc < 4)
			show_usage(argv[0]);
		return decrypt_hsm(argv[2], argv[3]);
	}

	if (streq(method, "encrypt")) {
		if (argc < 4)
			show_usage(argv[0]);
		return encrypt_hsm(argv[2], argv[3]);
	}

	if (streq(method, "dumpcommitments")) {
		/*   node_id    channel_id   depth    hsm_secret  ?password? */
		if (argc < 6)
			show_usage(argv[0]);
		struct node_id node_id;
		if (!node_id_from_hexstr(argv[2], strlen(argv[2]), &node_id))
			err(ERROR_USAGE, "Bad node id");
		return dump_commitments_infos(&node_id, atol(argv[3]), atol(argv[4]),
		                              argv[5], argc >= 7 ? argv[6] : NULL);
	}

	if (streq(method, "guesstoremote")) {
		/*   address    node_id    depth    hsm_secret  ?password? */
		if (argc < 6)
			show_usage(argv[0]);
		struct node_id node_id;
		if (!node_id_from_hexstr(argv[3], strlen(argv[3]), &node_id))
			errx(ERROR_USAGE, "Bad node id");
		return guess_to_remote(argv[2], &node_id, atol(argv[4]),
		                       argv[5], argc >= 7 ? argv[6] : NULL);
	}

	if (streq(method, "generatehsm")) {
		if ( argc != 3)
			show_usage(argv[0]);

		char *hsm_secret_path = argv[2];

		/* if hsm_secred already exists we abort the process
		 * we do not want to lose someone else's funds */
		struct stat st;
		if (stat(hsm_secret_path, &st) == 0)
			errx(ERROR_USAGE, "hsm_secret file already exists");

		return generate_hsm(hsm_secret_path);
	}

	show_usage(argv[0]);
}
