#!/usr/bin/python

import struct
import json

def main():
    # Define the format string for unpacking the binary data
    event_format = "<IIIIQIQ6QQ"  # Use little-endian format for struct fields

    # Open the device file for reading
    with open('/dev/scc', 'rb') as file:
        while True:
            try:
                # Read a slot of data
                slot_data = file.read(struct.calcsize(event_format))

                if not slot_data:
                    break  # Exit loop if there's no more data

                # Unpack the binary data into variables
                uid, pid, ppid, tid, timestamp, syscall_nr, *syscall_args, syscall_ret = struct.unpack(event_format, slot_data)

                # Create a dictionary to store the event data
                event_data = {
                    "uid": uid,
                    "pid": pid,
                    "ppid": ppid,
                    "tid": tid,
                    "timestamp": timestamp,
                    "syscall_nr": syscall_nr,
                    "syscall_args": syscall_args,
                    "syscall_ret": syscall_ret
                }

                # Print the event data in JSON format
                print(json.dumps(event_data))

            except struct.error as e:
                print("Error parsing data:", e)
            except IOError: # Ignore IOError exceptions, e.g. broken pipe
                break

    # Close the file when done
    file.close()

if __name__ == '__main__':
    main()
