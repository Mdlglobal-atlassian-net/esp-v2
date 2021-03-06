// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include "src/envoy/http/backend_auth/config_parser_impl.h"
namespace espv2 {
namespace envoy {
namespace http_filters {
namespace backend_auth {

using ::google::api::envoy::http::backend_auth::FilterConfig;
using ::google::api::envoy::http::common::AccessToken;
using token::GetTokenFunc;
using token::TokenSubscriber;
using token::TokenType;
using token::UpdateTokenCallback;

// TODO(kyuc): add unit tests for all possible backend rule configs.

AudienceContext::AudienceContext(
    const ::google::api::envoy::http::backend_auth::BackendAuthRule&
        proto_config,
    Envoy::Server::Configuration::FactoryContext& context,
    const FilterConfig& filter_config,
    const token::TokenSubscriberFactory& token_subscriber_factory,
    GetTokenFunc access_token_fn)
    : tls_(context.threadLocal().allocateSlot()) {
  tls_->set([](Envoy::Event::Dispatcher&)
                -> Envoy::ThreadLocal::ThreadLocalObjectSharedPtr {
    return std::make_shared<TokenCache>();
  });

  UpdateTokenCallback callback = [this](absl::string_view token) {
    TokenSharedPtr new_token = std::make_shared<std::string>(token);
    tls_->runOnAllThreads([this, new_token]() {
      tls_->getTyped<TokenCache>().token_ = new_token;
    });
  };

  switch (filter_config.id_token_info_case()) {
    case FilterConfig::kIamToken: {
      const std::string& uri = filter_config.iam_token().iam_uri().uri();
      const std::string& cluster =
          filter_config.iam_token().iam_uri().cluster();
      const std::string real_uri =
          absl::StrCat(uri, "?audience=", proto_config.jwt_audience());
      const ::google::protobuf::RepeatedPtrField<std::string>& delegates =
          filter_config.iam_token().delegates();
      iam_token_sub_ptr_ = token_subscriber_factory.createIamTokenSubscriber(
          TokenType::IdentityToken, cluster, real_uri, callback, delegates,
          ::google::protobuf::RepeatedPtrField<std::string>(), access_token_fn);
    }
      return;
    case FilterConfig::kImdsToken: {
      const std::string& uri = filter_config.imds_token().uri();
      const std::string& cluster = filter_config.imds_token().cluster();
      const std::string real_uri = absl::StrCat(
          uri, "?format=standard&audience=", proto_config.jwt_audience());

      imds_token_sub_ptr_ = token_subscriber_factory.createImdsTokenSubscriber(
          TokenType::IdentityToken, cluster, real_uri, callback);
    }
      return;
    default:
      return;
  }
}

FilterConfigParserImpl::FilterConfigParserImpl(
    const FilterConfig& config,
    Envoy::Server::Configuration::FactoryContext& context,
    const token::TokenSubscriberFactory& token_subscriber_factory) {
  // Subscribe access token for fetching id token from iam when IdTokenFromIam
  // is set.
  if (config.id_token_info_case() == FilterConfig::kIamToken) {
    // TODO(taoxuy): support getting access token from service account file.
    switch (config.iam_token().access_token().token_type_case()) {
      case AccessToken::kRemoteToken: {
        const std::string& cluster =
            config.iam_token().access_token().remote_token().cluster();
        const std::string& uri =
            config.iam_token().access_token().remote_token().uri();
        access_token_sub_ptr_ =
            token_subscriber_factory.createImdsTokenSubscriber(
                TokenType::AccessToken, cluster, uri,
                [this](absl::string_view access_token) {
                  access_token_ = std::string(access_token);
                });
        break;
      }
      default: {
        ENVOY_LOG(error,
                  "Not support getting access token by service account file");
        return;
      }
    }
  }

  for (const auto& rule : config.rules()) {
    operation_map_[rule.operation()] = rule.jwt_audience();
    auto it = audience_map_.find(rule.jwt_audience());
    if (it == audience_map_.end()) {
      audience_map_[rule.jwt_audience()] = AudienceContextPtr(
          new AudienceContext(rule, context, config, token_subscriber_factory,
                              [this]() { return access_token_; }));
    }
  }
}
}  // namespace backend_auth
}  // namespace http_filters
}  // namespace envoy
}  // namespace espv2
