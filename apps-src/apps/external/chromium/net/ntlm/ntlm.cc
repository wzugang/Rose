// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/ntlm/ntlm.h"

#include <string.h>

#include "base/logging.h"
#include "base/md5.h"
#include "base/strings/utf_string_conversions.h"
#include "net/base/net_string_util.h"
#include "net/ntlm/ntlm_buffer_writer.h"
#include "third_party/boringssl/src/include/openssl/des.h"
#include "third_party/boringssl/src/include/openssl/hmac.h"
#include "third_party/boringssl/src/include/openssl/md4.h"
#include "third_party/boringssl/src/include/openssl/md5.h"

namespace net {
namespace ntlm {

namespace {

// Takes the parsed target info in |av_pairs| and performs the following
// actions.
//
// 1) If a |TargetInfoAvId::kTimestamp| AvPair exists, |server_timestamp|
//    is set to the payload.
// 2) If |is_mic_enabled| is true, the existing |TargetInfoAvId::kFlags| AvPair
//    will have the |TargetInfoAvFlags::kMicPresent| bit set. If an existing
//    flags AvPair does not already exist, a new one is added with the value of
//    |TargetInfoAvFlags::kMicPresent|.
// 3) If |is_epa_enabled| is true, two new AvPair entries will be added to
//    |av_pairs|. The first will be of type |TargetInfoAvId::kChannelBindings|
//    and contains MD5(|channel_bindings|) as the payload. The second will be
//    of type |TargetInfoAvId::kTargetName| and contains |spn| as a little
//    endian UTF16 string.
// 4) Sets |target_info_len| to the size of |av_pairs| when serialized into
//    a payload.
void UpdateTargetInfoAvPairs(bool is_mic_enabled,
                             bool is_epa_enabled,
                             const std::string& channel_bindings,
                             const std::string& spn,
                             std::vector<AvPair>* av_pairs,
                             uint64_t* server_timestamp,
                             size_t* target_info_len) {
  // Do a pass to update flags and calculate current length and
  // pull out the server timestamp if it is there.
  *server_timestamp = UINT64_MAX;
  *target_info_len = 0;

  bool need_flags_added = is_mic_enabled;
  for (AvPair& pair : *av_pairs) {
    *target_info_len += pair.avlen + kAvPairHeaderLen;
    switch (pair.avid) {
      case TargetInfoAvId::kFlags:
        // The parsing phase already set the payload to the |flags| field.
        if (is_mic_enabled) {
          pair.flags = pair.flags | TargetInfoAvFlags::kMicPresent;
        }

        need_flags_added = false;
        break;
      case TargetInfoAvId::kTimestamp:
        // The parsing phase already set the payload to the |timestamp| field.
        *server_timestamp = pair.timestamp;
        break;
      case TargetInfoAvId::kEol:
      case TargetInfoAvId::kChannelBindings:
      case TargetInfoAvId::kTargetName:
        // The terminator, |kEol|, should already have been removed from the
        // end of the list and would have been rejected if it has been inside
        // the list. Additionally |kChannelBindings| and |kTargetName| pairs
        // would have been rejected during the initial parsing. See
        // |NtlmBufferReader::ReadTargetInfo|.
        NOTREACHED();
        break;
      default:
        // Ignore entries we don't care about.
        break;
    }
  }

  if (need_flags_added) {
    DCHECK(is_mic_enabled);
    AvPair flags_pair(TargetInfoAvId::kFlags, sizeof(uint32_t));
    flags_pair.flags = TargetInfoAvFlags::kMicPresent;

    av_pairs->push_back(flags_pair);
    *target_info_len += kAvPairHeaderLen + flags_pair.avlen;
  }

  if (is_epa_enabled) {
    Buffer channel_bindings_hash(kChannelBindingsHashLen, 0);

    // Hash the channel bindings if they exist otherwise they remain zeros.
    if (!channel_bindings.empty()) {
      GenerateChannelBindingHashV2(channel_bindings, &channel_bindings_hash[0]);
    }

    av_pairs->emplace_back(TargetInfoAvId::kChannelBindings,
                           std::move(channel_bindings_hash));

    // Convert the SPN to little endian unicode.
    base::string16 spn16 = base::UTF8ToUTF16(spn);
    NtlmBufferWriter spn_writer(spn16.length() * 2);
    bool spn_writer_result =
        spn_writer.WriteUtf16String(spn16) && spn_writer.IsEndOfBuffer();
    DCHECK(spn_writer_result);

    av_pairs->emplace_back(TargetInfoAvId::kTargetName, spn_writer.Pass());

    // Add the length of the two new AV Pairs to the total length.
    *target_info_len +=
        (2 * kAvPairHeaderLen) + kChannelBindingsHashLen + (spn16.length() * 2);
  }

  // Add extra space for the terminator at the end.
  *target_info_len += kAvPairHeaderLen;
}

Buffer WriteUpdatedTargetInfo(const std::vector<AvPair>& av_pairs,
                              size_t updated_target_info_len) {
  bool result = true;
  NtlmBufferWriter writer(updated_target_info_len);
  for (const AvPair& pair : av_pairs) {
    result = writer.WriteAvPair(pair);
    DCHECK(result);
  }

  result = writer.WriteAvPairTerminator() && writer.IsEndOfBuffer();
  DCHECK(result);
  return writer.Pass();
}

// Reads 7 bytes (56 bits) from |key_56| and writes them into 8 bytes of
// |key_64| with 7 bits in every byte. The least significant bits are
// undefined and a subsequent operation will set those bits with a parity bit.
// |key_56| must contain 7 bytes.
// |key_64| must contain 8 bytes.
void Splay56To64(const uint8_t* key_56, uint8_t* key_64) {
  key_64[0] = key_56[0];
  key_64[1] = key_56[0] << 7 | key_56[1] >> 1;
  key_64[2] = key_56[1] << 6 | key_56[2] >> 2;
  key_64[3] = key_56[2] << 5 | key_56[3] >> 3;
  key_64[4] = key_56[3] << 4 | key_56[4] >> 4;
  key_64[5] = key_56[4] << 3 | key_56[5] >> 5;
  key_64[6] = key_56[5] << 2 | key_56[6] >> 6;
  key_64[7] = key_56[6] << 1;
}

}  // namespace

void Create3DesKeysFromNtlmHash(const uint8_t* ntlm_hash, uint8_t* keys) {
  // Put the first 112 bits from |ntlm_hash| into the first 16 bytes of
  // |keys|.
  Splay56To64(ntlm_hash, keys);
  Splay56To64(ntlm_hash + 7, keys + 8);

  // Put the next 2x 7 bits in bytes 16 and 17 of |keys|, then
  // the last 2 bits in byte 18, then zero pad the rest of the final key.
  keys[16] = ntlm_hash[14];
  keys[17] = ntlm_hash[14] << 7 | ntlm_hash[15] >> 1;
  keys[18] = ntlm_hash[15] << 6;
  memset(keys + 19, 0, 5);
}

void GenerateNtlmHashV1(const base::string16& password, uint8_t* hash) {
  size_t length = password.length() * 2;
  NtlmBufferWriter writer(length);

  // The writer will handle the big endian case if necessary.
  bool result = writer.WriteUtf16String(password) && writer.IsEndOfBuffer();
  DCHECK(result);

  MD4(writer.GetBuffer().data(), writer.GetLength(), hash);
}

void GenerateResponseDesl(const uint8_t* hash,
                          const uint8_t* challenge,
                          uint8_t* response) {
  constexpr size_t block_count = 3;
  constexpr size_t block_size = sizeof(DES_cblock);
  static_assert(kChallengeLen == block_size,
                "kChallengeLen must equal block_size");
  static_assert(kResponseLenV1 == block_count * block_size,
                "kResponseLenV1 must equal block_count * block_size");

  const DES_cblock* challenge_block =
      reinterpret_cast<const DES_cblock*>(challenge);
  uint8_t keys[block_count * block_size];

  // Map the NTLM hash to three 8 byte DES keys, with 7 bits of the key in each
  // byte and the least significant bit set with odd parity. Then encrypt the
  // 8 byte challenge with each of the three keys. This produces three 8 byte
  // encrypted blocks into |response|.
  Create3DesKeysFromNtlmHash(hash, keys);
  for (size_t ix = 0; ix < block_count * block_size; ix += block_size) {
    DES_cblock* key_block = reinterpret_cast<DES_cblock*>(keys + ix);
    DES_cblock* response_block = reinterpret_cast<DES_cblock*>(response + ix);

    DES_key_schedule key_schedule;
    DES_set_odd_parity(key_block);
    DES_set_key(key_block, &key_schedule);
    DES_ecb_encrypt(challenge_block, response_block, &key_schedule,
                    DES_ENCRYPT);
  }
}

void GenerateNtlmResponseV1(const base::string16& password,
                            const uint8_t* challenge,
                            uint8_t* ntlm_response) {
  uint8_t ntlm_hash[kNtlmHashLen];
  GenerateNtlmHashV1(password, ntlm_hash);
  GenerateResponseDesl(ntlm_hash, challenge, ntlm_response);
}

void GenerateResponsesV1(const base::string16& password,
                         const uint8_t* server_challenge,
                         uint8_t* lm_response,
                         uint8_t* ntlm_response) {
  GenerateNtlmResponseV1(password, server_challenge, ntlm_response);

  // In NTLM v1 (with LMv1 disabled), the lm_response and ntlm_response are the
  // same. So just copy the ntlm_response into the lm_response.
  memcpy(lm_response, ntlm_response, kResponseLenV1);
}

void GenerateLMResponseV1WithSessionSecurity(const uint8_t* client_challenge,
                                             uint8_t* lm_response) {
  // In NTLM v1 with Session Security (aka NTLM2) the lm_response is 8 bytes of
  // client challenge and 16 bytes of zeros. (See 3.3.1)
  memcpy(lm_response, client_challenge, kChallengeLen);
  memset(lm_response + kChallengeLen, 0, kResponseLenV1 - kChallengeLen);
}

void GenerateSessionHashV1WithSessionSecurity(const uint8_t* server_challenge,
                                              const uint8_t* client_challenge,
                                              uint8_t* session_hash) {
  MD5_CTX ctx;
  MD5_Init(&ctx);
  MD5_Update(&ctx, server_challenge, kChallengeLen);
  MD5_Update(&ctx, client_challenge, kChallengeLen);
  MD5_Final(session_hash, &ctx);
}

void GenerateNtlmResponseV1WithSessionSecurity(const base::string16& password,
                                               const uint8_t* server_challenge,
                                               const uint8_t* client_challenge,
                                               uint8_t* ntlm_response) {
  // Generate the NTLMv1 Hash.
  uint8_t ntlm_hash[kNtlmHashLen];
  GenerateNtlmHashV1(password, ntlm_hash);

  // Generate the NTLMv1 Session Hash.
  uint8_t session_hash[kNtlmHashLen];
  GenerateSessionHashV1WithSessionSecurity(server_challenge, client_challenge,
                                           session_hash);

  // Only the first 8 bytes of |session_hash| are actually used.
  GenerateResponseDesl(ntlm_hash, session_hash, ntlm_response);
}

void GenerateResponsesV1WithSessionSecurity(const base::string16& password,
                                            const uint8_t* server_challenge,
                                            const uint8_t* client_challenge,
                                            uint8_t* lm_response,
                                            uint8_t* ntlm_response) {
  GenerateLMResponseV1WithSessionSecurity(client_challenge, lm_response);
  GenerateNtlmResponseV1WithSessionSecurity(password, server_challenge,
                                            client_challenge, ntlm_response);
}

void GenerateNtlmHashV2(const base::string16& domain,
                        const base::string16& username,
                        const base::string16& password,
                        uint8_t* v2_hash) {
  // NOTE: According to [MS-NLMP] Section 3.3.2 only the username and not the
  // domain is uppercased.
  base::string16 upper_username;
  bool result = ToUpper(username, &upper_username);
  DCHECK(result);

  uint8_t v1_hash[kNtlmHashLen];
  GenerateNtlmHashV1(password, v1_hash);
  NtlmBufferWriter input_writer((upper_username.length() + domain.length()) *
                                2);
  bool writer_result = input_writer.WriteUtf16String(upper_username) &&
                       input_writer.WriteUtf16String(domain) &&
                       input_writer.IsEndOfBuffer();
  DCHECK(writer_result);

  unsigned int outlen = kNtlmHashLen;
  v2_hash =
      HMAC(EVP_md5(), v1_hash, sizeof(v1_hash), input_writer.GetBuffer().data(),
           input_writer.GetLength(), v2_hash, &outlen);
  DCHECK_NE(nullptr, v2_hash);
  DCHECK_EQ(sizeof(v1_hash), outlen);
}

Buffer GenerateProofInputV2(uint64_t timestamp,
                            const uint8_t* client_challenge) {
  NtlmBufferWriter writer(kProofInputLenV2);
  bool result = writer.WriteUInt16(kProofInputVersionV2) &&
                writer.WriteZeros(6) && writer.WriteUInt64(timestamp) &&
                writer.WriteBytes(client_challenge, kChallengeLen) &&
                writer.WriteZeros(4) && writer.IsEndOfBuffer();

  DCHECK(result);
  return writer.Pass();
}

void GenerateNtlmProofV2(const uint8_t* v2_hash,
                         const uint8_t* server_challenge,
                         const Buffer& v2_input,
                         const Buffer& target_info,
                         uint8_t* v2_proof) {
  DCHECK_EQ(kProofInputLenV2, v2_input.size());

  bssl::ScopedHMAC_CTX ctx;
  HMAC_Init_ex(ctx.get(), v2_hash, kNtlmHashLen, EVP_md5(), NULL);
  DCHECK_EQ(kNtlmProofLenV2, HMAC_size(ctx.get()));
  HMAC_Update(ctx.get(), server_challenge, kChallengeLen);
  HMAC_Update(ctx.get(), v2_input.data(), v2_input.size());
  HMAC_Update(ctx.get(), target_info.data(), target_info.size());
  const uint32_t zero = 0;
  HMAC_Update(ctx.get(), reinterpret_cast<const uint8_t*>(&zero),
              sizeof(uint32_t));
  HMAC_Final(ctx.get(), v2_proof, nullptr);
}

void GenerateSessionBaseKeyV2(const uint8_t* v2_hash,
                              const uint8_t* v2_proof,
                              uint8_t* session_key) {
  unsigned int outlen = kSessionKeyLenV2;
  session_key = HMAC(EVP_md5(), v2_hash, kNtlmHashLen, v2_proof,
                     kNtlmProofLenV2, session_key, &outlen);
  DCHECK_NE(nullptr, session_key);
  DCHECK_EQ(kSessionKeyLenV2, outlen);
}

void GenerateChannelBindingHashV2(const std::string& channel_bindings,
                                  uint8_t* channel_bindings_hash) {
  NtlmBufferWriter writer(kEpaUnhashedStructHeaderLen);
  bool result = writer.WriteZeros(16) &&
                writer.WriteUInt32(channel_bindings.length()) &&
                writer.IsEndOfBuffer();
  DCHECK(result);

  MD5_CTX ctx;
  MD5_Init(&ctx);
  MD5_Update(&ctx, writer.GetBuffer().data(), writer.GetBuffer().size());
  MD5_Update(&ctx, channel_bindings.data(), channel_bindings.size());
  MD5_Final(channel_bindings_hash, &ctx);
}

void GenerateMicV2(const uint8_t* session_key,
                   const Buffer& negotiate_msg,
                   const Buffer& challenge_msg,
                   const Buffer& authenticate_msg,
                   uint8_t* mic) {
  bssl::ScopedHMAC_CTX ctx;
  HMAC_Init_ex(ctx.get(), session_key, kNtlmHashLen, EVP_md5(), NULL);
  DCHECK_EQ(kMicLenV2, HMAC_size(ctx.get()));
  HMAC_Update(ctx.get(), negotiate_msg.data(), negotiate_msg.size());
  HMAC_Update(ctx.get(), challenge_msg.data(), challenge_msg.size());
  HMAC_Update(ctx.get(), authenticate_msg.data(), authenticate_msg.size());
  HMAC_Final(ctx.get(), mic, nullptr);
}

NET_EXPORT_PRIVATE Buffer
GenerateUpdatedTargetInfo(bool is_mic_enabled,
                          bool is_epa_enabled,
                          const std::string& channel_bindings,
                          const std::string& spn,
                          const std::vector<AvPair>& av_pairs,
                          uint64_t* server_timestamp) {
  size_t updated_target_info_len = 0;
  std::vector<AvPair> updated_av_pairs(av_pairs);
  UpdateTargetInfoAvPairs(is_mic_enabled, is_epa_enabled, channel_bindings, spn,
                          &updated_av_pairs, server_timestamp,
                          &updated_target_info_len);
  return WriteUpdatedTargetInfo(updated_av_pairs, updated_target_info_len);
}

}  // namespace ntlm
}  // namespace net