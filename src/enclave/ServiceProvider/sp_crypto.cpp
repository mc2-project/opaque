#include "sp_crypto.h"
// print functions for debugging
lc_status_t print_priv_key(lc_ec256_private_t p_private) {
  uint8_t *ptr = (uint8_t *) p_private.r;
  printf("Private key: ");
  print_hex(ptr, LC_ECP256_KEY_SIZE);
  printf("\n");

  return LC_SUCCESS;
}


lc_status_t print_pub_key(lc_ec256_public_t p_public) {
  uint8_t *ptr = (uint8_t *) p_public.gx;
  printf("Public key gx: ");
  print_hex(ptr, LC_ECP256_KEY_SIZE);
  printf("\n");

  ptr = (uint8_t *) p_public.gy;
  printf("Public key gy: ");
  print_hex(ptr, LC_ECP256_KEY_SIZE);
  printf("\n");

  return LC_SUCCESS;
}

lc_status_t print_ec_key(EC_KEY *ec_key) {
  printf("Print ec_key \n");

  BIO *o = BIO_new_fp(stdout, BIO_NOCLOSE);

  EC_GROUP *group = (EC_GROUP *) EC_KEY_get0_group(ec_key);
  EC_POINT *point = (EC_POINT *) EC_KEY_get0_public_key(ec_key);

  BIGNUM *x_ec = BN_new();
  BIGNUM *y_ec = BN_new();
  EC_POINT_get_affine_coordinates_GFp(group, point, x_ec, y_ec, NULL);

  printf("Pub key coordinates: \n");
  BN_print(o, x_ec);
  printf("\n");
  BN_print(o, y_ec);
  printf("\n");

  const BIGNUM *priv_bn = EC_KEY_get0_private_key(ec_key);

  printf("Private key: ");
  BN_print(o, priv_bn);
  printf("\n");

  ECParameters_print(o, ec_key);

  BIO_free_all(o);
  BN_free(x_ec);
  BN_free(y_ec);

  return LC_SUCCESS;
}


// helper functions
void reverse_endian(uint8_t *input, uint8_t *output, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    *(output+i) = *(input+len-i-1);
  }
}

void reverse_endian_by_32(uint8_t *input, uint8_t *output, uint32_t len) {
  uint32_t actual_len = len / sizeof(uint32_t);
  for (uint32_t i = 0; i < actual_len; i++) {
    for (uint32_t j = 0; j < 4; j++) {
      //*(output+i*4+j) = *(input+(actual_len-i-1)*4+(4-j-1));
      *(output+i*4+j) = *(input+i*4+4-j-1);
    }
  }
}


lc_status_t lc_ssl2sgx(EC_KEY *ssl_key,
                       lc_ec256_private_t *p_private,
                       lc_ec256_public_t *p_public) {

  EC_GROUP *group = (EC_GROUP *) EC_KEY_get0_group(ssl_key);
  EC_POINT *point = (EC_POINT *) EC_KEY_get0_public_key(ssl_key);

  // get pub key coordinates
  BIGNUM *x_ec = BN_new();
  BIGNUM *y_ec = BN_new();
  EC_POINT_get_affine_coordinates_GF2m(group, point, x_ec, y_ec, NULL);

  // get private key
  const BIGNUM *priv_bn = EC_KEY_get0_private_key(ssl_key);

  // Store the public and private keys in binary format
  unsigned char *x_ = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);
  unsigned char *y_ = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);
  unsigned char *r_ = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);

  unsigned char *x = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);
  unsigned char *y = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);
  unsigned char *r = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);

  BN_bn2bin(x_ec, x_);
  BN_bn2bin(y_ec, y_);
  BN_bn2bin(priv_bn, r_);

  // reverse x_, y_, r_ because of endian-ness
  reverse_endian(x_, x, LC_ECP256_KEY_SIZE);
  reverse_endian(y_, y, LC_ECP256_KEY_SIZE);
  reverse_endian(r_, r, LC_ECP256_KEY_SIZE);

  memcpy_s(p_public->gx, LC_ECP256_KEY_SIZE, x, LC_ECP256_KEY_SIZE);
  memcpy_s(p_public->gy, LC_ECP256_KEY_SIZE, y, LC_ECP256_KEY_SIZE);
  memcpy_s(p_private->r, LC_ECP256_KEY_SIZE, r, LC_ECP256_KEY_SIZE);

  free(x_);
  free(y_);
  free(r_);
  free(x);
  free(y);
  free(r);

  // free BN
  BN_free(x_ec);
  BN_free(y_ec);

  return LC_SUCCESS;
}

// This is a wrapper around the OpenSSL EVP AES-GCM encryption
lc_status_t lc_rijndael128GCM_encrypt(const lc_aes_gcm_128bit_key_t *p_key,
                                      const uint8_t *p_src, uint32_t src_len,
                                      uint8_t *p_dst,
                                      const uint8_t *p_iv, uint32_t iv_len,
                                      const uint8_t *p_aad, uint32_t aad_len,
                                      lc_aes_gcm_128bit_tag_t *p_out_mac) {

  EVP_CIPHER_CTX *ctx = NULL;
  int ret = 0;
  int len = 0;
  uint32_t ciphertext_len;

  (void) p_aad;
  (void) aad_len;

  /* Create and initialise the context */
  ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    fprintf(stderr, "[%s] EVP context init failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  /* Initialise the encryption operation. */
  ret = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL);
  if (ret != 1) {
    fprintf(stderr, "[%s] encryption init failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  /* Set IV length if default 12 bytes (96 bits) is not appropriate */
  ret = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL);
  if (ret != 1) {
    fprintf(stderr, "[%s] encryption IV length init failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  print_hex((unsigned char *) *p_key, 16);
  printf("\n");

  /* Initialise key and IV */
  ret = EVP_EncryptInit_ex(ctx, NULL, NULL, (const unsigned char *) *p_key, p_iv);
  if (ret != 1) {
    fprintf(stderr, "[%s] encryption init failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  /* Provide any AAD data. This can be called zero or more times as
   * required
   */
  if (p_aad != NULL) {
    ret = EVP_EncryptUpdate(ctx, NULL, &len, p_aad, aad_len);
    if (ret != 1) {
      fprintf(stderr, "[%s] encryption AAD update failure\n", __FUNCTION__);
      return LC_ERROR_UNEXPECTED;
    }
  }

  /* Provide the message to be encrypted, and obtain the encrypted output.
   * EVP_EncryptUpdate can be called multiple times if necessary
   */
  ret = EVP_EncryptUpdate(ctx, p_dst, &len, p_src, (int) src_len);
  if (ret != 1) {
    fprintf(stderr, "[%s] encryption update failure, ret is %u, len is %u\n", __FUNCTION__, ret, len);
    return LC_ERROR_UNEXPECTED;
  }
  ciphertext_len = len;

  /* Finalise the encryption. Normally ciphertext bytes may be written at
   * this stage, but this does not occur in GCM mode
   */
  ret = EVP_EncryptFinal_ex(ctx, p_dst + len, &len);
  if (ret != 1) {
    fprintf(stderr, "[%s] encryption final failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }
  ciphertext_len += len;

#ifdef DEBUG
  printf("[%s] ciphertext_len is %u\n", __FUNCTION__, ciphertext_len);
#endif

  /* Get the tag */
  ret = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, LC_AESGCM_MAC_SIZE, (unsigned char *) *p_out_mac);
  if (ret != 1) {
    fprintf(stderr, "[%s]  \n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  /* Clean up */
  EVP_CIPHER_CTX_free(ctx);

  return LC_SUCCESS;
}


lc_status_t lc_rijndael128GCM_decrypt(unsigned char *ciphertext, int ciphertext_len, unsigned char *aad,
                                      int aad_len, unsigned char *tag, unsigned char *key, unsigned char *iv,
                                      unsigned char *plaintext)
{
  EVP_CIPHER_CTX *ctx;
  int len;
  int plaintext_len;
  int ret;

  (void)plaintext_len;
  (void)ret;

  /* Create and initialise the context */
  if(!(ctx = EVP_CIPHER_CTX_new())) {
    printf("ctx not initialized correct\n");
  }

  /* Initialise the decryption operation. */
  if(!EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL)) {
    printf("evp decryption not initialized correct\n");
  }

  /* Set IV length. Not necessary if this is 12 bytes (96 bits) */
  if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL)) {
    printf("evp IV size not correctly set\n");
    return LC_ERROR_UNEXPECTED;
  }

  /* Initialise key and IV */
  if(!EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv)) {
    fprintf(stderr, "evp decryption init incorrect\n");
    return LC_ERROR_UNEXPECTED;
  }

  /* Provide any AAD data. This can be called zero or more times as
   * required
   */
  if (aad != NULL)  {
    if (!EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len)) {
      fprintf(stderr, "evp decryption aad update failed\n");
      return LC_ERROR_UNEXPECTED;
    }
  }

  /* Provide the message to be decrypted, and obtain the plaintext output.
   * EVP_DecryptUpdate can be called multiple times if necessary
   */
  if (!EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len)) {
    fprintf(stderr, "decryption update failed\n");
    return LC_ERROR_UNEXPECTED;
  }

  plaintext_len = len;

  /* Set expected tag value. Works in OpenSSL 1.0.1d and later */
  if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag)) {
    fprintf(stderr, "decryption tag setting failed\n");
    return LC_ERROR_UNEXPECTED;
  }

  /* Finalise the decryption. A positive return value indicates success,
   * anything else is a failure - the plaintext is not trustworthy.
   */
  ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);

  /* Clean up */
  EVP_CIPHER_CTX_free(ctx);

  if (ret > 0) {
    return LC_SUCCESS;
  } else {
    return LC_ERROR_UNEXPECTED;
  }

  return LC_SUCCESS;
}

lc_status_t lc_rijndael128_cmac_msg(const lc_cmac_128bit_key_t *p_key,
                                    const uint8_t *p_src, uint32_t src_len,
                                    lc_cmac_128bit_tag_t *p_mac) {
  uint32_t p_mac_len = 16;
  int ret = 0;

  // reverse p_key
  lc_cmac_128bit_key_t p_key_;
  uint8_t *p_key_ptr = (uint8_t *) p_key_;
  reverse_endian((uint8_t *) p_key, p_key_ptr, sizeof(lc_cmac_128bit_key_t));

  CMAC_CTX *ctx = CMAC_CTX_new();
  if (!ctx) {
    fprintf(stderr, "[%s] CMAC context init failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  ret = CMAC_Init(ctx, p_key, LC_CMAC_KEY_SIZE, EVP_aes_128_cbc(), NULL);
  if (ret != 1) {
    fprintf(stderr, "[%s] CMAC key init failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  ret = CMAC_Update(ctx, p_src, src_len);
  if (ret != 1) {
    fprintf(stderr, "[%s] CMAC failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  ret = CMAC_Final(ctx, (unsigned char *) *p_mac, (size_t *) &p_mac_len);
  if (ret != 1) {
    fprintf(stderr, "[%s] CMAC final output failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  //printf("[%s] LC_CMAC_KEY_SIZE is %u, p_mac_len is %u\n", __FUNCTION__, LC_CMAC_KEY_SIZE, p_mac_len);
  CMAC_CTX_free(ctx);

  return LC_SUCCESS;
}

lc_status_t lc_ecc256_open_context(lc_ecc_state_handle_t* ecc_handle) {
  (void) ecc_handle;
  *ecc_handle = NULL;
  return LC_SUCCESS;
}


lc_status_t lc_ecc256_close_context(lc_ecc_state_handle_t ecc_handle) {
  (void) ecc_handle;
  ecc_handle = NULL;
  return LC_SUCCESS;
}

lc_status_t lc_ecc256_create_key_pair(lc_ec256_private_t *p_private,
                                      lc_ec256_public_t *p_public,
                                      lc_ecc_state_handle_t ecc_handle) {

  (void) ecc_handle;

  EC_KEY *key = NULL;
  int ret = 0;

  // enclave can only use P-256, so we will use that curve here as well
  key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  if (key == NULL) {
    fprintf(stderr, "[%s] EC key creation failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

  /* Generate the private and public key */
  ret = EC_KEY_generate_key(key);
  if (ret != 1) {
    fprintf(stderr, "[%s] EC key generation failure\n", __FUNCTION__);
    return LC_ERROR_UNEXPECTED;
  }

#ifdef DEBUG
  //print_ec_key(key);
#endif

  // convert the key information into sgx crypto library compatible formats
  lc_ssl2sgx(key, p_private, p_public);

  EC_KEY_free(key);

  return LC_SUCCESS;
}

EC_POINT *get_ec_point(lc_ec256_public_t *p_public) {

  unsigned char *x = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);
  unsigned char *y = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);

  // reverse for endian-ness
  reverse_endian(p_public->gx, x, LC_ECP256_KEY_SIZE);
  reverse_endian(p_public->gy, y, LC_ECP256_KEY_SIZE);

  BIGNUM *x_ec = BN_new();
  BIGNUM *y_ec = BN_new();
  BN_bin2bn(x, LC_ECP256_KEY_SIZE, x_ec);
  BN_bin2bn(y, LC_ECP256_KEY_SIZE, y_ec);

  EC_GROUP *curve = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  if (curve == NULL) {
    fprintf(stderr, "[%s] curve is NULL\n", __FUNCTION__);
    return NULL;
  }

  EC_POINT *pub_key_ret = EC_POINT_new(curve);
  EC_POINT_set_affine_coordinates_GFp(curve, pub_key_ret, x_ec, y_ec, NULL);
  assert(pub_key_ret != NULL);

#ifdef false
  BIO *o = BIO_new_fp(stdout, BIO_NOCLOSE);

  BIGNUM *x_ec_ = BN_new();
  BIGNUM *y_ec_ = BN_new();
  EC_POINT_get_affine_coordinates_GFp(curve, pub_key_ret, x_ec_, y_ec_, NULL);

  printf("[%s] Retrieved coordinates: \n", __FUNCTION__);
  BN_print(o, x_ec);
  printf("\n");
  BN_print(o, y_ec);
  printf("\n");

  printf("[%s] Pub key coordinates: \n", __FUNCTION__);
  BN_print(o, x_ec_);
  printf("\n");
  BN_print(o, y_ec_);
  printf("\n");

  BIO_free_all(o);
  BN_free(x_ec_);
  BN_free(y_ec_);
#endif

  BN_free(x_ec);
  BN_free(y_ec);
  EC_GROUP_free(curve);

  free(x);
  free(y);

  return pub_key_ret;
}

EC_KEY *get_priv_key(lc_ec256_private_t *p_private) {
  int ret = 0;

  unsigned char *r = (unsigned char *) malloc(LC_ECP256_KEY_SIZE);
  // reverse endian-ness
  reverse_endian(p_private->r, r, LC_ECP256_KEY_SIZE);

  BIGNUM *r_ec = BN_new();
  BN_bin2bn(r, LC_ECP256_KEY_SIZE, r_ec);

  EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  EC_GROUP *curve = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);

  ret = EC_KEY_set_private_key(key, r_ec);
  if (ret != 1) {
    fprintf(stderr, "[%s] set private key failure\n", __FUNCTION__);
    return NULL;
  }

  EC_POINT *pub_key = EC_POINT_new(curve);
  EC_POINT_mul(curve, pub_key, r_ec, NULL, NULL, NULL);

  ret = EC_KEY_set_public_key(key, pub_key);
  if (ret != 1) {
    fprintf(stderr, "[%s] set pub key failure\n", __FUNCTION__);
    return NULL;
  }

  free(r);
  BN_free(r_ec);
  EC_GROUP_free(curve);
  EC_POINT_free(pub_key);

  return key;
}

lc_status_t lc_ecc256_compute_shared_dhkey(lc_ec256_private_t *p_private_b,
                                           lc_ec256_public_t *p_public_ga,
                                           lc_ec256_dh_shared_t *p_shared_key,
                                           lc_ecc_state_handle_t ecc_handle) {

  (void)p_private_b;
  (void)p_public_ga;
  (void)p_shared_key;
  (void)ecc_handle;

  lc_ec256_dh_shared_t reverse;

  // shared secret is an AES symmetric key
  EC_POINT *pub_key = get_ec_point(p_public_ga);
  EC_KEY *priv_key = get_priv_key(p_private_b);
  EC_POINT *sec = EC_POINT_new(EC_KEY_get0_group(priv_key));

  ECDH_compute_key(reverse.s,
                   LC_ECP256_KEY_SIZE,
                   pub_key, priv_key, NULL);

  reverse_endian(reverse.s, p_shared_key->s, LC_ECP256_KEY_SIZE);


  EC_POINT_free(pub_key);
  EC_KEY_free(priv_key);
  EC_POINT_free(sec);

  return LC_SUCCESS;

}

lc_status_t lc_ecdsa_sign(const uint8_t *p_data,
                          uint32_t data_size,
                          lc_ec256_private_t *p_private,
                          lc_ec256_signature_t *p_signature,
                          lc_ecc_state_handle_t ecc_handle) {
  (void)p_data;
  (void)data_size;
  (void)p_private;
  (void)p_signature;
  (void)ecc_handle;

  EC_KEY *key = get_priv_key(p_private);
  assert(key != NULL);

#ifdef DEBUG
  printf("\n\n[%s]\t", __FUNCTION__);
  print_hex((uint8_t *) p_data, data_size);
  printf("data_size: %u\n", data_size);
  print_ec_key(key);
  printf("\n");
#endif

  // first, hash the data
  lc_sha_state_handle_t p_sha_handle;
  lc_sha256_hash_t p_hash;
  lc_sha256_init(&p_sha_handle);
  lc_sha256_update(p_data, data_size, p_sha_handle);
  lc_sha256_get_hash(p_sha_handle, &p_hash);
  lc_sha256_close(p_sha_handle);

  // sign the hash
  ECDSA_SIG *sig = ECDSA_do_sign((const unsigned char *) p_hash, sizeof(lc_sha256_hash_t), key);
  assert(sig != NULL);

  unsigned char * x_ = (unsigned char *) malloc(LC_NISTP_ECP256_KEY_SIZE * sizeof(uint32_t));
  unsigned char * y_ = (unsigned char *) malloc(LC_NISTP_ECP256_KEY_SIZE * sizeof(uint32_t));

  BN_bn2bin(sig->r, (uint8_t *) x_);
  BN_bn2bin(sig->s, (uint8_t *) y_);

  // reverse r and s
  reverse_endian(x_, (uint8_t *) p_signature->x, LC_NISTP_ECP256_KEY_SIZE * sizeof(uint32_t));
  reverse_endian(y_, (uint8_t *) p_signature->y, LC_NISTP_ECP256_KEY_SIZE * sizeof(uint32_t));

  free(x_);
  free(y_);
  EC_KEY_free(key);
  ECDSA_SIG_free(sig);

  return LC_SUCCESS;
}

lc_status_t lc_sha256_init(lc_sha_state_handle_t* p_sha_handle) {
  SHA256_CTX *sha256_ctx = new SHA256_CTX;
  SHA256_Init(sha256_ctx);
  *p_sha_handle = sha256_ctx;
  return LC_SUCCESS;
}



lc_status_t lc_sha256_update(const uint8_t *p_src, uint32_t src_len, lc_sha_state_handle_t sha_handle) {
  SHA256_CTX *ctx = (SHA256_CTX *) sha_handle;
  SHA256_Update(ctx, p_src, src_len);
  return LC_SUCCESS;
}

lc_status_t lc_sha256_get_hash(lc_sha_state_handle_t sha_handle, lc_sha256_hash_t *p_hash) {
  SHA256_CTX *ctx = (SHA256_CTX *) sha_handle;
  SHA256_Final((unsigned char *) p_hash, ctx);
  return LC_SUCCESS;
}

lc_status_t lc_sha256_close(lc_sha_state_handle_t sha_handle) {
  SHA256_CTX *ctx = (SHA256_CTX *) sha_handle;
  free(ctx);
  return LC_SUCCESS;
}


void encrypt(lc_aes_gcm_128bit_key_t *key,
             uint8_t *plaintext, uint32_t plaintext_length,
             uint8_t *ciphertext, uint32_t ciphertext_length) {

  // key size is 12 bytes/128 bits
  // IV size is 12 bytes/96 bits
  // MAC size is 16 bytes/128 bits

  // one buffer to store IV (12 bytes) + ciphertext + mac (16 bytes)
  (void)ciphertext_length;

  uint8_t *iv_ptr = ciphertext;
  // generate random IV
  RAND_bytes(iv_ptr, LC_AESGCM_IV_SIZE);
  lc_aes_gcm_128bit_tag_t *mac_ptr = (lc_aes_gcm_128bit_tag_t *) (ciphertext + LC_AESGCM_IV_SIZE);
  uint8_t *ciphertext_ptr = ciphertext + LC_AESGCM_IV_SIZE + LC_AESGCM_MAC_SIZE;

  lc_rijndael128GCM_encrypt(key,
                            plaintext, plaintext_length,
                            ciphertext_ptr,
                            iv_ptr, LC_AESGCM_IV_SIZE,
                            NULL, 0,
                            mac_ptr);
}


void decrypt(lc_aes_gcm_128bit_key_t *key,
             uint8_t *ciphertext, uint32_t ciphertext_length,
             uint8_t *plaintext, uint32_t plaintext_length) {

  // key size is 12 bytes/128 bits
  // IV size is 12 bytes/96 bits
  // MAC size is 16 bytes/128 bits

  // one buffer to store IV (12 bytes) + ciphertext + mac (16 bytes)

  uint32_t plength = ciphertext_length - LC_AESGCM_IV_SIZE - LC_AESGCM_MAC_SIZE;
  (void)plaintext_length;
  (void)plength;

  uint8_t *iv_ptr = (uint8_t *) ciphertext;
  lc_aes_gcm_128bit_tag_t *mac_ptr = (lc_aes_gcm_128bit_tag_t *) (ciphertext + LC_AESGCM_IV_SIZE);
  uint8_t *ciphertext_ptr = (uint8_t *) (ciphertext + LC_AESGCM_IV_SIZE + LC_AESGCM_MAC_SIZE);

  uint8_t tag[LC_AESGCM_MAC_SIZE];

  lc_rijndael128GCM_decrypt(ciphertext_ptr, ciphertext_length,
                            NULL, 0,
                            tag, *key, iv_ptr, plaintext);

  if (memcmp(mac_ptr, tag, LC_AESGCM_MAC_SIZE) != 0) {
    printf("Decrypt: invalid mac\n");
  }
}

uint32_t attr_upper_bound(uint8_t attr_type) {
  switch (attr_type & ~0x80) {
  case INT:
    return INT_UPPER_BOUND;

  case FLOAT:
    return FLOAT_UPPER_BOUND;

  case STRING:
    return STRING_UPPER_BOUND;

  case DATE:
  case LONG:
    return LONG_UPPER_BOUND;

  case DOUBLE:
    return DOUBLE_UPPER_BOUND;

  case URL_TYPE:
    return URL_UPPER_BOUND;

  case C_CODE:
    return C_CODE_UPPER_BOUND;

  case L_CODE:
    return L_CODE_UPPER_BOUND;

  case IP_TYPE:
    return IP_UPPER_BOUND;

  case USER_AGENT_TYPE:
    return USER_AGENT_UPPER_BOUND;

  case SEARCH_WORD_TYPE:
    return SEARCH_WORD_UPPER_BOUND;

  case TPCH_NATION_NAME_TYPE:
    return TPCH_NATION_NAME_UPPER_BOUND;

  default:
    printf("attr_upper_bound: Unknown type %d\n", attr_type);
    assert(false);
    return 0;
  }
}

uint32_t enc_size(uint32_t plaintext_size) {
  return plaintext_size + LC_AESGCM_IV_SIZE + LC_AESGCM_MAC_SIZE;
}

void encrypt_attribute(lc_aes_gcm_128bit_key_t *key,
                       uint8_t **input, uint8_t **output,
                       uint32_t *actual_size) {
  uint8_t *input_ptr = *input;
  uint8_t *output_ptr = *output;

  uint8_t attr_type = *input_ptr;
  uint32_t attr_len = 0;

  uint8_t temp[HEADER_SIZE + ATTRIBUTE_UPPER_BOUND];

  uint32_t upper_bound = attr_upper_bound(attr_type);

  *( (uint32_t *) output_ptr) = enc_size(HEADER_SIZE + upper_bound);
  output_ptr += 4;
  attr_len = *( (uint32_t *) (input_ptr + TYPE_SIZE));
  memcpy(temp, input_ptr, HEADER_SIZE + attr_len);
  encrypt(key,
          temp, HEADER_SIZE + upper_bound, output_ptr, HEADER_SIZE + upper_bound);

  input_ptr += HEADER_SIZE + attr_len;
  output_ptr += enc_size(HEADER_SIZE + upper_bound);

  *input = input_ptr;
  *output = output_ptr;
  *actual_size = output_ptr - input_ptr;
}
