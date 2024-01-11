# Replica migration test.
# Check that orphaned masters are joined by replicas of masters having
# multiple replicas attached, according to the migration barrier settings.

# Create a cluster with 5 master and 10 slaves, so that we have 2
# slaves for each master.
start_cluster 5 10 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Each master should have two replicas attached" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        if {$id < 5} {
            wait_for_condition 1000 50 {
                [llength [lindex [R $id role] 2]] == 2
            } else {
                fail "Master #$id does not have 2 slaves as expected"
            }
        }
    }
}

set paused_pid5 [srv -5 pid]
set paused_pid10 [srv -10 pid]
set paused_pid6 [srv -6 pid]
set paused_pid11 [srv -11 pid]
test "Killing all the slaves of master #0 and #1" {
    pause_process $paused_pid5
    pause_process $paused_pid10
    pause_process $paused_pid6
    pause_process $paused_pid11
}

for {set id 0} {$id < [llength $::servers]} {incr id} {
    if {$id < 5} {
        test "Master #$id should have at least one replica" {
            wait_for_condition 1000 50 {
                [llength [lindex [R $id role] 2]] >= 1
            } else {
                fail "Master #$id has no replicas"
            }
        }
    }
}

} ;# start_cluster

# Now test the migration to a master which used to be a slave, after
# a failover.

# Create a cluster with 5 master and 10 slaves, so that we have 2
# slaves for each master.
start_cluster 5 10 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

set paused_pid7 [srv -7 pid]
test "Kill slave #7 of master #2. Only slave left is #12 now" {
    pause_process $paused_pid7
}

set current_epoch [CI 1 cluster_current_epoch]

set paused_pid2 [srv -2 pid]
test "Killing master node #2, #12 should failover" {
    pause_process $paused_pid2
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
}

test "Cluster should eventually be up again" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused $paused_pid7]} continue
        if {[process_is_paused $paused_pid2]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "ok"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

test "Instance 12 is now a master without slaves" {
    assert {[s -12 role] eq {master}}
}

# The remaining instance is now without slaves. Some other slave
# should migrate to it.

test "Master #12 should get at least one migrated replica" {
    wait_for_condition 1000 50 {
        [llength [lindex [R 12 role] 2]] >= 1
    } else {
        fail "Master #12 has no replicas"
    }
}

}
