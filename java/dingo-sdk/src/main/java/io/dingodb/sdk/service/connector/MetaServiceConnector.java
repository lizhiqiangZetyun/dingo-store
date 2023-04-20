/*
 * Copyright 2021 DataCanvas
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package io.dingodb.sdk.service.connector;

import io.dingodb.common.Common;
import io.dingodb.coordinator.Coordinator;
import io.dingodb.coordinator.CoordinatorServiceGrpc;
import io.dingodb.meta.MetaServiceGrpc;
import io.dingodb.sdk.common.Location;
import io.dingodb.sdk.common.utils.Optional;
import io.grpc.ManagedChannel;
import lombok.extern.slf4j.Slf4j;

import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.stream.Collectors;

@Slf4j
public class MetaServiceConnector extends ServiceConnector<MetaServiceGrpc.MetaServiceBlockingStub> {

    private final static Coordinator.GetCoordinatorMapRequest getCoordinatorMapRequest =
        Coordinator.GetCoordinatorMapRequest.newBuilder().setClusterId(0).build();

    public MetaServiceConnector(List<Location> locations) {
        super(new HashSet<>(locations));
    }

    public static MetaServiceConnector getMetaServiceConnector(String servers) {
        return Optional.ofNullable(servers.split(","))
            .map(Arrays::stream)
            .map(ss -> ss
                .map(s -> s.split(":"))
                .map(__ -> new Location(__[0], Integer.parseInt(__[1])))
                .collect(Collectors.toList()))
            .map(MetaServiceConnector::new)
            .orElseThrow("Create meta service connector error.");
    }

    @Override
    public ManagedChannel transformToLeaderChannel(ManagedChannel channel) {
        Common.Location leaderLocation = CoordinatorServiceGrpc.newBlockingStub(channel)
            .getCoordinatorMap(getCoordinatorMapRequest)
            .getLeaderLocation();
        if (!leaderLocation.getHost().isEmpty()) {
            if (!channel.isShutdown()) {
                channel.shutdown();
            }
            return newChannel(leaderLocation.getHost(), leaderLocation.getPort());
        }
        return null;
    }

    @Override
    public MetaServiceGrpc.MetaServiceBlockingStub newStub(ManagedChannel channel) {
        return MetaServiceGrpc.newBlockingStub(channel);
    }

}