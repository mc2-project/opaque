#ifndef SERVICE_PROVIDER_H
#define SERVICE_PROVIDER_H

#include <string>
#include <memory>
//#include <sgx_key_exchange.h>

#include "sp_crypto.h"
#include "iasrequest.h"

// typedef struct _sample_ps_sec_prop_desc_t {
//   uint8_t  sample_ps_sec_prop_desc[256];
// } sample_ps_sec_prop_desc_t;

// typedef struct _sp_db_item_t {
//   sgx_epid_group_id_t           gid;
//   lc_ec256_public_t             g_a;
//   lc_ec256_public_t             g_b;
//   lc_ec256_private_t            b;
//   lc_aes_gcm_128bit_key_t       vk_key;   // Shared secret key for the REPORT_DATA
//   lc_aes_gcm_128bit_key_t       mk_key;   // Shared secret key for generating MAC's
//   lc_aes_gcm_128bit_key_t       sk_key;   // Shared secret key for encryption
//   lc_aes_gcm_128bit_key_t       smk_key;  // Used only for SIGMA protocol
//   sample_ps_sec_prop_desc_t     ps_sec_prop;
// } sp_db_item_t;

class ServiceProvider {
public:
  ServiceProvider(const std::string &spid, bool is_production, bool linkable_signature)
    : spid(spid), is_production(is_production), linkable_signature(linkable_signature),
      ias_api_version(3), require_attestation(std::getenv("OPAQUE_REQUIRE_ATTESTATION")) {}

  /** Load an OpenSSL private key from the specified file. */
  void load_private_key(const std::string &filename);

  /**
   * Set the symmetric key to send to the enclave. This key is securely sent to the enclaves if
   * attestation succeeds.
   */
  void set_shared_key(const uint8_t *shared_key);

  /**
   * After calling load_private_key, write the corresponding public key as a C++ header file. This
   * file should be compiled into the enclave.
   */
  void export_public_key_code(const std::string &filename);

  /**
   * Process attestation report from an enclave, verify the report, and send the shared key to the enclave
   */
  std::unique_ptr<oe_shared_key_msg_t> process_enclave_report(oe_report_msg_t *report_msg, uint32_t *shared_key_msg_size);

private:
  void connect_to_ias_helper(const std::string &ias_report_signing_ca_file);

  lc_ec256_public_t sp_pub_key;
  lc_ec256_private_t sp_priv_key;
  
  uint8_t shared_key[LC_AESGCM_KEY_SIZE];
  //sp_db_item_t sp_db;
  std::string spid;

  std::unique_ptr<IAS_Connection> ias;
  bool is_production;
  bool linkable_signature;
  uint16_t ias_api_version;

  bool require_attestation;
};

extern ServiceProvider service_provider;

#endif
