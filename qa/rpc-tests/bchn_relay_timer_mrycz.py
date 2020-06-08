#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Cash Node developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Credit: mtrycz and Calin Culianu

import sys
import threading
import time
import json
import csv
import math

from random import randrange, seed
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes,
    connect_nodes_bi,
    disconnect_nodes,
    assert_raises_rpc_error,
    try_rpc
)

txs = dict()
# It's nice to get reproducible results
# so we seed the randomness source with a fixed value
# This should yield the same topology on different runs,
# all other things being equal
seed(1) 

class TransactionRelayTimer(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self._zmq_address_prefix = "tcp://127.0.0.1"
        self._zmq_ports = []
        self.num_nodes = 25
        self.extra_args = []
        base_port = randrange(28900, 40000)  # we hope and pray this is unique!
        for i in range(self.num_nodes):
            # setup zmq notifier for the pubhashtx message on localhost port base_port + node_num
            port = base_port + i
            self.extra_args.append([
                "-zmqpubhashtx={}".format('{}:{}'.format(self._zmq_address_prefix, port)),
                "-maxmempool=81",
                "-net.txAdmissionThreads=1",
                "-net.msgHandlerThreads=1"
            ])
            self._zmq_ports.append(port)

    def skip_test_if_missing_module(self):
        self.skip_if_no_py3_zmq()
        self.skip_if_no_bitcoind_zmq()
        self.skip_if_no_wallet()
        try:
            import asyncio
        except ImportError:
            raise SkipTest("module asyncio is required for this test")
        if sys.version_info < (3,6,5):
            raise SkipTest("Python >= 3.6.5 is required for this test")

    def disconnect_all(self):
        for i in range(3):
            disconnect_nodes(self.nodes[i], i+1)
            disconnect_nodes(self.nodes[i+1], i)

    def setup_grid_topology(self):
        # This topology works only for square grids, self.num_nodes = n^2
        # Side is the number of nodes on each side of the grid
        n = int(math.sqrt(self.num_nodes))
        assert n*n == self.num_nodes
        self.disconnect_all()

        for i in range(self.num_nodes-1):
            if (i % n != n-1):
                connect_nodes_bi(self.nodes[i], self.nodes[i+1])
            if (i >= n):
                connect_nodes_bi(self.nodes[i], self.nodes[i-n])

    def setup_lattice_topology(self):
        # This is like a grid but the last node in a row is connected to the first etc
        # Think of grid as of a 2d square, and of this as of a globe or something
        self.setup_grid_topology()
        n = int(math.sqrt(self.num_nodes))
        for i in range(self.num_nodes-1):
            if (i % n == 0):
                connect_nodes_bi(self.nodes[i], self.nodes[i+n-1])
            if (i < n):
                connect_nodes_bi(self.nodes[i], self.nodes[n*n-n+i])

    def setup_random_topology(self):
        # n will be the number of outbound connection, sqrt of number of nodes
        n = int(math.sqrt(self.num_nodes))

        def get_random_candidate(from_node):
            candidate = None
            while candidate == None:
                try_this = randrange(self.num_nodes)
                if try_this != from_node and [from_node, try_this] not in existing_edges:
                    candidate = try_this
            return candidate

        def get_connected_to(from_node):
            to_nodes = []
            for edge in existing_edges:
                if (edge[0] == from_node):
                    to_nodes.append(edge[1])
            return to_nodes

        # If all nodes are reachable from a source node then it is fully connected
        def check_node_fully_connected(origin):
            visited = set()
            to_visit = set()

            to_visit.add(origin)

            while len(to_visit) != 0:
                current_node = to_visit.pop()
                visited.add(current_node)
                for to_node in get_connected_to(current_node):
                    if to_node not in visited:
                        to_visit.add(to_node)
            return len(visited) == self.num_nodes


        # If all nodes are fully connected then the network is fully connected
        def check_network_fully_connected():
            for i in range(self.num_nodes):
                if not check_node_fully_connected(i):
                    return False
            return True

        fully_connected = False # if after a full iteration the net not fully connected, simply start over until it is
        iterations = 0
        while not fully_connected:
            existing_edges = []
            self.disconnect_all()
            for from_node in range(self.num_nodes):
                for j in range(n):
                    to_node = get_random_candidate(from_node)
                    connect_nodes(self.nodes[from_node], to_node)
                    existing_edges.append([from_node, to_node])
            iterations = iterations+1
            fully_connected = check_network_fully_connected()

        print("Costructed random network in {} iterations".format(iterations))
        print(existing_edges)

    # Will send a transaction to the mempool of the indicated node
    # or to a random node if not provided
    # and poll the last node until it is recieved
    # Returns the starting node and the
    def relay_transaction(self, node):
        assert self.num_nodes > 1
        self.nodes[node].sendtoaddress(self.nodes[1].getnewaddress(), "0.00001", "", "", False)

    def run_test(self):
        import asyncio
        import zmq
        import zmq.asyncio

        # self.setup_grid_topology()
        # self.setup_lattice_topology()
        self.setup_random_topology()

        stop_flag = False
        sem = threading.Semaphore(0)  # will use this to synchronize and have master wait for slaves to be ready

        node_txid_times_raw, node_txid_times_hex = {}, {}  # this will be a dict of node_id -> dict of txid -> timestamp
        for i in range(self.num_nodes):
            node_txid_times_raw[i] = dict()
            node_txid_times_hex[i] = dict()

        # threading code credited to cculianu
        def slave_thread():
            """ This runs in a thread, it will write to the node_txid_times_* dicts as it receives messages from zmq """
            assert threading.current_thread() is not threading.main_thread()
            print("Slave thread started")
            loop = asyncio.new_event_loop()  # create an event loop for this thread
            socks = []
            async def runner():
                print("started runner")
                tasks = set()

                async def task(num: int):
                    ''' One of these runs as an async task on the thread event loop, per node '''
                    print("ZMQ task started for node {}".format(num))
                    ctx = zmq.asyncio.Context()
                    sock = ctx.socket(zmq.SUB)
                    socks.append(sock)
                    url = '{}:{}'.format(self._zmq_address_prefix, self._zmq_ports[num])
                    sock.connect(url)
                    sock.setsockopt(zmq.SUBSCRIBE, b'hashtx')
                    sem.release()  # indicate ready
                    while not stop_flag:
                        if await sock.poll(timeout=100):  # timeout here is in milliseconds for poll
                            now = time.time()  # mark the time now because if poll returned true, the msg was indeed ready
                            # we got a message, grab it and log its time
                            msg = await sock.recv_multipart()  # waits for msg to be ready -- it should return immediately
                            # print(msg)
                            name, hashtx = msg  # messages are [name, txid_bytes, some_extra_stuff]
                            assert name == b'hashtx' and len(hashtx) == 32
                            # save timestamps to dicts
                            node_txid_times_raw[num][hashtx] = now  # save raw bytes
                            txid = hashtx.hex()
                            node_txid_times_hex[num][txid] = now  # save as pretty hex (reversed)

                            if txid not in txs:
                                txs[txid] = [num, now]

                            if num == self.num_nodes-1 and len(txs[txid]) == 2:
                                txs[txid].append(now)
                                txs[txid].append(now - txs[txid][1])
                                print("{}: {}".format(len(txs), txs[txid]))
                # /task
                for i, _ in enumerate(self.nodes):
                    tasks.add(task(i))
                # The below line starts all the above tasks and blocks until they all return. The tasks all run in this
                # thread using this thread's event loop. The "await" points above are all points at which the above
                # tasks may yield to the event loop. All other non-await calls block this thread.
                await asyncio.wait(tasks, return_when=asyncio.ALL_COMPLETED)
                for sock in socks:
                    sock.close()
                print("Closed {} zmq sockets".format(len(socks)))
            # / runner
            loop.run_until_complete(runner())
            print("Slave thread exiting")
            loop.close()
        # /slave_thread

        print("Maturing some coins on all nodes for easy spending, please be patient")
        for node in self.nodes:
            node.generate(21)
            self.sync_all()

        self.nodes[0].generate(100)
        self.sync_all()

        # create the slave thread that will have the asyncio event loop for zmq notifications
        t = threading.Thread(target=slave_thread, daemon=True)
        t.start()

        num_ready = 0
        while num_ready < self.num_nodes:
            if not sem.acquire(timeout=5.0):
                raise RuntimeError("Timeout waiting for slave thread")
            num_ready += 1

        # generate a whole lot of transactions at (kinda) random intervals
        # totaling num_nodes*100 transactions over X seconds
        for i in range(self.num_nodes * 100):
            # send a random tx. Our slave_thread will receive notifications and mark the
            # timestamps of the tx's it receives.
            node = randrange(self.num_nodes - 1)
            try:
                self.relay_transaction(node)
            except Exception as e:
                print("Error on relay txs: %s" % (str(e)));
            # sleep for some milliseconds
            time.sleep(randrange(300, 350) / 1000)

        self.sync_all()  # wait for them to settle. THis may take a while...
        # print("Times: {}".format(node_txid_times_hex))
        # print("Num Times: {}".format(sum(len(x) for _, x in node_txid_times_hex.copy().items())))

        # The below stops the slave task and waits 5 seconds for it to exit gracefully
        stop_flag = True
        t.join(timeout=5)
        assert not t.is_alive()  # raise if the slave thread failed to exit as that indicates some programming error.

        # print(txs)
        # Save the arrival times in a file
        print("Writing times of arrival at n-th node to arrivalatlastnode.csv")
        with open('arrivalatlastnode.csv', 'w') as f:
            w = csv.writer(f)
            for txid in txs.keys():
                w.writerow(txs[txid])

        # txs contains the delay until a transaction reaches the last node (nodes[num_nodes-1])
        # We'll also calculate the time it takes from each transaction to travel from origin to the
        # every node
        # The former gets us time of propagation at a certain node, the latter accross the whole network
        # propagation_times is a simple dict of arrays: {"txid" : [first_seen, last_seen, delay]}
        propagation_times = dict()

        # We have a dict for time of arrival at every node, so we can construct the first two 
        # entries from that
        for i in range(self.num_nodes):
            for txid in node_txid_times_hex[i].keys():
                
                if txid not in propagation_times:
                    propagation_times[txid] = []
                    propagation_times[txid].append(node_txid_times_hex[i][txid]) # append twice because
                    propagation_times[txid].append(node_txid_times_hex[i][txid]) # first_seen time is also last_seen time for now
                else:
                    if propagation_times[txid][0] > node_txid_times_hex[i][txid]:
                        propagation_times[txid][0] = node_txid_times_hex[i][txid]
                    if propagation_times[txid][1] < node_txid_times_hex[i][txid]:
                        propagation_times[txid][1] = node_txid_times_hex[i][txid]

        # The third entry is now simple, just subtract the two times
        for txid in propagation_times.keys():
            propagation_times[txid].append(propagation_times[txid][1] - propagation_times[txid][0])

        # Save the result in a file
        print("Writing times of propagation accross the network to propagation.csv")
        with open('propagation.csv', 'w') as f:  # Just use 'w' mode in 3.x
            w = csv.writer(f)
            for txid in propagation_times.keys():
                w.writerow(propagation_times[txid])

if __name__ == '__main__':
    TransactionRelayTimer().main()
