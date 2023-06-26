# Primitive tests on cluster-enabled redis using redis-cli

source tests/support/cli.tcl

# make sure the test infra won't use SELECT
set old_singledb $::singledb
set ::singledb 1

# cluster creation is complicated with TLS, and the current tests don't really need that coverage
tags {tls:skip external:skip cluster} {

# start three servers
set base_conf [list cluster-enabled yes cluster-node-timeout 1000]
start_multiple_servers 6 [list overrides $base_conf] {

    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]
    set node3_pid [srv -2 pid]
    set node6_pid [srv -5 pid]
    set node3_rd [redis_deferring_client -2]

    test {Create 3 node cluster} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port] \
                           127.0.0.1:[srv -3 port] \
                           127.0.0.1:[srv -4 port] \
                           127.0.0.1:[srv -5 port] \
                           --cluster-replicas 1

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

    test "Run blocking command on cluster node3" {
        # key9184688 is mapped to slot 10923 (first slot of node 3)
        $node3_rd brpop key9184688 0
        $node3_rd flush

        wait_for_condition 50 100 {
            [s -2 blocked_clients] eq {1}
        } else {
            fail "Client not blocked"
        }
    }

    test "test failover" {
        set node3_rd_tmp [redis_deferring_client -2]

        $node3_rd_tmp ping
        puts [$node3_rd_tmp read]

        $node3_rd_tmp debug pause-beforesleep 1
        $node3_rd_tmp lpush key9184688 1

        set node6_rd [redis_deferring_client -5]
        $node6_rd cluster failover force
        puts [$node6_rd read]


        set node7_rd [redis_deferring_client -3]
        $node7_rd cluster failover force
        puts [$node7_rd read]


        set node8_rd [redis_deferring_client -4]
        $node8_rd cluster failover force
        puts [$node8_rd read]

        after 25000


        $node3_rd_tmp debug pause-beforesleep 0
        puts [$node3_rd_tmp read]

        puts "11111111111"
        set res [$node3_rd read]
        puts $res
    }

} ;# stop servers

}
set ::singledb $old_singledb
