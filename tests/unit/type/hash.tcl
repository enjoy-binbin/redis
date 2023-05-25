start_server {tags {"hash"}} {
    set commands [r command info]
    set string_commands {}
    set stream_commands {}

    foreach {command} $commands {
        set categories [lindex $command 6]
        foreach {category} $categories {
            if {$category == "@string"} {
                lappend string_commands $command
            } elseif {$category == "@stream"} {
                lappend stream_commands $command
            }
        }

        set sub_commands [lindex $command 9]
        foreach {sub_command} $sub_commands {
            set categories [lindex $sub_command 6]
            foreach {category} $categories {
                if {$category == "@string"} {
                    lappend string_commands $sub_command
                } elseif {$category == "@stream"} {
                    lappend stream_commands $sub_command
                }
            }
        }
    }
    foreach {command} $stream_commands {
        puts [lindex $command 0]
    }
}
