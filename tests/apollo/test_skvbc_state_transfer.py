# Concord
#
# Copyright (c) 2021 VMware, Inc. All Rights Reserved.
#
# This product is licensed to you under the Apache 2.0 license (the "License").
# You may not use this product except in compliance with the Apache 2.0 License.
#
# This product may include a number of subcomponents with separate copyright
# notices and license terms. Your use of these subcomponents is subject to the
# terms and conditions of the subcomponent's license, as noted in the LICENSE
# file.
import os.path
import random
import unittest
from os import environ
from util import bft_network_traffic_control as ntc
import trio

from util import skvbc as kvbc
from util.bft import with_trio, with_bft_network, KEY_FILE_PREFIX
from util import eliot_logging as log

def start_replica_cmd(builddir, replica_id):
    """
    Return a command that starts an skvbc replica when passed to
    subprocess.Popen.
    Note each arguments is an element in a list.
    """
    statusTimerMilli = "500"
    viewChangeTimeoutMilli = "10000"
    path = os.path.join(builddir, "tests", "simpleKVBC",
                        "TesterReplica", "skvbc_replica")
    return [path,
            "-k", KEY_FILE_PREFIX,
            "-i", str(replica_id),
            "-s", statusTimerMilli,
            "-v", viewChangeTimeoutMilli,
            "-e", str(True)
            ]


class SkvbcStateTransferTest(unittest.TestCase):

    __test__ = False  # so that PyTest ignores this test scenario

    @with_trio
    @with_bft_network(start_replica_cmd, rotate_keys=True)
    async def test_state_transfer(self, bft_network,exchange_keys=True):
        """
        Test that state transfer starts and completes.
        Stop one node, add a bunch of data to the rest of the cluster, restart
        the node and verify state transfer works as expected. We should be able
        to stop a different set of f nodes after state transfer completes and
        still operate correctly.
        """
        skvbc = kvbc.SimpleKVBCProtocol(bft_network)

        stale_node = random.choice(
            bft_network.all_replicas(without={0}))

        await skvbc.prime_for_state_transfer(
            stale_nodes={stale_node},
            checkpoints_num=3, # key-exchange channges the last executed seqnum
            persistency_enabled=False
        )
        bft_network.start_replica(stale_node)
        await bft_network.wait_for_state_transfer_to_start()
        await bft_network.wait_for_state_transfer_to_stop(0, stale_node)
        await skvbc.assert_successful_put_get()
        await bft_network.force_quorum_including_replica(stale_node)
        await skvbc.assert_successful_put_get()

    @with_trio
    @with_bft_network(start_replica_cmd, rotate_keys=True)
    async def test_state_transfer_with_multiple_clients(self, bft_network,exchange_keys=True):
        """
        Test that state transfer starts and completes.
        Stop one node, add a bunch of data to the rest of the cluster using
        multiple clients restart the node and verify state transfer works as
        expected. We should be able to stop a different set of f nodes after
        state transfer completes and still operate correctly.
        """
        skvbc = kvbc.SimpleKVBCProtocol(bft_network)

        stale_node = random.choice(
            bft_network.all_replicas(without={0}))

        await skvbc.start_replicas_and_write_with_multiple_clients(
            stale_nodes={stale_node},
            write_run_duration=30,
            persistency_enabled=False
        )
        bft_network.start_replica(stale_node)
        await bft_network.wait_for_state_transfer_to_start()
        await bft_network.wait_for_state_transfer_to_stop(0, stale_node)
        await skvbc.assert_successful_put_get()
        await bft_network.force_quorum_including_replica(stale_node)
        await skvbc.assert_successful_put_get()
    
    @unittest.skipIf(environ.get('BUILD_COMM_TCP_TLS', "").lower() == "true", "Unstable on CI (TCP/TLS only)")
    @with_trio
    @with_bft_network(start_replica_cmd,
                    selected_configs=lambda n, f, c: f >= 2,
                    rotate_keys=True)
    async def test_state_transfer_with_internal_cycle(self, bft_network, exchange_keys=True):
        """
        state transfer starts and destination replica(fetcher) is 
        blocked during getMissingBlocks state, a bunch of data is added 
        to the rest of the cluster  using multiple clients. Then destination 
        replica is unblocked and state transfer resume . After one cycle 
        of state transfer is done replica goes into antoher internal(
        from getReserved page state to getCheckpointSummary State) 
        cycle to fetch remaining blocks which were added during first cycle.
        """
        
        skvbc = kvbc.SimpleKVBCProtocol(bft_network)
        stale_node = random.choice(
            bft_network.all_replicas(without={0}))

        await skvbc.start_replicas_and_write_with_multiple_clients(
            stale_nodes={stale_node},
            write_run_duration=30,
            persistency_enabled=False
        )
        
        with ntc.NetworkTrafficControl() as tc:
            #delay added to loopback interface to capture the stale replica in GettingMissingBlocks state
            tc.put_loop_back_interface_delay(200)
            bft_network.start_replica(stale_node)
            with trio.fail_after(30):
                while(True):
                    fetchingState = await bft_network.get_metric(stale_node, bft_network,
                                                             "Statuses", "fetching_state", "bc_state_transfer")
                    if(fetchingState == "GettingMissingBlocks"):
                        break
                    await trio.sleep(0.1)
        await skvbc.send_concurrent_ops_with_isolated_replica(isolated_replica={stale_node}, run_duration=30)
        await bft_network.wait_for_state_transfer_to_stop(0, stale_node)
        await bft_network.force_quorum_including_replica(stale_node)
        await skvbc.assert_successful_put_get()
