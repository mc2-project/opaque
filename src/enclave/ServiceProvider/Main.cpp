/**
 *   Copyright(C) 2011-2015 Intel Corporation All Rights Reserved.
 *
 *   The source code, information  and  material ("Material") contained herein is
 *   owned  by Intel Corporation or its suppliers or licensors, and title to such
 *   Material remains  with Intel Corporation  or its suppliers or licensors. The
 *   Material  contains proprietary information  of  Intel or  its  suppliers and
 *   licensors. The  Material is protected by worldwide copyright laws and treaty
 *   provisions. No  part  of  the  Material  may  be  used,  copied, reproduced,
 *   modified, published, uploaded, posted, transmitted, distributed or disclosed
 *   in any way  without Intel's  prior  express written  permission. No  license
 *   under  any patent, copyright  or  other intellectual property rights  in the
 *   Material  is  granted  to  or  conferred  upon  you,  either  expressly,  by
 *   implication, inducement,  estoppel or  otherwise.  Any  license  under  such
 *   intellectual  property  rights must  be express  and  approved  by  Intel in
 *   writing.
 *
 *   *Third Party trademarks are the property of their respective owners.
 *
 *   Unless otherwise  agreed  by Intel  in writing, you may not remove  or alter
 *   this  notice or  any other notice embedded  in Materials by Intel or Intel's
 *   suppliers or licensors in any way.
 */

#include <cassert>
#include <cstdint>
#include <cstdio>

#include "SP.h"
#include "service_provider.h"

lc_aes_gcm_128bit_key_t key = "helloworld12312";

// These SP (service provider) calls are supposed to be made in a trusted environment
// For now we assume that the trusted master executes these calls
JNIEXPORT void JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_SPProcMsg0(JNIEnv *env, jobject obj, jbyteArray msg0_input) {
  // Master receives EPID information from the enclave

  (void)env;
  (void)obj;

  //uint32_t msg0_size = (uint32_t) env->GetArrayLength(msg0_input);
  jboolean if_copy = false;
  jbyte *ptr = env->GetByteArrayElements(msg0_input, &if_copy);
  uint32_t *extended_epid_group_id = (uint32_t *) ptr;

  sp_ra_proc_msg0_req(*extended_epid_group_id);
}

// Returns msg2 to the enclave
JNIEXPORT jbyteArray JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_SPProcMsg1(JNIEnv *env, jobject obj, jbyteArray msg1_input) {

  (void)env;
  (void)obj;

  uint32_t msg1_size = (uint32_t) env->GetArrayLength(msg1_input);
  jboolean if_copy = false;
  jbyte *ptr = env->GetByteArrayElements(msg1_input, &if_copy);
  sgx_ra_msg1_t *msg1 = (sgx_ra_msg1_t *) ptr;

  ra_samp_response_header_t *pp_msg2 = NULL;
  sp_ra_proc_msg1_req(msg1, msg1_size, &pp_msg2);

  jbyteArray array_ret = env->NewByteArray(pp_msg2->size);
  env->SetByteArrayRegion(array_ret, 0, pp_msg2->size, (jbyte *) pp_msg2->body);
  assert(pp_msg2->size == sizeof(sgx_ra_msg2_t));

  free(pp_msg2);

  return array_ret;
}

// Returns the attestation result to the enclave
JNIEXPORT jbyteArray JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_SPProcMsg3(JNIEnv *env, jobject obj, jbyteArray msg3_input) {

  (void)env;
  (void)obj;

  uint32_t msg3_size = (uint32_t) env->GetArrayLength(msg3_input);
  jboolean if_copy = false;
  jbyte *ptr = env->GetByteArrayElements(msg3_input, &if_copy);
  sgx_ra_msg3_t *msg3 = (sgx_ra_msg3_t *) ptr;


  ra_samp_response_header_t *pp_att_full = NULL;
  sp_ra_proc_msg3_req(msg3, msg3_size, &pp_att_full);

#ifdef DEBUG
  sample_ra_att_result_msg_t *result = (sample_ra_att_result_msg_t *) pp_att_full->body;
  printf("[SPProcMsg3] pp_att_full->size is %u, payload's size is %u\n", pp_att_full->size, result->secret.payload_size);
#endif

  jbyteArray array_ret = env->NewByteArray(pp_att_full->size + sizeof(ra_samp_response_header_t));
  env->SetByteArrayRegion(array_ret, 0, pp_att_full->size + sizeof(ra_samp_response_header_t), (jbyte *) pp_att_full);
  free(pp_att_full);

  return array_ret;
}

JNIEXPORT void JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_LoadKeys(
    JNIEnv *env, jobject obj) {
  (void)env;
  (void)obj;

  const char *private_key_filename = std::getenv("PRIVATE_KEY_PATH");
  read_secret_key(private_key_filename, NULL);
}

int main(int argc, char **argv) {
  (void)(argc);
  (void)(argv);


#ifndef TEST_EC
  if (argc < 2) {
    printf("Please input the public key's cpp file location\n");
    return 1;
  }

  const char *private_key_filename = std::getenv("PRIVATE_KEY_PATH");
  read_secret_key(private_key_filename, argv[1]);
#else

  printf("TEST_EC\n");

  uint8_t data[11] = "helloworld";

  lc_ec256_private_t p_private;
  lc_ec256_public_t p_public;
  lc_ec256_dh_shared_t p_shared_key;
  lc_ecc_state_handle_t ecc_handle = NULL;
  lc_ecc256_create_key_pair(&p_private, &p_public, ecc_handle);
  printf("Created key pair\n");

  print_pub_key(p_public);
  print_priv_key(p_private);

  // test compute shared key
  lc_ecc256_compute_shared_dhkey(&p_private, &p_public, &p_shared_key, ecc_handle);
  printf("Computed shared key\n");

  EC_POINT *pub_key = get_ec_point(&p_public);
  EC_POINT_free(pub_key);

  // test signature
  lc_ec256_signature_t sig;
  lc_ecdsa_sign(data, 10, &p_private, &sig, ecc_handle);
  printf("Signed data using ECDSA\n");

  lc_sha_state_handle_t sha_handle;
  lc_sha256_hash_t hash;

  // test sha
  lc_sha256_init(&sha_handle);
  lc_sha256_update(data, 10, sha_handle);
  lc_sha256_get_hash(sha_handle, &hash);
  lc_sha256_close(sha_handle);

  print_hex((uint8_t *) hash, sizeof(lc_sha256_hash_t));
  printf("\n");


#endif

  return 0;
}
