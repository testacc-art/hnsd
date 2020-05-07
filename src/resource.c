#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "addr.h"
#include "base32.h"
#include "bio.h"
#include "dns.h"
#include "dnssec.h"
#include "error.h"
#include "resource.h"
#include "utils.h"

static const uint8_t hsk_zero_inet4[4] = {
  0x00, 0x00, 0x00, 0x00
};

static const uint8_t hsk_zero_inet6[16] = {
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// NS SOA RRSIG NSEC DNSKEY
// Possibly add A, AAAA, and DS
static const uint8_t hsk_type_map[] = {
  0x00, 0x07, 0x22, 0x00, 0x00,
  0x00, 0x00, 0x03, 0x80
};

/*
 * Helpers
 */

static void
to_fqdn(char *name);

static void
ip_size(const uint8_t *ip, size_t *s, size_t *l);

static size_t
ip_write(const uint8_t *ip, uint8_t *data);

static bool
ip_read(const uint8_t *data, uint8_t *ip);

static bool
target_to_dns(const hsk_target_t *target, const char *name, char *host);

/*
 * Resource serialization version 0
 * Record types: read
 */

bool
hsk_ds_record_read(
  uint8_t **data,
  size_t *data_len,
  hsk_ds_record_t *rec
) {
  if (*data_len < 5)
    return false;

  uint8_t size = 0;
  read_u16be(data, data_len, &rec->key_tag);
  read_u8(data, data_len, &rec->algorithm);
  read_u8(data, data_len, &rec->digest_type);
  read_u8(data, data_len, &size);

  if (size > 64)
    return false;

  if (!read_bytes(data, data_len, rec->digest, size))
    return false;

  rec->digest_len = size;

  return true;
}

bool
hsk_ns_record_read(
  uint8_t **data,
  size_t *data_len,
  const hsk_dns_dmp_t *dmp,
  hsk_ns_record_t *rec
) {
  return hsk_dns_name_read(data, data_len, dmp, rec->name);
}

bool
hsk_glue4_record_read(
  uint8_t **data,
  size_t *data_len,
  const hsk_dns_dmp_t *dmp,
  hsk_glue4_record_t *rec
) {
  if (!hsk_dns_name_read(data, data_len, dmp, rec->name))
    return false;

  return read_bytes(data, data_len, rec->inet4, 4);
}

bool
hsk_glue6_record_read(
  uint8_t **data,
  size_t *data_len,
  const hsk_dns_dmp_t *dmp,
  hsk_glue6_record_t *rec
) {
  if (!hsk_dns_name_read(data, data_len, dmp, rec->name))
    return false;

  return read_bytes(data, data_len, rec->inet6, 16);
}

bool
hsk_synth4_record_read(
  uint8_t **data,
  size_t *data_len,
  hsk_synth4_record_t *rec
) {
  return read_bytes(data, data_len, rec->inet4, 4);
}

bool
hsk_synth6_record_read(
  uint8_t **data,
  size_t *data_len,
  hsk_synth6_record_t *rec
) {
  return read_bytes(data, data_len, rec->inet6, 16);
}

bool
hsk_resource_str_read(
  uint8_t **data,
  size_t *data_len,
  char *str,
  size_t limit
) {
  uint8_t size = 0;
  uint8_t *chunk;

  if (!read_u8(data, data_len, &size))
    return false;

  if (!slice_bytes(data, data_len, &chunk, size))
    return false;

  int real_size = 0;
  int i;

  for (i = 0; i < size; i++) {
    uint8_t ch = chunk[i];

    // No DEL.
    if (ch == 0x7f)
      return false;

    // Any non-printable character can screw.
    // Tab, line feed, and carriage return all valid.
    if (ch < 0x20
        && ch != 0x09
        && ch != 0x0a
        && ch != 0x0d) {
      return false;
    }

    real_size += 1;
  }

  if (real_size > limit)
    return false;

  char *s = str;
  for (i = 0; i < size; i++) {
    uint8_t ch = chunk[i];

    *s = ch;
    s += 1;
  }

  *s ='\0';

  return true;
}

bool
hsk_txt_record_read(
  uint8_t **data,
  size_t *data_len,
  hsk_txt_record_t *rec
) {
  return hsk_resource_str_read(data, data_len, rec->text, 255);
}

void
hsk_record_init(hsk_record_t *r) {
  if (r == NULL)
    return;

  r->type = r->type;

  switch (r->type) {
    case HSK_DS: {
      hsk_ds_record_t *rec = (hsk_ds_record_t *)r;
      rec->key_tag = 0;
      rec->algorithm = 0;
      rec->digest_type = 0;
      rec->digest_len = 0;
      memset(rec->digest, 0, sizeof(rec->digest));
      break;
    }
    case HSK_NS: {
      hsk_ns_record_t *rec = (hsk_ns_record_t *)r;
      memset(rec->name, 0, sizeof(rec->name));
      break;
    }
    case HSK_GLUE4: {
      hsk_glue4_record_t *rec = (hsk_glue4_record_t *)r;
      memset(rec->name, 0, sizeof(rec->name));
      memset(rec->inet4, 0, sizeof(rec->inet4));
      break;
    }
    case HSK_GLUE6: {
      hsk_glue6_record_t *rec = (hsk_glue6_record_t *)r;
      memset(rec->name, 0, sizeof(rec->name));
      memset(rec->inet6, 0, sizeof(rec->inet6));
      break;
    }
    case HSK_SYNTH4: {
      hsk_synth4_record_t *rec = (hsk_synth4_record_t *)r;
      memset(rec->inet4, 0, sizeof(rec->inet4));
      break;
    }
    case HSK_SYNTH6: {
      hsk_synth6_record_t *rec = (hsk_synth6_record_t *)r;
      memset(rec->inet6, 0, sizeof(rec->inet6));
      break;
    }
    case HSK_TEXT: {
      hsk_txt_record_t *rec = (hsk_txt_record_t *)r;
      memset(rec->text, 0, sizeof(rec->text));
      break;
    }
  }
}

hsk_record_t *
hsk_record_alloc(uint8_t type) {
  hsk_record_t *r = NULL;

  switch (type) {
    case HSK_DS: {
      r = (hsk_record_t *)malloc(sizeof(hsk_ds_record_t));
      break;
    }
    case HSK_NS: {
      r = (hsk_record_t *)malloc(sizeof(hsk_ns_record_t));
      break;
    }
    case HSK_GLUE4: {
      r = (hsk_record_t *)malloc(sizeof(hsk_glue4_record_t));
      break;
    }
    case HSK_GLUE6: {
      r = (hsk_record_t *)malloc(sizeof(hsk_glue6_record_t));
      break;
    }
    case HSK_SYNTH4: {
      r = (hsk_record_t *)malloc(sizeof(hsk_synth4_record_t));
      break;
    }
    case HSK_SYNTH6: {
      r = (hsk_record_t *)malloc(sizeof(hsk_synth6_record_t));
      break;
    }
    case HSK_TEXT: {
      r = (hsk_record_t *)malloc(sizeof(hsk_txt_record_t));
      break;
    }
    default: {
      // Unknown record type.
      return NULL;
    }
  }

  r->type = type;

  hsk_record_init(r);
  return r;
}

void
hsk_record_free(hsk_record_t *r) {
  if (r == NULL)
    return;

  switch (r->type) {
    case HSK_DS: {
      hsk_ds_record_t *rec = (hsk_ds_record_t *)r;
      free(rec);
      break;
    }
    case HSK_NS: {
      hsk_ns_record_t *rec = (hsk_ns_record_t *)r;
      free(rec);
      break;
    }
    case HSK_GLUE4: {
      hsk_glue4_record_t *rec = (hsk_glue4_record_t *)r;
      free(rec);
      break;
    }
    case HSK_GLUE6: {
      hsk_glue6_record_t *rec = (hsk_glue6_record_t *)r;
      free(rec);
      break;
    }
    case HSK_SYNTH4: {
      hsk_synth4_record_t *rec = (hsk_synth4_record_t *)r;
      free(rec);
      break;
    }
    case HSK_SYNTH6: {
      hsk_synth6_record_t *rec = (hsk_synth6_record_t *)r;
      free(rec);
      break;
    }
    case HSK_TEXT: {
      hsk_txt_record_t *rec = (hsk_txt_record_t *)r;
      free(rec);
      break;
    }
    default: {
      // Why are we freeing memory for an unknown record type?
      break;
    }
  }
}

void
hsk_resource_free(hsk_resource_t *res) {
  if (res == NULL)
    return;

  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *rec = res->records[i];
    hsk_record_free(rec);
  }

  free(res);
}

bool
hsk_record_read(
  uint8_t **data,
  size_t *data_len,
  uint8_t type,
  const hsk_dns_dmp_t *dmp,
  hsk_record_t **res
) {
  hsk_record_t *r = hsk_record_alloc(type);

  if (r == NULL)
    return false;

  bool result = true;

  switch (type) {
    case HSK_DS: {
      hsk_ds_record_t *rec = (hsk_ds_record_t *)r;
      result = hsk_ds_record_read(data, data_len, rec);
      break;
    }
    case HSK_NS: {
      hsk_ns_record_t *rec = (hsk_ns_record_t *)r;
      result = hsk_ns_record_read(data, data_len, dmp, rec);
      break;
    }
    case HSK_GLUE4: {
      hsk_glue4_record_t *rec = (hsk_glue4_record_t *)r;
      result = hsk_glue4_record_read(data, data_len, dmp, rec);
      break;
    }
    case HSK_GLUE6: {
      hsk_glue6_record_t *rec = (hsk_glue6_record_t *)r;
      result = hsk_glue6_record_read(data, data_len, dmp, rec);
      break;
    }
    case HSK_SYNTH4: {
      hsk_synth4_record_t *rec = (hsk_synth4_record_t *)r;
      result = hsk_synth4_record_read(data, data_len, rec);
      break;
    }
    case HSK_SYNTH6: {
      hsk_synth6_record_t *rec = (hsk_synth6_record_t *)r;
      result = hsk_synth6_record_read(data, data_len, rec);
      break;
    }
    case HSK_TEXT: {
      hsk_txt_record_t *rec = (hsk_txt_record_t *)r;
      result = hsk_txt_record_read(data, data_len, rec);
      break;
    }
    default: {
      // Unknown record type.
      free(r);
      return false;
    }
  }

  *res = r;

  return true;
}

bool
hsk_resource_decode(
  const uint8_t *data,
  size_t data_len,
  hsk_resource_t **resource
) {
  // Pointer to iterate through input resource data.
  uint8_t *dat = (uint8_t *)data;

  // Initialize DNS Message Compression by storing
  // a "copy" of the entire message for pointer reference.
  // See rfc1035 section 4.1.4
  hsk_dns_dmp_t dmp;
  dmp.msg = dat;
  dmp.msg_len = data_len;

  // Initialize response struct.
  hsk_resource_t *res = malloc(sizeof(hsk_resource_t));

  if (res == NULL)
    goto fail;

  res->version = 0;
  res->record_count = 0;
  memset(res->records, 0, sizeof(hsk_record_t *));

  // Copy version from input.
  if (!read_u8(&dat, &data_len, &res->version))
    goto fail;

  // Only version 0 is valid at this time.
  if (res->version != 0)
    goto fail;

  // TTL is always constant due to tree interval.
  res->ttl = HSK_DEFAULT_TTL;

  // The rest of the data is records, read until empty.
  int i = 0;
  while (data_len > 0) {
    // Get record type.
    uint8_t type;
    read_u8(&dat, &data_len, &type);

    // Read the body of the record.
    if (!hsk_record_read(&dat, &data_len, type, &dmp, &res->records[i]))
      goto fail;

    // Increment total amount of records in this resource.
    i++;
  }

  res->record_count = i;

  *resource = res;

  return true;

fail:
  hsk_resource_free(res);
  return false;
}

const hsk_record_t *
hsk_resource_get(const hsk_resource_t *res, uint8_t type) {
  int i;
  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *rec = res->records[i];
    if (rec->type == type)
      return rec;
  }
  return NULL;
}

bool
hsk_resource_has(const hsk_resource_t *res, uint8_t type) {
  return hsk_resource_get(res, type) != NULL;
}

bool
hsk_resource_has_ns(const hsk_resource_t *res) {
  int i;
  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *rec = res->records[i];
    if (rec->type >= HSK_NS && rec->type <= HSK_SYNTH6)
      return true;
  }
  return false;
}

static bool
hsk_resource_to_a(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_INET4)
      continue;

    hsk_inet4_record_t *rec = (hsk_inet4_record_t *)c;
    hsk_target_t *target = &rec->target;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_A);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_a_rd_t *rd = rr->rd;
    memcpy(&rd->addr[0], &target->inet4[0], 4);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_aaaa(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_INET6)
      continue;

    hsk_inet6_record_t *rec = (hsk_inet6_record_t *)c;
    hsk_target_t *target = &rec->target;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_AAAA);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_aaaa_rd_t *rd = rr->rd;
    memcpy(&rd->addr[0], &target->inet6[0], 16);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_cname(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;
  char cname[HSK_DNS_MAX_NAME + 1];

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_CANONICAL)
      continue;

    hsk_canonical_record_t *rec = (hsk_canonical_record_t *)c;
    hsk_target_t *target = &rec->target;

    if (target->type != HSK_NAME && target->type != HSK_GLUE)
      continue;

    if (!target_to_dns(target, name, cname))
      continue;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_CNAME);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_cname_rd_t *rd = rr->rd;
    strcpy(rd->target, cname);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_dname(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;
  char dname[HSK_DNS_MAX_NAME + 1];

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_DELEGATE)
      continue;

    hsk_delegate_record_t *rec = (hsk_delegate_record_t *)c;
    hsk_target_t *target = &rec->target;

    if (target->type != HSK_NAME && target->type != HSK_GLUE)
      continue;

    if (!target_to_dns(target, name, dname))
      continue;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_DNAME);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_dname_rd_t *rd = rr->rd;
    strcpy(rd->target, dname);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_ns(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;
  char nsname[HSK_DNS_MAX_NAME + 1];

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    bool synth = false;

    switch (c->type) {
      case HSK_SYNTH4:
      case HSK_SYNTH6:
        synth = true;
      case HSK_NS:
      case HSK_GLUE4:
      case HSK_GLUE6:
        break;
      default:
        continue;
    }

    if (synth) {
      // SYNTH records only actually contain an IP address
      // for the adiditonal section. The NS name must
      // be computed on the fly by encoding the IP into base32.
      char b32[29];

      if (c->type == HSK_SYNTH4)
        ip_to_b32(c->inet4, b32);
      else
        ip_to_b32(c->inet6, b32);

      // Magic pseudo-TLD can also be directly resolved by hnsd
      sprintf(nsname, "_%s._synth.", b32);
    } else {
      // NS and GLUE records have the NS names ready to go.
      assert(hsk_dns_name_is_fqdn(c->name));
      strcpy(nsname, c->name);
    }

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_NS);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_ns_rd_t *rd = rr->rd;
    strcpy(rd->ns, nsname);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_nsip(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *ar
) {
  int i;
  char ptr[HSK_DNS_MAX_NAME + 1];

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_NS)
      continue;

    hsk_ns_record_t *rec = (hsk_ns_record_t *)c;
    hsk_target_t *target = &rec->target;

    if (target->type != HSK_INET4 && target->type != HSK_INET6)
      continue;

    if (!target_to_dns(target, name, ptr))
      continue;

    uint16_t rrtype = HSK_DNS_A;

    if (target->type == HSK_INET6)
      rrtype = HSK_DNS_AAAA;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(rrtype);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, ptr);
    rr->ttl = res->ttl;

    if (rrtype == HSK_DNS_A) {
      hsk_dns_a_rd_t *rd = rr->rd;
      memcpy(&rd->addr[0], &target->inet4[0], 4);
    } else {
      hsk_dns_aaaa_rd_t *rd = rr->rd;
      memcpy(&rd->addr[0], &target->inet6[0], 16);
    }

    hsk_dns_rrs_push(ar, rr);
  }

  return true;
}

static bool
hsk_resource_to_mx(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;
  char mx[HSK_DNS_MAX_NAME + 1];

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_SERVICE)
      continue;

    hsk_service_record_t *rec = (hsk_service_record_t *)c;
    hsk_target_t *target = &rec->target;

    if (strcasecmp(rec->service, "smtp.") != 0
        || strcasecmp(rec->protocol, "tcp.") != 0) {
      continue;
    }

    if (!target_to_dns(target, name, mx))
      continue;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_MX);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_mx_rd_t *rd = rr->rd;
    rd->preference = rec->priority;
    strcpy(rd->mx, mx);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_mxip(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  return hsk_resource_to_srvip(res, name, "tcp.", "smtp.", an);
}

static bool
hsk_resource_to_srv(
  const hsk_resource_t *res,
  const char *name,
  const char *protocol,
  const char *service,
  hsk_dns_rrs_t *an
) {
  int i;
  char host[HSK_DNS_MAX_NAME + 1];

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_SERVICE)
      continue;

    hsk_service_record_t *rec = (hsk_service_record_t *)c;
    hsk_target_t *target = &rec->target;

    if (strcasecmp(protocol, rec->protocol) != 0)
      continue;

    if (strcasecmp(service, rec->service) != 0)
      continue;

    if (!target_to_dns(target, name, host))
      continue;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_SRV);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_srv_rd_t *rd = rr->rd;

    rd->priority = rec->priority;
    rd->weight = rec->weight;
    rd->port = rec->port;
    strcpy(rd->target, host);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_txt(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_TEXT)
      continue;

    hsk_text_record_t *rec = (hsk_text_record_t *)c;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_TXT);

    if (!rr)
      return false;

    rr->ttl = res->ttl;
    hsk_dns_rr_set_name(rr, name);

    hsk_dns_txt_rd_t *rd = rr->rd;

    hsk_dns_txt_t *txt = hsk_dns_txt_alloc();

    if (!txt) {
      hsk_dns_rr_free(rr);
      return false;
    }

    txt->data_len = strlen(rec->text);
    assert(txt->data_len <= 255);

    memcpy(&txt->data[0], rec->text, txt->data_len);

    hsk_dns_txts_push(&rd->txts, txt);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_loc(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_LOCATION)
      continue;

    hsk_location_record_t *rec = (hsk_location_record_t *)c;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_LOC);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_loc_rd_t *rd = rr->rd;

    rd->version = rec->version;
    rd->size = rec->size;
    rd->horiz_pre = rec->horiz_pre;
    rd->vert_pre = rec->vert_pre;
    rd->latitude = rec->latitude;
    rd->longitude = rec->longitude;
    rd->altitude = rec->altitude;

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_ds(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_DS)
      continue;

    hsk_ds_record_t *rec = (hsk_ds_record_t *)c;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_DS);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_ds_rd_t *rd = rr->rd;

    rd->key_tag = rec->key_tag;
    rd->algorithm = rec->algorithm;
    rd->digest_type = rec->digest_type;
    rd->digest_len = rec->digest_len;

    rd->digest = malloc(rec->digest_len);

    if (!rd->digest) {
      hsk_dns_rr_free(rr);
      return false;
    }

    memcpy(rd->digest, &rec->digest[0], rec->digest_len);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_sshfp(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_SSH)
      continue;

    hsk_ssh_record_t *rec = (hsk_ssh_record_t *)c;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_SSHFP);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_sshfp_rd_t *rd = rr->rd;

    rd->algorithm = rec->algorithm;
    rd->digest_type = rec->digest_type;
    rd->fingerprint_len = rec->fingerprint_len;

    rd->fingerprint = malloc(rec->fingerprint_len);

    if (!rd->fingerprint) {
      hsk_dns_rr_free(rr);
      return false;
    }

    memcpy(rd->fingerprint, &rec->fingerprint[0], rec->fingerprint_len);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_uri(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_URI)
      continue;

    hsk_uri_record_t *rec = (hsk_uri_record_t *)c;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_URI);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_uri_rd_t *rd = rr->rd;

    rd->priority = 0;
    rd->weight = 0;
    rd->data_len = strlen(rec->text);

    assert(rd->data_len <= 255);

    memcpy(&rd->data[0], &rec->text[0], rd->data_len);

    hsk_dns_rrs_push(an, rr);
  }

  char nid[HSK_DNS_MAX_LABEL + 1];
  char nin[HSK_DNS_MAX_NAME + 1];

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_MAGNET)
      continue;

    hsk_magnet_record_t *rec = (hsk_magnet_record_t *)c;

    size_t nid_len = hsk_dns_label_get(rec->nid, 0, nid);
    hsk_to_lower(nid);

    size_t len = 16 + nid_len + rec->nin_len * 2;

    if (len + 1 > 255)
      continue;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_URI);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    assert(rec->nin_len <= 64);
    hsk_hex_encode(rec->nin, rec->nin_len, nin);

    hsk_dns_uri_rd_t *rd = rr->rd;

    rd->priority = 0;
    rd->weight = 0;
    rd->data_len = len;

    assert(rd->data_len <= 255);

    sprintf((char *)rd->data, "magnet:?xt=urn:%s:%s", nid, nin);

    hsk_dns_rrs_push(an, rr);
  }

  char *currency = &nid[0];
  char *addr = &nin[0];

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_ADDR)
      continue;

    hsk_addr_record_t *rec = (hsk_addr_record_t *)c;

    if (rec->ctype != 0 && rec->ctype != 3)
      continue;

    size_t currency_len = hsk_dns_label_get(rec->currency, 0, currency);
    hsk_to_lower(currency);

    size_t addr_len = 0;

    if (rec->ctype == 0) {
      addr_len = strlen(rec->address);
      memcpy(addr, rec->address, addr_len);
    } else if (rec->ctype == 3) {
      assert(rec->hash_len <= 64);
      addr_len = 2 + rec->hash_len * 2;
      addr[0] = '0';
      addr[1] = 'x';
      hsk_hex_encode(rec->hash, rec->hash_len, &addr[2]);
    } else {
      assert(0);
    }

    size_t len = currency_len + 1 + addr_len;

    if (len + 1 > 255)
      continue;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_URI);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_uri_rd_t *rd = rr->rd;

    rd->priority = 0;
    rd->weight = 0;
    rd->data_len = len;

    assert(rd->data_len <= 255);

    sprintf((char *)rd->data, "%s:%s", currency, addr);

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_rp(
  const hsk_resource_t *res,
  const char *name,
  hsk_dns_rrs_t *an
) {
  char mbox[HSK_DNS_MAX_NAME + 2];
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];

    if (c->type != HSK_EMAIL)
      continue;

    hsk_email_record_t *rec = (hsk_email_record_t *)c;

    if (strlen(rec->text) > 63)
      continue;

    sprintf(mbox, "%s.", rec->text);

    if (!hsk_dns_name_verify(mbox))
      continue;

    hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_RP);

    if (!rr)
      return false;

    hsk_dns_rr_set_name(rr, name);
    rr->ttl = res->ttl;

    hsk_dns_rp_rd_t *rd = rr->rd;

    strcpy(rd->mbox, mbox);
    strcpy(rd->txt, ".");

    hsk_dns_rrs_push(an, rr);
  }

  return true;
}

static bool
hsk_resource_to_glue(
  const hsk_resource_t *res,
  hsk_dns_rrs_t *an,
  uint16_t rrtype
) {
  int i;

  for (i = 0; i < res->record_count; i++) {
    hsk_record_t *c = res->records[i];
    hsk_target_t *target;

    switch (c->type) {
      case HSK_CANONICAL: {
        if (rrtype != HSK_DNS_CNAME)
          continue;

        break;
      }
      case HSK_DELEGATE: {
        if (rrtype != HSK_DNS_DNAME)
          continue;

        break;
      }
      case HSK_NS: {
        if (rrtype != HSK_DNS_NS)
          continue;

        break;
      }
      case HSK_SERVICE: {
        if (rrtype != HSK_DNS_SRV && rrtype != HSK_DNS_MX)
          continue;

        if (rrtype == HSK_DNS_MX) {
          hsk_service_record_t *rec = (hsk_service_record_t *)c;

          if (strcasecmp(rec->service, "smtp.") != 0
              || strcasecmp(rec->protocol, "tcp.") != 0) {
            continue;
          }
        }

        break;
      }
      default: {
        continue;
      }
    }

    switch (c->type) {
      case HSK_CANONICAL:
      case HSK_DELEGATE:
      case HSK_NS: {
        hsk_host_record_t *rec = (hsk_host_record_t *)c;
        target = &rec->target;
        break;
      }
      case HSK_SERVICE: {
        hsk_service_record_t *rec = (hsk_service_record_t *)c;
        target = &rec->target;
        break;
      }
      default: {
        continue;
      }
    }

    if (target->type != HSK_GLUE)
      continue;

    if (memcmp(target->inet4, hsk_zero_inet4, 4) != 0) {
      hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_A);

      if (!rr)
        return false;

      hsk_dns_rr_set_name(rr, target->name);
      rr->ttl = res->ttl;

      hsk_dns_a_rd_t *rd = rr->rd;
      memcpy(&rd->addr[0], &target->inet4[0], 4);

      hsk_dns_rrs_push(an, rr);
    }

    if (memcmp(target->inet6, hsk_zero_inet6, 16) != 0) {
      hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_AAAA);

      if (!rr)
        return false;

      hsk_dns_rr_set_name(rr, target->name);
      rr->ttl = res->ttl;

      hsk_dns_aaaa_rd_t *rd = rr->rd;
      memcpy(&rd->addr[0], &target->inet6[0], 16);

      hsk_dns_rrs_push(an, rr);
    }
  }

  return true;
}

static bool
hsk_resource_root_to_soa(hsk_dns_rrs_t *an) {
  hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_SOA);

  if (!rr)
    return false;

  rr->ttl = 86400;

  hsk_dns_rr_set_name(rr, ".");

  hsk_dns_soa_rd_t *rd = rr->rd;
  strcpy(rd->ns, ".");
  strcpy(rd->mbox, ".");

  uint32_t year;
  uint32_t month;
  uint32_t day;
  uint32_t hour;

  hsk_ymdh(&year, &month, &day, &hour);

  uint32_t y = year * 1e6;
  uint32_t m = month * 1e4;
  uint32_t d = day * 1e2;
  uint32_t h = hour;

  rd->serial = y + m + d + h;
  rd->refresh = 1800;
  rd->retry = 900;
  rd->expire = 604800;
  rd->minttl = 86400;

  hsk_dns_rrs_push(an, rr);

  return true;
}

static bool
hsk_resource_root_to_ns(hsk_dns_rrs_t *an) {
  hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_NS);

  if (!rr)
    return false;

  rr->ttl = 518400;
  hsk_dns_rr_set_name(rr, ".");

  hsk_dns_ns_rd_t *rd = rr->rd;
  strcpy(rd->ns, ".");

  hsk_dns_rrs_push(an, rr);

  return true;
}

static bool
hsk_resource_root_to_a(hsk_dns_rrs_t *an, const hsk_addr_t *addr) {
  if (!addr || !hsk_addr_is_ip4(addr))
    return true;

  const uint8_t *ip = hsk_addr_get_ip(addr);

  hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_A);

  if (!rr)
    return false;

  rr->ttl = 518400;

  hsk_dns_rr_set_name(rr, ".");

  hsk_dns_a_rd_t *rd = rr->rd;

  memcpy(&rd->addr[0], ip, 4);

  hsk_dns_rrs_push(an, rr);

  return true;
}

static bool
hsk_resource_root_to_aaaa(hsk_dns_rrs_t *an, const hsk_addr_t *addr) {
  if (!addr || !hsk_addr_is_ip6(addr))
    return true;

  const uint8_t *ip = hsk_addr_get_ip(addr);

  hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_AAAA);

  if (!rr)
    return false;

  rr->ttl = 518400;

  hsk_dns_rr_set_name(rr, ".");

  hsk_dns_aaaa_rd_t *rd = rr->rd;

  memcpy(&rd->addr[0], ip, 16);

  hsk_dns_rrs_push(an, rr);

  return true;
}

static bool
hsk_resource_root_to_dnskey(hsk_dns_rrs_t *an) {
  const hsk_dns_rr_t *ksk = hsk_dnssec_get_ksk();
  hsk_dns_rr_t *ksk_rr = hsk_dns_rr_clone(ksk);

  if (!ksk_rr)
    return false;

  hsk_dns_rrs_push(an, ksk_rr);

  const hsk_dns_rr_t *zsk = hsk_dnssec_get_zsk();
  hsk_dns_rr_t *zsk_rr = hsk_dns_rr_clone(zsk);

  if (!zsk_rr)
    return false;

  hsk_dns_rrs_push(an, zsk_rr);

  return true;
}

static bool
hsk_resource_root_to_ds(hsk_dns_rrs_t *an) {
  const hsk_dns_rr_t *ds = hsk_dnssec_get_ds();
  hsk_dns_rr_t *rr = hsk_dns_rr_clone(ds);

  if (!rr)
    return false;

  hsk_dns_rrs_push(an, rr);

  return true;
}

static bool
hsk_resource_to_empty(
  const char *name,
  const uint8_t *type_map,
  size_t type_map_len,
  hsk_dns_rrs_t *an
) {
  hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_NSEC);

  if (!rr)
    return false;

  rr->ttl = 86400;

  hsk_dns_rr_set_name(rr, name);

  hsk_dns_nsec_rd_t *rd = rr->rd;

  strcpy(rd->next_domain, ".");
  rd->type_map = NULL;
  rd->type_map_len = 0;

  if (type_map) {
    uint8_t *buf = malloc(type_map_len);

    if (!buf) {
      hsk_dns_rr_free(rr);
      return false;
    }

    memcpy(buf, type_map, type_map_len);

    rd->type_map = buf;
    rd->type_map_len = type_map_len;
  }

  hsk_dns_rrs_push(an, rr);

  return true;
}

static bool
hsk_resource_root_to_nsec(hsk_dns_rrs_t *an) {
  hsk_dns_rr_t *rr = hsk_dns_rr_create(HSK_DNS_NSEC);

  if (!rr)
    return false;

  uint8_t *bitmap = malloc(sizeof(hsk_type_map));

  if (!bitmap) {
    hsk_dns_rr_free(rr);
    return false;
  }

  memcpy(bitmap, &hsk_type_map[0], sizeof(hsk_type_map));

  rr->ttl = 86400;

  hsk_dns_rr_set_name(rr, ".");

  hsk_dns_nsec_rd_t *rd = rr->rd;

  strcpy(rd->next_domain, ".");
  rd->type_map = bitmap;
  rd->type_map_len = sizeof(hsk_type_map);

  hsk_dns_rrs_push(an, rr);

  return true;
}

hsk_dns_msg_t *
hsk_resource_to_dns(const hsk_resource_t *rs, const char *name, uint16_t type) {
  assert(hsk_dns_name_is_fqdn(name));

  int labels = hsk_dns_label_count(name);

  if (labels == 0)
    return NULL;

  char tld[HSK_DNS_MAX_LABEL + 1];
  hsk_dns_label_from(name, -1, tld);

  hsk_dns_msg_t *msg = hsk_dns_msg_alloc();

  if (!msg)
    return NULL;

  hsk_dns_rrs_t *an = &msg->an; // answer
  hsk_dns_rrs_t *ns = &msg->ns; // authority
  hsk_dns_rrs_t *ar = &msg->ar; // additional

  // Referral.
  if (labels > 1) {
    if (hsk_resource_has_ns(rs)) {
      hsk_resource_to_ns(rs, tld, ns);
      hsk_resource_to_ds(rs, tld, ns);
      hsk_resource_to_nsip(rs, tld, ar);
      hsk_resource_to_glue(rs, ar, HSK_DNS_NS);
      if (!hsk_resource_has(rs, HSK_DS))
        hsk_dnssec_sign_zsk(ns, HSK_DNS_NS);
      else
        hsk_dnssec_sign_zsk(ns, HSK_DNS_DS);
    } else if (hsk_resource_has(rs, HSK_DELEGATE)) {
      hsk_resource_to_dname(rs, name, an);
      hsk_resource_to_glue(rs, ar, HSK_DNS_DNAME);
      hsk_dnssec_sign_zsk(an, HSK_DNS_DNAME);
      hsk_dnssec_sign_zsk(ar, HSK_DNS_A);
      hsk_dnssec_sign_zsk(ar, HSK_DNS_AAAA);
    } else {
      // Needs SOA.
      // Empty proof:
      hsk_resource_to_empty(tld, NULL, 0, ns);
      hsk_dnssec_sign_zsk(ns, HSK_DNS_NSEC);
      hsk_resource_root_to_soa(ns);
      hsk_dnssec_sign_zsk(ns, HSK_DNS_SOA);
    }

    return msg;
  }

  switch (type) {
    case HSK_DNS_A:
      hsk_resource_to_a(rs, name, an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_A);
      break;
    case HSK_DNS_AAAA:
      hsk_resource_to_aaaa(rs, name, an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_AAAA);
      break;
    case HSK_DNS_CNAME:
      hsk_resource_to_cname(rs, name, an);
      hsk_resource_to_glue(rs, ar, HSK_DNS_CNAME);
      hsk_dnssec_sign_zsk(an, HSK_DNS_CNAME);
      hsk_dnssec_sign_zsk(ar, HSK_DNS_A);
      hsk_dnssec_sign_zsk(ar, HSK_DNS_AAAA);
      break;
    case HSK_DNS_DNAME:
      hsk_resource_to_dname(rs, name, an);
      hsk_resource_to_glue(rs, ar, HSK_DNS_DNAME);
      hsk_dnssec_sign_zsk(an, HSK_DNS_DNAME);
      hsk_dnssec_sign_zsk(ar, HSK_DNS_A);
      hsk_dnssec_sign_zsk(ar, HSK_DNS_AAAA);
      break;
    case HSK_DNS_NS:
      hsk_resource_to_ns(rs, name, ns);
      hsk_resource_to_glue(rs, ar, HSK_DNS_NS);
      hsk_resource_to_nsip(rs, name, ar);
      hsk_dnssec_sign_zsk(ns, HSK_DNS_NS);
      break;
    case HSK_DNS_MX:
      hsk_resource_to_mx(rs, name, an);
      hsk_resource_to_mxip(rs, name, ar);
      hsk_resource_to_glue(rs, ar, HSK_DNS_MX);
      hsk_dnssec_sign_zsk(an, HSK_DNS_MX);
      break;
    case HSK_DNS_TXT:
      hsk_resource_to_txt(rs, name, an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_TXT);
      break;
    case HSK_DNS_LOC:
      hsk_resource_to_loc(rs, name, an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_LOC);
      break;
    case HSK_DNS_DS:
      hsk_resource_to_ds(rs, name, an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_DS);
      break;
    case HSK_DNS_SSHFP:
      hsk_resource_to_sshfp(rs, name, an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_SSHFP);
      break;
    case HSK_DNS_URI:
      hsk_resource_to_uri(rs, name, an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_URI);
      break;
    case HSK_DNS_RP:
      hsk_resource_to_rp(rs, name, an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_RP);
      break;
  }

  if (an->size > 0)
    msg->flags |= HSK_DNS_AA;

  if (an->size == 0 && ns->size == 0) {
    if (hsk_resource_has(rs, HSK_CANONICAL)) {
      msg->flags |= HSK_DNS_AA;
      hsk_resource_to_cname(rs, name, an);
      hsk_resource_to_glue(rs, ar, HSK_DNS_CNAME);
      hsk_dnssec_sign_zsk(an, HSK_DNS_CNAME);
      hsk_dnssec_sign_zsk(ar, HSK_DNS_A);
      hsk_dnssec_sign_zsk(ar, HSK_DNS_AAAA);
    } else if (hsk_resource_has(rs, HSK_NS)) {
      hsk_resource_to_ns(rs, name, ns);
      hsk_resource_to_ds(rs, name, ns);
      hsk_resource_to_nsip(rs, name, ar);
      hsk_resource_to_glue(rs, ar, HSK_DNS_NS);
      if (!hsk_resource_has(rs, HSK_DS))
        hsk_dnssec_sign_zsk(ns, HSK_DNS_NS);
      else
        hsk_dnssec_sign_zsk(ns, HSK_DNS_DS);
    } else {
      // Needs SOA.
      // Empty proof:
      hsk_resource_to_empty(name, NULL, 0, ns);
      hsk_dnssec_sign_zsk(ns, HSK_DNS_NSEC);
      hsk_resource_root_to_soa(ns);
      hsk_dnssec_sign_zsk(ns, HSK_DNS_SOA);
    }
  }

  return msg;
}

hsk_dns_msg_t *
hsk_resource_root(uint16_t type, const hsk_addr_t *addr) {
  hsk_dns_msg_t *msg = hsk_dns_msg_alloc();

  if (!msg)
    return NULL;

  msg->flags |= HSK_DNS_AA;

  hsk_dns_rrs_t *an = &msg->an;
  hsk_dns_rrs_t *ns = &msg->ns;
  hsk_dns_rrs_t *ar = &msg->ar;

  switch (type) {
    case HSK_DNS_ANY:
    case HSK_DNS_NS:
      hsk_resource_root_to_ns(an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_NS);

      if (hsk_addr_is_ip4(addr)) {
        hsk_resource_root_to_a(ar, addr);
        hsk_dnssec_sign_zsk(ar, HSK_DNS_A);
      }

      if (hsk_addr_is_ip6(addr)) {
        hsk_resource_root_to_aaaa(ar, addr);
        hsk_dnssec_sign_zsk(ar, HSK_DNS_AAAA);
      }

      break;
    case HSK_DNS_SOA:
      hsk_resource_root_to_soa(an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_SOA);

      hsk_resource_root_to_ns(ns);
      hsk_dnssec_sign_zsk(ns, HSK_DNS_NS);

      if (hsk_addr_is_ip4(addr)) {
        hsk_resource_root_to_a(ar, addr);
        hsk_dnssec_sign_zsk(ar, HSK_DNS_A);
      }

      if (hsk_addr_is_ip6(addr)) {
        hsk_resource_root_to_aaaa(ar, addr);
        hsk_dnssec_sign_zsk(ar, HSK_DNS_AAAA);
      }

      break;
    case HSK_DNS_DNSKEY:
      hsk_resource_root_to_dnskey(an);
      hsk_dnssec_sign_ksk(an, HSK_DNS_DNSKEY);
      break;
    case HSK_DNS_DS:
      hsk_resource_root_to_ds(an);
      hsk_dnssec_sign_zsk(an, HSK_DNS_DS);
      break;
    default:
      // Empty Proof:
      // Show all the types that we signed.
      hsk_resource_root_to_nsec(ns);
      hsk_dnssec_sign_zsk(ns, HSK_DNS_NSEC);
      hsk_resource_root_to_soa(ns);
      hsk_dnssec_sign_zsk(ns, HSK_DNS_SOA);
      break;
  }

  return msg;
}

hsk_dns_msg_t *
hsk_resource_to_nx(void) {
  hsk_dns_msg_t *msg = hsk_dns_msg_alloc();

  if (!msg)
    return NULL;

  msg->code = HSK_DNS_NXDOMAIN;
  msg->flags |= HSK_DNS_AA;

  hsk_dns_rrs_t *ns = &msg->ns;

  // NX Proof:
  // Just make it look like an
  // empty zone for the NX proof.
  // It seems to fool unbound without
  // breaking anything.
  hsk_resource_root_to_nsec(ns);
  hsk_resource_root_to_nsec(ns);
  hsk_dnssec_sign_zsk(ns, HSK_DNS_NSEC);

  hsk_resource_root_to_soa(ns);
  hsk_dnssec_sign_zsk(ns, HSK_DNS_SOA);

  return msg;
}

hsk_dns_msg_t *
hsk_resource_to_servfail(void) {
  hsk_dns_msg_t *msg = hsk_dns_msg_alloc();

  if (!msg)
    return NULL;

  msg->code = HSK_DNS_SERVFAIL;

  return msg;
}

hsk_dns_msg_t *
hsk_resource_to_notimp(void) {
  hsk_dns_msg_t *msg = hsk_dns_msg_alloc();

  if (!msg)
    return NULL;

  msg->code = HSK_DNS_NOTIMP;

  return msg;
}

/*
 * Helpers
 */

static void
to_fqdn(char *name) {
  size_t len = strlen(name);
  assert(len <= 63);
  name[len] = '.';
  name[len + 1] = '\0';
}

static void
ip_size(const uint8_t *ip, size_t *s, size_t *l) {
  bool out = true;
  int last = 0;
  int i = 0;

  int start = 0;
  int len = 0;

  for (; i < 16; i++) {
    uint8_t ch = ip[i];
    if (out == (ch == 0)) {
      if (!out && i - last > len) {
        start = last;
        len = i - last;
      }
      out = !out;
      last = i;
    }
  }

  if (!out && i - last > len) {
    start = last;
    len = i - last;
  }

  // The worst case:
  // We need at least 2 zeroes in a row to
  // get any benefit from the compression.
  if (len == 16) {
    assert(start == 0);
    len = 0;
  }

  assert(start < 16);
  assert(len < 16);
  assert(start + len <= 16);

  *s = (size_t)start;
  *l = (size_t)len;
}

static size_t
ip_write(const uint8_t *ip, uint8_t *data) {
  size_t start, len;
  ip_size(ip, &start, &len);
  uint8_t left = 16 - (start + len);
  data[0] = (start << 4) | len;
  // Ignore the missing section.
  memcpy(&data[1], ip, start);
  memcpy(&data[1 + start], &ip[start + len], left);
  return 1 + start + left;
}

static bool
ip_read(const uint8_t *data, uint8_t *ip) {
  uint8_t field = data[0];

  uint8_t start = field >> 4;
  uint8_t len = field & 0x0f;

  if (start + len > 16)
    return false;

  uint8_t left = 16 - (start + len);

  // Front half.
  if (ip)
    memcpy(ip, &data[1], start);

  // Fill in the missing section.
  if (ip)
    memset(&ip[start], 0x00, len);

  // Back half.
  if (ip)
    memcpy(&ip[start + len], &data[1 + start], left);

  return true;
}

static void
ip_to_b32(const uint8_t *ip, char *dst) {
  uint8_t mapped[16];
  const size_t family = sizeof(ip);

  if (family == 4) {
    // https://tools.ietf.org/html/rfc4291#section-2.5.5.2
    memset(&mapped[0], 0x00, 10);
    memset(&mapped[10], 0xff, 2);
    memcpy(&mapped[12], ip, 4);
  } else {
    memcpy(&mapped[0], ip, 16);
  }

  uint8_t data[17];

  size_t size = ip_write(mapped, data);
  assert(size <= 17);

  size_t b32_size = hsk_base32_encode_hex_size(data, size, false);
  assert(b32_size <= 29);

  hsk_base32_encode_hex(data, size, dst, false);
}

static bool
b32_to_ip(const char *str, uint8_t *ip, uint16_t *family) {
  size_t size = hsk_base32_decode_hex_size(str);

  if (size == 0 || size > 17)
    return false;

  uint8_t data[17];
  assert(hsk_base32_decode_hex(str, data, false));

  if (!ip_read(data, ip))
    return false;

  if (ip) {
    static const uint8_t mapped[12] = {
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xff, 0xff
    };

    if (memcmp(ip, (void *)&mapped[0], 12) == 0) {
      memcpy(&ip[0], &ip[12], 4);
      if (family)
        *family = HSK_DNS_A;
    } else {
      if (family)
        *family = HSK_DNS_AAAA;
    }
  }

  return true;
}

static bool
pointer_to_ip(const char *name, uint8_t *ip, uint16_t *family) {
  char label[HSK_DNS_MAX_LABEL + 1];
  size_t len = hsk_dns_label_get(name, 0, label);

  if (len < 2 || len > 29 || label[0] != '_')
    return false;

  return b32_to_ip(&label[1], ip, family);
}

static bool
target_to_dns(const hsk_target_t *target, const char *name, char *host) {
  if (target->type == HSK_NAME || target->type == HSK_GLUE) {
    assert(hsk_dns_name_is_fqdn(target->name));
    strcpy(host, target->name);
    return true;
  }

  if (target->type == HSK_INET4 || target->type == HSK_INET6) {
    char b32[29];
    char tld[HSK_DNS_MAX_LABEL + 1];

    ip_to_b32(target, b32);

    int len = hsk_dns_label_get(name, -1, tld);

    if (len <= 0)
      return false;

    sprintf(host, "_%s.%s.", b32, tld);

    return true;
  }

  return false;
}

bool
hsk_resource_is_ptr(const char *name) {
  return pointer_to_ip(name, NULL, NULL);
}
