/*
 *  Copyright 2017 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/jseptransportcontroller.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "p2p/base/port.h"
#include "rtc_base/bind.h"
#include "rtc_base/checks.h"
#include "rtc_base/ptr_util.h"
#include "rtc_base/thread.h"

using webrtc::SdpType;

namespace {

enum {
  MSG_ICECONNECTIONSTATE,
  MSG_ICEGATHERINGSTATE,
  MSG_ICECANDIDATESGATHERED,
};

struct CandidatesData : public rtc::MessageData {
  CandidatesData(const std::string& transport_name,
                 const cricket::Candidates& candidates)
      : transport_name(transport_name), candidates(candidates) {}

  std::string transport_name;
  cricket::Candidates candidates;
};

webrtc::RTCError VerifyCandidate(const cricket::Candidate& cand) {
  // No address zero.
  if (cand.address().IsNil() || cand.address().IsAnyIP()) {
    return webrtc::RTCError(webrtc::RTCErrorType::INVALID_PARAMETER,
                            "candidate has address of zero");
  }

  // Disallow all ports below 1024, except for 80 and 443 on public addresses.
  int port = cand.address().port();
  if (cand.protocol() == cricket::TCP_PROTOCOL_NAME &&
      (cand.tcptype() == cricket::TCPTYPE_ACTIVE_STR || port == 0)) {
    // Expected for active-only candidates per
    // http://tools.ietf.org/html/rfc6544#section-4.5 so no error.
    // Libjingle clients emit port 0, in "active" mode.
    return webrtc::RTCError::OK();
  }
  if (port < 1024) {
    if ((port != 80) && (port != 443)) {
      return webrtc::RTCError(
          webrtc::RTCErrorType::INVALID_PARAMETER,
          "candidate has port below 1024, but not 80 or 443");
    }

    if (cand.address().IsPrivateIP()) {
      return webrtc::RTCError(
          webrtc::RTCErrorType::INVALID_PARAMETER,
          "candidate has port of 80 or 443 with private IP address");
    }
  }

  return webrtc::RTCError::OK();
}

webrtc::RTCError VerifyCandidates(const cricket::Candidates& candidates) {
  for (const cricket::Candidate& candidate : candidates) {
    webrtc::RTCError error = VerifyCandidate(candidate);
    if (!error.ok()) {
      return error;
    }
  }
  return webrtc::RTCError::OK();
}

}  // namespace

namespace webrtc {

JsepTransportController::JsepTransportController(
    rtc::Thread* signaling_thread,
    rtc::Thread* network_thread,
    cricket::PortAllocator* port_allocator,
    Config config)
    : signaling_thread_(signaling_thread),
      network_thread_(network_thread),
      port_allocator_(port_allocator),
      config_(config) {}

JsepTransportController::~JsepTransportController() {
  // Channel destructors may try to send packets, so this needs to happen on
  // the network thread.
  network_thread_->Invoke<void>(
      RTC_FROM_HERE,
      rtc::Bind(&JsepTransportController::DestroyAllJsepTransports_n, this));
}

RTCError JsepTransportController::SetLocalDescription(
    SdpType type,
    const cricket::SessionDescription* description) {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<RTCError>(
        RTC_FROM_HERE, [=] { return SetLocalDescription(type, description); });
  }

  if (!initial_offerer_.has_value()) {
    initial_offerer_.emplace(type == SdpType::kOffer);
    if (*initial_offerer_) {
      SetIceRole_n(cricket::ICEROLE_CONTROLLING);
    } else {
      SetIceRole_n(cricket::ICEROLE_CONTROLLED);
    }
  }
  return ApplyDescription_n(/*local=*/true, type, description);
}

RTCError JsepTransportController::SetRemoteDescription(
    SdpType type,
    const cricket::SessionDescription* description) {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<RTCError>(
        RTC_FROM_HERE, [=] { return SetRemoteDescription(type, description); });
  }

  return ApplyDescription_n(/*local=*/false, type, description);
}

RtpTransportInternal* JsepTransportController::GetRtpTransport(
    const std::string& mid) const {
  auto jsep_transport = GetJsepTransportForMid(mid);
  if (!jsep_transport) {
    return nullptr;
  }
  return jsep_transport->rtp_transport();
}

cricket::DtlsTransportInternal* JsepTransportController::GetDtlsTransport(
    const std::string& mid) const {
  auto jsep_transport = GetJsepTransportForMid(mid);
  if (!jsep_transport) {
    return nullptr;
  }
  return jsep_transport->rtp_dtls_transport();
}

cricket::DtlsTransportInternal* JsepTransportController::GetRtcpDtlsTransport(
    const std::string& mid) const {
  auto jsep_transport = GetJsepTransportForMid(mid);
  if (!jsep_transport) {
    return nullptr;
  }
  return jsep_transport->rtcp_dtls_transport();
}

void JsepTransportController::SetIceConfig(const cricket::IceConfig& config) {
  if (!network_thread_->IsCurrent()) {
    network_thread_->Invoke<void>(RTC_FROM_HERE, [&] { SetIceConfig(config); });
    return;
  }

  ice_config_ = config;
  for (auto& dtls : GetDtlsTransports()) {
    dtls->ice_transport()->SetIceConfig(ice_config_);
  }
}

void JsepTransportController::SetNeedsIceRestartFlag() {
  for (auto& kv : jsep_transports_by_name_) {
    kv.second->SetNeedsIceRestartFlag();
  }
}

bool JsepTransportController::NeedsIceRestart(
    const std::string& transport_name) const {
  const cricket::JsepTransport2* transport =
      GetJsepTransportByName(transport_name);
  if (!transport) {
    return false;
  }
  return transport->needs_ice_restart();
}

rtc::Optional<rtc::SSLRole> JsepTransportController::GetDtlsRole(
    const std::string& mid) const {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<rtc::Optional<rtc::SSLRole>>(
        RTC_FROM_HERE, [&] { return GetDtlsRole(mid); });
  }

  const cricket::JsepTransport2* t = GetJsepTransportForMid(mid);
  if (!t) {
    return rtc::Optional<rtc::SSLRole>();
  }
  return t->GetDtlsRole();
}

bool JsepTransportController::SetLocalCertificate(
    const rtc::scoped_refptr<rtc::RTCCertificate>& certificate) {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<bool>(
        RTC_FROM_HERE, [&] { return SetLocalCertificate(certificate); });
  }

  // Can't change a certificate, or set a null certificate.
  if (certificate_ || !certificate) {
    return false;
  }
  certificate_ = certificate;

  // Set certificate for JsepTransport, which verifies it matches the
  // fingerprint in SDP, and DTLS transport.
  // Fallback from DTLS to SDES is not supported.
  for (auto& kv : jsep_transports_by_name_) {
    kv.second->SetLocalCertificate(certificate_);
  }
  for (auto& dtls : GetDtlsTransports()) {
    bool set_cert_success = dtls->SetLocalCertificate(certificate_);
    RTC_DCHECK(set_cert_success);
  }
  return true;
}

rtc::scoped_refptr<rtc::RTCCertificate>
JsepTransportController::GetLocalCertificate(
    const std::string& transport_name) const {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<rtc::scoped_refptr<rtc::RTCCertificate>>(
        RTC_FROM_HERE, [&] { return GetLocalCertificate(transport_name); });
  }

  const cricket::JsepTransport2* t = GetJsepTransportByName(transport_name);
  if (!t) {
    return nullptr;
  }
  return t->GetLocalCertificate();
}

std::unique_ptr<rtc::SSLCertChain>
JsepTransportController::GetRemoteSSLCertChain(
    const std::string& transport_name) const {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<std::unique_ptr<rtc::SSLCertChain>>(
        RTC_FROM_HERE, [&] { return GetRemoteSSLCertChain(transport_name); });
  }

  // Get the certificate from the RTP transport's DTLS handshake. Should be
  // identical to the RTCP transport's, since they were given the same remote
  // fingerprint.
  auto jsep_transport = GetJsepTransportByName(transport_name);
  if (!jsep_transport) {
    return nullptr;
  }
  auto dtls = jsep_transport->rtp_dtls_transport();
  if (!dtls) {
    return nullptr;
  }

  return dtls->GetRemoteSSLCertChain();
}

void JsepTransportController::MaybeStartGathering() {
  if (!network_thread_->IsCurrent()) {
    network_thread_->Invoke<void>(RTC_FROM_HERE,
                                  [&] { MaybeStartGathering(); });
    return;
  }

  for (auto& dtls : GetDtlsTransports()) {
    dtls->ice_transport()->MaybeStartGathering();
  }
}

RTCError JsepTransportController::AddRemoteCandidates(
    const std::string& transport_name,
    const cricket::Candidates& candidates) {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<RTCError>(RTC_FROM_HERE, [&] {
      return AddRemoteCandidates(transport_name, candidates);
    });
  }

  // Verify each candidate before passing down to the transport layer.
  RTCError error = VerifyCandidates(candidates);
  if (!error.ok()) {
    return error;
  }
  auto jsep_transport = GetJsepTransportByName(transport_name);
  if (!jsep_transport) {
    RTC_LOG(LS_WARNING) << "Not adding candidate because the JsepTransport "
                           "doesn't exist. Ignore it.";
    return RTCError::OK();
  }
  return jsep_transport->AddRemoteCandidates(candidates);
}

RTCError JsepTransportController::RemoveRemoteCandidates(
    const cricket::Candidates& candidates) {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<RTCError>(
        RTC_FROM_HERE, [&] { return RemoveRemoteCandidates(candidates); });
  }

  // Verify each candidate before passing down to the transport layer.
  RTCError error = VerifyCandidates(candidates);
  if (!error.ok()) {
    return error;
  }

  std::map<std::string, cricket::Candidates> candidates_by_transport_name;
  for (const cricket::Candidate& cand : candidates) {
    if (!cand.transport_name().empty()) {
      candidates_by_transport_name[cand.transport_name()].push_back(cand);
    } else {
      RTC_LOG(LS_ERROR) << "Not removing candidate because it does not have a "
                           "transport name set: "
                        << cand.ToString();
    }
  }

  for (const auto& kv : candidates_by_transport_name) {
    const std::string& transport_name = kv.first;
    const cricket::Candidates& candidates = kv.second;
    cricket::JsepTransport2* jsep_transport =
        GetJsepTransportByName(transport_name);
    if (!jsep_transport) {
      RTC_LOG(LS_WARNING)
          << "Not removing candidate because the JsepTransport doesn't exist.";
      continue;
    }
    for (const cricket::Candidate& candidate : candidates) {
      auto dtls = candidate.component() == cricket::ICE_CANDIDATE_COMPONENT_RTP
                      ? jsep_transport->rtp_dtls_transport()
                      : jsep_transport->rtcp_dtls_transport();
      if (dtls) {
        dtls->ice_transport()->RemoveRemoteCandidate(candidate);
      }
    }
  }
  return RTCError::OK();
}

bool JsepTransportController::GetStats(const std::string& transport_name,
                                       cricket::TransportStats* stats) {
  if (!network_thread_->IsCurrent()) {
    return network_thread_->Invoke<bool>(
        RTC_FROM_HERE, [=] { return GetStats(transport_name, stats); });
  }

  cricket::JsepTransport2* transport = GetJsepTransportByName(transport_name);
  if (!transport) {
    return false;
  }
  return transport->GetStats(stats);
}

void JsepTransportController::SetMetricsObserver(
    webrtc::MetricsObserverInterface* metrics_observer) {
  if (!network_thread_->IsCurrent()) {
    network_thread_->Invoke<void>(
        RTC_FROM_HERE, [=] { SetMetricsObserver(metrics_observer); });
    return;
  }

  metrics_observer_ = metrics_observer;
  for (auto& dtls : GetDtlsTransports()) {
    dtls->ice_transport()->SetMetricsObserver(metrics_observer);
  }
}

std::unique_ptr<cricket::DtlsTransportInternal>
JsepTransportController::CreateDtlsTransport(const std::string& transport_name,
                                             bool rtcp) {
  RTC_DCHECK(network_thread_->IsCurrent());
  int component = rtcp ? cricket::ICE_CANDIDATE_COMPONENT_RTCP
                       : cricket::ICE_CANDIDATE_COMPONENT_RTP;

  std::unique_ptr<cricket::DtlsTransportInternal> dtls;
  if (config_.external_transport_factory) {
    auto ice = config_.external_transport_factory->CreateIceTransport(
        transport_name, component);
    dtls = config_.external_transport_factory->CreateDtlsTransport(
        std::move(ice), config_.crypto_options);
  } else {
    auto ice = rtc::MakeUnique<cricket::P2PTransportChannel>(
        transport_name, component, port_allocator_);
    dtls = rtc::MakeUnique<cricket::DtlsTransport>(std::move(ice),
                                                   config_.crypto_options);
  }

  RTC_DCHECK(dtls);
  dtls->SetSslMaxProtocolVersion(config_.ssl_max_version);
  dtls->ice_transport()->SetMetricsObserver(metrics_observer_);
  dtls->ice_transport()->SetIceRole(ice_role_);
  dtls->ice_transport()->SetIceTiebreaker(ice_tiebreaker_);
  dtls->ice_transport()->SetIceConfig(ice_config_);
  if (certificate_) {
    bool set_cert_success = dtls->SetLocalCertificate(certificate_);
    RTC_DCHECK(set_cert_success);
  }

  // Connect to signals offered by the DTLS and ICE transport.
  dtls->SignalWritableState.connect(
      this, &JsepTransportController::OnTransportWritableState_n);
  dtls->SignalReceivingState.connect(
      this, &JsepTransportController::OnTransportReceivingState_n);
  dtls->SignalDtlsHandshakeError.connect(
      this, &JsepTransportController::OnDtlsHandshakeError);
  dtls->ice_transport()->SignalGatheringState.connect(
      this, &JsepTransportController::OnTransportGatheringState_n);
  dtls->ice_transport()->SignalCandidateGathered.connect(
      this, &JsepTransportController::OnTransportCandidateGathered_n);
  dtls->ice_transport()->SignalCandidatesRemoved.connect(
      this, &JsepTransportController::OnTransportCandidatesRemoved_n);
  dtls->ice_transport()->SignalRoleConflict.connect(
      this, &JsepTransportController::OnTransportRoleConflict_n);
  dtls->ice_transport()->SignalStateChanged.connect(
      this, &JsepTransportController::OnTransportStateChanged_n);
  return dtls;
}

std::unique_ptr<webrtc::RtpTransport>
JsepTransportController::CreateUnencryptedRtpTransport(
    const std::string& transport_name,
    rtc::PacketTransportInternal* rtp_packet_transport,
    rtc::PacketTransportInternal* rtcp_packet_transport) {
  RTC_DCHECK(network_thread_->IsCurrent());
  auto unencrypted_rtp_transport =
      rtc::MakeUnique<RtpTransport>(rtcp_packet_transport == nullptr);
  unencrypted_rtp_transport->SetRtpPacketTransport(rtp_packet_transport);
  if (rtcp_packet_transport) {
    unencrypted_rtp_transport->SetRtcpPacketTransport(rtcp_packet_transport);
  }
  return unencrypted_rtp_transport;
}

std::unique_ptr<webrtc::SrtpTransport>
JsepTransportController::CreateSdesTransport(
    const std::string& transport_name,
    cricket::DtlsTransportInternal* rtp_dtls_transport,
    cricket::DtlsTransportInternal* rtcp_dtls_transport) {
  RTC_DCHECK(network_thread_->IsCurrent());
  auto srtp_transport =
      rtc::MakeUnique<webrtc::SrtpTransport>(rtcp_dtls_transport == nullptr);
  RTC_DCHECK(rtp_dtls_transport);
  srtp_transport->SetRtpPacketTransport(rtp_dtls_transport);
  if (rtcp_dtls_transport) {
    srtp_transport->SetRtcpPacketTransport(rtcp_dtls_transport);
  }
  if (config_.enable_external_auth) {
    srtp_transport->EnableExternalAuth();
  }
  return srtp_transport;
}

std::unique_ptr<webrtc::DtlsSrtpTransport>
JsepTransportController::CreateDtlsSrtpTransport(
    const std::string& transport_name,
    cricket::DtlsTransportInternal* rtp_dtls_transport,
    cricket::DtlsTransportInternal* rtcp_dtls_transport) {
  RTC_DCHECK(network_thread_->IsCurrent());
  auto srtp_transport =
      rtc::MakeUnique<webrtc::SrtpTransport>(rtcp_dtls_transport == nullptr);
  if (config_.enable_external_auth) {
    srtp_transport->EnableExternalAuth();
  }

  auto dtls_srtp_transport =
      rtc::MakeUnique<webrtc::DtlsSrtpTransport>(std::move(srtp_transport));

  dtls_srtp_transport->SetDtlsTransports(rtp_dtls_transport,
                                         rtcp_dtls_transport);
  return dtls_srtp_transport;
}

std::vector<cricket::DtlsTransportInternal*>
JsepTransportController::GetDtlsTransports() {
  std::vector<cricket::DtlsTransportInternal*> dtls_transports;
  for (auto it = jsep_transports_by_name_.begin();
       it != jsep_transports_by_name_.end(); ++it) {
    auto jsep_transport = it->second.get();
    RTC_DCHECK(jsep_transport);
    if (jsep_transport->rtp_dtls_transport()) {
      dtls_transports.push_back(jsep_transport->rtp_dtls_transport());
    }

    if (jsep_transport->rtcp_dtls_transport()) {
      dtls_transports.push_back(jsep_transport->rtcp_dtls_transport());
    }
  }
  return dtls_transports;
}

void JsepTransportController::OnMessage(rtc::Message* pmsg) {
  RTC_DCHECK(signaling_thread_->IsCurrent());

  switch (pmsg->message_id) {
    case MSG_ICECONNECTIONSTATE: {
      rtc::TypedMessageData<cricket::IceConnectionState>* data =
          static_cast<rtc::TypedMessageData<cricket::IceConnectionState>*>(
              pmsg->pdata);
      SignalIceConnectionState(data->data());
      delete data;
      break;
    }
    case MSG_ICEGATHERINGSTATE: {
      rtc::TypedMessageData<cricket::IceGatheringState>* data =
          static_cast<rtc::TypedMessageData<cricket::IceGatheringState>*>(
              pmsg->pdata);
      SignalIceGatheringState(data->data());
      delete data;
      break;
    }
    case MSG_ICECANDIDATESGATHERED: {
      CandidatesData* data = static_cast<CandidatesData*>(pmsg->pdata);
      SignalIceCandidatesGathered(data->transport_name, data->candidates);
      delete data;
      break;
    }
    default:
      RTC_NOTREACHED();
  }
}

RTCError JsepTransportController::ApplyDescription_n(
    bool local,
    SdpType type,
    const cricket::SessionDescription* description) {
  RTC_DCHECK(network_thread_->IsCurrent());
  RTC_DCHECK(description);

  if (local) {
    local_desc_ = description;
  } else {
    remote_desc_ = description;
  }

  RTCError error;
  error = ValidateAndMaybeUpdateBundleGroup(local, type, description);
  if (!error.ok()) {
    return error;
  }

  std::vector<int> merged_encrypted_extension_ids;
  if (bundle_group_) {
    merged_encrypted_extension_ids =
        MergeEncryptedHeaderExtensionIdsForBundle(description);
  }

  for (const cricket::ContentInfo& content_info : description->contents()) {
    // Don't create transports for rejected m-lines and bundled m-lines."
    if (content_info.rejected ||
        (IsBundled(content_info.name) && content_info.name != *bundled_mid())) {
      continue;
    }
    error = MaybeCreateJsepTransport(content_info);
    if (!error.ok()) {
      return error;
    }
  }

  RTC_DCHECK(description->contents().size() ==
             description->transport_infos().size());
  for (size_t i = 0; i < description->contents().size(); ++i) {
    const cricket::ContentInfo& content_info = description->contents()[i];
    const cricket::TransportInfo& transport_info =
        description->transport_infos()[i];
    if (content_info.rejected) {
      HandleRejectedContent(content_info, description);
      continue;
    }

    if (IsBundled(content_info.name) && content_info.name != *bundled_mid()) {
      HandleBundledContent(content_info);
      continue;
    }

    error = ValidateContent(content_info);
    if (!error.ok()) {
      return error;
    }

    std::vector<int> extension_ids;
    if (bundled_mid() && content_info.name == *bundled_mid()) {
      extension_ids = merged_encrypted_extension_ids;
    } else {
      extension_ids = GetEncryptedHeaderExtensionIds(content_info);
    }

    int rtp_abs_sendtime_extn_id =
        GetRtpAbsSendTimeHeaderExtensionId(content_info);

    cricket::JsepTransport2* transport =
        GetJsepTransportForMid(content_info.name);
    RTC_DCHECK(transport);

    SetIceRole_n(DetermineIceRole(transport, transport_info, type, local));

    cricket::JsepTransportDescription jsep_description =
        CreateJsepTransportDescription(content_info, transport_info,
                                       extension_ids, rtp_abs_sendtime_extn_id);
    if (local) {
      error =
          transport->SetLocalJsepTransportDescription(jsep_description, type);
    } else {
      error =
          transport->SetRemoteJsepTransportDescription(jsep_description, type);
    }

    if (!error.ok()) {
      LOG_AND_RETURN_ERROR(RTCErrorType::INVALID_PARAMETER,
                           "Failed to apply the description for " +
                               content_info.name + ": " + error.message());
    }
  }
  return RTCError::OK();
}

RTCError JsepTransportController::ValidateAndMaybeUpdateBundleGroup(
    bool local,
    SdpType type,
    const cricket::SessionDescription* description) {
  RTC_DCHECK(description);
  const cricket::ContentGroup* new_bundle_group =
      description->GetGroupByName(cricket::GROUP_TYPE_BUNDLE);

  // The BUNDLE group containing a MID that no m= section has is invalid.
  if (new_bundle_group) {
    for (auto content_name : new_bundle_group->content_names()) {
      if (!description->GetContentByName(content_name)) {
        return RTCError(RTCErrorType::INVALID_PARAMETER,
                        "The BUNDLE group contains MID:" + content_name +
                            " matching no m= section.");
      }
    }
  }

  if (type == SdpType::kAnswer) {
    const cricket::ContentGroup* offered_bundle_group =
        local ? remote_desc_->GetGroupByName(cricket::GROUP_TYPE_BUNDLE)
              : local_desc_->GetGroupByName(cricket::GROUP_TYPE_BUNDLE);

    if (new_bundle_group) {
      // The BUNDLE group in answer should be a subset of offered group.
      for (auto content_name : new_bundle_group->content_names()) {
        if (!offered_bundle_group ||
            !offered_bundle_group->HasContentName(content_name)) {
          return RTCError(RTCErrorType::INVALID_PARAMETER,
                          "The BUNDLE group in answer contains a MID that was "
                          "not in the offered group.");
        }
      }
    }

    if (bundle_group_) {
      for (auto content_name : bundle_group_->content_names()) {
        // An answer that removes m= sections from pre-negotiated BUNDLE group
        // without rejecting it, is invalid.
        if (!new_bundle_group ||
            !new_bundle_group->HasContentName(content_name)) {
          auto* content_info = description->GetContentByName(content_name);
          if (!content_info || !content_info->rejected) {
            return RTCError(RTCErrorType::INVALID_PARAMETER,
                            "Answer cannot remove m= section  " + content_name +
                                " from already-established BUNDLE group.");
          }
        }
      }
    }
  }

  if (config_.bundle_policy ==
          PeerConnectionInterface::kBundlePolicyMaxBundle &&
      !description->HasGroup(cricket::GROUP_TYPE_BUNDLE)) {
    return RTCError(RTCErrorType::INVALID_PARAMETER,
                    "max-bundle is used but no bundle group found.");
  }

  if (ShouldUpdateBundleGroup(type, description)) {
    const std::string* new_bundled_mid = new_bundle_group->FirstContentName();
    if (bundled_mid() && new_bundled_mid &&
        *bundled_mid() != *new_bundled_mid) {
      return RTCError(RTCErrorType::UNSUPPORTED_OPERATION,
                      "Changing the negotiated BUNDLE-tag is not supported.");
    }

    bundle_group_ = *new_bundle_group;
  }

  if (!bundled_mid()) {
    return RTCError::OK();
  }

  auto bundled_content = description->GetContentByName(*bundled_mid());
  if (!bundled_content) {
    return RTCError(
        RTCErrorType::INVALID_PARAMETER,
        "An m= section associated with the BUNDLE-tag doesn't exist.");
  }

  // If the |bundled_content| is rejected, other contents in the bundle group
  // should be rejected.
  if (bundled_content->rejected) {
    for (auto content_name : bundle_group_->content_names()) {
      auto other_content = description->GetContentByName(content_name);
      if (!other_content->rejected) {
        return RTCError(
            RTCErrorType::INVALID_PARAMETER,
            "The m= section:" + content_name + " should be rejected.");
      }
    }
  }

  return RTCError::OK();
}

RTCError JsepTransportController::ValidateContent(
    const cricket::ContentInfo& content_info) {
  if (config_.rtcp_mux_policy ==
          PeerConnectionInterface::kRtcpMuxPolicyRequire &&
      content_info.type == cricket::MediaProtocolType::kRtp &&
      !content_info.media_description()->rtcp_mux()) {
    return RTCError(RTCErrorType::INVALID_PARAMETER,
                    "The m= section:" + content_info.name +
                        " is invalid. RTCP-MUX is not "
                        "enabled when it is required.");
  }
  return RTCError::OK();
}

void JsepTransportController::HandleRejectedContent(
    const cricket::ContentInfo& content_info,
    const cricket::SessionDescription* description) {
  // If the content is rejected, let the
  // BaseChannel/SctpTransport change the RtpTransport/DtlsTransport first,
  // then destroy the cricket::JsepTransport2.
  RemoveTransportForMid(content_info.name, content_info.type);
  // If the answerer rejects the first content, which other contents are bundled
  // on, all the other contents in the bundle group will be rejected.
  if (content_info.name == bundled_mid()) {
    for (auto content_name : bundle_group_->content_names()) {
      const cricket::ContentInfo* content_in_group =
          description->GetContentByName(content_name);
      RTC_DCHECK(content_in_group);
      RemoveTransportForMid(content_name, content_in_group->type);
    }
    bundle_group_.reset();
  } else if (IsBundled(content_info.name)) {
    // Remove the rejected content from the |bundle_group_|.
    bundle_group_->RemoveContentName(content_info.name);
    // Reset the bundle group if nothing left.
    if (!bundle_group_->FirstContentName()) {
      bundle_group_.reset();
    }
  }
  MaybeDestroyJsepTransport(content_info.name);
}

void JsepTransportController::HandleBundledContent(
    const cricket::ContentInfo& content_info) {
  auto jsep_transport = GetJsepTransportByName(*bundled_mid());
  RTC_DCHECK(jsep_transport);
  // If the content is bundled, let the
  // BaseChannel/SctpTransport change the RtpTransport/DtlsTransport first,
  // then destroy the cricket::JsepTransport2.
  SetTransportForMid(content_info.name, jsep_transport, content_info.type);
  MaybeDestroyJsepTransport(content_info.name);
}

void JsepTransportController::SetTransportForMid(
    const std::string& mid,
    cricket::JsepTransport2* jsep_transport,
    cricket::MediaProtocolType protocol_type) {
  if (mid_to_transport_[mid] == jsep_transport) {
    return;
  }

  mid_to_transport_[mid] = jsep_transport;
  if (protocol_type == cricket::MediaProtocolType::kRtp) {
    SignalRtpTransportChanged(mid, jsep_transport->rtp_transport());
  } else {
    SignalDtlsTransportChanged(mid, jsep_transport->rtp_dtls_transport());
  }
}

void JsepTransportController::RemoveTransportForMid(
    const std::string& mid,
    cricket::MediaProtocolType protocol_type) {
  if (protocol_type == cricket::MediaProtocolType::kRtp) {
    SignalRtpTransportChanged(mid, nullptr);
  } else {
    SignalDtlsTransportChanged(mid, nullptr);
  }
  mid_to_transport_.erase(mid);
}

cricket::JsepTransportDescription
JsepTransportController::CreateJsepTransportDescription(
    cricket::ContentInfo content_info,
    cricket::TransportInfo transport_info,
    const std::vector<int>& encrypted_extension_ids,
    int rtp_abs_sendtime_extn_id) {
  const cricket::MediaContentDescription* content_desc =
      static_cast<const cricket::MediaContentDescription*>(
          content_info.description);
  RTC_DCHECK(content_desc);
  bool rtcp_mux_enabled = content_info.type == cricket::MediaProtocolType::kSctp
                              ? true
                              : content_desc->rtcp_mux();

  return cricket::JsepTransportDescription(
      rtcp_mux_enabled, content_desc->cryptos(), encrypted_extension_ids,
      rtp_abs_sendtime_extn_id, transport_info.description);
}

bool JsepTransportController::ShouldUpdateBundleGroup(
    SdpType type,
    const cricket::SessionDescription* description) {
  if (config_.bundle_policy ==
      PeerConnectionInterface::kBundlePolicyMaxBundle) {
    return true;
  }

  if (type != SdpType::kAnswer) {
    return false;
  }

  RTC_DCHECK(local_desc_ && remote_desc_);
  const cricket::ContentGroup* local_bundle =
      local_desc_->GetGroupByName(cricket::GROUP_TYPE_BUNDLE);
  const cricket::ContentGroup* remote_bundle =
      remote_desc_->GetGroupByName(cricket::GROUP_TYPE_BUNDLE);
  return local_bundle && remote_bundle;
}

std::vector<int> JsepTransportController::GetEncryptedHeaderExtensionIds(
    const cricket::ContentInfo& content_info) {
  const cricket::MediaContentDescription* content_desc =
      static_cast<const cricket::MediaContentDescription*>(
          content_info.description);

  if (!config_.crypto_options.enable_encrypted_rtp_header_extensions) {
    return std::vector<int>();
  }

  std::vector<int> encrypted_header_extension_ids;
  for (auto extension : content_desc->rtp_header_extensions()) {
    if (!extension.encrypt) {
      continue;
    }
    auto it = std::find(encrypted_header_extension_ids.begin(),
                        encrypted_header_extension_ids.end(), extension.id);
    if (it == encrypted_header_extension_ids.end()) {
      encrypted_header_extension_ids.push_back(extension.id);
    }
  }
  return encrypted_header_extension_ids;
}

std::vector<int>
JsepTransportController::MergeEncryptedHeaderExtensionIdsForBundle(
    const cricket::SessionDescription* description) {
  RTC_DCHECK(description);
  RTC_DCHECK(bundle_group_);

  std::vector<int> merged_ids;
  // Union the encrypted header IDs in the group when bundle is enabled.
  for (const cricket::ContentInfo& content_info : description->contents()) {
    if (bundle_group_->HasContentName(content_info.name)) {
      std::vector<int> extension_ids =
          GetEncryptedHeaderExtensionIds(content_info);
      for (int id : extension_ids) {
        auto it = std::find(merged_ids.begin(), merged_ids.end(), id);
        if (it == merged_ids.end()) {
          merged_ids.push_back(id);
        }
      }
    }
  }
  return merged_ids;
}

int JsepTransportController::GetRtpAbsSendTimeHeaderExtensionId(
    const cricket::ContentInfo& content_info) {
  if (!config_.enable_external_auth) {
    return -1;
  }

  const cricket::MediaContentDescription* content_desc =
      static_cast<const cricket::MediaContentDescription*>(
          content_info.description);

  const webrtc::RtpExtension* send_time_extension =
      webrtc::RtpExtension::FindHeaderExtensionByUri(
          content_desc->rtp_header_extensions(),
          webrtc::RtpExtension::kAbsSendTimeUri);
  return send_time_extension ? send_time_extension->id : -1;
}

const cricket::JsepTransport2* JsepTransportController::GetJsepTransportForMid(
    const std::string& mid) const {
  auto it = mid_to_transport_.find(mid);
  return it == mid_to_transport_.end() ? nullptr : it->second;
}

cricket::JsepTransport2* JsepTransportController::GetJsepTransportForMid(
    const std::string& mid) {
  auto it = mid_to_transport_.find(mid);
  return it == mid_to_transport_.end() ? nullptr : it->second;
}

const cricket::JsepTransport2* JsepTransportController::GetJsepTransportByName(
    const std::string& transport_name) const {
  auto it = jsep_transports_by_name_.find(transport_name);
  return (it == jsep_transports_by_name_.end()) ? nullptr : it->second.get();
}

cricket::JsepTransport2* JsepTransportController::GetJsepTransportByName(
    const std::string& transport_name) {
  auto it = jsep_transports_by_name_.find(transport_name);
  return (it == jsep_transports_by_name_.end()) ? nullptr : it->second.get();
}

RTCError JsepTransportController::MaybeCreateJsepTransport(
    const cricket::ContentInfo& content_info) {
  RTC_DCHECK(network_thread_->IsCurrent());
  cricket::JsepTransport2* transport =
      GetJsepTransportByName(content_info.name);
  if (transport) {
    return RTCError::OK();
  }

  const cricket::MediaContentDescription* content_desc =
      static_cast<const cricket::MediaContentDescription*>(
          content_info.description);
  if (certificate_ && !content_desc->cryptos().empty()) {
    return RTCError(RTCErrorType::INVALID_PARAMETER,
                    "SDES and DTLS-SRTP cannot be enabled at the same time.");
  }

  std::unique_ptr<cricket::DtlsTransportInternal> rtp_dtls_transport =
      CreateDtlsTransport(content_info.name, /*rtcp =*/false);
  std::unique_ptr<cricket::DtlsTransportInternal> rtcp_dtls_transport;
  if (config_.rtcp_mux_policy !=
          PeerConnectionInterface::kRtcpMuxPolicyRequire &&
      content_info.type == cricket::MediaProtocolType::kRtp) {
    rtcp_dtls_transport =
        CreateDtlsTransport(content_info.name, /*rtcp =*/true);
  }

  std::unique_ptr<RtpTransport> unencrypted_rtp_transport;
  std::unique_ptr<SrtpTransport> sdes_transport;
  std::unique_ptr<DtlsSrtpTransport> dtls_srtp_transport;
  if (config_.disable_encryption) {
    unencrypted_rtp_transport = CreateUnencryptedRtpTransport(
        content_info.name, rtp_dtls_transport.get(), rtcp_dtls_transport.get());
  } else if (!content_desc->cryptos().empty()) {
    sdes_transport = CreateSdesTransport(
        content_info.name, rtp_dtls_transport.get(), rtcp_dtls_transport.get());
  } else {
    dtls_srtp_transport = CreateDtlsSrtpTransport(
        content_info.name, rtp_dtls_transport.get(), rtcp_dtls_transport.get());
  }

  std::unique_ptr<cricket::JsepTransport2> jsep_transport =
      rtc::MakeUnique<cricket::JsepTransport2>(
          content_info.name, certificate_, std::move(unencrypted_rtp_transport),
          std::move(sdes_transport), std::move(dtls_srtp_transport),
          std::move(rtp_dtls_transport), std::move(rtcp_dtls_transport));
  jsep_transport->SignalRtcpMuxActive.connect(
      this, &JsepTransportController::UpdateAggregateStates_n);
  SetTransportForMid(content_info.name, jsep_transport.get(),
                     content_info.type);

  jsep_transports_by_name_[content_info.name] = std::move(jsep_transport);
  UpdateAggregateStates_n();
  return RTCError::OK();
}

void JsepTransportController::MaybeDestroyJsepTransport(
    const std::string& mid) {
  auto jsep_transport = GetJsepTransportByName(mid);
  if (!jsep_transport) {
    return;
  }

  // Don't destroy the JsepTransport if there are still media sections referring
  // to it.
  for (const auto& kv : mid_to_transport_) {
    if (kv.second == jsep_transport) {
      return;
    }
  }
  jsep_transports_by_name_.erase(mid);
  UpdateAggregateStates_n();
}

void JsepTransportController::DestroyAllJsepTransports_n() {
  RTC_DCHECK(network_thread_->IsCurrent());
  jsep_transports_by_name_.clear();
}

void JsepTransportController::SetIceRole_n(cricket::IceRole ice_role) {
  RTC_DCHECK(network_thread_->IsCurrent());

  ice_role_ = ice_role;
  for (auto& dtls : GetDtlsTransports()) {
    dtls->ice_transport()->SetIceRole(ice_role_);
  }
}

cricket::IceRole JsepTransportController::DetermineIceRole(
    cricket::JsepTransport2* jsep_transport,
    const cricket::TransportInfo& transport_info,
    SdpType type,
    bool local) {
  cricket::IceRole ice_role = ice_role_;
  auto tdesc = transport_info.description;
  if (local) {
    // The initial offer side may use ICE Lite, in which case, per RFC5245
    // Section 5.1.1, the answer side should take the controlling role if it is
    // in the full ICE mode.
    //
    // When both sides use ICE Lite, the initial offer side must take the
    // controlling role, and this is the default logic implemented in
    // SetLocalDescription in JsepTransportController.
    if (jsep_transport->remote_description() &&
        jsep_transport->remote_description()->transport_desc.ice_mode ==
            cricket::ICEMODE_LITE &&
        ice_role_ == cricket::ICEROLE_CONTROLLED &&
        tdesc.ice_mode == cricket::ICEMODE_FULL) {
      ice_role = cricket::ICEROLE_CONTROLLING;
    }

    // Older versions of Chrome expect the ICE role to be re-determined when an
    // ICE restart occurs, and also don't perform conflict resolution correctly,
    // so for now we can't safely stop doing this, unless the application opts
    // in by setting |config_.redetermine_role_on_ice_restart_| to false. See:
    // https://bugs.chromium.org/p/chromium/issues/detail?id=628676
    // TODO(deadbeef): Remove this when these old versions of Chrome reach a low
    // enough population.
    if (config_.redetermine_role_on_ice_restart &&
        jsep_transport->local_description() &&
        cricket::IceCredentialsChanged(
            jsep_transport->local_description()->transport_desc.ice_ufrag,
            jsep_transport->local_description()->transport_desc.ice_pwd,
            tdesc.ice_ufrag, tdesc.ice_pwd) &&
        // Don't change the ICE role if the remote endpoint is ICE lite; we
        // should always be controlling in that case.
        (!jsep_transport->remote_description() ||
         jsep_transport->remote_description()->transport_desc.ice_mode !=
             cricket::ICEMODE_LITE)) {
      ice_role = (type == SdpType::kOffer) ? cricket::ICEROLE_CONTROLLING
                                           : cricket::ICEROLE_CONTROLLED;
    }
  } else {
    // If our role is cricket::ICEROLE_CONTROLLED and the remote endpoint
    // supports only ice_lite, this local endpoint should take the CONTROLLING
    // role.
    // TODO(deadbeef): This is a session-level attribute, so it really shouldn't
    // be in a TransportDescription in the first place...
    if (ice_role_ == cricket::ICEROLE_CONTROLLED &&
        tdesc.ice_mode == cricket::ICEMODE_LITE) {
      ice_role = cricket::ICEROLE_CONTROLLING;
    }

    // If we use ICE Lite and the remote endpoint uses the full implementation
    // of ICE, the local endpoint must take the controlled role, and the other
    // side must be the controlling role.
    if (jsep_transport->local_description() &&
        jsep_transport->local_description()->transport_desc.ice_mode ==
            cricket::ICEMODE_LITE &&
        ice_role_ == cricket::ICEROLE_CONTROLLING &&
        tdesc.ice_mode == cricket::ICEMODE_FULL) {
      ice_role = cricket::ICEROLE_CONTROLLED;
    }
  }

  return ice_role;
}

void JsepTransportController::OnTransportWritableState_n(
    rtc::PacketTransportInternal* transport) {
  RTC_DCHECK(network_thread_->IsCurrent());
  RTC_LOG(LS_INFO) << " Transport " << transport->transport_name()
                   << " writability changed to " << transport->writable()
                   << ".";
  UpdateAggregateStates_n();
}

void JsepTransportController::OnTransportReceivingState_n(
    rtc::PacketTransportInternal* transport) {
  RTC_DCHECK(network_thread_->IsCurrent());
  UpdateAggregateStates_n();
}

void JsepTransportController::OnTransportGatheringState_n(
    cricket::IceTransportInternal* transport) {
  RTC_DCHECK(network_thread_->IsCurrent());
  UpdateAggregateStates_n();
}

void JsepTransportController::OnTransportCandidateGathered_n(
    cricket::IceTransportInternal* transport,
    const cricket::Candidate& candidate) {
  RTC_DCHECK(network_thread_->IsCurrent());

  // We should never signal peer-reflexive candidates.
  if (candidate.type() == cricket::PRFLX_PORT_TYPE) {
    RTC_NOTREACHED();
    return;
  }
  std::vector<cricket::Candidate> candidates;
  candidates.push_back(candidate);
  CandidatesData* data =
      new CandidatesData(transport->transport_name(), candidates);
  signaling_thread_->Post(RTC_FROM_HERE, this, MSG_ICECANDIDATESGATHERED, data);
}

void JsepTransportController::OnTransportCandidatesRemoved_n(
    cricket::IceTransportInternal* transport,
    const cricket::Candidates& candidates) {
  invoker_.AsyncInvoke<void>(
      RTC_FROM_HERE, signaling_thread_,
      rtc::Bind(&JsepTransportController::OnTransportCandidatesRemoved, this,
                candidates));
}

void JsepTransportController::OnTransportCandidatesRemoved(
    const cricket::Candidates& candidates) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  SignalIceCandidatesRemoved(candidates);
}

void JsepTransportController::OnTransportRoleConflict_n(
    cricket::IceTransportInternal* transport) {
  RTC_DCHECK(network_thread_->IsCurrent());
  // Note: since the role conflict is handled entirely on the network thread,
  // we don't need to worry about role conflicts occurring on two ports at
  // once. The first one encountered should immediately reverse the role.
  cricket::IceRole reversed_role = (ice_role_ == cricket::ICEROLE_CONTROLLING)
                                       ? cricket::ICEROLE_CONTROLLED
                                       : cricket::ICEROLE_CONTROLLING;
  RTC_LOG(LS_INFO) << "Got role conflict; switching to "
                   << (reversed_role == cricket::ICEROLE_CONTROLLING
                           ? "controlling"
                           : "controlled")
                   << " role.";
  SetIceRole_n(reversed_role);
}

void JsepTransportController::OnTransportStateChanged_n(
    cricket::IceTransportInternal* transport) {
  RTC_DCHECK(network_thread_->IsCurrent());
  RTC_LOG(LS_INFO) << transport->transport_name() << " Transport "
                   << transport->component()
                   << " state changed. Check if state is complete.";
  UpdateAggregateStates_n();
}

void JsepTransportController::UpdateAggregateStates_n() {
  RTC_DCHECK(network_thread_->IsCurrent());

  auto dtls_transports = GetDtlsTransports();
  cricket::IceConnectionState new_connection_state =
      cricket::kIceConnectionConnecting;
  cricket::IceGatheringState new_gathering_state = cricket::kIceGatheringNew;
  bool any_failed = false;
  bool all_connected = !dtls_transports.empty();
  bool all_completed = !dtls_transports.empty();
  bool any_gathering = false;
  bool all_done_gathering = !dtls_transports.empty();
  for (const auto& dtls : dtls_transports) {
    any_failed = any_failed || dtls->ice_transport()->GetState() ==
                                   cricket::IceTransportState::STATE_FAILED;
    all_connected = all_connected && dtls->writable();
    all_completed =
        all_completed && dtls->writable() &&
        dtls->ice_transport()->GetState() ==
            cricket::IceTransportState::STATE_COMPLETED &&
        dtls->ice_transport()->GetIceRole() == cricket::ICEROLE_CONTROLLING &&
        dtls->ice_transport()->gathering_state() ==
            cricket::kIceGatheringComplete;
    any_gathering = any_gathering || dtls->ice_transport()->gathering_state() !=
                                         cricket::kIceGatheringNew;
    all_done_gathering =
        all_done_gathering && dtls->ice_transport()->gathering_state() ==
                                  cricket::kIceGatheringComplete;
  }
  if (any_failed) {
    new_connection_state = cricket::kIceConnectionFailed;
  } else if (all_completed) {
    new_connection_state = cricket::kIceConnectionCompleted;
  } else if (all_connected) {
    new_connection_state = cricket::kIceConnectionConnected;
  }
  if (ice_connection_state_ != new_connection_state) {
    ice_connection_state_ = new_connection_state;
    signaling_thread_->Post(
        RTC_FROM_HERE, this, MSG_ICECONNECTIONSTATE,
        new rtc::TypedMessageData<cricket::IceConnectionState>(
            new_connection_state));
  }

  if (all_done_gathering) {
    new_gathering_state = cricket::kIceGatheringComplete;
  } else if (any_gathering) {
    new_gathering_state = cricket::kIceGatheringGathering;
  }
  if (ice_gathering_state_ != new_gathering_state) {
    ice_gathering_state_ = new_gathering_state;
    signaling_thread_->Post(
        RTC_FROM_HERE, this, MSG_ICEGATHERINGSTATE,
        new rtc::TypedMessageData<cricket::IceGatheringState>(
            new_gathering_state));
  }
}

void JsepTransportController::OnDtlsHandshakeError(
    rtc::SSLHandshakeError error) {
  SignalDtlsHandshakeError(error);
}

}  // namespace webrtc
