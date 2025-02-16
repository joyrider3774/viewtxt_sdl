#!/bin/sh

# Launch the process in background, record the PID into a file, wait
# for the process to terminate and erase the recorded PID
"/mnt/Text Files/viewtxt" "-conf=/mnt/Text Files/viewtxt.conf" "$1"&
pid record $!
wait $!
pid erase
