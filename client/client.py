#!/usr/bin/python

import struct
import json

# Define the corrected format string to match the struct event_schema
EVENT_FORMAT = "IIIIQIQ6Q"

def unpack_event(binary_data) -> dict:
    """Unpack binary data into a dictionary"""
    event_tuple = struct.unpack(EVENT_FORMAT, binary_data)
    event_dict = {
        "uid": event_tuple[0],
        "pid": event_tuple[1],
        "ppid": event_tuple[2],
        "tid": event_tuple[3],
        "timestamp": event_tuple[4],
        "syscall_nr": event_tuple[5],
        "syscall_args": list(event_tuple[6:12]),
        "syscall_ret": event_tuple[12]
    }
    return event_dict


def main() -> None:
    """Read binary data from /dev/scc and print as JSON."""
    with open('/dev/scc', 'rb') as scc_file:
        try:
            while True:
                binary_data = scc_file.read(struct.calcsize(EVENT_FORMAT))
                if not binary_data:
                    break  # End of file
                event_dict = unpack_event(binary_data)
                event_json = json.dumps(event_dict, indent=4)
                print(event_json)
        except KeyboardInterrupt:
            pass
        except IOError:  # Handle a broken pipe
            pass


if __name__ == '__main__':
    main()
