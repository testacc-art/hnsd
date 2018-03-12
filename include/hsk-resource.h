#ifndef _HSK_RESOURCE_H
#define _HSK_RESOURCE_H

#define HSK_INET4 1
#define HSK_INET6 2
#define HSK_ONION 3
#define HSK_ONIONNG 4
#define HSK_NAME 5
#define HSK_GLUE 6
#define HSK_CANONICAL 7
#define HSK_DELEGATE 8
#define HSK_NS 9
#define HSK_SERVICE 10
#define HSK_URL 11
#define HSK_EMAIL 12
#define HSK_TEXT 13
#define HSK_LOCATION 14
#define HSK_MAGNET 15
#define HSK_DS 16
#define HSK_TLS 17
#define HSK_SSH 18
#define HSK_PGP 19
#define HSK_ADDR 20
#define HSK_EXTRA 255

#include <stdint.h>
#include <stdbool.h>
#include "hsk-addr.h"

// Records
typedef struct hsk_record_s {
  uint8_t type;
} hsk_record_t;

typedef struct hsk_target_s {
  uint8_t type;
  char name[256];
  uint8_t inet4[4];
  uint8_t inet6[16];
  uint8_t onion[33];
} hsk_target_t;

typedef struct hsk_host_record_s {
  uint8_t type;
  hsk_target_t target;
} hsk_host_record_t;

typedef hsk_host_record_t hsk_inet4_record_t;
typedef hsk_host_record_t hsk_inet6_record_t;
typedef hsk_host_record_t hsk_onion_record_t;
typedef hsk_host_record_t hsk_onionng_record_t;
typedef hsk_host_record_t hsk_name_record_t;
typedef hsk_host_record_t hsk_canonical_record_t;
typedef hsk_host_record_t hsk_delegate_record_t;
typedef hsk_host_record_t hsk_ns_record_t;

typedef struct hsk_service_record_s {
  uint8_t type;
  char service[33];
  char protocol[33];
  uint8_t priority;
  uint8_t weight;
  hsk_target_t target;
  uint16_t port;
} hsk_service_record_t;

// url, email, text
typedef struct hsk_txt_record_s {
  uint8_t type;
  char text[256];
} hsk_txt_record_t;

typedef hsk_txt_record_t hsk_url_record_t;
typedef hsk_txt_record_t hsk_email_record_t;
typedef hsk_txt_record_t hsk_text_record_t;

typedef struct hsk_location_record_s {
  uint8_t type;
  uint8_t version;
  uint8_t size;
  uint8_t horiz_pre;
  uint8_t vert_pre;
  uint32_t latitude;
  uint32_t longitude;
  uint32_t altitude;
} hsk_location_record_t;

typedef struct hsk_magnet_record_s {
  uint8_t type;
  char nid[33];
  size_t nin_len;
  uint8_t nin[64];
} hsk_magnet_record_t;

typedef struct hsk_ds_record_s {
  uint8_t type;
  uint16_t key_tag;
  uint8_t algorithm;
  uint8_t digest_type;
  size_t digest_len;
  uint8_t digest[64];
} hsk_ds_record_t;

typedef struct hsk_tls_record_s {
  uint8_t type;
  char protocol[33];
  uint16_t port;
  uint8_t usage;
  uint8_t selector;
  uint8_t matching_type;
  size_t certificate_len;
  uint8_t certificate[64];
} hsk_tls_record_t;

typedef struct hsk_ssh_record_s {
  uint8_t type;
  uint8_t algorithm;
  uint8_t key_type;
  size_t fingerprint_len;
  uint8_t fingerprint[64];
} hsk_ssh_record_t;

typedef hsk_ssh_record_t hsk_pgp_record_t;

typedef struct hsk_addr_record_s {
  uint8_t type;
  char currency[33];
  char address[256];
  uint8_t ctype;
  bool testnet;
  uint8_t version;
  size_t hash_len;
  uint8_t hash[64];
} hsk_addr_record_t;

typedef struct hsk_extra_record_s {
  uint8_t type;
  uint8_t rtype;
  size_t data_len;
  uint8_t data[255];
} hsk_extra_record_t;

// Symbol Table
typedef struct hsk_symbol_table_s {
  char *strings[255];
  uint8_t sizes[255];
  uint8_t size;
} hsk_symbol_table_t;

// Resource
typedef struct hsk_resource_s {
  uint8_t version;
  uint32_t ttl;
  size_t record_count;
  hsk_record_t *records[255];
} hsk_resource_t;

void
hsk_resource_free(hsk_resource_t *res);

bool
hsk_resource_decode(uint8_t *data, size_t data_len, hsk_resource_t **res);

hsk_record_t *
hsk_resource_get(hsk_resource_t *res, uint8_t type);

bool
hsk_resource_has(hsk_resource_t *res, uint8_t type);

bool
hsk_resource_to_dns(
  hsk_resource_t *rs,
  uint16_t id,
  char *fqdn,
  uint16_t type,
  bool edns,
  bool dnssec,
  uint8_t **wire,
  size_t *wire_len
);

bool
hsk_resource_root(
  uint16_t id,
  uint16_t type,
  bool edns,
  bool dnssec,
  hsk_addr_t *addr,
  uint8_t **wire,
  size_t *wire_len
);

bool
hsk_resource_to_nx(
  uint16_t id,
  char *fqdn,
  uint16_t type,
  bool edns,
  bool dnssec,
  uint8_t **wire,
  size_t *wire_len
);

bool
hsk_resource_to_servfail(
  uint16_t id,
  char *fqdn,
  uint16_t type,
  bool edns,
  bool dnssec,
  uint8_t **wire,
  size_t *wire_len
);
#endif
