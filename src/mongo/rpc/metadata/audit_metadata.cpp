/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/none.hpp>
#include <cmath>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/metadata/audit_client_attrs.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {
namespace rpc {
namespace {
const auto auditUserAttrsDecoration =
    OperationContext::declareDecoration<synchronized_value<boost::optional<AuditUserAttrs>>>();
}  // namespace

boost::optional<AuditUserAttrs> AuditUserAttrs::get(OperationContext* opCtx) {
    if (opCtx) {
        auto auditUserAttrsOptional = auditUserAttrsDecoration(opCtx).synchronize();
        if (auditUserAttrsOptional->has_value()) {
            return auditUserAttrsOptional->value();
        }
    }
    return boost::none;
}

void AuditUserAttrs::set(OperationContext* opCtx, AuditUserAttrs auditUserAttrs) {
    *auditUserAttrsDecoration(opCtx) = std::move(auditUserAttrs);
}

boost::optional<AuditMetadata> getImpersonatedUserMetadata(OperationContext* opCtx) {
    if (!opCtx) {
        return boost::none;
    }
    auto auditUserAttrs = AuditUserAttrs::get(opCtx);
    if (!auditUserAttrs) {
        return boost::none;
    }
    AuditMetadata metadata;
    metadata.setUser(auditUserAttrs->userName);
    metadata.setRoles(std::move(auditUserAttrs->roleNames));
    return metadata;
}

void setImpersonatedUserMetadata(OperationContext* opCtx,
                                 const boost::optional<AuditMetadata>& data) {
    if (!data) {
        // Reset AuditUserAttrs decoration to boost::none if data is absent.
        auditUserAttrsDecoration(opCtx) = boost::none;
        return;
    }
    auto userName = data->getUser();
    auto roleNames = data->getRoles();
    if (userName) {
        AuditUserAttrs::set(opCtx, AuditUserAttrs(*userName, std::move(roleNames)));
    }
}

void setAuditClientMetadata(OperationContext* opCtx, const boost::optional<AuditMetadata>& data) {
    // TODO SERVER-83990: remove
    if (!gFeatureFlagExposeClientIpInAuditLogs.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }
    // Do not set/reset client metadata if it was not sent from a mongos
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) ||
        opCtx->getClient()->isFromUserConnection()) {
        return;
    }

    auto client = opCtx->getClient();
    auto session = client->session();

    if (!data || !data->getClientMetadata()) {
        // Reset AuditClientAttrs decoration to default value if data is absent.
        AuditClientAttrs::reset(client);
        return;
    }

    auto local = session->local();
    const auto& hosts = data->getClientMetadata()->getHosts();
    auto remote = hosts[0];

    std::vector<HostAndPort> intermediates;
    for (size_t i = 1; i < hosts.size(); ++i) {
        intermediates.push_back(hosts[i]);
    }

    AuditClientAttrs::set(
        client, AuditClientAttrs(std::move(local), std::move(remote), std::move(intermediates)));
}

void setAuditMetadata(OperationContext* opCtx, const boost::optional<AuditMetadata>& data) {
    setImpersonatedUserMetadata(opCtx, data);
    setAuditClientMetadata(opCtx, data);
}

boost::optional<AuditMetadata> getAuditAttrsToAuditMetadata(OperationContext* opCtx) {
    // If we have no opCtx, which does appear to happen, don't do anything.
    if (!opCtx) {
        return {};
    }

    // Otherwise construct a metadata section from the list of authenticated users/roles
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    auto userName = authSession->getImpersonatedUserName();
    auto roleNames = authSession->getImpersonatedRoleNames();
    if (!userName && !roleNames.more()) {
        userName = authSession->getAuthenticatedUserName();
        roleNames = authSession->getAuthenticatedRoleNames();
    }

    // Add user/role information if available
    boost::optional<AuditMetadata> metadata;
    if (userName || roleNames.more()) {
        metadata = AuditMetadata();
        if (userName) {
            metadata->setUser(userName.value());
        }
        metadata->setRoles(roleNameIteratorToContainer<std::vector<RoleName>>(roleNames));
    } else {
        return metadata;
    }

    // TODO SERVER-83990: remove
    if (gFeatureFlagExposeClientIpInAuditLogs.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        auto clientAttrs = AuditClientAttrs::get(opCtx->getClient());
        if (clientAttrs) {
            auto clientMetadata = clientAttrs->generateClientMetadataObj();
            if (!metadata) {
                metadata = AuditMetadata();
            }
            metadata->setClientMetadata(std::move(clientMetadata));
        }
    }

    return metadata;
}

void writeAuditMetadata(OperationContext* opCtx, BSONObjBuilder* out) {
    if (auto meta = getAuditAttrsToAuditMetadata(opCtx)) {
        BSONObjBuilder section(out->subobjStart(kImpersonationMetadataSectionName));
        meta->serialize(&section);
    }
}

std::size_t estimateAuditMetadataSize(
    const boost::optional<UserName>& userName,
    RoleNameIterator roleNames,
    const boost::optional<ImpersonatedClientMetadata>& clientMetadata = boost::none) {
    // If there are no users/roles being impersonated just exit
    if (!userName && !roleNames.more()) {
        return 0;
    }

    std::size_t ret = 4 +                                   // BSONObj size
        1 + kImpersonationMetadataSectionName.size() + 1 +  // "$audit" sub-object key
        4;                                                  // $audit object length

    if (userName) {
        // BSONObjType + "impersonatedUser" + NULL + UserName object.
        ret += 1 + AuditMetadata::kUserFieldName.size() + 1 + userName->getBSONObjSize();
    }

    // BSONArrayType + "impersonatedRoles" + NULL + BSONArray Length
    ret += 1 + AuditMetadata::kRolesFieldName.size() + 1 + 4;
    for (std::size_t i = 0; roleNames.more(); roleNames.next(), ++i) {
        // BSONType::Object + strlen(indexId) + NULL byte
        // to_string(i).size() will be log10(i) plus some rounding and fuzzing.
        // Increment prior to taking the log so that we never take log10(0) which is NAN.
        // This estimates one extra byte every time we reach (i % 10) == 9.
        ret += 1 + static_cast<std::size_t>(1.1 + log10(i + 1)) + 1;
        ret += roleNames.get().getBSONObjSize();
    }

    // EOD terminator for impersonatedRoles array
    ret += 1;

    // TODO SERVER-83990: remove
    if (gFeatureFlagExposeClientIpInAuditLogs.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        clientMetadata) {
        // BSONObjType + "impersonatedClient" + NULL + Object start
        ret += 1 + AuditMetadata::kClientMetadataFieldName.size() + 1 + 4;

        // BSONArrayType + "hosts" + NULL + Array length
        ret += 1 + ImpersonatedClientMetadata::kHostsFieldName.size() + 1 + 4;

        const auto& hosts = clientMetadata->getHosts();
        for (std::size_t i = 0; i < hosts.size(); ++i) {
            // BSONType::String + strlen(indexId) + NULL byte
            ret += 1 + static_cast<std::size_t>(1.1 + log10(i + 1)) + 1;
            // String size + string content + NULL terminator
            ret += 4 + hosts[i].toString().size() + 1;
        }

        // EOD terminators for: hosts array and impersonatedClient object
        ret += 1 + 1;
    }

    // EOD terminators for: $audit object and metadata object
    ret += 1 + 1;

    return ret;
}

std::size_t estimateAuditMetadataSize(OperationContext* opCtx) {
    if (!opCtx) {
        return 0;
    }

    // Otherwise construct a metadata section from the list of authenticated users/roles
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    auto userName = authSession->getImpersonatedUserName();
    auto roleNames = authSession->getImpersonatedRoleNames();
    if (!userName && !roleNames.more()) {
        userName = authSession->getAuthenticatedUserName();
        roleNames = authSession->getAuthenticatedRoleNames();
    }
    auto clientAttrs = AuditClientAttrs::get(opCtx->getClient());
    if (clientAttrs) {
        const auto& clientMetadata = clientAttrs->generateClientMetadataObj();
        return estimateAuditMetadataSize(userName, roleNames, clientMetadata);
    }

    return estimateAuditMetadataSize(userName, roleNames);
}

std::size_t estimateAuditMetadataSize(const AuditMetadata& md) {
    return estimateAuditMetadataSize(
        md.getUser(), makeRoleNameIteratorForContainer(md.getRoles()), md.getClientMetadata());
}

}  // namespace rpc
}  // namespace mongo
